// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "net_processing.h"

#include "budget/budgetmanager.h"
#include "chain.h"
#include "evo/deterministicmns.h"
#include "masternodeman.h"
#include "masternode-payments.h"
#include "masternode-sync.h"
#include "merkleblock.h"
#include "netbase.h"
#include "netmessagemaker.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "sporkdb.h"

int64_t nTimeBestReceived = 0;  // Used only to inform the wallet of when we last received a block

static const uint64_t RANDOMIZER_ID_ADDRESS_RELAY = 0x3cac0035b5866b90ULL; // SHA256("main address relay")[0:8]

struct IteratorComparator
{
    template<typename I>
    bool operator()(const I& a, const I& b) const
    {
        return &(*a) < &(*b);
    }
};

struct COrphanTx {
    // When modifying, adapt the copy of this definition in tests/DoS_tests.
    CTransactionRef tx;
    NodeId fromPeer;
    int64_t nTimeExpire;
    size_t list_pos;
};
RecursiveMutex g_cs_orphans;
std::map<uint256, COrphanTx> mapOrphanTransactions GUARDED_BY(g_cs_orphans);
std::map<COutPoint, std::set<std::map<uint256, COrphanTx>::iterator, IteratorComparator>> mapOrphanTransactionsByPrev GUARDED_BY(g_cs_orphans);
std::vector<std::map<uint256, COrphanTx>::iterator> g_orphan_list GUARDED_BY(g_cs_orphans); //! For random eviction

void EraseOrphansFor(NodeId peer);

// Internal stuff
namespace {

/** Number of nodes with fSyncStarted. */
int nSyncStarted = 0;

/**
 * Sources of received blocks, to be able to send them reject messages or ban
 * them, if processing happens afterwards. Protected by cs_main.
 */
std::map<uint256, NodeId> mapBlockSource;

/**
 * Filter for transactions that were recently rejected by
 * AcceptToMemoryPool. These are not rerequested until the chain tip
 * changes, at which point the entire filter is reset. Protected by
 * cs_main.
 *
 * Without this filter we'd be re-requesting txs from each of our peers,
 * increasing bandwidth consumption considerably. For instance, with 100
 * peers, half of which relay a tx we don't accept, that might be a 50x
 * bandwidth increase. A flooding attacker attempting to roll-over the
 * filter using minimum-sized, 60byte, transactions might manage to send
 * 1000/sec if we have fast peers, so we pick 120,000 to give our peers a
 * two minute window to send invs to us.
 *
 * Decreasing the false positive rate is fairly cheap, so we pick one in a
 * million to make it highly unlikely for users to have issues with this
 * filter.
 *
 * Memory used: 1.3MB
 */
std::unique_ptr<CRollingBloomFilter> recentRejects;
uint256 hashRecentRejectsChainTip;

/** Blocks that are in flight, and that are in the queue to be downloaded. Protected by cs_main. */
struct QueuedBlock {
    uint256 hash;
    const CBlockIndex* pindex; //! Optional.
    int64_t nTime;              //! Time of "getdata" request in microseconds.
    int nValidatedQueuedBefore; //! Number of blocks queued with validated headers (globally) at the time this one is requested.
    bool fValidatedHeaders;     //! Whether this block has validated headers at the time of request.
};
std::map<uint256, std::pair<NodeId, std::list<QueuedBlock>::iterator> > mapBlocksInFlight;

/** Number of blocks in flight with validated headers. */
int nQueuedValidatedHeaders = 0;

/** Number of preferable block download peers. */
int nPreferredDownload = 0;

} // anon namespace

namespace
{

class CNodeBlocks
{
public:
    CNodeBlocks():
            maxSize(0),
            maxAvg(0)
    {
        maxSize = gArgs.GetArg("-blockspamfiltermaxsize", DEFAULT_BLOCK_SPAM_FILTER_MAX_SIZE);
        maxAvg = gArgs.GetArg("-blockspamfiltermaxavg", DEFAULT_BLOCK_SPAM_FILTER_MAX_AVG);
    }

    bool onBlockReceived(int nHeight) {
        if(nHeight > 0 && maxSize && maxAvg) {
            addPoint(nHeight);
            return true;
        }
        return false;
    }

    bool updateState(CValidationState& state, bool ret)
    {
        // No Blocks
        size_t size = points.size();
        if(size == 0)
            return ret;

        // Compute the number of the received blocks
        size_t nBlocks = 0;
        for (auto point : points) {
            nBlocks += point.second;
        }

        // Compute the average value per height
        double nAvgValue = (double)nBlocks / size;

        // Ban the node if try to spam
        bool banNode = (nAvgValue >= 1.5 * maxAvg && size >= maxAvg) ||
                       (nAvgValue >= maxAvg && nBlocks >= maxSize) ||
                       (nBlocks >= maxSize * 3);
        if (banNode) {
            // Clear the points and ban the node
            points.clear();
            return state.DoS(100, error("block-spam ban node for sending spam"));
        }

        return ret;
    }

private:
    void addPoint(int height)
    {
        // Remove the last element in the list
        if(points.size() == maxSize)
        {
            points.erase(points.begin());
        }

        // Add the point to the list
        int occurrence = 0;
        auto mi = points.find(height);
        if (mi != points.end())
            occurrence = (*mi).second;
        occurrence++;
        points[height] = occurrence;
    }

private:
    std::map<int,int> points;
    size_t maxSize;
    size_t maxAvg;
};



 /**
 * Maintain validation-specific state about nodes, protected by cs_main, instead
 * by CNode's own locks. This simplifies asynchronous operation, where
 * processing of incoming data is done after the ProcessMessage call returns,
 * and we're no longer holding the node's locks.
 */
struct CNodeState {
    //! The peer's address
    const CService address;
    //! Whether we have a fully established connection.
    bool fCurrentlyConnected;
    //! Accumulated misbehaviour score for this peer.
    int nMisbehavior;
    //! Whether this peer should be disconnected and banned (unless whitelisted).
    bool fShouldBan;
    //! String name of this peer (debugging/logging purposes).
    const std::string name;
    //! The best known block we know this peer has announced.
    const CBlockIndex* pindexBestKnownBlock;
    //! The hash of the last unknown block this peer has announced.
    uint256 hashLastUnknownBlock;
    //! The last full block we both have.
    const CBlockIndex* pindexLastCommonBlock;
    //! Whether we've started headers synchronization with this peer.
    bool fSyncStarted;
    //! Since when we're stalling block download progress (in microseconds), or 0.
    int64_t nStallingSince;
    std::list<QueuedBlock> vBlocksInFlight;
    int nBlocksInFlight;
    //! Whether we consider this a preferred download peer.
    bool fPreferredDownload;

    CNodeBlocks nodeBlocks;

    CNodeState(CAddress addrIn, std::string addrNameIn) : address(addrIn), name(addrNameIn) {
        fCurrentlyConnected = false;
        nMisbehavior = 0;
        fShouldBan = false;
        pindexBestKnownBlock = NULL;
        hashLastUnknownBlock.SetNull();
        pindexLastCommonBlock = NULL;
        fSyncStarted = false;
        nStallingSince = 0;
        nBlocksInFlight = 0;
        fPreferredDownload = false;
    }
};

/** Map maintaining per-node state. Requires cs_main. */
std::map<NodeId, CNodeState> mapNodeState;

// Requires cs_main.
CNodeState* State(NodeId pnode)
{
    std::map<NodeId, CNodeState>::iterator it = mapNodeState.find(pnode);
    if (it == mapNodeState.end())
        return NULL;
    return &it->second;
}

void UpdatePreferredDownload(CNode* node, CNodeState* state)
{
    nPreferredDownload -= state->fPreferredDownload;

    // Whether this node should be marked as a preferred download node.
    state->fPreferredDownload = (!node->fInbound || node->fWhitelisted) && !node->fOneShot && !node->fClient;

    nPreferredDownload += state->fPreferredDownload;
}

void PushNodeVersion(CNode* pnode, CConnman* connman, int64_t nTime)
{
    ServiceFlags nLocalNodeServices = pnode->GetLocalServices();
    uint64_t nonce = pnode->GetLocalNonce();
    int nNodeStartingHeight = pnode->GetMyStartingHeight();
    NodeId nodeid = pnode->GetId();
    CAddress addr = pnode->addr;

    CAddress addrYou = (addr.IsRoutable() && !IsProxy(addr) ? addr : CAddress(CService(), addr.nServices));
    CAddress addrMe = CAddress(CService(), nLocalNodeServices);

    connman->PushMessage(pnode, CNetMsgMaker(INIT_PROTO_VERSION).Make(NetMsgType::VERSION, PROTOCOL_VERSION, (uint64_t)nLocalNodeServices, nTime, addrYou, addrMe,
            nonce, strSubVersion, nNodeStartingHeight, true));

    if (fLogIPs)
        LogPrint(BCLog::NET, "send version message: version %d, blocks=%d, us=%s, them=%s, peer=%d\n", PROTOCOL_VERSION, nNodeStartingHeight, addrMe.ToString(), addrYou.ToString(), nodeid);
    else
        LogPrint(BCLog::NET, "send version message: version %d, blocks=%d, us=%s, peer=%d\n", PROTOCOL_VERSION, nNodeStartingHeight, addrMe.ToString(), nodeid);
}

// Requires cs_main.
void MarkBlockAsReceived(const uint256& hash)
{
    std::map<uint256, std::pair<NodeId, std::list<QueuedBlock>::iterator> >::iterator itInFlight = mapBlocksInFlight.find(hash);
    if (itInFlight != mapBlocksInFlight.end()) {
        CNodeState* state = State(itInFlight->second.first);
        nQueuedValidatedHeaders -= itInFlight->second.second->fValidatedHeaders;
        state->vBlocksInFlight.erase(itInFlight->second.second);
        state->nBlocksInFlight--;
        state->nStallingSince = 0;
        mapBlocksInFlight.erase(itInFlight);
    }
}

// Requires cs_main.
void MarkBlockAsInFlight(NodeId nodeid, const uint256& hash, const CBlockIndex* pindex = nullptr)
{
    CNodeState* state = State(nodeid);
    assert(state != NULL);

    // Make sure it's not listed somewhere already.
    MarkBlockAsReceived(hash);

    QueuedBlock newentry = {hash, pindex, GetTimeMicros(), nQueuedValidatedHeaders, pindex != NULL};
    nQueuedValidatedHeaders += newentry.fValidatedHeaders;
    std::list<QueuedBlock>::iterator it = state->vBlocksInFlight.insert(state->vBlocksInFlight.end(), newentry);
    state->nBlocksInFlight++;
    mapBlocksInFlight[hash] = std::make_pair(nodeid, it);
}

/** Check whether the last unknown block a peer advertised is not yet known. */
void ProcessBlockAvailability(NodeId nodeid)
{
    CNodeState* state = State(nodeid);
    assert(state != NULL);

    if (!state->hashLastUnknownBlock.IsNull()) {
        BlockMap::iterator itOld = mapBlockIndex.find(state->hashLastUnknownBlock);
        if (itOld != mapBlockIndex.end() && itOld->second->nChainWork > 0) {
            if (state->pindexBestKnownBlock == NULL || itOld->second->nChainWork >= state->pindexBestKnownBlock->nChainWork)
                state->pindexBestKnownBlock = itOld->second;
            state->hashLastUnknownBlock.SetNull();
        }
    }
}

/** Update tracking information about which blocks a peer is assumed to have. */
void UpdateBlockAvailability(NodeId nodeid, const uint256& hash)
{
    CNodeState* state = State(nodeid);
    assert(state != NULL);

    ProcessBlockAvailability(nodeid);

    BlockMap::iterator it = mapBlockIndex.find(hash);
    if (it != mapBlockIndex.end() && it->second->nChainWork > 0) {
        // An actually better block was announced.
        if (state->pindexBestKnownBlock == NULL || it->second->nChainWork >= state->pindexBestKnownBlock->nChainWork)
            state->pindexBestKnownBlock = it->second;
    } else {
        // An unknown block was announced; just assume that the latest one is the best one.
        state->hashLastUnknownBlock = hash;
    }
}

/** Update pindexLastCommonBlock and add not-in-flight missing successors to vBlocks, until it has
 *  at most count entries. */
void FindNextBlocksToDownload(NodeId nodeid, unsigned int count, std::vector<const CBlockIndex*>& vBlocks, NodeId& nodeStaller)
{
    if (count == 0)
        return;

    vBlocks.reserve(vBlocks.size() + count);
    CNodeState* state = State(nodeid);
    assert(state != NULL);

    // Make sure pindexBestKnownBlock is up to date, we'll need it.
    ProcessBlockAvailability(nodeid);

    if (state->pindexBestKnownBlock == NULL || state->pindexBestKnownBlock->nChainWork < chainActive.Tip()->nChainWork) {
        // This peer has nothing interesting.
        return;
    }

    if (state->pindexLastCommonBlock == NULL) {
        // Bootstrap quickly by guessing a parent of our best tip is the forking point.
        // Guessing wrong in either direction is not a problem.
        state->pindexLastCommonBlock = chainActive[std::min(state->pindexBestKnownBlock->nHeight, chainActive.Height())];
    }

    // If the peer reorganized, our previous pindexLastCommonBlock may not be an ancestor
    // of their current tip anymore. Go back enough to fix that.
    state->pindexLastCommonBlock = LastCommonAncestor(state->pindexLastCommonBlock, state->pindexBestKnownBlock);
    if (state->pindexLastCommonBlock == state->pindexBestKnownBlock)
        return;

    std::vector<const CBlockIndex*> vToFetch;
    const CBlockIndex* pindexWalk = state->pindexLastCommonBlock;
    // Never fetch further than the best block we know the peer has, or more than BLOCK_DOWNLOAD_WINDOW + 1 beyond the last
    // linked block we have in common with this peer. The +1 is so we can detect stalling, namely if we would be able to
    // download that next block if the window were 1 larger.
    int nWindowEnd = state->pindexLastCommonBlock->nHeight + BLOCK_DOWNLOAD_WINDOW;
    int nMaxHeight = std::min<int>(state->pindexBestKnownBlock->nHeight, nWindowEnd + 1);
    NodeId waitingfor = -1;
    while (pindexWalk->nHeight < nMaxHeight) {
        // Read up to 128 (or more, if more blocks than that are needed) successors of pindexWalk (towards
        // pindexBestKnownBlock) into vToFetch. We fetch 128, because CBlockIndex::GetAncestor may be as expensive
        // as iterating over ~100 CBlockIndex* entries anyway.
        int nToFetch = std::min(nMaxHeight - pindexWalk->nHeight, std::max<int>(count - vBlocks.size(), 128));
        vToFetch.resize(nToFetch);
        pindexWalk = state->pindexBestKnownBlock->GetAncestor(pindexWalk->nHeight + nToFetch);
        vToFetch[nToFetch - 1] = pindexWalk;
        for (unsigned int i = nToFetch - 1; i > 0; i--) {
            vToFetch[i - 1] = vToFetch[i]->pprev;
        }

        // Iterate over those blocks in vToFetch (in forward direction), adding the ones that
        // are not yet downloaded and not in flight to vBlocks. In the mean time, update
        // pindexLastCommonBlock as long as all ancestors are already downloaded.
        for (const CBlockIndex* pindex : vToFetch) {
            if (!pindex->IsValid(BLOCK_VALID_TREE)) {
                // We consider the chain that this peer is on invalid.
                return;
            }
            if (pindex->nStatus & BLOCK_HAVE_DATA) {
                if (pindex->nChainTx)
                    state->pindexLastCommonBlock = pindex;
            } else if (mapBlocksInFlight.count(pindex->GetBlockHash()) == 0) {
                // The block is not already downloaded, and not yet in flight.
                if (pindex->nHeight > nWindowEnd) {
                    // We reached the end of the window.
                    if (vBlocks.size() == 0 && waitingfor != nodeid) {
                        // We aren't able to fetch anything, but we would be if the download window was one larger.
                        nodeStaller = waitingfor;
                    }
                    return;
                }
                vBlocks.push_back(pindex);
                if (vBlocks.size() == count) {
                    return;
                }
            } else if (waitingfor == -1) {
                // This is the first already-in-flight block.
                waitingfor = mapBlocksInFlight[pindex->GetBlockHash()].first;
            }
        }
    }
}

} // anon namespace

