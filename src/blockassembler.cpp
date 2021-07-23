// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2021 The PIVX Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "blockassembler.h"

#include "amount.h"
#include "blocksignature.h"
#include "chain.h"
#include "chainparams.h"
#include "consensus/consensus.h"
#include "consensus/merkle.h"
#include "consensus/upgrades.h"
#include "consensus/validation.h"
#include "masternode-payments.h"
#include "policy/policy.h"
#include "pow.h"
#include "primitives/transaction.h"
#include "spork.h"
#include "timedata.h"
#include "txmempool.h"
#include "util/system.h"
#include "validation.h"
#include "validationinterface.h"

#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif

#include <algorithm>
#include <boost/thread.hpp>

// Unconfirmed transactions in the memory pool often depend on other
// transactions in the memory pool. When we select transactions from the
// pool, we select by highest priority or fee rate, so we might consider
// transactions that depend on transactions that aren't yet in the block.

uint64_t nLastBlockTx = 0;
uint64_t nLastBlockSize = 0;

class ScoreCompare
{
public:
    ScoreCompare() {}

    bool operator()(const CTxMemPool::txiter a, const CTxMemPool::txiter b)
    {
        return CompareTxMemPoolEntryByScore()(*b,*a); // Convert to less than
    }
};

int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime = std::max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

    if (nOldTime < nNewTime)
        pblock->nTime = nNewTime;

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks)
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock);

    return nNewTime - nOldTime;
}

static CMutableTransaction NewCoinbase(const int nHeight, const CScript* pScriptPubKey = nullptr)
{
    CMutableTransaction txCoinbase;
    txCoinbase.vout.emplace_back();
    txCoinbase.vout[0].SetEmpty();
    if (pScriptPubKey) txCoinbase.vout[0].scriptPubKey = *pScriptPubKey;
    txCoinbase.vin.emplace_back();
    txCoinbase.vin[0].scriptSig = CScript() << nHeight << OP_0;
    return txCoinbase;
}

bool SolveProofOfStake(CBlock* pblock, CBlockIndex* pindexPrev, CWallet* pwallet, std::vector<CStakeableOutput>* availableCoins)
{
    boost::this_thread::interruption_point();

    assert(pindexPrev);
    pblock->nBits = GetNextWorkRequired(pindexPrev, pblock);

    // Sync wallet before create coinstake
    pwallet->BlockUntilSyncedToCurrentChain();

    CMutableTransaction txCoinStake;
    int64_t nTxNewTime = 0;
    if (!pwallet->CreateCoinStake(pindexPrev, pblock->nBits, txCoinStake, nTxNewTime, availableCoins)) {
        LogPrint(BCLog::STAKING, "%s : stake not found\n", __func__);
        return false;
    }
    // Stake found

    // Create coinbase tx and add masternode/budget payments
    CMutableTransaction txCoinbase = NewCoinbase(pindexPrev->nHeight + 1);
    FillBlockPayee(txCoinbase, txCoinStake, pindexPrev, true);

    // Sign coinstake
    if (!pwallet->SignCoinStake(txCoinStake)) {
        const COutPoint& stakeIn = txCoinStake.vin[0].prevout;
        return error("Unable to sign coinstake with input %s-%d", stakeIn.hash.ToString(), stakeIn.n);
    }

    pblock->vtx.emplace_back(MakeTransactionRef(txCoinbase));
    pblock->vtx.emplace_back(MakeTransactionRef(txCoinStake));
    pblock->nTime = nTxNewTime;
    return true;
}

CMutableTransaction CreateCoinbaseTx(const CScript& scriptPubKeyIn, CBlockIndex* pindexPrev)
{
    assert(pindexPrev);
    const int nHeight = pindexPrev->nHeight + 1;

    // Create coinbase tx
    CMutableTransaction txCoinbase = NewCoinbase(nHeight, &scriptPubKeyIn);

    //Masternode and general budget payments
    CMutableTransaction txDummy;    // POW blocks have no coinstake
    FillBlockPayee(txCoinbase, txDummy, pindexPrev, false);

    // If no payee was detected, then the whole block value goes to the first output.
    if (txCoinbase.vout.size() == 1) {
        txCoinbase.vout[0].nValue = GetBlockValue(nHeight);
    }

    return txCoinbase;
}