void PeerLogicValidation::InitializeNode(CNode *pnode) {
    CAddress addr = pnode->addr;
    std::string addrName = pnode->GetAddrName();
    NodeId nodeid = pnode->GetId();
    {
        LOCK(cs_main);
        mapNodeState.emplace_hint(mapNodeState.end(), std::piecewise_construct, std::forward_as_tuple(nodeid), std::forward_as_tuple(addr, std::move(addrName)));
    }
    if(!pnode->fInbound)
        PushNodeVersion(pnode, connman, GetTime());
}

void PeerLogicValidation::FinalizeNode(NodeId nodeid, bool& fUpdateConnectionTime)
{
    fUpdateConnectionTime = false;
    LOCK(cs_main);
    CNodeState* state = State(nodeid);

    if (state->fSyncStarted)
        nSyncStarted--;

    if (state->nMisbehavior == 0 && state->fCurrentlyConnected) {
        fUpdateConnectionTime = true;
    }

    for (const QueuedBlock& entry : state->vBlocksInFlight)
        mapBlocksInFlight.erase(entry.hash);
    EraseOrphansFor(nodeid);
    nPreferredDownload -= state->fPreferredDownload;

    mapNodeState.erase(nodeid);
}

bool GetNodeStateStats(NodeId nodeid, CNodeStateStats& stats)
{
    LOCK(cs_main);
    CNodeState* state = State(nodeid);
    if (state == NULL)
        return false;
    stats.nMisbehavior = state->nMisbehavior;
    stats.nSyncHeight = state->pindexBestKnownBlock ? state->pindexBestKnownBlock->nHeight : -1;
    stats.nCommonHeight = state->pindexLastCommonBlock ? state->pindexLastCommonBlock->nHeight : -1;
    for (const QueuedBlock& queue : state->vBlocksInFlight) {
        if (queue.pindex)
            stats.vHeightInFlight.push_back(queue.pindex->nHeight);
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////////
//
// mapOrphanTransactions
//

bool AddOrphanTx(const CTransactionRef& tx, NodeId peer) EXCLUSIVE_LOCKS_REQUIRED(g_cs_orphans)
{
    const uint256& hash = tx->GetHash();
    if (mapOrphanTransactions.count(hash))
        return false;

    // Ignore big transactions, to avoid a
    // send-big-orphans memory exhaustion attack. If a peer has a legitimate
    // large transaction with a missing parent then we assume
    // it will rebroadcast it later, after the parent transaction(s)
    // have been mined or received.
    // 25 orphans, each of which is at most 400,000 bytes big is
    // at most 10 megabytes of orphans and somewhat more byprev index (in the worst case):
    unsigned int sz = tx->GetTotalSize();
    unsigned int nMaxSize = tx->IsShieldedTx() ? MAX_TX_SIZE_AFTER_SAPLING : MAX_STANDARD_TX_SIZE;
    if (sz >= nMaxSize) {
        LogPrint(BCLog::MEMPOOL, "ignoring large orphan tx (size: %u, hash: %s)\n", sz, hash.ToString());
        return false;
    }

    auto ret = mapOrphanTransactions.emplace(hash, COrphanTx{tx, peer, GetTime() + ORPHAN_TX_EXPIRE_TIME, g_orphan_list.size()});
    assert(ret.second);
    g_orphan_list.emplace_back(ret.first);
    for (const CTxIn& txin : tx->vin) {
        mapOrphanTransactionsByPrev[txin.prevout].insert(ret.first);
    }

    LogPrint(BCLog::MEMPOOL, "stored orphan tx %s (mapsz %u outsz %u)\n", hash.ToString(),
        mapOrphanTransactions.size(), mapOrphanTransactionsByPrev.size());
    return true;
}

int static EraseOrphanTx(uint256 hash) EXCLUSIVE_LOCKS_REQUIRED(g_cs_orphans)
{
    std::map<uint256, COrphanTx>::iterator it = mapOrphanTransactions.find(hash);
    if (it == mapOrphanTransactions.end())
        return 0;
    for (const CTxIn& txin : it->second.tx->vin) {
        auto itPrev = mapOrphanTransactionsByPrev.find(txin.prevout);
        if (itPrev == mapOrphanTransactionsByPrev.end())
            continue;
        itPrev->second.erase(it);
        if (itPrev->second.empty())
            mapOrphanTransactionsByPrev.erase(itPrev);
    }

    size_t old_pos = it->second.list_pos;
    assert(g_orphan_list[old_pos] == it);
    if (old_pos + 1 != g_orphan_list.size()) {
        // Unless we're deleting the last entry in g_orphan_list, move the last
        // entry to the position we're deleting.
        auto it_last = g_orphan_list.back();
        g_orphan_list[old_pos] = it_last;
        it_last->second.list_pos = old_pos;
    }
    g_orphan_list.pop_back();

    mapOrphanTransactions.erase(it);
    return 1;
}

void EraseOrphansFor(NodeId peer)
{
    LOCK(g_cs_orphans);
    int nErased = 0;
    std::map<uint256, COrphanTx>::iterator iter = mapOrphanTransactions.begin();
    while (iter != mapOrphanTransactions.end()) {
        std::map<uint256, COrphanTx>::iterator maybeErase = iter++; // increment to avoid iterator becoming invalid
        if (maybeErase->second.fromPeer == peer) {
            nErased += EraseOrphanTx(maybeErase->second.tx->GetHash());
        }
    }
    if (nErased > 0) LogPrint(BCLog::MEMPOOL, "Erased %d orphan tx from peer %d\n", nErased, peer);
}


unsigned int LimitOrphanTxSize(unsigned int nMaxOrphans)
{
    LOCK(g_cs_orphans);

    unsigned int nEvicted = 0;
    static int64_t nNextSweep;
    int64_t nNow = GetTime();
    if (nNextSweep <= nNow) {
        // Sweep out expired orphan pool entries:
        int nErased = 0;
        int64_t nMinExpTime = nNow + ORPHAN_TX_EXPIRE_TIME - ORPHAN_TX_EXPIRE_INTERVAL;
        auto iter = mapOrphanTransactions.begin();
        while (iter != mapOrphanTransactions.end()) {
            auto maybeErase = iter++;
            if (maybeErase->second.nTimeExpire <= nNow) {
                nErased += EraseOrphanTx(maybeErase->second.tx->GetHash());
            } else {
                nMinExpTime = std::min(maybeErase->second.nTimeExpire, nMinExpTime);
            }
        }
        // Sweep again 5 minutes after the next entry that expires in order to batch the linear scan.
        nNextSweep = nMinExpTime + ORPHAN_TX_EXPIRE_INTERVAL;
        if (nErased > 0) LogPrint(BCLog::MEMPOOL, "Erased %d orphan tx due to expiration\n", nErased);
    }
    FastRandomContext rng;
    while (mapOrphanTransactions.size() > nMaxOrphans) {
        // Evict a random orphan:
        size_t randompos = rng.randrange(g_orphan_list.size());
        EraseOrphanTx(g_orphan_list[randompos]->first);
        ++nEvicted;
    }
    return nEvicted;
}

// Requires cs_main.
void Misbehaving(NodeId pnode, int howmuch) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    if (howmuch == 0)
        return;

    CNodeState* state = State(pnode);
    if (state == NULL)
        return;

    state->nMisbehavior += howmuch;
    int banscore = gArgs.GetArg("-banscore", DEFAULT_BANSCORE_THRESHOLD);
    if (state->nMisbehavior >= banscore && state->nMisbehavior - howmuch < banscore) {
        LogPrintf("Misbehaving: %s (%d -> %d) BAN THRESHOLD EXCEEDED\n", state->name, state->nMisbehavior - howmuch, state->nMisbehavior);
        state->fShouldBan = true;
    } else
        LogPrintf("Misbehaving: %s (%d -> %d)\n", state->name, state->nMisbehavior - howmuch, state->nMisbehavior);
}

static void CheckBlockSpam(NodeId nodeId, const uint256& hashBlock)
{
    // Block spam filtering
    if (!gArgs.GetBoolArg("-blockspamfilter", DEFAULT_BLOCK_SPAM_FILTER)) {
        return;
    }

    CNodeState* nodestate = nullptr;
    int blockReceivedHeight = 0;
    {
        LOCK(cs_main);
        nodestate = State(nodeId);
        if (!nodestate) { return; }

        const auto it = mapBlockIndex.find(hashBlock);
        if (it == mapBlockIndex.end()) { return; }
        blockReceivedHeight = it->second->nHeight;
    }

    nodestate->nodeBlocks.onBlockReceived(blockReceivedHeight);
    bool nodeStatus = true;
    // UpdateState will return false if the node is attacking us or update the score and return true.
    CValidationState state;
    nodeStatus = nodestate->nodeBlocks.updateState(state, nodeStatus);
    int nDoS = 0;
    if (state.IsInvalid(nDoS)) {
        if (nDoS > 0) {
            LOCK(cs_main);
            Misbehaving(nodeId, nDoS);
        }
        nodeStatus = false;
    }

    if (!nodeStatus) {
        LogPrintf("Block spam protection: %s\n", hashBlock.ToString());
    }
}


//////////////////////////////////////////////////////////////////////////////
//
// blockchain -> download logic notification
//

PeerLogicValidation::PeerLogicValidation(CConnman* connmanIn) :
        connman(connmanIn)
{
    // Initialize global variables that cannot be constructed at startup.
    recentRejects.reset(new CRollingBloomFilter(120000, 0.000001));
}

void PeerLogicValidation::BlockConnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex* pindex)
{
    LOCK(g_cs_orphans);

    std::vector<uint256> vOrphanErase;

    for (const CTransactionRef& ptx : pblock->vtx) {
        const CTransaction& tx = *ptx;

        // Which orphan pool entries must we evict?
        for (size_t j = 0; j < tx.vin.size(); j++) {
            auto itByPrev = mapOrphanTransactionsByPrev.find(tx.vin[j].prevout);
            if (itByPrev == mapOrphanTransactionsByPrev.end()) continue;
            for (auto mi = itByPrev->second.begin(); mi != itByPrev->second.end(); ++mi) {
                const CTransaction& orphanTx = *(*mi)->second.tx;
                const uint256& orphanHash = orphanTx.GetHash();
                vOrphanErase.emplace_back(orphanHash);
            }
        }
    }

    // Erase orphan transactions include or precluded by this block
    if (!vOrphanErase.empty()) {
        int nErased = 0;
        for (uint256& orphanHash : vOrphanErase) {
            nErased += EraseOrphanTx(orphanHash);
        }
        LogPrint(BCLog::MEMPOOL, "Erased %d orphan tx included or conflicted by block\n", nErased);
    }
}

void PeerLogicValidation::UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload)
{
    const int nNewHeight = pindexNew->nHeight;
    connman->SetBestHeight(nNewHeight);

    if (!fInitialDownload) {
        const uint256& hashNewTip = pindexNew->GetBlockHash();
        // Relay inventory, but don't relay old inventory during initial block download.
        connman->ForEachNode([nNewHeight, hashNewTip](CNode* pnode) {
            if (nNewHeight > (pnode->nStartingHeight != -1 ? pnode->nStartingHeight - 2000 : 0)) {
                pnode->PushInventory(CInv(MSG_BLOCK, hashNewTip));
            }
        });
    }

    nTimeBestReceived = GetTime();
}

void PeerLogicValidation::BlockChecked(const CBlock& block, const CValidationState& state)
{
    LOCK(cs_main);

    const uint256& hash = block.GetHash();
    std::map<uint256, NodeId>::iterator it = mapBlockSource.find(hash);

    int nDoS = 0;
    if (state.IsInvalid(nDoS)) {
        if (it != mapBlockSource.end() && State(it->second)) {
            assert (state.GetRejectCode() < REJECT_INTERNAL); // Blocks are never rejected with internal reject codes
            if (nDoS > 0) {
                Misbehaving(it->second, nDoS);
            }

            // Spam filter
            CheckBlockSpam(it->second, block.GetHash());
        }
    }

    if (it != mapBlockSource.end())
        mapBlockSource.erase(it);
}

//////////////////////////////////////////////////////////////////////////////
//
// Messages
//

bool static AlreadyHave(const CInv& inv) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    switch (inv.type) {
    case MSG_TX: {
        assert(recentRejects);
        if (chainActive.Tip()->GetBlockHash() != hashRecentRejectsChainTip) {
            // If the chain tip has changed previously rejected transactions
            // might be now valid, e.g. due to a nLockTime'd tx becoming valid,
            // or a double-spend. Reset the rejects filter and give those
            // txs a second chance.
            hashRecentRejectsChainTip = chainActive.Tip()->GetBlockHash();
            recentRejects->reset();
        }

        {
            LOCK(g_cs_orphans);
            if (mapOrphanTransactions.count(inv.hash)) return true;
        }

        return recentRejects->contains(inv.hash) ||
               mempool.exists(inv.hash) ||
               pcoinsTip->HaveCoinInCache(COutPoint(inv.hash, 0)) || // Best effort: only try output 0 and 1
               pcoinsTip->HaveCoinInCache(COutPoint(inv.hash, 1));
    }

    case MSG_BLOCK:
        return mapBlockIndex.count(inv.hash);
    case MSG_TXLOCK_REQUEST:
        // deprecated
        return true;
    case MSG_TXLOCK_VOTE:
        // deprecated
        return true;
    case MSG_SPORK:
        return mapSporks.count(inv.hash);
    case MSG_MASTERNODE_WINNER:
        if (masternodePayments.mapMasternodePayeeVotes.count(inv.hash)) {
            masternodeSync.AddedMasternodeWinner(inv.hash);
            return true;
        }
        return false;
    case MSG_BUDGET_VOTE:
        if (g_budgetman.HaveSeenProposalVote(inv.hash)) {
            masternodeSync.AddedBudgetItem(inv.hash);
            return true;
        }
        return false;
    case MSG_BUDGET_PROPOSAL:
        if (g_budgetman.HaveProposal(inv.hash)) {
            masternodeSync.AddedBudgetItem(inv.hash);
            return true;
        }
        return false;
    case MSG_BUDGET_FINALIZED_VOTE:
        if (g_budgetman.HaveSeenFinalizedBudgetVote(inv.hash)) {
            masternodeSync.AddedBudgetItem(inv.hash);
            return true;
        }
        return false;
    case MSG_BUDGET_FINALIZED:
        if (g_budgetman.HaveFinalizedBudget(inv.hash)) {
            masternodeSync.AddedBudgetItem(inv.hash);
            return true;
        }
        return false;
    case MSG_MASTERNODE_ANNOUNCE:
        if (mnodeman.mapSeenMasternodeBroadcast.count(inv.hash)) {
            masternodeSync.AddedMasternodeList(inv.hash);
            return true;
        }
        return false;
    case MSG_MASTERNODE_PING:
        return mnodeman.mapSeenMasternodePing.count(inv.hash);
    }
    // Don't know what it is, just say we already got one
    return true;
}

static void RelayTransaction(const CTransaction& tx, CConnman* connman)
{
    CInv inv(MSG_TX, tx.GetHash());
    connman->ForEachNode([&inv](CNode* pnode)
    {
        pnode->PushInventory(inv);
    });
}

static void RelayAddress(const CAddress& addr, bool fReachable, CConnman* connman)
{
    int nRelayNodes = fReachable ? 2 : 1; // limited relaying of addresses outside our network(s)

    // Relay to a limited number of other nodes
    // Use deterministic randomness to send to the same nodes for 24 hours
    // at a time so the addrKnowns of the chosen nodes prevent repeats
    uint64_t hashAddr = addr.GetHash();
    std::multimap<uint64_t, CNode*> mapMix;
    const CSipHasher hasher = connman->GetDeterministicRandomizer(RANDOMIZER_ID_ADDRESS_RELAY).Write(hashAddr << 32).Write((GetTime() + hashAddr) / (24*60*60));

    auto sortfunc = [&mapMix, &hasher](CNode* pnode) {
        if (pnode->nVersion >= CADDR_TIME_VERSION) {
            uint64_t hashKey = CSipHasher(hasher).Write(pnode->id).Finalize();
            mapMix.emplace(hashKey, pnode);
        }
    };

    auto pushfunc = [&addr, &mapMix, &nRelayNodes] {
        FastRandomContext insecure_rand;
        for (auto mi = mapMix.begin(); mi != mapMix.end() && nRelayNodes-- > 0; ++mi)
            mi->second->PushAddress(addr, insecure_rand);
    };

    connman->ForEachNodeThen(std::move(sortfunc), std::move(pushfunc));
}

bool static PushTierTwoGetDataRequest(const CInv& inv,
                                      CNode* pfrom,
                                      CConnman* connman,
                                      CNetMsgMaker& msgMaker)
{
    if (inv.type == MSG_SPORK) {
        if (mapSporks.count(inv.hash)) {
            CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
            ss.reserve(1000);
            ss << mapSporks[inv.hash];
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::SPORK, ss));
            return true;
        }
    }

    // !TODO: remove when transition to DMN is complete
    if (inv.type == MSG_MASTERNODE_WINNER && !deterministicMNManager->LegacyMNObsolete()) {
        if (masternodePayments.mapMasternodePayeeVotes.count(inv.hash)) {
            CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
            ss.reserve(1000);
            ss << masternodePayments.mapMasternodePayeeVotes[inv.hash];
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::MNWINNER, ss));
            return true;
        }
    }

    if (inv.type == MSG_BUDGET_VOTE) {
        if (g_budgetman.HaveSeenProposalVote(inv.hash)) {
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::BUDGETVOTE, g_budgetman.GetProposalVoteSerialized(inv.hash)));
            return true;
        }
    }

    if (inv.type == MSG_BUDGET_PROPOSAL) {
        if (g_budgetman.HaveProposal(inv.hash)) {
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::BUDGETPROPOSAL, g_budgetman.GetProposalSerialized(inv.hash)));
            return true;
        }
    }

    if (inv.type == MSG_BUDGET_FINALIZED_VOTE) {
        if (g_budgetman.HaveSeenFinalizedBudgetVote(inv.hash)) {
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::FINALBUDGETVOTE, g_budgetman.GetFinalizedBudgetVoteSerialized(inv.hash)));
            return true;
        }
    }

    if (inv.type == MSG_BUDGET_FINALIZED) {
        if (g_budgetman.HaveFinalizedBudget(inv.hash)) {
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::FINALBUDGET, g_budgetman.GetFinalizedBudgetSerialized(inv.hash)));
            return true;
        }
    }

    // !TODO: remove when transition to DMN is complete
    if (inv.type == MSG_MASTERNODE_ANNOUNCE && !deterministicMNManager->LegacyMNObsolete()) {
        if (mnodeman.mapSeenMasternodeBroadcast.count(inv.hash)) {
            CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
            ss.reserve(1000);
            ss << mnodeman.mapSeenMasternodeBroadcast[inv.hash];
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::MNBROADCAST, ss));
            return true;
        }
    }

    // !TODO: remove when transition to DMN is complete
    if (inv.type == MSG_MASTERNODE_PING && !deterministicMNManager->LegacyMNObsolete()) {
        if (mnodeman.mapSeenMasternodePing.count(inv.hash)) {
            CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
            ss.reserve(1000);
            ss << mnodeman.mapSeenMasternodePing[inv.hash];
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::MNPING, ss));
            return true;
        }
    }

    // nothing was pushed.
    return false;
}