bool CreateCoinbaseTx(CBlock* pblock, const CScript& scriptPubKeyIn, CBlockIndex* pindexPrev)
{
    pblock->vtx.emplace_back(MakeTransactionRef(CreateCoinbaseTx(scriptPubKeyIn, pindexPrev)));
    return true;
}

BlockAssembler::BlockAssembler(const CChainParams& _chainparams, const bool _defaultPrintPriority)
        : chainparams(_chainparams), defaultPrintPriority(_defaultPrintPriority)
{
    // Largest block you're willing to create:
    nBlockMaxSize = gArgs.GetArg("-blockmaxsize", DEFAULT_BLOCK_MAX_SIZE);
    // Limit to between 1K and MAX_BLOCK_SIZE-1K for sanity:
    nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SIZE_CURRENT - 1000), nBlockMaxSize));
}

void BlockAssembler::resetBlock()
{
    inBlock.clear();

    // Reserve space for coinbase tx
    nBlockSize = 1000;
    nBlockSigOps = 100;

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees = 0;
}

std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock(const CScript& scriptPubKeyIn,
                                               CWallet* pwallet,
                                               bool fProofOfStake,
                                               std::vector<CStakeableOutput>* availableCoins,
                                               bool fNoMempoolTx)
{
    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());

    if(!pblocktemplate) return nullptr;
    pblock = &pblocktemplate->block; // pointer for convenience

    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOps.push_back(-1); // updated at end

    CBlockIndex* pindexPrev = WITH_LOCK(cs_main, return chainActive.Tip());
    assert(pindexPrev);
    nHeight = pindexPrev->nHeight + 1;

    pblock->nVersion = ComputeBlockVersion(chainparams.GetConsensus(), nHeight);
    // -regtest only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (Params().IsRegTestNet()) {
        pblock->nVersion = gArgs.GetArg("-blockversion", pblock->nVersion);
    }

    // Depending on the tip height, try to find a coinstake who solves the block or create a coinbase tx.
    if (!(fProofOfStake ? SolveProofOfStake(pblock, pindexPrev, pwallet, availableCoins)
                        : CreateCoinbaseTx(pblock, scriptPubKeyIn, pindexPrev))) {
        return nullptr;
    }

    if (!fNoMempoolTx) {
        // Add transactions from mempool
        LOCK2(cs_main,mempool.cs);
        addPackageTxs();
    }

    if (!fProofOfStake) {
        // Coinbase can get the fees.
        CMutableTransaction txCoinbase(*pblock->vtx[0]);
        txCoinbase.vout[0].nValue += nFees;
        pblock->vtx[0] = MakeTransactionRef(txCoinbase);
        pblocktemplate->vTxFees[0] = -nFees;
    }

    nLastBlockTx = nBlockTx;
    nLastBlockSize = nBlockSize;
    LogPrintf("CreateNewBlock(): total size %u txs: %u fees: %ld sigops %d\n", nBlockSize, nBlockTx, nFees, nBlockSigOps);

    const Consensus::Params& consensus = Params().GetConsensus();
    // Fill in header
    pblock->hashPrevBlock = pindexPrev->GetBlockHash();
    if (!fProofOfStake) UpdateTime(pblock, consensus, pindexPrev);
    pblock->nBits = GetNextWorkRequired(pindexPrev, pblock);
    pblock->nNonce = 0;
    pblocktemplate->vTxSigOps[0] = GetLegacySigOpCount(*(pblock->vtx[0]));
    appendSaplingTreeRoot();

    if (fProofOfStake) { // this is only for PoS because the IncrementExtraNonce does it for PoW
        pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
        LogPrintf("CPUMiner : proof-of-stake block found %s \n", pblock->GetHash().GetHex());
        if (!SignBlock(*pblock, *pwallet)) {
            LogPrintf("%s: Signing new block with UTXO key failed \n", __func__);
            return nullptr;
        }
    }

    {
        LOCK(cs_main);
        if (chainActive.Tip() != pindexPrev) return nullptr; // new block came in, move on

        CValidationState state;
        if (!TestBlockValidity(state, *pblock, pindexPrev, false, false, false)) {
            throw std::runtime_error(
                    strprintf("%s: TestBlockValidity failed: %s", __func__, FormatStateMessage(state)));
        }
    }

    return std::move(pblocktemplate);
}