void static ProcessGetBlockData(CNode* pfrom, const CInv& inv, CConnman* connman, const std::atomic<bool>& interruptMsgProc)
{
    LOCK(cs_main);
    CNetMsgMaker msgMaker(pfrom->GetSendVersion());

    bool send = false;
    BlockMap::iterator mi = mapBlockIndex.find(inv.hash);
    if (mi != mapBlockIndex.end()) {
        if (chainActive.Contains(mi->second)) {
            send = true;
        } else {
            // To prevent fingerprinting attacks, only send blocks outside of the active
            // chain if they are valid, and no more than a max reorg depth than the best header
            // chain we know about.
            send = mi->second->IsValid(BLOCK_VALID_SCRIPTS) && (pindexBestHeader != NULL) &&
                   (chainActive.Height() - mi->second->nHeight < gArgs.GetArg("-maxreorg", DEFAULT_MAX_REORG_DEPTH));
            if (!send) {
                LogPrint(BCLog::NET, "ProcessGetData(): ignoring request from peer=%i for old block that isn't in the main chain\n", pfrom->GetId());
            }
        }
    }
    // Don't send not-validated blocks
    if (send && (mi->second->nStatus & BLOCK_HAVE_DATA)) {
        // Send block from disk
        CBlock block;
        if (!ReadBlockFromDisk(block, (*mi).second))
            assert(!"cannot load block from disk");
        if (inv.type == MSG_BLOCK)
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::BLOCK, block));
        else // MSG_FILTERED_BLOCK)
        {
            bool send_ = false;
            CMerkleBlock merkleBlock;
            {
                LOCK(pfrom->cs_filter);
                if (pfrom->pfilter) {
                    send_ = true;
                    merkleBlock = CMerkleBlock(block, *pfrom->pfilter);
                }
            }
            if (send_) {
                connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::MERKLEBLOCK, merkleBlock));
                // CMerkleBlock just contains hashes, so also push any transactions in the block the client did not see
                // This avoids hurting performance by pointlessly requiring a round-trip
                // Note that there is currently no way for a node to request any single transactions we didnt send here -
                // they must either disconnect and retry or request the full block.
                // Thus, the protocol spec specified allows for us to provide duplicate txn here,
                // however we MUST always provide at least what the remote peer needs
                for (std::pair<unsigned int, uint256>& pair : merkleBlock.vMatchedTxn)
                    connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::TX, *block.vtx[pair.first]));
            }
            // else
            // no response
        }

        // Trigger them to send a getblocks request for the next batch of inventory
        if (inv.hash == pfrom->hashContinue) {
            // Bypass PushInventory, this must send even if redundant,
            // and we want it right after the last block so they don't
            // wait for other stuff first.
            std::vector<CInv> vInv;
            vInv.emplace_back(MSG_BLOCK, chainActive.Tip()->GetBlockHash());
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::INV, vInv));
            pfrom->hashContinue.SetNull();
        }
    }
}

// Only return true if the inv type can be answered, not supported types return false.
bool static IsTierTwoInventoryTypeKnown(int type)
{
    return type == MSG_SPORK ||
           type == MSG_MASTERNODE_WINNER ||
           type == MSG_BUDGET_VOTE ||
           type == MSG_BUDGET_PROPOSAL ||
           type == MSG_BUDGET_FINALIZED ||
           type == MSG_BUDGET_FINALIZED_VOTE ||
           type == MSG_MASTERNODE_ANNOUNCE ||
           type == MSG_MASTERNODE_PING;
}

void static ProcessGetData(CNode* pfrom, CConnman* connman, const std::atomic<bool>& interruptMsgProc)
{
    AssertLockNotHeld(cs_main);

    std::deque<CInv>::iterator it = pfrom->vRecvGetData.begin();
    std::vector<CInv> vNotFound;
    CNetMsgMaker msgMaker(pfrom->GetSendVersion());
    {
        LOCK(cs_main);

        while (it != pfrom->vRecvGetData.end() && (it->type == MSG_TX || IsTierTwoInventoryTypeKnown(it->type))) {
            if (interruptMsgProc)
                return;
            // Don't bother if send buffer is too full to respond anyway
            if (pfrom->fPauseSend)
                break;

            const CInv &inv = *it;
            it++;

            // Send stream from relay memory
            bool pushed = false;
            if (inv.type == MSG_TX) {
                auto txinfo = mempool.info(inv.hash);
                if (txinfo.tx) { // future: add timeLastMempoolReq check
                    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                    ss.reserve(1000);
                    ss << *txinfo.tx;
                    connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::TX, ss));
                    pushed = true;
                }
            }

            if (!pushed) {
                // Now check if it's a tier two data request and push it.
                pushed = PushTierTwoGetDataRequest(inv, pfrom, connman, msgMaker);
            }

            if (!pushed) {
                vNotFound.push_back(inv);
            }

            // todo: inventory signal
        }
    } // release cs_main

    if (it != pfrom->vRecvGetData.end()) {
        const CInv &inv = *it;
        it++;
        if (inv.type == MSG_BLOCK || inv.type == MSG_FILTERED_BLOCK) {
            ProcessGetBlockData(pfrom, inv, connman, interruptMsgProc);
        }
    }

    pfrom->vRecvGetData.erase(pfrom->vRecvGetData.begin(), it);

    if (!vNotFound.empty()) {
        // Let the peer know that we didn't find what it asked for, so it doesn't
        // have to wait around forever. Currently only SPV clients actually care
        // about this message: it's needed when they are recursively walking the
        // dependencies of relevant unconfirmed transactions. SPV clients want to
        // do that because they want to know about (and store and rebroadcast and
        // risk analyze) the dependencies of transactions relevant to them, without
        // having to download the entire memory pool.
        connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::NOTFOUND, vNotFound));
    }
}

bool fRequestedSporksIDB = false;
bool static ProcessMessage(CNode* pfrom, std::string strCommand, CDataStream& vRecv, int64_t nTimeReceived, CConnman* connman, std::atomic<bool>& interruptMsgProc)
{
    LogPrint(BCLog::NET, "received: %s (%u bytes) peer=%d\n", SanitizeString(strCommand), vRecv.size(), pfrom->id);
    if (gArgs.IsArgSet("-dropmessagestest") && GetRand(gArgs.GetArg("-dropmessagestest", 0)) == 0) {
        LogPrintf("dropmessagestest DROPPING RECV MESSAGE\n");
        return true;
    }

    if (strCommand == NetMsgType::VERSION) {
        // Each connection can only send one version message
        if (pfrom->nVersion != 0) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 1);
            return false;
        }

        int64_t nTime;
        CAddress addrMe;
        CAddress addrFrom;
        uint64_t nNonce = 1;
        uint64_t nServiceInt;
        ServiceFlags nServices;
        int nVersion;
        int nSendVersion;
        std::string strSubVer;
        std::string cleanSubVer;
        int nStartingHeight = -1;
        vRecv >> nVersion >> nServiceInt >> nTime >> addrMe;
        nSendVersion = std::min(nVersion, PROTOCOL_VERSION);
        nServices = ServiceFlags(nServiceInt);
        if (!pfrom->fInbound) {
            connman->SetServices(pfrom->addr, nServices);
        }
        if (pfrom->nServicesExpected & ~nServices) {
            LogPrint(BCLog::NET, "peer=%d does not offer the expected services (%08x offered, %08x expected); disconnecting\n", pfrom->id, nServices, pfrom->nServicesExpected);
            pfrom->fDisconnect = true;
            return false;
        }

        if (pfrom->DisconnectOldProtocol(nVersion, ActiveProtocol())) {
            return false;
        }

        if (nVersion == 10300)
            nVersion = 300;
        if (!vRecv.empty())
            vRecv >> addrFrom >> nNonce;
        if (!vRecv.empty()) {
            vRecv >> LIMITED_STRING(strSubVer, MAX_SUBVERSION_LENGTH);
            cleanSubVer = SanitizeString(strSubVer);
        }
        if (!vRecv.empty()) {
            vRecv >> nStartingHeight;
        }

        // Disconnect if we connected to ourself
        if (pfrom->fInbound && !connman->CheckIncomingNonce(nNonce)) {
            LogPrintf("connected to self at %s, disconnecting\n", pfrom->addr.ToString());
            pfrom->fDisconnect = true;
            return true;
        }

        if (pfrom->fInbound && addrMe.IsRoutable()) {
            SeenLocal(addrMe);
        }

        // Be shy and don't send version until we hear
        if (pfrom->fInbound)
            PushNodeVersion(pfrom, connman, GetAdjustedTime());

        connman->PushMessage(pfrom, CNetMsgMaker(INIT_PROTO_VERSION).Make(NetMsgType::VERACK));

        pfrom->nServices = nServices;
        pfrom->SetAddrLocal(addrMe);
        {
            LOCK(pfrom->cs_SubVer);
            pfrom->strSubVer = strSubVer;
            pfrom->cleanSubVer = cleanSubVer;
        }
        pfrom->nStartingHeight = nStartingHeight;
        pfrom->fClient = !(nServices & NODE_NETWORK);

        {
            LOCK(pfrom->cs_filter);
            if (!vRecv.empty()) {
                vRecv >> pfrom->fRelayTxes; // set to true after we get the first filter* message
            } else {
                pfrom->fRelayTxes = true;
            }
        }

        // Change version
        pfrom->SetSendVersion(nSendVersion);
        pfrom->nVersion = nVersion;

        {
            LOCK(cs_main);
            // Potentially mark this peer as a preferred download peer.
            UpdatePreferredDownload(pfrom, State(pfrom->GetId()));
        }

        if (!pfrom->fInbound) {
            // Advertise our address
            if (fListen && !IsInitialBlockDownload()) {
                CAddress addr = GetLocalAddress(&pfrom->addr, pfrom->GetLocalServices());
                FastRandomContext insecure_rand;
                if (addr.IsRoutable()) {
                    LogPrintf("ProcessMessages: advertising address %s\n", addr.ToString());
                    pfrom->PushAddress(addr, insecure_rand);
                } else if (IsPeerAddrLocalGood(pfrom)) {
                    addr.SetIP(addrMe);
                    LogPrintf("ProcessMessages: advertising address %s\n", addr.ToString());
                    pfrom->PushAddress(addr, insecure_rand);
                }
            }

            // Get recent addresses
            if (pfrom->fOneShot || pfrom->nVersion >= CADDR_TIME_VERSION || connman->GetAddressCount() < 1000) {
                connman->PushMessage(pfrom, CNetMsgMaker(nSendVersion).Make(NetMsgType::GETADDR));
                pfrom->fGetAddr = true;
            }
            connman->MarkAddressGood(pfrom->addr);
        }

        std::string remoteAddr;
        if (fLogIPs)
            remoteAddr = ", peeraddr=" + pfrom->addr.ToString();

        LogPrint(BCLog::NET, "receive version message: %s: version %d, blocks=%d, us=%s, peer=%d%s\n",
            cleanSubVer, pfrom->nVersion,
            pfrom->nStartingHeight, addrMe.ToString(), pfrom->id,
            remoteAddr);

        int64_t nTimeOffset = nTime - GetTime();
        pfrom->nTimeOffset = nTimeOffset;
        const int nTimeSlotLength = Params().GetConsensus().nTimeSlotLength;
        if (abs64(nTimeOffset) < 2 * nTimeSlotLength) {
            AddTimeData(pfrom->addr, nTimeOffset, nTimeSlotLength);
        } else {
            LogPrintf("timeOffset (%d seconds) too large. Disconnecting node %s\n",
                    nTimeOffset, pfrom->addr.ToString().c_str());
            pfrom->fDisconnect = true;
            CheckOffsetDisconnectedPeers(pfrom->addr);
        }

        // Feeler connections exist only to verify if address is online.
        if (pfrom->fFeeler) {
            assert(pfrom->fInbound == false);
            pfrom->fDisconnect = true;
        }

        // PIVX: We use certain sporks during IBD, so check to see if they are
        // available. If not, ask the first peer connected for them.
        // TODO: Move this to an instant broadcast of the sporks.
        bool fMissingSporks = !pSporkDB->SporkExists(SPORK_14_NEW_PROTOCOL_ENFORCEMENT) ||
                              !pSporkDB->SporkExists(SPORK_15_NEW_PROTOCOL_ENFORCEMENT_2) ||
                              !pSporkDB->SporkExists(SPORK_16_ZEROCOIN_MAINTENANCE_MODE) ||
                              !pSporkDB->SporkExists(SPORK_18_ZEROCOIN_PUBLICSPEND_V4) ||
                              !pSporkDB->SporkExists(SPORK_19_COLDSTAKING_MAINTENANCE) ||
                              !pSporkDB->SporkExists(SPORK_20_SAPLING_MAINTENANCE);

        if (fMissingSporks || !fRequestedSporksIDB){
            LogPrintf("asking peer for sporks\n");
            connman->PushMessage(pfrom, CNetMsgMaker(nSendVersion).Make(NetMsgType::GETSPORKS));
            fRequestedSporksIDB = true;
        }

        return true;
    }


    else if (pfrom->nVersion == 0) {
        // Must have a version message before anything else
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), 1);
        return false;
    }

    // At this point, the outgoing message serialization version can't change.
    CNetMsgMaker msgMaker(pfrom->GetSendVersion());

    if (strCommand == NetMsgType::VERACK) {
        pfrom->SetRecvVersion(std::min(pfrom->nVersion.load(), PROTOCOL_VERSION));

        // Mark this node as currently connected, so we update its timestamp later.
        if (pfrom->fNetworkNode) {
            LOCK(cs_main);
            State(pfrom->GetId())->fCurrentlyConnected = true;
        }
        pfrom->fSuccessfullyConnected = true;
        LogPrintf("New outbound peer connected: version: %d, blocks=%d, peer=%d%s\n",
                  pfrom->nVersion.load(), pfrom->nStartingHeight, pfrom->GetId(),
                  (fLogIPs ? strprintf(", peeraddr=%s", pfrom->addr.ToString()) : ""));
    }


    else if (strCommand == NetMsgType::ADDR) {
        std::vector<CAddress> vAddr;
        vRecv >> vAddr;

        // Don't want addr from older versions unless seeding
        if (pfrom->nVersion < CADDR_TIME_VERSION && connman->GetAddressCount() > 1000)
            return true;
        if (vAddr.size() > MAX_ADDR_TO_SEND) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 20);
            return error("message addr size() = %u", vAddr.size());
        }

        // Store the new addresses
        std::vector<CAddress> vAddrOk;
        int64_t nNow = GetAdjustedTime();
        int64_t nSince = nNow - 10 * 60;
        for (CAddress& addr : vAddr) {
            if (interruptMsgProc)
                return true;

            if ((addr.nServices & REQUIRED_SERVICES) != REQUIRED_SERVICES)
                continue;

            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
                addr.nTime = nNow - 5 * 24 * 60 * 60;
            pfrom->AddAddressKnown(addr);
            bool fReachable = IsReachable(addr);
            if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable()) {
                // Relay to a limited number of other nodes
                RelayAddress(addr, fReachable, connman);
            }
            // Do not store addresses outside our network
            if (fReachable)
                vAddrOk.push_back(addr);
        }
        connman->AddNewAddresses(vAddrOk, pfrom->addr, 2 * 60 * 60);
        if (vAddr.size() < 1000)
            pfrom->fGetAddr = false;
        if (pfrom->fOneShot)
            pfrom->fDisconnect = true;
    }


    else if (strCommand == NetMsgType::INV) {
        std::vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 20);
            return error("peer=%d message inv size() = %u", pfrom->GetId(), vInv.size());
        }

        LOCK(cs_main);

        std::vector<CInv> vToFetch;

        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++) {
            const CInv& inv = vInv[nInv];

            if (interruptMsgProc)
                return true;

            // Reject deprecated messages
            if (inv.type == MSG_TXLOCK_REQUEST || inv.type == MSG_TXLOCK_VOTE) {
                Misbehaving(pfrom->GetId(), 20);
                return error("message inv deprecated %d", (int)inv.type);
            }

            pfrom->AddInventoryKnown(inv);

            bool fAlreadyHave = AlreadyHave(inv);
            LogPrint(BCLog::NET, "got inv: %s  %s peer=%d\n", inv.ToString(), fAlreadyHave ? "have" : "new", pfrom->id);

            if (!fAlreadyHave && !fImporting && !fReindex && inv.type != MSG_BLOCK)
                pfrom->AskFor(inv);


            if (inv.type == MSG_BLOCK) {
                UpdateBlockAvailability(pfrom->GetId(), inv.hash);
                if (!fAlreadyHave && !fImporting && !fReindex && !mapBlocksInFlight.count(inv.hash)) {
                    // Add this to the list of blocks to request
                    vToFetch.push_back(inv);
                    LogPrint(BCLog::NET, "getblocks (%d) %s to peer=%d\n", pindexBestHeader->nHeight, inv.hash.ToString(), pfrom->id);
                }
            }

        }

        if (!vToFetch.empty())
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::GETDATA, vToFetch));
    }


    else if (strCommand == NetMsgType::GETDATA) {
        std::vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 20);
            return error("peer=%d message getdata size() = %u", pfrom->GetId(), vInv.size());
        }

        if (vInv.size() != 1)
            LogPrint(BCLog::NET, "received getdata (%u invsz) peer=%d\n", vInv.size(), pfrom->id);

        if (vInv.size() > 0)
            LogPrint(BCLog::NET, "received getdata for: %s peer=%d\n", vInv[0].ToString(), pfrom->id);

        pfrom->vRecvGetData.insert(pfrom->vRecvGetData.end(), vInv.begin(), vInv.end());
        ProcessGetData(pfrom, connman, interruptMsgProc);
    }


    else if (strCommand == NetMsgType::GETBLOCKS || strCommand == NetMsgType::GETHEADERS) {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        if (locator.vHave.size() > MAX_LOCATOR_SZ) {
            LogPrint(BCLog::NET, "getblocks locator size %lld > %d, disconnect peer=%d\n", locator.vHave.size(), MAX_LOCATOR_SZ, pfrom->GetId());
            pfrom->fDisconnect = true;
            return true;
        }

        LOCK(cs_main);

        // Find the last block the caller has in the main chain
        CBlockIndex* pindex = FindForkInGlobalIndex(chainActive, locator);

        // Send the rest of the chain
        if (pindex)
            pindex = chainActive.Next(pindex);
        int nLimit = 500;
        LogPrint(BCLog::NET, "getblocks %d to %s limit %d from peer=%d\n", (pindex ? pindex->nHeight : -1), hashStop.IsNull() ? "end" : hashStop.ToString(), nLimit, pfrom->id);
        for (; pindex; pindex = chainActive.Next(pindex)) {
            if (pindex->GetBlockHash() == hashStop) {
                LogPrint(BCLog::NET, "  getblocks stopping at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                break;
            }
            pfrom->PushInventory(CInv(MSG_BLOCK, pindex->GetBlockHash()));
            if (--nLimit <= 0) {
                // When this block is requested, we'll send an inv that'll make them
                // getblocks the next batch of inventory.
                LogPrint(BCLog::NET, "  getblocks stopping at limit %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString());
                pfrom->hashContinue = pindex->GetBlockHash();
                break;
            }
        }
    }


    else if (strCommand == NetMsgType::HEADERS && Params().HeadersFirstSyncingActive()) {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        if (locator.vHave.size() > MAX_LOCATOR_SZ) {
            LogPrint(BCLog::NET, "getblocks locator size %lld > %d, disconnect peer=%d\n", locator.vHave.size(), MAX_LOCATOR_SZ, pfrom->GetId());
            pfrom->fDisconnect = true;
            return true;
        }

        LOCK(cs_main);

        if (IsInitialBlockDownload())
            return true;

        CBlockIndex* pindex = NULL;
        if (locator.IsNull()) {
            // If locator is null, return the hashStop block
            BlockMap::iterator mi = mapBlockIndex.find(hashStop);
            if (mi == mapBlockIndex.end())
                return true;
            pindex = (*mi).second;
        } else {
            // Find the last block the caller has in the main chain
            pindex = FindForkInGlobalIndex(chainActive, locator);
            if (pindex)
                pindex = chainActive.Next(pindex);
        }

        // we must use CBlocks, as CBlockHeaders won't include the 0x00 nTx count at the end
        std::vector<CBlock> vHeaders;
        int nLimit = MAX_HEADERS_RESULTS;
        LogPrintf("getheaders %d to %s from peer=%d\n", (pindex ? pindex->nHeight : -1), hashStop.ToString(), pfrom->id);
        for (; pindex; pindex = chainActive.Next(pindex)) {
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
        }
        connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::HEADERS, vHeaders));
    }


    else if (strCommand == NetMsgType::TX) {
        std::deque<COutPoint> vWorkQueue;
        std::vector<uint256> vEraseQueue;
        CTransaction tx(deserialize, vRecv);
        CTransactionRef ptx = MakeTransactionRef(tx);

        CInv inv(MSG_TX, tx.GetHash());
        pfrom->AddInventoryKnown(inv);

        LOCK2(cs_main, g_cs_orphans);

        bool ignoreFees = false;
        bool fMissingInputs = false;
        CValidationState state;

        pfrom->setAskFor.erase(inv.hash);
        mapAlreadyAskedFor.erase(inv);

        if (ptx->ContainsZerocoins()) {
            // Don't even try to check zerocoins at all.
            Misbehaving(pfrom->GetId(), 100);
            LogPrint(BCLog::NET, "   misbehaving peer, received a zc transaction, peer: %s\n", pfrom->GetAddrName());
        }

        if (AcceptToMemoryPool(mempool, state, ptx, true, &fMissingInputs, false, ignoreFees)) {
            mempool.check(pcoinsTip);
            RelayTransaction(tx, connman);
            for (unsigned int i = 0; i < tx.vout.size(); i++) {
                vWorkQueue.emplace_back(inv.hash, i);
            }

            LogPrint(BCLog::MEMPOOL, "%s : peer=%d %s : accepted %s (poolsz %u txn, %u kB)\n",
                    __func__, pfrom->id, pfrom->cleanSubVer, tx.GetHash().ToString(),
                    mempool.size(), mempool.DynamicMemoryUsage() / 1000);

            // Recursively process any orphan transactions that depended on this one
            std::set<NodeId> setMisbehaving;
            while (!vWorkQueue.empty()) {
                auto itByPrev = mapOrphanTransactionsByPrev.find(vWorkQueue.front());
                vWorkQueue.pop_front();
                if(itByPrev == mapOrphanTransactionsByPrev.end())
                    continue;
                for (auto mi = itByPrev->second.begin();
                    mi != itByPrev->second.end();
                    ++mi) {
                    const CTransactionRef& orphanTx = (*mi)->second.tx;
                    const uint256& orphanHash = orphanTx->GetHash();
                    NodeId fromPeer = (*mi)->second.fromPeer;
                    bool fMissingInputs2 = false;
                    // Use a dummy CValidationState so someone can't setup nodes to counter-DoS based on orphan
                    // resolution (that is, feeding people an invalid transaction based on LegitTxX in order to get
                    // anyone relaying LegitTxX banned)
                    CValidationState stateDummy;


                    if (setMisbehaving.count(fromPeer))
                        continue;
                    if (AcceptToMemoryPool(mempool, stateDummy, orphanTx, true, &fMissingInputs2)) {
                        LogPrint(BCLog::MEMPOOL, "   accepted orphan tx %s\n", orphanHash.ToString());
                        RelayTransaction(*orphanTx, connman);
                        for (unsigned int i = 0; i < orphanTx->vout.size(); i++) {
                            vWorkQueue.emplace_back(orphanHash, i);
                        }
                        vEraseQueue.push_back(orphanHash);
                    } else if (!fMissingInputs2) {
                        int nDos = 0;
                        if(stateDummy.IsInvalid(nDos) && nDos > 0) {
                            // Punish peer that gave us an invalid orphan tx
                            Misbehaving(fromPeer, nDos);
                            setMisbehaving.insert(fromPeer);
                            LogPrint(BCLog::MEMPOOL, "   invalid orphan tx %s\n", orphanHash.ToString());
                        }
                        // Has inputs but not accepted to mempool
                        // Probably non-standard or insufficient fee
                        LogPrint(BCLog::MEMPOOL, "   removed orphan tx %s\n", orphanHash.ToString());
                        vEraseQueue.push_back(orphanHash);
                        assert(recentRejects);
                        recentRejects->insert(orphanHash);
                    }
                    mempool.check(pcoinsTip);
                }
            }

            for (uint256& hash : vEraseQueue) EraseOrphanTx(hash);

        } else if (fMissingInputs) {
            bool fRejectedParents = false; // It may be the case that the orphans parents have all been rejected

            // Deduplicate parent txids, so that we don't have to loop over
            // the same parent txid more than once down below.
            std::vector<uint256> unique_parents;
            unique_parents.reserve(tx.vin.size());
            for (const CTxIn& txin : ptx->vin) {
                // We start with all parents, and then remove duplicates below.
                unique_parents.emplace_back(txin.prevout.hash);
            }
            std::sort(unique_parents.begin(), unique_parents.end());
            unique_parents.erase(std::unique(unique_parents.begin(), unique_parents.end()), unique_parents.end());
            for (const uint256& parent_txid : unique_parents) {
                if (recentRejects->contains(parent_txid)) {
                    fRejectedParents = true;
                    break;
                }
            }
            if (!fRejectedParents) {
                for (const uint256& parent_txid : unique_parents) {
                    CInv _inv(MSG_TX, parent_txid);
                    pfrom->AddInventoryKnown(_inv);
                    if (!AlreadyHave(_inv)) pfrom->AskFor(_inv);
                }
                AddOrphanTx(ptx, pfrom->GetId());

                // DoS prevention: do not allow mapOrphanTransactions to grow unbounded
                unsigned int nMaxOrphanTx = (unsigned int)std::max((int64_t)0, gArgs.GetArg("-maxorphantx", DEFAULT_MAX_ORPHAN_TRANSACTIONS));
                unsigned int nEvicted = LimitOrphanTxSize(nMaxOrphanTx);
                if (nEvicted > 0)
                    LogPrint(BCLog::MEMPOOL, "mapOrphan overflow, removed %u tx\n", nEvicted);
            } else {
                LogPrint(BCLog::MEMPOOL, "not keeping orphan with rejected parents %s\n",tx.GetHash().ToString());
            }
        } else {
            // AcceptToMemoryPool() returned false, possibly because the tx is
            // already in the mempool; if the tx isn't in the mempool that
            // means it was rejected and we shouldn't ask for it again.
            if (!mempool.exists(tx.GetHash())) {
                assert(recentRejects);
                recentRejects->insert(tx.GetHash());
            }
            if (pfrom->fWhitelisted) {
                // Always relay transactions received from whitelisted peers, even
                // if they were rejected from the mempool, allowing the node to
                // function as a gateway for nodes hidden behind it.
                //
                // FIXME: This includes invalid transactions, which means a
                // whitelisted peer could get us banned! We may want to change
                // that.
                RelayTransaction(tx, connman);
            }
        }

        int nDoS = 0;
        if (state.IsInvalid(nDoS)) {
            LogPrint(BCLog::MEMPOOLREJ, "%s from peer=%d %s was not accepted into the memory pool: %s\n", tx.GetHash().ToString(),
                pfrom->id, pfrom->cleanSubVer,
                FormatStateMessage(state));
            if (nDoS > 0) {
                Misbehaving(pfrom->GetId(), nDoS);
            }
        }
    }

    else if (strCommand == NetMsgType::HEADERS && Params().HeadersFirstSyncingActive() && !fImporting && !fReindex) // Ignore headers received while importing
    {
        std::vector<CBlockHeader> headers;

        // Bypass the normal CBlock deserialization, as we don't want to risk deserializing 2000 full blocks.
        unsigned int nCount = ReadCompactSize(vRecv);
        if (nCount > MAX_HEADERS_RESULTS) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 20);
            return error("headers message size = %u", nCount);
        }
        headers.resize(nCount);
        for (unsigned int n = 0; n < nCount; n++) {
            vRecv >> headers[n];
            ReadCompactSize(vRecv); // ignore tx count; assume it is 0.
        }

        LOCK(cs_main);

        if (nCount == 0) {
            // Nothing interesting. Stop asking this peers for more headers.
            return true;
        }
        CBlockIndex* pindexLast = NULL;
        for (const CBlockHeader& header : headers) {
            CValidationState state;
            if (pindexLast != NULL && header.hashPrevBlock != pindexLast->GetBlockHash()) {
                Misbehaving(pfrom->GetId(), 20);
                return error("non-continuous headers sequence");
            }

            /*TODO: this has a CBlock cast on it so that it will compile. There should be a solution for this
             * before headers are reimplemented on mainnet
             */
            if (!AcceptBlockHeader((CBlock)header, state, &pindexLast)) {
                int nDoS;
                if (state.IsInvalid(nDoS)) {
                    if (nDoS > 0)
                        Misbehaving(pfrom->GetId(), nDoS);
                    std::string strError = "invalid header received " + header.GetHash().ToString();
                    return error(strError.c_str());
                }
            }
        }

        if (pindexLast)
            UpdateBlockAvailability(pfrom->GetId(), pindexLast->GetBlockHash());

        if (nCount == MAX_HEADERS_RESULTS && pindexLast) {
            // Headers message had its maximum size; the peer may have more headers.
            // TODO: optimize: if pindexLast is an ancestor of chainActive.Tip or pindexBestHeader, continue
            // from there instead.
            LogPrintf("more getheaders (%d) to end to peer=%d (startheight:%d)\n", pindexLast->nHeight, pfrom->id, pfrom->nStartingHeight);
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::GETHEADERS, chainActive.GetLocator(pindexLast), UINT256_ZERO));
        }
    }

    else if (strCommand == NetMsgType::BLOCK && !fImporting && !fReindex) // Ignore blocks received while importing
    {
        std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
        vRecv >> *pblock;
        const uint256& hashBlock = pblock->GetHash();
        CInv inv(MSG_BLOCK, hashBlock);
        LogPrint(BCLog::NET, "received block %s peer=%d\n", inv.hash.ToString(), pfrom->id);

        // sometimes we will be sent their most recent block and its not the one we want, in that case tell where we are
        if (!mapBlockIndex.count(pblock->hashPrevBlock)) {
            CBlockLocator locator = WITH_LOCK(cs_main, return chainActive.GetLocator(););
            if (find(pfrom->vBlockRequested.begin(), pfrom->vBlockRequested.end(), hashBlock) != pfrom->vBlockRequested.end()) {
                // we already asked for this block, so lets work backwards and ask for the previous block
                connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::GETBLOCKS, locator, pblock->hashPrevBlock));
                pfrom->vBlockRequested.emplace_back(pblock->hashPrevBlock);
            } else {
                // ask to sync to this block
                connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::GETBLOCKS, locator, hashBlock));
                pfrom->vBlockRequested.emplace_back(hashBlock);
            }
        } else {
            pfrom->AddInventoryKnown(inv);
            if (!mapBlockIndex.count(hashBlock)) {
                {
                    LOCK(cs_main);
                    MarkBlockAsReceived(hashBlock);
                    mapBlockSource.emplace(hashBlock, pfrom->GetId());
                }
                ProcessNewBlock(pblock, nullptr);

                // Disconnect node if its running an old protocol version,
                // used during upgrades, when the node is already connected.
                pfrom->DisconnectOldProtocol(pfrom->nVersion, ActiveProtocol());
            } else {
                LogPrint(BCLog::NET, "%s : Already processed block %s, skipping ProcessNewBlock()\n", __func__, pblock->GetHash().GetHex());
            }
        }
    }

    // This asymmetric behavior for inbound and outbound connections was introduced
    // to prevent a fingerprinting attack: an attacker can send specific fake addresses
    // to users' AddrMan and later request them by sending getaddr messages.
    // Making users (which are behind NAT and can only make outgoing connections) ignore
    // getaddr message mitigates the attack.
    else if ((strCommand == NetMsgType::GETADDR) && (pfrom->fInbound)) {
        pfrom->vAddrToSend.clear();
        std::vector<CAddress> vAddr = connman->GetAddresses();
        FastRandomContext insecure_rand;
        for (const CAddress& addr : vAddr)
            pfrom->PushAddress(addr, insecure_rand);
    }


    else if (strCommand == NetMsgType::MEMPOOL) {

        if (!(pfrom->GetLocalServices() & NODE_BLOOM) && !pfrom->fWhitelisted) {
            LogPrint(BCLog::NET, "mempool request with bloom filters disabled, disconnect peer=%d\n", pfrom->GetId());
            pfrom->fDisconnect = true;
            return true;
        }

        // todo: limit mempool request with a bandwidth limit
        LOCK(pfrom->cs_inventory);
        pfrom->fSendMempool = true;
    }


    else if (strCommand == NetMsgType::PING) {
        if (pfrom->nVersion > BIP0031_VERSION) {
            uint64_t nonce = 0;
            vRecv >> nonce;
            // Echo the message back with the nonce. This allows for two useful features:
            //
            // 1) A remote node can quickly check if the connection is operational
            // 2) Remote nodes can measure the latency of the network thread. If this node
            //    is overloaded it won't respond to pings quickly and the remote node can
            //    avoid sending us more work, like chain download requests.
            //
            // The nonce stops the remote getting confused between different pings: without
            // it, if the remote node sends a ping once per second and this node takes 5
            // seconds to respond to each, the 5th ping the remote sends would appear to
            // return very quickly.
            connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::PONG, nonce));
        }
    }


    else if (strCommand == NetMsgType::PONG) {
        int64_t pingUsecEnd = nTimeReceived;
        uint64_t nonce = 0;
        size_t nAvail = vRecv.in_avail();
        bool bPingFinished = false;
        std::string sProblem;

        if (nAvail >= sizeof(nonce)) {
            vRecv >> nonce;

            // Only process pong message if there is an outstanding ping (old ping without nonce should never pong)
            if (pfrom->nPingNonceSent != 0) {
                if (nonce == pfrom->nPingNonceSent) {
                    // Matching pong received, this ping is no longer outstanding
                    bPingFinished = true;
                    int64_t pingUsecTime = pingUsecEnd - pfrom->nPingUsecStart;
                    if (pingUsecTime > 0) {
                        // Successful ping time measurement, replace previous
                        pfrom->nPingUsecTime = pingUsecTime;
                        pfrom->nMinPingUsecTime = std::min(pfrom->nMinPingUsecTime.load(), pingUsecTime);
                    } else {
                        // This should never happen
                        sProblem = "Timing mishap";
                    }
                } else {
                    // Nonce mismatches are normal when pings are overlapping
                    sProblem = "Nonce mismatch";
                    if (nonce == 0) {
                        // This is most likely a bug in another implementation somewhere, cancel this ping
                        bPingFinished = true;
                        sProblem = "Nonce zero";
                    }
                }
            } else {
                sProblem = "Unsolicited pong without ping";
            }
        } else {
            // This is most likely a bug in another implementation somewhere, cancel this ping
            bPingFinished = true;
            sProblem = "Short payload";
        }

        if (!(sProblem.empty())) {
            LogPrint(BCLog::NET, "pong peer=%d %s: %s, %x expected, %x received, %u bytes\n",
                pfrom->id,
                pfrom->cleanSubVer,
                sProblem,
                pfrom->nPingNonceSent,
                nonce,
                nAvail);
        }
        if (bPingFinished) {
            pfrom->nPingNonceSent = 0;
        }
    }

    else if (!(pfrom->GetLocalServices() & NODE_BLOOM) &&
             (strCommand == NetMsgType::FILTERLOAD ||
                 strCommand == NetMsgType::FILTERADD ||
                 strCommand == NetMsgType::FILTERCLEAR)) {
        LogPrintf("bloom message=%s\n", strCommand);
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), 100);
    }

    else if (strCommand == NetMsgType::FILTERLOAD) {
        CBloomFilter filter;
        vRecv >> filter;

        LOCK(pfrom->cs_filter);

        if (!filter.IsWithinSizeConstraints()) {
            // There is no excuse for sending a too-large filter
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 100);
        } else {
            delete pfrom->pfilter;
            pfrom->pfilter = new CBloomFilter(filter);
            pfrom->pfilter->UpdateEmptyFull();
            pfrom->fRelayTxes = true;
        }
    }


    else if (strCommand == NetMsgType::FILTERADD) {
        std::vector<unsigned char> vData;
        vRecv >> vData;

        // Nodes must NEVER send a data item > 520 bytes (the max size for a script data object,
        // and thus, the maximum size any matched object can have) in a filteradd message
        bool bad = false;
        if (vData.size() > MAX_SCRIPT_ELEMENT_SIZE) {
            bad = true;
        } else {
            LOCK(pfrom->cs_filter);
            if (pfrom->pfilter) {
                pfrom->pfilter->insert(vData);
            } else {
                bad = true;
            }
        }
        if (bad) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), 100);
        }
    }


    else if (strCommand == NetMsgType::FILTERCLEAR) {
        LOCK(pfrom->cs_filter);
        delete pfrom->pfilter;
        pfrom->pfilter = new CBloomFilter();
        pfrom->fRelayTxes = true;
    }

    else {
        // Tier two msg type search
        const std::vector<std::string>& allMessages = getTierTwoNetMessageTypes();
        if (std::find(allMessages.begin(), allMessages.end(), strCommand) != allMessages.end()) {
            // Check if the dispatcher can process this message first. If not, try going with the old flow.
            if (!masternodeSync.MessageDispatcher(pfrom, strCommand, vRecv)) {
                // Probably one the extensions
                mnodeman.ProcessMessage(pfrom, strCommand, vRecv);
                g_budgetman.ProcessMessage(pfrom, strCommand, vRecv);
                masternodePayments.ProcessMessageMasternodePayments(pfrom, strCommand, vRecv);
                sporkManager.ProcessSpork(pfrom, strCommand, vRecv);
                masternodeSync.ProcessMessage(pfrom, strCommand, vRecv);
            }
        } else {
            // Ignore unknown commands for extensibility
            LogPrint(BCLog::NET, "Unknown command \"%s\" from peer=%d\n", SanitizeString(strCommand), pfrom->id);
        }
    }

    return true;
}