void BlockAssembler::onlyUnconfirmed(CTxMemPool::setEntries& testSet)
{
    for (CTxMemPool::setEntries::iterator iit = testSet.begin(); iit != testSet.end(); ) {
        // Only test txs not already in the block
        if (inBlock.count(*iit)) {
            testSet.erase(iit++);
        }
        else {
            iit++;
        }
    }
}

bool BlockAssembler::TestPackage(uint64_t packageSize, unsigned int packageSigOps)
{
    if (nBlockSize + packageSize >= nBlockMaxSize)
        return false;
    if (nBlockSigOps + packageSigOps >= MAX_BLOCK_SIGOPS_CURRENT)
        return false;
    return true;
}

// Block size and sigops have already been tested.  Check that all transactions
// are final.
bool BlockAssembler::TestPackageFinality(const CTxMemPool::setEntries& package)
{
    for (const CTxMemPool::txiter& it : package) {
        if (!IsFinalTx(it->GetSharedTx(), nHeight))
            return false;
    }
    return true;
}

void BlockAssembler::AddToBlock(CTxMemPool::txiter iter)
{
    pblock->vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vTxSigOps.push_back(iter->GetSigOpCount());
    nBlockSize += iter->GetTxSize();
    ++nBlockTx;
    nBlockSigOps += iter->GetSigOpCount();
    nFees += iter->GetFee();
    inBlock.insert(iter);

    bool fPrintPriority = gArgs.GetBoolArg("-printpriority", defaultPrintPriority);
    if (fPrintPriority) {
        LogPrintf("feerate %s txid %s\n",
                  CFeeRate(iter->GetModifiedFee(), iter->GetTxSize()).ToString(),
                  iter->GetTx().GetHash().ToString());
    }
}

void BlockAssembler::UpdatePackagesForAdded(const CTxMemPool::setEntries& alreadyAdded,
                                            indexed_modified_transaction_set& mapModifiedTx)
{
    for (const CTxMemPool::txiter& it : alreadyAdded) {
        CTxMemPool::setEntries descendants;
        mempool.CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set
        for (CTxMemPool::txiter desc : descendants) {
            if (alreadyAdded.count(desc))
                continue;
            modtxiter mit = mapModifiedTx.find(desc);
            if (mit == mapModifiedTx.end()) {
                CTxMemPoolModifiedEntry modEntry(desc);
                modEntry.nSizeWithAncestors -= it->GetTxSize();
                modEntry.nModFeesWithAncestors -= it->GetModifiedFee();
                modEntry.nSigOpCountWithAncestors -= it->GetSigOpCount();
                mapModifiedTx.insert(modEntry);
            } else {
                mapModifiedTx.modify(mit, update_for_parent_inclusion(it));
            }
        }
    }
}

// Skip entries in mapTx that are already in a block or are present
// in mapModifiedTx (which implies that the mapTx ancestor state is
// stale due to ancestor inclusion in the block)
// Also skip transactions that we've already failed to add. This can happen if
// we consider a transaction in mapModifiedTx and it fails: we can then
// potentially consider it again while walking mapTx.  It's currently
// guaranteed to fail again, but as a belt-and-suspenders check we put it in
// failedTx and avoid re-evaluation, since the re-evaluation would be using
// cached size/sigops/fee values that are not actually correct.
bool BlockAssembler::SkipMapTxEntry(CTxMemPool::txiter it, indexed_modified_transaction_set &mapModifiedTx, CTxMemPool::setEntries &failedTx)
{
    assert (it != mempool.mapTx.end());
    if (mapModifiedTx.count(it) || inBlock.count(it) || failedTx.count(it))
        return true;
    return false;
}