bool PeerLogicValidation::ProcessMessages(CNode* pfrom, std::atomic<bool>& interruptMsgProc)
{
    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (4) checksum
    //  (x) data
    //
    bool fMoreWork = false;

    if (!pfrom->vRecvGetData.empty())
        ProcessGetData(pfrom, connman, interruptMsgProc);

    if (pfrom->fDisconnect)
        return false;

    // this maintains the order of responses
    if (!pfrom->vRecvGetData.empty()) return true;

    // Don't bother if send buffer is too full to respond anyway
    if (pfrom->fPauseSend)
        return false;

    std::list<CNetMessage> msgs;
    {
        LOCK(pfrom->cs_vProcessMsg);
        if (pfrom->vProcessMsg.empty())
            return false;
        // Just take one message
        msgs.splice(msgs.begin(), pfrom->vProcessMsg, pfrom->vProcessMsg.begin());
        pfrom->nProcessQueueSize -= msgs.front().vRecv.size() + CMessageHeader::HEADER_SIZE;
        pfrom->fPauseRecv = pfrom->nProcessQueueSize > connman->GetReceiveFloodSize();
        fMoreWork = !pfrom->vProcessMsg.empty();
    }
    CNetMessage& msg(msgs.front());

    msg.SetVersion(pfrom->GetRecvVersion());
    // Scan for message start
    if (memcmp(msg.hdr.pchMessageStart, Params().MessageStart(), MESSAGE_START_SIZE) != 0) {
        LogPrint(BCLog::NET, "PROCESSMESSAGE: INVALID MESSAGESTART %s peer=%d\n", SanitizeString(msg.hdr.GetCommand()), pfrom->id);
        pfrom->fDisconnect = true;
        return false;
    }

    // Read header
    CMessageHeader& hdr = msg.hdr;
    if (!hdr.IsValid(Params().MessageStart())) {
        LogPrint(BCLog::NET, "PROCESSMESSAGE: ERRORS IN HEADER '%s' peer=%d\n", SanitizeString(hdr.GetCommand()), pfrom->id);
        return fMoreWork;
    }
    std::string strCommand = hdr.GetCommand();

    // Message size
    unsigned int nMessageSize = hdr.nMessageSize;

    // Checksum
    CDataStream& vRecv = msg.vRecv;
    uint256 hash = msg.GetMessageHash();
    if (memcmp(hash.begin(), hdr.pchChecksum, CMessageHeader::CHECKSUM_SIZE) != 0)
    {
        LogPrint(BCLog::NET, "%s(%s, %u bytes): CHECKSUM ERROR expected %s was %s\n", __func__,
           SanitizeString(strCommand), nMessageSize,
           HexStr(hash.begin(), hash.begin()+CMessageHeader::CHECKSUM_SIZE),
           HexStr(hdr.pchChecksum, hdr.pchChecksum+CMessageHeader::CHECKSUM_SIZE));
        return fMoreWork;
    }

    // Process message
    bool fRet = false;
    try {
        fRet = ProcessMessage(pfrom, strCommand, vRecv, msg.nTime, connman, interruptMsgProc);
        if (interruptMsgProc)
            return false;
        if (!pfrom->vRecvGetData.empty())
            fMoreWork = true;
    } catch (const std::ios_base::failure& e) {
        if (strstr(e.what(), "end of data")) {
            // Allow exceptions from under-length message on vRecv
            LogPrint(BCLog::NET, "ProcessMessages(%s, %u bytes): Exception '%s' caught, normally caused by a message being shorter than its stated length\n", SanitizeString(strCommand), nMessageSize, e.what());
        } else if (strstr(e.what(), "size too large")) {
            // Allow exceptions from over-long size
            LogPrint(BCLog::NET, "ProcessMessages(%s, %u bytes): Exception '%s' caught\n", SanitizeString(strCommand), nMessageSize, e.what());
        } else {
            PrintExceptionContinue(&e, "ProcessMessages()");
        }
    } catch (const std::exception& e) {
        PrintExceptionContinue(&e, "ProcessMessages()");
    } catch (...) {
        PrintExceptionContinue(NULL, "ProcessMessages()");
    }

    if (!fRet)
        LogPrint(BCLog::NET, "ProcessMessage(%s, %u bytes) FAILED peer=%d\n", SanitizeString(strCommand), nMessageSize, pfrom->id);

    return fMoreWork;
}