void BlockAssembler::SortForBlock(const CTxMemPool::setEntries& package, CTxMemPool::txiter entry, std::vector<CTxMemPool::txiter>& sortedEntries)
{
    // Sort package by ancestor count
    // If a transaction A depends on transaction B, then A's ancestor count
    // must be greater than B's.  So this is sufficient to validly order the
    // transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(), CompareTxIterByAncestorCount());
}

// This transaction selection algorithm orders the mempool based
// on feerate of a transaction including all unconfirmed ancestors.
// Since we don't remove transactions from the mempool as we select them
// for block inclusion, we need an alternate method of updating the feerate
// of a transaction with its not-yet-selected ancestors as we go.
// This is accomplished by walking the in-mempool descendants of selected
// transactions and storing a temporary modified state in mapModifiedTxs.
// Each time through the loop, we compare the best transaction in
// mapModifiedTxs with the next transaction in the mempool to decide what
// transaction package to work on next.
void BlockAssembler::addPackageTxs()
{
    // mapModifiedTx will store sorted packages after they are modified
    // because some of their txs are already in the block
    indexed_modified_transaction_set mapModifiedTx;
    // Keep track of entries that failed inclusion, to avoid duplicate work
    CTxMemPool::setEntries failedTx;

    // Start by adding all descendants of previously added txs to mapModifiedTx
    // and modifying them for their already included ancestors
    UpdatePackagesForAdded(inBlock, mapModifiedTx);

    CTxMemPool::indexed_transaction_set::index<ancestor_score>::type::iterator mi = mempool.mapTx.get<ancestor_score>().begin();
    CTxMemPool::txiter iter;
    while (mi != mempool.mapTx.get<ancestor_score>().end() || !mapModifiedTx.empty())
    {
        // First try to find a new transaction in mapTx to evaluate.
        if (mi != mempool.mapTx.get<ancestor_score>().end() &&
                SkipMapTxEntry(mempool.mapTx.project<0>(mi), mapModifiedTx, failedTx)) {
            ++mi;
            continue;
        }

        // Now that mi is not stale, determine which transaction to evaluate:
        // the next entry from mapTx, or the best from mapModifiedTx?
        bool fUsingModified = false;

        modtxscoreiter modit = mapModifiedTx.get<ancestor_score>().begin();
        if (mi == mempool.mapTx.get<ancestor_score>().end()) {
            // We're out of entries in mapTx; use the entry from mapModifiedTx
            iter = modit->iter;
            fUsingModified = true;
        } else {
            // Try to compare the mapTx entry to the mapModifiedTx entry
            iter = mempool.mapTx.project<0>(mi);
            if (modit != mapModifiedTx.get<ancestor_score>().end() &&
                    CompareModifiedEntry()(*modit, CTxMemPoolModifiedEntry(iter))) {
                // The best entry in mapModifiedTx has higher score
                // than the one from mapTx.
                // Switch which transaction (package) to consider
                iter = modit->iter;
                fUsingModified = true;
            } else {
                // Either no entry in mapModifiedTx, or it's worse than mapTx.
                // Increment mi for the next loop iteration.
                ++mi;
            }
        }

        // We skip mapTx entries that are inBlock, and mapModifiedTx shouldn't
        // contain anything that is inBlock.
        assert(!inBlock.count(iter));

        uint64_t packageSize = iter->GetSizeWithAncestors();
        CAmount packageFees = iter->GetModFeesWithAncestors();
        unsigned int packageSigOps = iter->GetSigOpCountWithAncestors();
        if (fUsingModified) {
            packageSize = modit->nSizeWithAncestors;
            packageFees = modit->nModFeesWithAncestors;
            packageSigOps = modit->nSigOpCountWithAncestors;
        }

        if (packageFees < ::minRelayTxFee.GetFee(packageSize)) {
            // Everything else we might consider has a lower fee rate
            return;
        }

        if (!TestPackage(packageSize, packageSigOps)) {
            if (fUsingModified) {
                // Since we always look at the best entry in mapModifiedTx,
                // we must erase failed entries so that we can consider the
                // next best entry on the next loop iteration
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        CTxMemPool::setEntries ancestors;
        uint64_t nNoLimit = std::numeric_limits<uint64_t>::max();
        std::string dummy;
        mempool.CalculateMemPoolAncestors(*iter, ancestors, nNoLimit, nNoLimit, nNoLimit, nNoLimit, dummy, false);

        onlyUnconfirmed(ancestors);
        ancestors.insert(iter);

        // Test if all tx's are Final
        if (!TestPackageFinality(ancestors)) {
            if (fUsingModified) {
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        // Package can be added. Sort the entries in a valid order.
        std::vector<CTxMemPool::txiter> sortedEntries;
        SortForBlock(ancestors, iter, sortedEntries);

        for (size_t i = 0; i < sortedEntries.size(); ++i) {
            CTxMemPool::txiter& iterSortedEntries = sortedEntries[i];
            if (iterSortedEntries->IsShielded()) {
                // Don't add SHIELD transactions if in maintenance (SPORK_20)
                if (sporkManager.IsSporkActive(SPORK_20_SAPLING_MAINTENANCE)) {
                    break;
                }
                // Don't add SHIELD transactions if there's no reserved space left in the block
                if (nSizeShielded + iterSortedEntries->GetTxSize() > MAX_BLOCK_SHIELDED_TXES_SIZE) {
                    break;
                }
                // Update cumulative size of SHIELD transactions in this block
                nSizeShielded += iterSortedEntries->GetTxSize();
            }
            AddToBlock(iterSortedEntries);
            // Erase from the modified set, if present
            mapModifiedTx.erase(iterSortedEntries);
        }

        // Update transactions that depend on each of these
        UpdatePackagesForAdded(ancestors, mapModifiedTx);
    }
}

void BlockAssembler::appendSaplingTreeRoot()
{
    // Update header
    pblock->hashFinalSaplingRoot = CalculateSaplingTreeRoot(pblock, nHeight, chainparams);
}

uint256 CalculateSaplingTreeRoot(CBlock* pblock, int nHeight, const CChainParams& chainparams)
{
    if (NetworkUpgradeActive(nHeight, chainparams.GetConsensus(), Consensus::UPGRADE_V5_0)) {
        SaplingMerkleTree sapling_tree;
        assert(pcoinsTip->GetSaplingAnchorAt(pcoinsTip->GetBestAnchor(), sapling_tree));

        // Update the Sapling commitment tree.
        for (const auto &tx : pblock->vtx) {
            if (tx->IsShieldedTx()) {
                for (const OutputDescription &odesc : tx->sapData->vShieldedOutput) {
                    sapling_tree.append(odesc.cmu);
                }
            }
        }
        return sapling_tree.root();
    }
    return UINT256_ZERO;
}

bool SolveBlock(std::shared_ptr<CBlock>& pblock, int nHeight)
{
    unsigned int extraNonce = 0;
    IncrementExtraNonce(pblock, nHeight, extraNonce);
    while (pblock->nNonce < std::numeric_limits<uint32_t>::max() &&
           !CheckProofOfWork(pblock->GetHash(), pblock->nBits)) {
        ++pblock->nNonce;
    }
    return pblock->nNonce != std::numeric_limits<uint32_t>::max();
}

void IncrementExtraNonce(std::shared_ptr<CBlock>& pblock, int nHeight, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock) {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    CMutableTransaction txCoinbase(*pblock->vtx[0]);
    txCoinbase.vin[0].scriptSig = (CScript() << nHeight << CScriptNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(txCoinbase.vin[0].scriptSig.size() <= 100);

    pblock->vtx[0] = MakeTransactionRef(txCoinbase);
    pblock->hashMerkleRoot = BlockMerkleRoot(*pblock);
}

int32_t ComputeBlockVersion(const Consensus::Params& consensus, int nHeight)
{
    if (NetworkUpgradeActive(nHeight, consensus, Consensus::UPGRADE_V5_0)) {
        return CBlockHeader::CURRENT_VERSION;       // v10 (since 5.1.99)
    } else if (consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_V4_0)) {
        return 7;
    } else if (consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_V3_4)) {
        return 6;
    } else if (consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_BIP65)) {
        return 5;
    } else if (consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_ZC)) {
        return 4;
    } else {
        return 3;
    }
}