class CompareInvMempoolOrder
{
    CTxMemPool *mp;
public:
    CompareInvMempoolOrder(CTxMemPool *_mempool)
    {
        mp = _mempool;
    }

    bool operator()(std::set<uint256>::iterator a, std::set<uint256>::iterator b)
    {
        /* As std::make_heap produces a max-heap, we want the entries with the
         * fewest ancestors/highest fee to sort later. */
        return mp->CompareDepthAndScore(*b, *a);
    }
};

bool PeerLogicValidation::SendMessages(CNode* pto, std::atomic<bool>& interruptMsgProc)
{
    {
        // Don't send anything until the version handshake is complete
        if (!pto->fSuccessfullyConnected || pto->fDisconnect)
            return true;

        // If we get here, the outgoing message serialization version is set and can't change.
        CNetMsgMaker msgMaker(pto->GetSendVersion());

        //
        // Message: ping
        //
        bool pingSend = false;
        if (pto->fPingQueued) {
            // RPC ping request by user
            pingSend = true;
        }
        if (pto->nPingNonceSent == 0 && pto->nPingUsecStart + PING_INTERVAL * 1000000 < GetTimeMicros()) {
            // Ping automatically sent as a latency probe & keepalive.
            pingSend = true;
        }
        if (pingSend) {
            uint64_t nonce = 0;
            while (nonce == 0) {
                GetRandBytes((unsigned char*)&nonce, sizeof(nonce));
            }
            pto->fPingQueued = false;
            pto->nPingUsecStart = GetTimeMicros();
            if (pto->nVersion > BIP0031_VERSION) {
                pto->nPingNonceSent = nonce;
                connman->PushMessage(pto, msgMaker.Make(NetMsgType::PING, nonce));
            } else {
                // Peer is too old to support ping command with nonce, pong will never arrive.
                pto->nPingNonceSent = 0;
                connman->PushMessage(pto, msgMaker.Make(NetMsgType::PING));
            }
        }

        TRY_LOCK(cs_main, lockMain); // Acquire cs_main for IsInitialBlockDownload() and CNodeState()
        if (!lockMain)
            return true;

        CNodeState& state = *State(pto->GetId());

        if (state.fShouldBan) {
            state.fShouldBan = false;
            if (pto->fWhitelisted)
                LogPrintf("Warning: not punishing whitelisted peer %s!\n", pto->addr.ToString());
            else {
                pto->fDisconnect = true;
                if (pto->addr.IsLocal())
                    LogPrintf("Warning: not banning local peer %s!\n", pto->addr.ToString());
                else {
                    connman->Ban(pto->addr, BanReasonNodeMisbehaving);
                }
                return true;
            }
        }

        // Address refresh broadcast
        int64_t nNow = GetTimeMicros();
        auto current_time = GetTime<std::chrono::microseconds>();

        if (!IsInitialBlockDownload() && pto->nNextLocalAddrSend < nNow) {
            AdvertiseLocal(pto);
            pto->nNextLocalAddrSend = PoissonNextSend(nNow, AVG_LOCAL_ADDRESS_BROADCAST_INTERVAL);
        }

        //
        // Message: addr
        //
        if (pto->nNextAddrSend < nNow) {
            pto->nNextAddrSend = PoissonNextSend(nNow, AVG_ADDRESS_BROADCAST_INTERVAL);
            std::vector<CAddress> vAddr;
            vAddr.reserve(pto->vAddrToSend.size());
            for (const CAddress& addr : pto->vAddrToSend) {
                if (!pto->addrKnown.contains(addr.GetKey())) {
                    pto->addrKnown.insert(addr.GetKey());
                    vAddr.push_back(addr);
                    // receiver rejects addr messages larger than 1000
                    if (vAddr.size() >= 1000) {
                        connman->PushMessage(pto, msgMaker.Make(NetMsgType::ADDR, vAddr));
                        vAddr.clear();
                    }
                }
            }
            pto->vAddrToSend.clear();
            if (!vAddr.empty())
                connman->PushMessage(pto, msgMaker.Make(NetMsgType::ADDR, vAddr));
        }

        // Start block sync
        if (pindexBestHeader == NULL)
            pindexBestHeader = chainActive.Tip();
        bool fFetch = state.fPreferredDownload || (nPreferredDownload == 0 && !pto->fClient && !pto->fOneShot); // Download if this is a nice peer, or we have no nice peers and this one might do.
        if (!state.fSyncStarted && !pto->fClient && !fImporting && !fReindex) {
            // Only actively request headers from a single peer, unless we're close to end of initial download.
            if ((nSyncStarted == 0 && fFetch) || pindexBestHeader->GetBlockTime() > GetAdjustedTime() - 6 * 60 * 60) { // NOTE: was "close to today" and 24h in Bitcoin
                state.fSyncStarted = true;
                nSyncStarted++;
                //CBlockIndex *pindexStart = pindexBestHeader->pprev ? pindexBestHeader->pprev : pindexBestHeader;
                //LogPrint(BCLog::NET, "initial getheaders (%d) to peer=%d (startheight:%d)\n", pindexStart->nHeight, pto->id, pto->nStartingHeight);
                //pto->PushMessage(NetMsgType::GETHEADERS, chainActive.GetLocator(pindexStart), UINT256_ZERO);
                connman->PushMessage(pto, msgMaker.Make(NetMsgType::GETBLOCKS, chainActive.GetLocator(chainActive.Tip()), UINT256_ZERO));
            }
        }

        // Resend wallet transactions that haven't gotten in a block yet
        // Except during reindex, importing and IBD, when old wallet
        // transactions become unconfirmed and spams other nodes.
        if (!fReindex && !fImporting && !IsInitialBlockDownload()) {
            GetMainSignals().Broadcast(connman);
        }

        //
        // Message: inventory
        //
        std::vector<CInv> vInv;
        std::vector<CInv> vInvWait;
        {
            LOCK(pto->cs_inventory);
            vInv.reserve(std::max<size_t>(pto->vInventoryBlockToSend.size(), INVENTORY_BROADCAST_MAX));

            // Add blocks
            for (const uint256& hash : pto->vInventoryBlockToSend) {
                vInv.emplace_back(CInv(MSG_BLOCK, hash));
                if (vInv.size() == MAX_INV_SZ) {
                    connman->PushMessage(pto, msgMaker.Make(NetMsgType::INV, vInv));
                    vInv.clear();
                }
            }
            pto->vInventoryBlockToSend.clear();

            // Add tier two INVs
            for (const CInv& tInv : pto->vInventoryTierTwoToSend) {
                vInv.emplace_back(tInv);
                if (vInv.size() == MAX_INV_SZ) {
                    connman->PushMessage(pto, msgMaker.Make(NetMsgType::INV, vInv));
                    vInv.clear();
                }
            }
            pto->vInventoryTierTwoToSend.clear();

            // Check whether periodic send should happen
            bool fSendTrickle = pto->fWhitelisted;
            if (pto->nNextInvSend < current_time) {
                fSendTrickle = true;
                // Use half the delay for outbound peers, as there is less privacy concern for them.
                pto->nNextInvSend = PoissonNextSend(current_time, std::chrono::seconds{INVENTORY_BROADCAST_INTERVAL >> !pto->fInbound});
            }

            // Time to send but the peer has requested we not relay transactions.
            if (fSendTrickle) {
                LOCK(pto->cs_filter);
                if (!pto->fRelayTxes) pto->setInventoryTxToSend.clear();
            }

            // Respond to BIP35 mempool requests
            if (fSendTrickle && pto->fSendMempool) {
                auto vtxinfo = mempool.infoAll();
                pto->fSendMempool = false;
                // future: back port fee filter rate
                LOCK(pto->cs_filter);

                for (const auto& txinfo : vtxinfo) {
                    const uint256& hash = txinfo.tx->GetHash();
                    CInv inv(MSG_TX, hash);
                    pto->setInventoryTxToSend.erase(hash);
                    // future: add fee filter check here..
                    if (pto->pfilter) {
                        if (!pto->pfilter->IsRelevantAndUpdate(*txinfo.tx)) continue;
                    }
                    pto->filterInventoryKnown.insert(hash);
                    vInv.emplace_back(inv);
                    if (vInv.size() == MAX_INV_SZ) {
                        connman->PushMessage(pto, msgMaker.Make(NetMsgType::INV, vInv));
                        vInv.clear();
                    }
                }
                pto->timeLastMempoolReq = GetTime();
            }

            // Determine transactions to relay
            if (fSendTrickle) {
                // Produce a vector with all candidates for sending
                std::vector<std::set<uint256>::iterator> vInvTx;
                vInvTx.reserve(pto->setInventoryTxToSend.size());
                for (std::set<uint256>::iterator it = pto->setInventoryTxToSend.begin(); it != pto->setInventoryTxToSend.end(); it++) {
                    vInvTx.push_back(it);
                }
                // Topologically and fee-rate sort the inventory we send for privacy and priority reasons.
                // A heap is used so that not all items need sorting if only a few are being sent.
                CompareInvMempoolOrder compareInvMempoolOrder(&mempool);
                std::make_heap(vInvTx.begin(), vInvTx.end(), compareInvMempoolOrder);
                // No reason to drain out at many times the network's capacity,
                // especially since we have many peers and some will draw much shorter delays.
                unsigned int nRelayedTransactions = 0;
                LOCK(pto->cs_filter);
                while (!vInvTx.empty() && nRelayedTransactions < INVENTORY_BROADCAST_MAX) {
                    // Fetch the top element from the heap
                    std::pop_heap(vInvTx.begin(), vInvTx.end(), compareInvMempoolOrder);
                    std::set<uint256>::iterator it = vInvTx.back();
                    vInvTx.pop_back();
                    uint256 hash = *it;
                    // Remove it from the to-be-sent set
                    pto->setInventoryTxToSend.erase(it);
                    // Check if not in the filter already
                    if (pto->filterInventoryKnown.contains(hash)) {
                        continue;
                    }
                    // Not in the mempool anymore? don't bother sending it.
                    auto txinfo = mempool.info(hash);
                    if (!txinfo.tx) {
                        continue;
                    }
                    // todo: back port feerate filter.
                    if (pto->pfilter && !pto->pfilter->IsRelevantAndUpdate(*txinfo.tx)) continue;
                    // Send
                    vInv.emplace_back(CInv(MSG_TX, hash));
                    nRelayedTransactions++;
                    if (vInv.size() == MAX_INV_SZ) {
                        connman->PushMessage(pto, msgMaker.Make(NetMsgType::INV, vInv));
                        vInv.clear();
                    }
                    pto->filterInventoryKnown.insert(hash);
                }
            }
        }
        if (!vInv.empty())
            connman->PushMessage(pto, msgMaker.Make(NetMsgType::INV, vInv));

        // Detect whether we're stalling
        current_time = GetTime<std::chrono::microseconds>();
        nNow = GetTimeMicros();
        if (state.nStallingSince && state.nStallingSince < nNow - 1000000 * BLOCK_STALLING_TIMEOUT) {
            // Stalling only triggers when the block download window cannot move. During normal steady state,
            // the download window should be much larger than the to-be-downloaded set of blocks, so disconnection
            // should only happen during initial block download.
            LogPrintf("Peer=%d is stalling block download, disconnecting\n", pto->id);
            pto->fDisconnect = true;
            return true;
        }
        // In case there is a block that has been in flight from this peer for (2 + 0.5 * N) times the block interval
        // (with N the number of validated blocks that were in flight at the time it was requested), disconnect due to
        // timeout. We compensate for in-flight blocks to prevent killing off peers due to our own downstream link
        // being saturated. We only count validated in-flight blocks so peers can't advertise nonexisting block hashes
        // to unreasonably increase our timeout.
        if (state.vBlocksInFlight.size() > 0 && state.vBlocksInFlight.front().nTime < nNow - 500000 * Params().GetConsensus().nTargetSpacing * (4 + state.vBlocksInFlight.front().nValidatedQueuedBefore)) {
            LogPrintf("Timeout downloading block %s from peer=%d, disconnecting\n", state.vBlocksInFlight.front().hash.ToString(), pto->id);
            pto->fDisconnect = true;
            return true;
        }

        //
        // Message: getdata (blocks)
        //
        std::vector<CInv> vGetData;
        if (!pto->fClient && fFetch && state.nBlocksInFlight < MAX_BLOCKS_IN_TRANSIT_PER_PEER) {
            std::vector<const CBlockIndex*> vToDownload;
            NodeId staller = -1;
            FindNextBlocksToDownload(pto->GetId(), MAX_BLOCKS_IN_TRANSIT_PER_PEER - state.nBlocksInFlight, vToDownload, staller);
            for (const CBlockIndex* pindex : vToDownload) {
                vGetData.emplace_back(MSG_BLOCK, pindex->GetBlockHash());
                MarkBlockAsInFlight(pto->GetId(), pindex->GetBlockHash(), pindex);
                LogPrintf("Requesting block %s (%d) peer=%d\n", pindex->GetBlockHash().ToString(),
                    pindex->nHeight, pto->id);
            }
            if (state.nBlocksInFlight == 0 && staller != -1) {
                if (State(staller)->nStallingSince == 0) {
                    State(staller)->nStallingSince = nNow;
                    LogPrint(BCLog::NET, "Stall started peer=%d\n", staller);
                }
            }
        }

        //
        // Message: getdata (non-blocks)
        //
        while (!pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow) {
            const CInv& inv = (*pto->mapAskFor.begin()).second;
            if (!AlreadyHave(inv)) {
                LogPrint(BCLog::NET, "Requesting %s peer=%d\n", inv.ToString(), pto->id);
                vGetData.push_back(inv);
                if (vGetData.size() >= 1000) {
                    connman->PushMessage(pto, msgMaker.Make(NetMsgType::GETDATA, vGetData));
                    vGetData.clear();
                }
            } else {
                //If we're not going to ask, don't expect a response.
                pto->setAskFor.erase(inv.hash);
            }
            pto->mapAskFor.erase(pto->mapAskFor.begin());
        }
        if (!vGetData.empty())
            connman->PushMessage(pto, msgMaker.Make(NetMsgType::GETDATA, vGetData));
    }
    return true;
}

class CNetProcessingCleanup
{
public:
    CNetProcessingCleanup() {}
    ~CNetProcessingCleanup() {
        // orphan transactions
        mapOrphanTransactions.clear();
        mapOrphanTransactionsByPrev.clear();
    }
} instance_of_cnetprocessingcleanup;
