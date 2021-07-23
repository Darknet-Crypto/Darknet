// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2011-2013 The PPCoin developers
// Copyright (c) 2013-2014 The NovaCoin Developers
// Copyright (c) 2014-2018 The BlackCoin Developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "validation.h"

#include "addrman.h"
#include "amount.h"
#include "blocksignature.h"
#include "util/blockstatecatcher.h"
#include "budget/budgetmanager.h"
#include "chainparams.h"
#include "checkpoints.h"
#include "checkqueue.h"
#include "consensus/consensus.h"
#include "consensus/merkle.h"
#include "consensus/tx_verify.h"
#include "consensus/validation.h"
#include "consensus/zerocoin_verify.h"
#include "evo/specialtx.h"
#include "flatfile.h"
#include "fs.h"
#include "guiinterface.h"
#include "init.h"
#include "invalid.h"
#include "interfaces/handler.h"
#include "legacy/validation_zerocoin_legacy.h"
#include "kernel.h"
#include "masternode-payments.h"
#include "masternode-sync.h"
#include "masternodeman.h"
#include "policy/policy.h"
#include "pow.h"
#include "reverse_iterate.h"
#include "script/sigcache.h"
#include "spork.h"
#include "sporkdb.h"
#include "evo/evodb.h"
#include "txdb.h"
#include "txmempool.h"
#include "undo.h"
#include "util/system.h"
#include "utilmoneystr.h"
#include "validationinterface.h"
#include "warnings.h"
#include "zpivchain.h"
#include "zpiv/zpivmodule.h"

#include <future>

#include <boost/algorithm/string/replace.hpp>
#include <boost/thread.hpp>
#include <atomic>
#include <queue>


#if defined(NDEBUG)
#error "PIVX cannot be compiled without assertions."
#endif

/**
 * Global state
 */


/**
 * Mutex to guard access to validation specific variables, such as reading
 * or changing the chainstate.
 *
 * This may also need to be locked when updating the transaction pool, e.g. on
 * AcceptToMemoryPool. See CTxMemPool::cs comment for details.
 *
 * The transaction pool has a separate lock to allow reading from it and the
 * chainstate at the same time.
 */
RecursiveMutex cs_main;

BlockMap mapBlockIndex;
CChain chainActive;
CBlockIndex* pindexBestHeader = NULL;

// Best block section
Mutex g_best_block_mutex;
std::condition_variable g_best_block_cv;
uint256 g_best_block;
int64_t g_best_block_time = 0;

int nScriptCheckThreads = 0;
std::atomic<bool> fImporting{false};
std::atomic<bool> fReindex{false};
bool fTxIndex = true;
bool fRequireStandard = true;
bool fCheckBlockIndex = false;
size_t nCoinCacheUsage = 5000 * 300;

/* If the tip is older than this (in seconds), the node is considered to be in initial block download. */
int64_t nMaxTipAge = DEFAULT_MAX_TIP_AGE;

/** Fees smaller than this (in upiv) are considered zero fee (for relaying, mining and transaction creation)
 * We are ~100 times smaller then bitcoin now (2015-06-23), set minRelayTxFee only 10 times higher
 * so it's still 10 times lower comparing to bitcoin.
 */
CFeeRate minRelayTxFee = CFeeRate(10000);

CTxMemPool mempool(::minRelayTxFee);

std::map<uint256, int64_t> mapRejectedBlocks;

CMoneySupply MoneySupply;

static void CheckBlockIndex();

/** Constant stuff for coinbase transactions we create: */
CScript COINBASE_FLAGS;

// Internal stuff
namespace
{
struct CBlockIndexWorkComparator {
    bool operator()(const CBlockIndex* pa, const CBlockIndex* pb) const
    {
        // First sort by most total work, ...
        if (pa->nChainWork > pb->nChainWork) return false;
        if (pa->nChainWork < pb->nChainWork) return true;

        // ... then by earliest time received, ...
        if (pa->nSequenceId < pb->nSequenceId) return false;
        if (pa->nSequenceId > pb->nSequenceId) return true;

        // Use pointer address as tie breaker (should only happen with blocks
        // loaded from disk, as those all have id 0).
        if (pa < pb) return false;
        if (pa > pb) return true;

        // Identical blocks.
        return false;
    }
};

CBlockIndex* pindexBestInvalid;

/**
 * The set of all CBlockIndex entries with BLOCK_VALID_TRANSACTIONS (for itself and all ancestors) and
 * as good as our current tip or better. Entries may be failed, though.
 */
std::set<CBlockIndex*, CBlockIndexWorkComparator> setBlockIndexCandidates;

/**
 * the ChainState Mutex
 * A lock that must be held when modifying this ChainState - held in ActivateBestChain()
 */
Mutex m_cs_chainstate;

/** All pairs A->B, where A (or one if its ancestors) misses transactions, but B has transactions. */
std::multimap<CBlockIndex*, CBlockIndex*> mapBlocksUnlinked;

RecursiveMutex cs_LastBlockFile;
std::vector<CBlockFileInfo> vinfoBlockFile;
int nLastBlockFile = 0;

/**
     * Every received block is assigned a unique and increasing identifier, so we
     * know which one to give priority in case of a fork.
     */
RecursiveMutex cs_nBlockSequenceId;
/** Blocks loaded from disk are assigned id 0, so start the counter at 1. */
uint32_t nBlockSequenceId = 1;

/** Dirty block index entries. */
std::set<CBlockIndex*> setDirtyBlockIndex;

/** Dirty block file entries. */
std::set<int> setDirtyFileInfo;
} // anon namespace

CBlockIndex* FindForkInGlobalIndex(const CChain& chain, const CBlockLocator& locator)
{
    AssertLockHeld(cs_main);
    // Find the first block the caller has in the main chain
    for (const uint256& hash : locator.vHave) {
        BlockMap::iterator mi = mapBlockIndex.find(hash);
        if (mi != mapBlockIndex.end()) {
            CBlockIndex* pindex = (*mi).second;
            if (chain.Contains(pindex))
                return pindex;
        }
    }
    return chain.Genesis();
}

CBlockIndex* GetChainTip()
{
    LOCK(cs_main);
    CBlockIndex* p = chainActive.Tip();
    if (!p)
        return nullptr;
    // Do not pass in the chain active tip, because it can change.
    // Instead pass the blockindex directly from mapblockindex, which is const
    return mapBlockIndex.at(p->GetBlockHash());
}

CCoinsViewCache* pcoinsTip = NULL;
CBlockTreeDB* pblocktree = NULL;
CZerocoinDB* zerocoinDB = NULL;
CSporkDB* pSporkDB = NULL;

enum FlushStateMode {
    FLUSH_STATE_NONE,
    FLUSH_STATE_IF_NEEDED,
    FLUSH_STATE_PERIODIC,
    FLUSH_STATE_ALWAYS
};

// See definition for documentation
bool static FlushStateToDisk(CValidationState &state, FlushStateMode mode);
static FlatFileSeq BlockFileSeq();
static FlatFileSeq UndoFileSeq();

bool CheckFinalTx(const CTransactionRef& tx, int flags)
{
    AssertLockHeld(cs_main);

    // By convention a negative value for flags indicates that the
    // current network-enforced consensus rules should be used. In
    // a future soft-fork scenario that would mean checking which
    // rules would be enforced for the next block and setting the
    // appropriate flags. At the present time no soft-forks are
    // scheduled, so no flags are set.
    flags = std::max(flags, 0);

    // CheckFinalTx() uses chainActive.Height()+1 to evaluate
    // nLockTime because when IsFinalTx() is called within
    // CBlock::AcceptBlock(), the height of the block *being*
    // evaluated is what is used. Thus if we want to know if a
    // transaction can be part of the *next* block, we need to call
    // IsFinalTx() with one more than chainActive.Height().
    const int nBlockHeight = chainActive.Height() + 1;

    // BIP113 will require that time-locked transactions have nLockTime set to
    // less than the median time of the previous block they're contained in.
    // When the next block is created its previous block will be the current
    // chain tip, so we use that to calculate the median time passed to
    // IsFinalTx() if LOCKTIME_MEDIAN_TIME_PAST is set.
    const int64_t nBlockTime = (flags & LOCKTIME_MEDIAN_TIME_PAST) ? chainActive.Tip()->GetMedianTimePast() : GetAdjustedTime();

    return IsFinalTx(tx, nBlockHeight, nBlockTime);
}

bool GetUTXOCoin(const COutPoint& outpoint, Coin& coin)
{
    LOCK(cs_main);
    if (!pcoinsTip->GetCoin(outpoint, coin))
        return false;
    if (coin.IsSpent())
        return false;
    return true;
}

Optional<int> GetUTXOHeight(const COutPoint& outpoint)
{
    // nullopt means UTXO is yet unknown or already spent
    Coin coin;
    return GetUTXOCoin(outpoint, coin) ? Optional<int>(coin.nHeight) : nullopt;
}

void LimitMempoolSize(CTxMemPool& pool, size_t limit, unsigned long age) {
    int expired = pool.Expire(GetTime() - age);
    if (expired != 0)
        LogPrint(BCLog::MEMPOOL, "Expired %i transactions from the memory pool\n", expired);

    std::vector<COutPoint> vNoSpendsRemaining;
    pool.TrimToSize(limit, &vNoSpendsRemaining);
    for (const COutPoint& removed: vNoSpendsRemaining)
        pcoinsTip->Uncache(removed);
}

CAmount GetMinRelayFee(const CTransaction& tx, const CTxMemPool& pool, unsigned int nBytes)
{
    if (tx.IsShieldedTx()) {
        return GetShieldedTxMinFee(tx);
    }
    uint256 hash = tx.GetHash();
    CAmount nFeeDelta = 0;
    pool.ApplyDelta(hash, nFeeDelta);
    if (nFeeDelta > 0)
        return 0;

    return GetMinRelayFee(nBytes);
}

CAmount GetMinRelayFee(unsigned int nBytes)
{
    CAmount nMinFee = ::minRelayTxFee.GetFee(nBytes);
    if (!Params().GetConsensus().MoneyRange(nMinFee)) {
        nMinFee = Params().GetConsensus().nMaxMoneyOut;
    }
    return nMinFee;
}

CAmount GetShieldedTxMinFee(const CTransaction& tx)
{
    assert (tx.IsShieldedTx());
    unsigned int K = DEFAULT_SHIELDEDTXFEE_K;   // Fixed (100) for now
    CAmount nMinFee = ::minRelayTxFee.GetFee(tx.GetTotalSize()) * K;
    if (!Params().GetConsensus().MoneyRange(nMinFee))
        nMinFee = Params().GetConsensus().nMaxMoneyOut;
    return nMinFee;
}

/** Convert CValidationState to a human-readable message for logging */
std::string FormatStateMessage(const CValidationState &state)
{
    return strprintf("%s%s (code %i)",
        state.GetRejectReason(),
        (state.GetDebugMessage().empty() ? "" : ", " + state.GetDebugMessage()),
        state.GetRejectCode());
}

/* Make mempool consistent after a reorg, by re-adding or recursively erasing
 * disconnected block transactions from the mempool, and also removing any
 * other transactions from the mempool that are no longer valid given the new
 * tip/height.
 *
 * Note: we assume that disconnectpool only contains transactions that are NOT
 * confirmed in the current chain nor already in the mempool (otherwise,
 * in-mempool descendants of such transactions would be removed).
 *
 * Passing fAddToMempool=false will skip trying to add the transactions back,
 * and instead just erase from the mempool as needed.
 */

void UpdateMempoolForReorg(DisconnectedBlockTransactions &disconnectpool, bool fAddToMempool)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(mempool.cs);
    std::vector<uint256> vHashUpdate;
    // disconnectpool's insertion_order index sorts the entries from
    // oldest to newest, but the oldest entry will be the last tx from the
    // latest mined block that was disconnected.
    // Iterate disconnectpool in reverse, so that we add transactions
    // back to the mempool starting with the earliest transaction that had
    // been previously seen in a block.
    auto it = disconnectpool.queuedTx.get<insertion_order>().rbegin();
    while (it != disconnectpool.queuedTx.get<insertion_order>().rend()) {
        // if we are resurrecting a ProReg tx, we need to evict any special transaction that
        // depends on it (which would not be accepted in the mempool, with the current chain)
        if ((*it)->IsProRegTx()) {
            mempool.removeProTxReferences((*it)->GetHash(), MemPoolRemovalReason::REORG);
        }
        // ignore validation errors in resurrected transactions
        CValidationState stateDummy;
        if (!fAddToMempool || (*it)->IsCoinBase() || (*it)->IsCoinStake() ||
                !AcceptToMemoryPool(mempool, stateDummy, *it, false, nullptr, true)) {
            // If the transaction doesn't make it in to the mempool, remove any
            // transactions that depend on it (which would now be orphans).
            mempool.removeRecursive(**it, MemPoolRemovalReason::REORG);
        } else if (mempool.exists((*it)->GetHash())) {
            vHashUpdate.emplace_back((*it)->GetHash());
        }
        ++it;
    }
    disconnectpool.queuedTx.clear();
    // AcceptToMemoryPool/addUnchecked all assume that new mempool entries have
    // no in-mempool children, which is generally not true when adding
    // previously-confirmed transactions back to the mempool.
    // UpdateTransactionsFromBlock finds descendants of any transactions in
    // the disconnectpool that were added back and cleans up the mempool state.
    mempool.UpdateTransactionsFromBlock(vHashUpdate);

    // We also need to remove any now-immature transactions
    mempool.removeForReorg(pcoinsTip, chainActive.Tip()->nHeight + 1, STANDARD_LOCKTIME_VERIFY_FLAGS);
    // Re-limit mempool size, in case we added any transactions
    LimitMempoolSize(mempool, gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000,
                              gArgs.GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60);
}

static bool IsCurrentForFeeEstimation()
{
    AssertLockHeld(cs_main);
    if (IsInitialBlockDownload())
        return false;
    if (chainActive.Tip()->GetBlockTime() < (GetTime() - MAX_FEE_ESTIMATION_TIP_AGE))
        return false;
    if (chainActive.Height() < pindexBestHeader->nHeight - 1)
        return false;
    return true;
}

bool AcceptToMemoryPoolWorker(CTxMemPool& pool, CValidationState &state, const CTransactionRef& _tx, bool fLimitFree,
                              bool* pfMissingInputs, int64_t nAcceptTime, bool fOverrideMempoolLimit, bool fRejectAbsurdFee, bool ignoreFees,
                              std::vector<COutPoint>& coins_to_uncache)
{
    AssertLockHeld(cs_main);
    const CTransaction& tx = *_tx;
    if (pfMissingInputs)
        *pfMissingInputs = false;

    // Check maintenance mode
    bool hasTxZerocoins = tx.ContainsZerocoins();
    if (sporkManager.IsSporkActive(SPORK_16_ZEROCOIN_MAINTENANCE_MODE) && hasTxZerocoins)
        return state.DoS(10, error("%s : Zerocoin transactions are temporarily disabled for maintenance",
                __func__), REJECT_INVALID, "bad-tx-zerocoin-maintenance");
    if (sporkManager.IsSporkActive(SPORK_20_SAPLING_MAINTENANCE) && tx.IsShieldedTx())
        return state.DoS(10, error("%s : Shielded transactions are temporarily disabled for maintenance",
                __func__), REJECT_INVALID, "bad-tx-sapling-maintenance");

    const CChainParams& params = Params();
    const Consensus::Params& consensus = params.GetConsensus();
    int chainHeight = chainActive.Height();

    // Zerocoin txes are not longer accepted in the mempool.
    if (hasTxZerocoins) {
        return state.DoS(100, error("%s : v5 upgrade enforced, zerocoin disabled", __func__),
                         REJECT_INVALID, "bad-tx-with-zc");
    }

    // Check transaction
    bool fColdStakingActive = !sporkManager.IsSporkActive(SPORK_19_COLDSTAKING_MAINTENANCE);
    if (!CheckTransaction(tx, state, fColdStakingActive))
        return error("%s : transaction checks for %s failed with %s", __func__, tx.GetHash().ToString(), FormatStateMessage(state));

    int nextBlockHeight = chainHeight + 1;
    // Check transaction contextually against consensus rules at block height
    if (!ContextualCheckTransaction(_tx, state, params, nextBlockHeight, false /* isMined */, IsInitialBlockDownload())) {
        return error("AcceptToMemoryPool: ContextualCheckTransaction failed");
    }

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return state.DoS(100, false, REJECT_INVALID, "coinbase");

    //Coinstake is also only valid in a block, not as a loose transaction
    if (tx.IsCoinStake())
        return state.DoS(100, false, REJECT_INVALID, "coinstake");

    if (pool.existsProviderTxConflict(tx)) {
        return state.DoS(0, false, REJECT_DUPLICATE, "protx-dup");
    }

    // Only accept nLockTime-using transactions that can be mined in the next
    // block; we don't want our mempool filled up with transactions that can't
    // be mined yet.
    if (!CheckFinalTx(_tx, STANDARD_LOCKTIME_VERIFY_FLAGS))
        return state.DoS(0, false, REJECT_NONSTANDARD, "non-final");

    // Rather not work on nonstandard transactions
    std::string reason;
    if (fRequireStandard && !IsStandardTx(_tx, nextBlockHeight, reason))
        return state.DoS(0, false, REJECT_NONSTANDARD, reason);
    // is it already in the memory pool?
    const uint256& hash = tx.GetHash();
    if (pool.exists(hash)) {
        return state.Invalid(false, REJECT_ALREADY_KNOWN, "txn-already-in-mempool");
    }

    // Check for conflicts with in-memory transactions

    {
        LOCK(pool.cs); // protect pool.mapNextTx
        for (const auto& in : tx.vin) {
            COutPoint outpoint = in.prevout;
            if (pool.mapNextTx.count(outpoint)) {
                // Disable replacement feature for now
                return state.Invalid(false, REJECT_CONFLICT, "txn-mempool-conflict");
            }
        }
    }

    // Check sapling nullifiers
    if (tx.IsShieldedTx()) {
        for (const auto& sd : tx.sapData->vShieldedSpend) {
            if (pool.nullifierExists(sd.nullifier))
                return state.Invalid(false, REJECT_INVALID, "bad-txns-nullifier-double-spent");
        }
    }

    {
        CCoinsView dummy;
        CCoinsViewCache view(&dummy);

        CAmount nValueIn = 0;

        LOCK(pool.cs);
        CCoinsViewMemPool viewMemPool(pcoinsTip, pool);
        view.SetBackend(viewMemPool);

        // do we already have it?
        for (size_t out = 0; out < tx.vout.size(); out++) {
            COutPoint outpoint(hash, out);
            bool had_coin_in_cache = pcoinsTip->HaveCoinInCache(outpoint);
            if (view.HaveCoin(outpoint)) {
                if (!had_coin_in_cache) {
                    coins_to_uncache.push_back(outpoint);
                }
                return state.Invalid(false, REJECT_ALREADY_KNOWN, "txn-already-known");
            }
        }

        // do all inputs exist?
        for (const CTxIn& txin : tx.vin) {
            if (!pcoinsTip->HaveCoinInCache(txin.prevout)) {
                coins_to_uncache.push_back(txin.prevout);
            }
            if (!view.HaveCoin(txin.prevout)) {
                if (pfMissingInputs) {
                    *pfMissingInputs = true;
                }
                return false; // fMissingInputs and !state.IsInvalid() is used to detect this condition, don't set state.Invalid()
            }
        }

        // Sapling: are the sapling spends' requirements met in tx(valid anchors/nullifiers)?
        if (!view.HaveShieldedRequirements(tx))
            return state.Invalid(error("AcceptToMemoryPool: shielded requirements not met"),
                                 REJECT_DUPLICATE, "bad-txns-shielded-requirements-not-met");

        // Bring the best block into scope
        view.GetBestBlock();

        nValueIn = view.GetValueIn(tx);

        // we have all inputs cached now, so switch back to dummy, so we don't need to keep lock on mempool
        view.SetBackend(dummy);

        // Check for non-standard pay-to-script-hash in inputs
        if (fRequireStandard && !AreInputsStandard(tx, view))
            return state.Invalid(false, REJECT_NONSTANDARD, "bad-txns-nonstandard-inputs");

        // Check that the transaction doesn't have an excessive number of
        // sigops, making it impossible to mine. Since the coinbase transaction
        // itself can contain sigops MAX_TX_SIGOPS is less than
        // MAX_BLOCK_SIGOPS; we still consider this an invalid rather than
        // merely non-standard transaction.
        unsigned int nSigOps = GetLegacySigOpCount(tx);
        unsigned int nMaxSigOps = MAX_TX_SIGOPS_CURRENT;
        nSigOps += GetP2SHSigOpCount(tx, view);
        if(nSigOps > nMaxSigOps)
            return state.DoS(0, false, REJECT_NONSTANDARD, "bad-txns-too-many-sigops", false,
                strprintf("%d > %d", nSigOps, nMaxSigOps));

        CAmount nValueOut = tx.GetValueOut();
        CAmount nFees = nValueIn - nValueOut;
        bool fSpendsCoinbaseOrCoinstake = false;

        // Keep track of transactions that spend a coinbase, which we re-scan
        // during reorgs to ensure COINBASE_MATURITY is still met.
        for (const CTxIn &txin : tx.vin) {
            const Coin &coin = view.AccessCoin(txin.prevout);
            if (coin.IsCoinBase() || coin.IsCoinStake()) {
                fSpendsCoinbaseOrCoinstake = true;
                break;
            }
        }

        CTxMemPoolEntry entry(_tx, nFees, nAcceptTime, chainHeight,
                              fSpendsCoinbaseOrCoinstake, nSigOps);
        unsigned int nSize = entry.GetTxSize();

        // Don't accept it if it can't get into a block
        if (!ignoreFees) {
            const CAmount txMinFee = GetMinRelayFee(tx, pool, nSize);
            if (fLimitFree && nFees < txMinFee) {
                return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "insufficient fee", false,
                    strprintf("%d < %d", nFees, txMinFee));
            }

            // No transactions are allowed below minRelayTxFee except from disconnected blocks
            if (fLimitFree && nFees < ::minRelayTxFee.GetFee(nSize)) {
                return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "min relay fee not met");
            }
        }

        if (fRejectAbsurdFee) {
            const CAmount nMaxFee = tx.IsShieldedTx() ? GetShieldedTxMinFee(tx) * 100 :
                                                        GetMinRelayFee(nSize) * 10000;
            if (nFees > nMaxFee)
                return state.Invalid(false, REJECT_HIGHFEE, "absurdly-high-fee",
                                     strprintf("%d > %d", nFees, nMaxFee));
        }

        // Calculate in-mempool ancestors, up to a limit.
        CTxMemPool::setEntries setAncestors;
        size_t nLimitAncestors = gArgs.GetArg("-limitancestorcount", DEFAULT_ANCESTOR_LIMIT);
        size_t nLimitAncestorSize = gArgs.GetArg("-limitancestorsize", DEFAULT_ANCESTOR_SIZE_LIMIT)*1000;
        size_t nLimitDescendants = gArgs.GetArg("-limitdescendantcount", DEFAULT_DESCENDANT_LIMIT);
        size_t nLimitDescendantSize = gArgs.GetArg("-limitdescendantsize", DEFAULT_DESCENDANT_SIZE_LIMIT)*1000;
        std::string errString;
        if (!pool.CalculateMemPoolAncestors(entry, setAncestors, nLimitAncestors, nLimitAncestorSize, nLimitDescendants, nLimitDescendantSize, errString)) {
            return state.DoS(0, error("%s : %s", __func__, errString), REJECT_NONSTANDARD, "too-long-mempool-chain", false);
        }

        if (!CheckSpecialTx(tx, chainActive.Tip(), state)) {
            // pass the state returned by the function above
            return false;
        }

        bool fCLTVIsActivated = consensus.NetworkUpgradeActive(chainHeight, Consensus::UPGRADE_BIP65);

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        int flags = STANDARD_SCRIPT_VERIFY_FLAGS;
        if (fCLTVIsActivated)
            flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;

        PrecomputedTransactionData precomTxData(tx);
        if (!CheckInputs(tx, state, view, true, flags, true, precomTxData)) {
            return false;
        }

        // Check again against just the consensus-critical mandatory script
        // verification flags, in case of bugs in the standard flags that cause
        // transactions to pass as valid when they're actually invalid. For
        // instance the STRICTENC flag was incorrectly allowing certain
        // CHECKSIG NOT scripts to pass, even though they were invalid.
        //
        // There is a similar check in CreateNewBlock() to prevent creating
        // invalid blocks, however allowing such transactions into the mempool
        // can be exploited as a DoS attack.
        flags = MANDATORY_SCRIPT_VERIFY_FLAGS;
        if (fCLTVIsActivated)
            flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;
        if (!CheckInputs(tx, state, view, true, flags, true, precomTxData)) {
            return error("%s: BUG! PLEASE REPORT THIS! ConnectInputs failed against MANDATORY but not STANDARD flags %s, %s",
                    __func__, hash.ToString(), FormatStateMessage(state));
        }
        // todo: pool.removeStaged for all conflicting entries

        // This transaction should only count for fee estimation if
        // the node is not behind and it is not dependent on any other
        // transactions in the mempool
        bool validForFeeEstimation = IsCurrentForFeeEstimation() && pool.HasNoInputsOf(tx);

        // Store transaction in memory
        pool.addUnchecked(hash, entry, setAncestors, validForFeeEstimation);

        // trim mempool and check if tx was trimmed
        if (!fOverrideMempoolLimit) {
            LimitMempoolSize(pool, gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000, gArgs.GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60);
            if (!pool.exists(hash))
                return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "mempool full");
        }

        pool.TrimToSize(gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000);
        if (!pool.exists(tx.GetHash()))
            return state.DoS(0, false, REJECT_INSUFFICIENTFEE, "mempool full");
    }

    GetMainSignals().TransactionAddedToMempool(_tx);

    return true;
}

bool AcceptToMemoryPoolWithTime(CTxMemPool& pool, CValidationState &state, const CTransactionRef& tx, bool fLimitFree,
                        bool* pfMissingInputs, int64_t nAcceptTime, bool fOverrideMempoolLimit, bool fRejectAbsurdFee, bool fIgnoreFees)
{
    std::vector<COutPoint> coins_to_uncache;
    bool res = AcceptToMemoryPoolWorker(pool, state, tx, fLimitFree, pfMissingInputs, nAcceptTime, fOverrideMempoolLimit, fRejectAbsurdFee, fIgnoreFees, coins_to_uncache);
    if (!res) {
        for (const COutPoint& outpoint: coins_to_uncache)
            pcoinsTip->Uncache(outpoint);
    }
    // After we've (potentially) uncached entries, ensure our coins cache is still within its size limits
    CValidationState stateDummy;
    FlushStateToDisk(stateDummy, FLUSH_STATE_PERIODIC);
    return res;
}

bool AcceptToMemoryPool(CTxMemPool& pool, CValidationState& state, const CTransactionRef& tx,
                        bool fLimitFree, bool* pfMissingInputs, bool fOverrideMempoolLimit,
                        bool fRejectInsaneFee, bool ignoreFees)
{
    return AcceptToMemoryPoolWithTime(pool, state, tx, fLimitFree, pfMissingInputs, GetTime(), fOverrideMempoolLimit, fRejectInsaneFee, ignoreFees);
}

bool GetOutput(const uint256& hash, unsigned int index, CValidationState& state, CTxOut& out)
{
    CTransactionRef txPrev;
    uint256 hashBlock;
    if (!GetTransaction(hash, txPrev, hashBlock, true)) {
        return state.DoS(100, error("Output not found"));
    }
    if (index > txPrev->vout.size()) {
        return state.DoS(100, error("Output not found, invalid index %d for %s",index, hash.GetHex()));
    }
    out = txPrev->vout[index];
    return true;
}

/** Return transaction in tx, and if it was found inside a block, its hash is placed in hashBlock */
bool GetTransaction(const uint256& hash, CTransactionRef& txOut, uint256& hashBlock, bool fAllowSlow, CBlockIndex* blockIndex)
{
    CBlockIndex* pindexSlow = blockIndex;

    LOCK(cs_main);

    if (!blockIndex) {

        CTransactionRef ptx = mempool.get(hash);
        if (ptx) {
            txOut = ptx;
            return true;
        }

        if (fTxIndex) {
            CDiskTxPos postx;
            if (pblocktree->ReadTxIndex(hash, postx)) {
                CAutoFile file(OpenBlockFile(postx, true), SER_DISK, CLIENT_VERSION);
                if (file.IsNull())
                    return error("%s: OpenBlockFile failed", __func__);
                CBlockHeader header;
                try {
                    file >> header;
                    fseek(file.Get(), postx.nTxOffset, SEEK_CUR);
                    file >> txOut;
                } catch (const std::exception& e) {
                    return error("%s : Deserialize or I/O error - %s", __func__, e.what());
                }
                hashBlock = header.GetHash();
                if (txOut->GetHash() != hash)
                    return error("%s : txid mismatch", __func__);
                return true;
            }

            // transaction not found in the index, nothing more can be done
            return false;
        }

        if (fAllowSlow) { // use coin database to locate block that contains transaction, and scan it
            const Coin& coin = AccessByTxid(*pcoinsTip, hash);
            if (!coin.IsSpent()) pindexSlow = chainActive[coin.nHeight];
        }
    }

    if (pindexSlow) {
        CBlock block;
        if (ReadBlockFromDisk(block, pindexSlow)) {
            for (const auto& tx : block.vtx) {
                if (tx->GetHash() == hash) {
                    txOut = tx;
                    hashBlock = pindexSlow->GetBlockHash();
                    return true;
                }
            }
        }
    }

    return false;
}


//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

bool WriteBlockToDisk(const CBlock& block, FlatFilePos& pos)
{
    // Open history file to append
    CAutoFile fileout(OpenBlockFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("WriteBlockToDisk : OpenBlockFile failed");

    // Write index header
    unsigned int nSize = GetSerializeSize(block, fileout.GetVersion());
    fileout << Params().MessageStart() << nSize;

    // Write block
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0)
        return error("WriteBlockToDisk : ftell failed");
    pos.nPos = (unsigned int)fileOutPos;
    fileout << block;

    return true;
}

bool ReadBlockFromDisk(CBlock& block, const FlatFilePos& pos)
{
    block.SetNull();

    // Open history file to read
    CAutoFile filein(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("ReadBlockFromDisk : OpenBlockFile failed");

    // Read block
    try {
        filein >> block;
    } catch (const std::exception& e) {
        return error("%s : Deserialize or I/O error - %s", __func__, e.what());
    }

    // Check the header
    if (block.IsProofOfWork()) {
        if (!CheckProofOfWork(block.GetHash(), block.nBits))
            return error("ReadBlockFromDisk : Errors in block header");
    }

    return true;
}

bool ReadBlockFromDisk(CBlock& block, const CBlockIndex* pindex)
{
    FlatFilePos blockPos = WITH_LOCK(cs_main, return pindex->GetBlockPos(); );
    if (!ReadBlockFromDisk(block, blockPos)) {
        return false;
    }
    if (block.GetHash() != pindex->GetBlockHash()) {
        LogPrintf("%s : block=%s index=%s\n", __func__, block.GetHash().GetHex(), pindex->GetBlockHash().GetHex());
        return error("ReadBlockFromDisk(CBlock&, CBlockIndex*) : GetHash() doesn't match index");
    }
    return true;
}


double ConvertBitsToDouble(unsigned int nBits)
{
    int nShift = (nBits >> 24) & 0xff;

    double dDiff =
        (double)0x0000ffff / (double)(nBits & 0x00ffffff);

    while (nShift < 29) {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29) {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

CAmount GetBlockValue(int nHeight)
{
    // Fixed block value on regtest
    if (Params().IsRegTestNet()) {
        return 250 * COIN;
    }
    // Testnet high-inflation blocks [2, 200] with value 250k PIV
    const bool isTestnet = Params().IsTestnet();
    if (isTestnet && nHeight < 201 && nHeight > 1) {
        return 250000 * COIN;
    }
    // Mainnet/Testnet block reward reduction schedule
    const int nLast = Params().GetConsensus().vUpgrades[Consensus::UPGRADE_ZC_V2].nActivationHeight;
    if (nHeight > nLast)   return 5    * COIN;
    if (nHeight > 648000)  return 4.5  * COIN;
    if (nHeight > 604800)  return 9    * COIN;
    if (nHeight > 561600)  return 13.5 * COIN;
    if (nHeight > 518400)  return 18   * COIN;
    if (nHeight > 475200)  return 22.5 * COIN;
    if (nHeight > 432000)  return 27   * COIN;
    if (nHeight > 388800)  return 31.5 * COIN;
    if (nHeight > 345600)  return 36   * COIN;
    if (nHeight > 302400)  return 40.5 * COIN;
    if (nHeight > 151200)  return 45   * COIN;
    if (nHeight > 86400)   return 225  * COIN;
    if (nHeight !=1)       return 250  * COIN;
    // Premine for 6 masternodes at block 1
    return 60001 * COIN;
}

int64_t GetMasternodePayment()
{
    return 3 * COIN;
}

bool IsInitialBlockDownload()
{
    // Once this function has returned false, it must remain false.
    static std::atomic<bool> latchToFalse{false};
    // Optimization: pre-test latch before taking the lock.
    if (latchToFalse.load(std::memory_order_relaxed))
        return false;

    LOCK(cs_main);
    if (latchToFalse.load(std::memory_order_relaxed))
         return false;
    const int chainHeight = chainActive.Height();
    if (fImporting || fReindex || chainHeight < Checkpoints::GetTotalBlocksEstimate())
        return true;
    bool state = (chainHeight < pindexBestHeader->nHeight - 24 * 6 ||
            pindexBestHeader->GetBlockTime() < GetTime() - nMaxTipAge);
    if (!state)
        latchToFalse.store(true, std::memory_order_relaxed);
    return state;
}

CBlockIndex *pindexBestForkTip = NULL, *pindexBestForkBase = NULL;

static void AlertNotify(const std::string& strMessage)
{
    uiInterface.NotifyAlertChanged();
    std::string strCmd = gArgs.GetArg("-alertnotify", "");
    if (strCmd.empty()) return;

    // Alert text should be plain ascii coming from a trusted source, but to
    // be safe we first strip anything not in safeChars, then add single quotes around
    // the whole string before passing it to the shell:
    std::string singleQuote("'");
    std::string safeStatus = SanitizeString(strMessage);
    safeStatus = singleQuote+safeStatus+singleQuote;
    boost::replace_all(strCmd, "%s", safeStatus);

    std::thread t(runCommand, strCmd);
    t.detach(); // thread runs free
}

void CheckForkWarningConditions()
{
    AssertLockHeld(cs_main);
    // Before we get past initial download, we cannot reliably alert about forks
    // (we assume we don't get stuck on a fork before the last checkpoint)
    if (IsInitialBlockDownload())
        return;

    const CBlockIndex* pChainTip = chainActive.Tip();
    if (!pChainTip)
        return;

    // If our best fork is no longer within 72 blocks (+/- 3 hours if no one mines it)
    // of our head, drop it
    if (pindexBestForkTip && pChainTip->nHeight - pindexBestForkTip->nHeight >= 72)
        pindexBestForkTip = nullptr;

    if (pindexBestForkTip || (pindexBestInvalid && pindexBestInvalid->nChainWork > pChainTip->nChainWork + (GetBlockProof(*pChainTip) * 6))) {
        if (!GetfLargeWorkForkFound() && pindexBestForkBase) {
            if (pindexBestForkBase->phashBlock) {
                std::string warning = std::string("'Warning: Large-work fork detected, forking after block ") +
                                      pindexBestForkBase->phashBlock->ToString() + std::string("'");
                AlertNotify(warning);
            }
        }
        if (pindexBestForkTip && pindexBestForkBase) {
            if (pindexBestForkBase->phashBlock) {
                LogPrintf("CheckForkWarningConditions: Warning: Large valid fork found\n  forking the chain at height %d (%s)\n  lasting to height %d (%s).\nChain state database corruption likely.\n",
                    pindexBestForkBase->nHeight, pindexBestForkBase->phashBlock->ToString(),
                    pindexBestForkTip->nHeight, pindexBestForkTip->phashBlock->ToString());
                SetfLargeWorkForkFound(true);
            }
        } else {
            LogPrintf("CheckForkWarningConditions: Warning: Found invalid chain at least ~6 blocks longer than our best chain.\nChain state database corruption likely.\n");
            SetfLargeWorkInvalidChainFound(true);
        }
    } else {
        SetfLargeWorkForkFound(false);
        SetfLargeWorkInvalidChainFound(false);
    }
}

void CheckForkWarningConditionsOnNewFork(CBlockIndex* pindexNewForkTip)
{
    AssertLockHeld(cs_main);
    // If we are on a fork that is sufficiently large, set a warning flag
    CBlockIndex* pfork = pindexNewForkTip;
    CBlockIndex* plonger = chainActive.Tip();
    while (pfork && pfork != plonger) {
        while (plonger && plonger->nHeight > pfork->nHeight)
            plonger = plonger->pprev;
        if (pfork == plonger)
            break;
        pfork = pfork->pprev;
    }

    // We define a condition which we should warn the user about as a fork of at least 7 blocks
    // who's tip is within 72 blocks (+/- 3 hours if no one mines it) of ours
    // or a chain that is entirely longer than ours and invalid (note that this should be detected by both)
    // We use 7 blocks rather arbitrarily as it represents just under 10% of sustained network
    // hash rate operating on the fork.
    // We define it this way because it allows us to only store the highest fork tip (+ base) which meets
    // the 7-block condition and from this always have the most-likely-to-cause-warning fork
    if (pfork && (!pindexBestForkTip || (pindexBestForkTip && pindexNewForkTip->nHeight > pindexBestForkTip->nHeight)) &&
        pindexNewForkTip->nChainWork - pfork->nChainWork > (GetBlockProof(*pfork) * 7) &&
        chainActive.Height() - pindexNewForkTip->nHeight < 72) {
        pindexBestForkTip = pindexNewForkTip;
        pindexBestForkBase = pfork;
    }

    CheckForkWarningConditions();
}

void static InvalidChainFound(CBlockIndex* pindexNew)
{
    if (!pindexBestInvalid || pindexNew->nChainWork > pindexBestInvalid->nChainWork)
        pindexBestInvalid = pindexNew;

    LogPrintf("InvalidChainFound: invalid block=%s  height=%d  log2_work=%.16f  date=%s\n",
        pindexNew->GetBlockHash().ToString(), pindexNew->nHeight,
        log(pindexNew->nChainWork.getdouble()) / log(2.0), FormatISO8601DateTime(pindexNew->GetBlockTime()));

    const CBlockIndex* pChainTip = chainActive.Tip();
    assert(pChainTip);
    LogPrintf("InvalidChainFound:  current best=%s  height=%d  log2_work=%.16f  date=%s\n",
        pChainTip->GetBlockHash().GetHex(), pChainTip->nHeight, log(pChainTip->nChainWork.getdouble()) / log(2.0),
        FormatISO8601DateTime(pChainTip->GetBlockTime()));

    CheckForkWarningConditions();
}

void static InvalidBlockFound(CBlockIndex* pindex, const CValidationState& state)
{
    if (!state.CorruptionPossible()) {
        pindex->nStatus |= BLOCK_FAILED_VALID;
        setDirtyBlockIndex.insert(pindex);
        setBlockIndexCandidates.erase(pindex);
        InvalidChainFound(pindex);
    }
}

static bool SkipInvalidUTXOS(int nHeight)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    return Params().NetworkIDString() == CBaseChainParams::MAIN &&
           consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_ZC) &&
           nHeight <= consensus.height_last_invalid_UTXO;
}

void UpdateCoins(const CTransaction& tx, CCoinsViewCache& inputs, CTxUndo& txundo, int nHeight, bool fSkipInvalid)
{
    // mark inputs spent
    if (!tx.IsCoinBase() && !tx.HasZerocoinSpendInputs()) {
        txundo.vprevout.reserve(tx.vin.size());
        for (const CTxIn& txin : tx.vin) {
            txundo.vprevout.emplace_back();
            inputs.SpendCoin(txin.prevout, &txundo.vprevout.back());
        }
    }

    // update spent nullifiers
    inputs.SetNullifiers(tx, true);

    // add outputs
    AddCoins(inputs, tx, nHeight, false, fSkipInvalid);
}

void UpdateCoins(const CTransaction& tx, CCoinsViewCache &inputs, int nHeight, bool fSkipInvalid)
{
    CTxUndo txundo;
    UpdateCoins(tx, inputs, txundo, nHeight, fSkipInvalid);
}

bool CScriptCheck::operator()()
{
    const CScript& scriptSig = ptxTo->vin[nIn].scriptSig;
    return VerifyScript(scriptSig, m_tx_out.scriptPubKey, nFlags, CachingTransactionSignatureChecker(ptxTo, nIn, m_tx_out.nValue, cacheStore, *precomTxData), ptxTo->GetRequiredSigVersion(), &error);
}

int GetSpendHeight(const CCoinsViewCache& inputs)
{
    LOCK(cs_main);
    CBlockIndex* pindexPrev = mapBlockIndex.find(inputs.GetBestBlock())->second;
    return pindexPrev->nHeight + 1;
}

namespace Consensus {
bool CheckTxInputs(const CTransaction& tx, CValidationState& state, const CCoinsViewCache& inputs, int nSpendHeight)
{
    // This doesn't trigger the DoS code on purpose; if it did, it would make it easier
    // for an attacker to attempt to split the network.
    if (!inputs.HaveInputs(tx))
        return state.Invalid(false, 0, "", "Inputs unavailable");

    // are the Sapling's requirements met?
    if (!inputs.HaveShieldedRequirements(tx))
        return state.Invalid(error("CheckInputs(): %s Sapling requirements not met", tx.GetHash().ToString()));

    const Consensus::Params& consensus = ::Params().GetConsensus();
    CAmount nValueIn = 0;
    CAmount nFees = 0;
    for (unsigned int i = 0; i < tx.vin.size(); i++) {
        const COutPoint &prevout = tx.vin[i].prevout;
        const Coin& coin = inputs.AccessCoin(prevout);
        assert(!coin.IsSpent());

        // If prev is coinbase, check that it's matured
        if (coin.IsCoinBase() || coin.IsCoinStake()) {
            if ((signed long)nSpendHeight - coin.nHeight < (signed long)consensus.nCoinbaseMaturity)
                return state.Invalid(false, REJECT_INVALID, "bad-txns-premature-spend-of-coinbase-coinstake",
                        strprintf("tried to spend coinbase/coinstake at depth %d", nSpendHeight - coin.nHeight));
        }

        // Check for negative or overflow input values
        nValueIn += coin.out.nValue;
        if (!consensus.MoneyRange(coin.out.nValue) || !consensus.MoneyRange(nValueIn))
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-inputvalues-outofrange");
    }

    // Sapling
    nValueIn += tx.GetShieldedValueIn();

    if (!tx.IsCoinStake()) {
        if (nValueIn < tx.GetValueOut())
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-in-belowout", false,
                    strprintf("value in (%s) < value out (%s)", FormatMoney(nValueIn), FormatMoney(tx.GetValueOut())));

        // Tally transaction fees
        CAmount nTxFee = nValueIn - tx.GetValueOut();
        if (nTxFee < 0)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-fee-negative");
        nFees += nTxFee;
        if (!consensus.MoneyRange(nFees))
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-fee-outofrange");
    }
    return true;
}
}// namespace Consensus

bool CheckInputs(const CTransaction& tx, CValidationState &state, const CCoinsViewCache &inputs, bool fScriptChecks, unsigned int flags, bool cacheStore, PrecomputedTransactionData& precomTxData, std::vector<CScriptCheck> *pvChecks)
{
    if (!tx.IsCoinBase() && !tx.HasZerocoinSpendInputs()) {

        if (!Consensus::CheckTxInputs(tx, state, inputs, GetSpendHeight(inputs)))
            return false;

        if (pvChecks)
            pvChecks->reserve(tx.vin.size());

        // The first loop above does all the inexpensive checks.
        // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
        // Helps prevent CPU exhaustion attacks.

        // Skip ECDSA signature verification when connecting blocks
        // before the last block chain checkpoint. This is safe because block merkle hashes are
        // still computed and checked, and any change will be caught at the next checkpoint.
        if (fScriptChecks) {
            for (unsigned int i = 0; i < tx.vin.size(); i++) {
                const COutPoint& prevout = tx.vin[i].prevout;
                const Coin& coin = inputs.AccessCoin(prevout);
                assert(!coin.IsSpent());

                // We very carefully only pass in things to CScriptCheck which
                // are clearly committed to by tx' witness hash. This provides
                // a sanity check that our caching is not introducing consensus
                // failures through additional data in, eg, the coins being
                // spent being checked as a part of CScriptCheck.

                // Verify signature
                CScriptCheck check(coin.out, tx, i, flags, cacheStore, &precomTxData);
                if (pvChecks) {
                    pvChecks->emplace_back();
                    check.swap(pvChecks->back());
                } else if (!check()) {
                    if (flags & STANDARD_NOT_MANDATORY_VERIFY_FLAGS) {
                        // Check whether the failure was caused by a
                        // non-mandatory script verification check, such as
                        // non-standard DER encodings or non-null dummy
                        // arguments; if so, don't trigger DoS protection to
                        // avoid splitting the network between upgraded and
                        // non-upgraded nodes.
                        CScriptCheck check2(coin.out, tx, i,
                            flags & ~STANDARD_NOT_MANDATORY_VERIFY_FLAGS, cacheStore, &precomTxData);
                        if (check2())
                            return state.Invalid(false, REJECT_NONSTANDARD, strprintf("non-mandatory-script-verify-flag (%s)", ScriptErrorString(check.GetScriptError())));
                    }
                    // Failures of other flags indicate a transaction that is
                    // invalid in new blocks, e.g. a invalid P2SH. We DoS ban
                    // such nodes as they are not following the protocol. That
                    // said during an upgrade careful thought should be taken
                    // as to the correct behavior - we may want to continue
                    // peering with non-upgraded nodes even after a soft-fork
                    // super-majority vote has passed.
                    return state.DoS(100, false, REJECT_INVALID, strprintf("mandatory-script-verify-flag-failed (%s)", ScriptErrorString(check.GetScriptError())));
                }
            }
        }
    }

    return true;
}

/** Abort with a message */
static bool AbortNode(const std::string& strMessage, const std::string& userMessage="")
{
    SetMiscWarning(strMessage);
    LogPrintf("*** %s\n", strMessage);
    uiInterface.ThreadSafeMessageBox(
        userMessage.empty() ? _("Error: A fatal internal error occured, see debug.log for details") : userMessage,
        "", CClientUIInterface::MSG_ERROR);
    StartShutdown();
    return false;
}

static bool AbortNode(CValidationState& state, const std::string& strMessage, const std::string& userMessage="")
{
    AbortNode(strMessage, userMessage);
    return state.Error(strMessage);
}

namespace {

bool UndoWriteToDisk(const CBlockUndo& blockundo, FlatFilePos& pos, const uint256& hashBlock)
{
    // Open history file to append
    CAutoFile fileout(OpenUndoFile(pos), SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : OpenUndoFile failed", __func__);

    // Write index header
    unsigned int nSize = GetSerializeSize(blockundo, fileout.GetVersion());
    fileout << Params().MessageStart() << nSize;

    // Write undo data
    long fileOutPos = ftell(fileout.Get());
    if (fileOutPos < 0)
        return error("%s : ftell failed", __func__);
    pos.nPos = (unsigned int)fileOutPos;
    fileout << blockundo;

    // calculate & write checksum
    CHashWriter hasher(SER_GETHASH, PROTOCOL_VERSION);
    hasher << hashBlock;
    hasher << blockundo;
    fileout << hasher.GetHash();

    return true;
}

bool UndoReadFromDisk(CBlockUndo& blockundo, const FlatFilePos& pos, const uint256& hashBlock)
{
    // Open history file to read
    CAutoFile filein(OpenUndoFile(pos, true), SER_DISK, CLIENT_VERSION);
    if (filein.IsNull())
        return error("%s : OpenBlockFile failed", __func__);

    // Read block
    uint256 hashChecksum;
    CHashVerifier<CAutoFile> verifier(&filein); // We need a CHashVerifier as reserializing may lose data
    try {
        verifier << hashBlock;
        verifier >> blockundo;
        filein >> hashChecksum;
    } catch (const std::exception& e) {
        return error("%s : Deserialize or I/O error - %s", __func__, e.what());
    }

    // Verify checksum
    if (hashChecksum != verifier.GetHash())
        return error("%s : Checksum mismatch", __func__);

    return true;
}

} // anon namespace

enum DisconnectResult
{
    DISCONNECT_OK,      // All good.
    DISCONNECT_UNCLEAN, // Rolled back, but UTXO set was inconsistent with block.
    DISCONNECT_FAILED   // Something else went wrong.
};

/**
 * Restore the UTXO in a Coin at a given COutPoint
 * @param undo The Coin to be restored.
 * @param view The coins view to which to apply the changes.
 * @param out The out point that corresponds to the tx input.
 * @return A DisconnectResult as an int
 */
int ApplyTxInUndo(Coin&& undo, CCoinsViewCache& view, const COutPoint& out)
{
    bool fClean = true;

    if (view.HaveCoin(out)) fClean = false; // overwriting transaction output

    if (undo.nHeight == 0) {
        // Missing undo metadata (height and coinbase/coinstake). Older versions included this
        // information only in undo records for the last spend of a transactions'
        // outputs. This implies that it must be present for some other output of the same tx.
        const Coin& alternate = AccessByTxid(view, out.hash);
        if (!alternate.IsSpent()) {
            undo.nHeight = alternate.nHeight;
            undo.fCoinBase = alternate.fCoinBase;
            undo.fCoinStake = alternate.fCoinStake;
        } else {
            return DISCONNECT_FAILED; // adding output for transaction without known metadata
        }
    }
    // The potential_overwrite parameter to AddCoin is only allowed to be false if we know for
    // sure that the coin did not already exist in the cache. As we have queried for that above
    // using HaveCoin, we don't need to guess. When fClean is false, a coin already existed and
    // it is an overwrite.
    view.AddCoin(out, std::move(undo), !fClean);

    return fClean ? DISCONNECT_OK : DISCONNECT_UNCLEAN;
}


/** Undo the effects of this block (with given index) on the UTXO set represented by coins.
 *  When FAILED is returned, view is left in an indeterminate state. */
DisconnectResult DisconnectBlock(CBlock& block, const CBlockIndex* pindex, CCoinsViewCache& view)
{
    AssertLockHeld(cs_main);

    bool fDIP3Active = Params().GetConsensus().NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_V6_0);
    bool fHasBestBlock = evoDb->VerifyBestBlock(pindex->GetBlockHash());

    if (fDIP3Active && !fHasBestBlock) {
        AbortNode("Found EvoDB inconsistency, you must reindex to continue");
        return DISCONNECT_FAILED;
    }

    bool fClean = true;

    CBlockUndo blockUndo;
    FlatFilePos pos = pindex->GetUndoPos();
    if (pos.IsNull()) {
        error("%s: no undo data available", __func__);
        return DISCONNECT_FAILED;
    }
    if (!UndoReadFromDisk(blockUndo, pos, pindex->pprev->GetBlockHash())) {
        error("%s: failure reading undo data", __func__);
        return DISCONNECT_FAILED;
    }

    if (blockUndo.vtxundo.size() + 1 != block.vtx.size()) {
        error("%s: block and undo data inconsistent", __func__);
        return DISCONNECT_FAILED;
    }

    if (!UndoSpecialTxsInBlock(block, pindex)) {
        return DISCONNECT_FAILED;
    }

    // undo transactions in reverse order
    for (int i = block.vtx.size() - 1; i >= 0; i--) {
        const CTransaction& tx = *block.vtx[i];

        if (!DisconnectZerocoinTx(tx, zerocoinDB))
            return DISCONNECT_FAILED;

        const uint256& hash = tx.GetHash();

        // if tx is a budget collateral tx, remove relative object
        g_budgetman.RemoveByFeeTxId(hash);

        // Check that all outputs are available and match the outputs in the block itself
        // exactly.
        for (size_t o = 0; o < tx.vout.size(); o++) {
            if (!tx.vout[o].scriptPubKey.IsUnspendable() && !tx.vout[o].IsZerocoinMint()) {
                COutPoint out(hash, o);
                Coin coin;
                view.SpendCoin(out, &coin);
                if (tx.vout[o] != coin.out) {
                    fClean = false; // transaction output mismatch
                }
            }
        }

        // not coinbases or zerocoinspend because they dont have traditional inputs
        if (tx.IsCoinBase() || tx.HasZerocoinSpendInputs())
            continue;

        // Sapling, update unspent nullifiers
        view.SetNullifiers(tx, false);

        // restore inputs
        CTxUndo& txundo = blockUndo.vtxundo[i - 1];
        if (txundo.vprevout.size() != tx.vin.size()) {
            error("%s: transaction and undo data inconsistent - txundo.vprevout.siz=%d tx.vin.siz=%d",
                    __func__, txundo.vprevout.size(), tx.vin.size());
            return DISCONNECT_FAILED;
        }
        for (unsigned int j = tx.vin.size(); j-- > 0;) {
            const COutPoint& out = tx.vin[j].prevout;
            int res = ApplyTxInUndo(std::move(txundo.vprevout[j]), view, out);
            if (res == DISCONNECT_FAILED) return DISCONNECT_FAILED;
            fClean = fClean && res != DISCONNECT_UNCLEAN;
        }
        // At this point, all of txundo.vprevout should have been moved out.
    }

    const Consensus::Params& consensus = Params().GetConsensus();

    // set the old best Sapling anchor back
    // We can get this from the `hashFinalSaplingRoot` of the last block
    // However, this is only reliable if the last block was on or after
    // the Sapling activation height. Otherwise, the last anchor was the
    // empty root.
    if (consensus.NetworkUpgradeActive(pindex->pprev->nHeight, Consensus::UPGRADE_V5_0)) {
        view.PopAnchor(pindex->pprev->hashFinalSaplingRoot);
    } else {
        view.PopAnchor(SaplingMerkleTree::empty_root());
    }

    // move best block pointer to prevout block
    view.SetBestBlock(pindex->pprev->GetBlockHash());
    evoDb->WriteBestBlock(pindex->pprev->GetBlockHash());

    if (consensus.NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_ZC_V2) &&
            pindex->nHeight <= consensus.height_last_ZC_AccumCheckpoint) {
        // Legacy Zerocoin DB: If Accumulators Checkpoint is changed, remove changed checksums
        DataBaseAccChecksum(pindex, false);
    }

    return fClean ? DISCONNECT_OK : DISCONNECT_UNCLEAN;
}

void static FlushBlockFile(bool fFinalize = false)
{
    LOCK(cs_LastBlockFile);

    FlatFilePos block_pos_old(nLastBlockFile, vinfoBlockFile[nLastBlockFile].nSize);
    FlatFilePos undo_pos_old(nLastBlockFile, vinfoBlockFile[nLastBlockFile].nUndoSize);

    bool status = true;
    status &= BlockFileSeq().Flush(block_pos_old, fFinalize);
    status &= UndoFileSeq().Flush(undo_pos_old, fFinalize);
    if (!status) {
        AbortNode("Flushing block file to disk failed. This is likely the result of an I/O error.");
    }
}

bool FindUndoPos(CValidationState& state, int nFile, FlatFilePos& pos, unsigned int nAddSize);

static CCheckQueue<CScriptCheck> scriptcheckqueue(128);

void ThreadScriptCheck()
{
    util::ThreadRename("pivx-scriptch");
    scriptcheckqueue.Thread();
}

static int64_t nTimeVerify = 0;
static int64_t nTimeProcessSpecial = 0;
static int64_t nTimeConnect = 0;
static int64_t nTimeIndex = 0;
static int64_t nTimeTotal = 0;

/** Apply the effects of this block (with given index) on the UTXO set represented by coins.
 *  Validity checks that depend on the UTXO set are also done; ConnectBlock()
 *  can fail if those validity checks fail (among other reasons). */
static bool ConnectBlock(const CBlock& block, CValidationState& state, CBlockIndex* pindex, CCoinsViewCache& view, bool fJustCheck = false)
{
    AssertLockHeld(cs_main);
    // Check it again in case a previous version let a bad block in
    if (!CheckBlock(block, state, !fJustCheck, !fJustCheck, !fJustCheck)) {
        if (state.CorruptionPossible()) {
            // We don't write down blocks to disk if they may have been
            // corrupted, so this should be impossible unless we're having hardware
            // problems.
            return AbortNode(state, "Corrupt block found indicating potential hardware failure; shutting down");
        }
        return error("%s: CheckBlock failed for %s: %s", __func__, block.GetHash().ToString(), FormatStateMessage(state));
    }

    // verify that the view's current state corresponds to the previous block
    uint256 hashPrevBlock = pindex->pprev == NULL ? UINT256_ZERO : pindex->pprev->GetBlockHash();
    if (hashPrevBlock != view.GetBestBlock())
        LogPrintf("%s: hashPrev=%s view=%s\n", __func__, hashPrevBlock.GetHex(), view.GetBestBlock().GetHex());
    assert(hashPrevBlock == view.GetBestBlock());

    const bool isPoSBlock = block.IsProofOfStake();
    const Consensus::Params& consensus = Params().GetConsensus();
    const bool isPoSActive = consensus.NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_POS);
    const bool isV5UpgradeEnforced = consensus.NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_V5_0);
    const bool isV6UpgradeEnforced = consensus.NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_V6_0);

    // Coinbase output should be empty if proof-of-stake block (before v6 enforcement)
    if (!isV6UpgradeEnforced && isPoSBlock && (block.vtx[0]->vout.size() != 1 || !block.vtx[0]->vout[0].IsEmpty()))
        return state.DoS(100, false, REJECT_INVALID, "bad-cb-pos", false, "coinbase output not empty for proof-of-stake block");

    if (pindex->pprev) {
        bool fHasBestBlock = evoDb->VerifyBestBlock(hashPrevBlock);

        if (isV6UpgradeEnforced && !fHasBestBlock) {
            return AbortNode(state, "Found EvoDB inconsistency, you must reindex to continue");
        }
    }

    // Special case for the genesis block, skipping connection of its transactions
    // (its coinbase is unspendable)
    if (block.GetHash() == consensus.hashGenesisBlock) {
        if (!fJustCheck) {
            view.SetBestBlock(pindex->GetBlockHash());
        }
        return true;
    }

    if (!isPoSActive && isPoSBlock)
        return state.DoS(100, error("ConnectBlock() : PoS period not active"),
            REJECT_INVALID, "PoS-early");

    if (isPoSActive && !isPoSBlock)
        return state.DoS(100, error("ConnectBlock() : PoW period ended"),
            REJECT_INVALID, "PoW-ended");

    // Sapling
    // Reject a block that results in a negative shielded value pool balance.
    // Description under ZIP209 turnstile violation.

    // If we've reached ConnectBlock, we have all transactions of
    // parents and can expect nChainSaplingValue not to be boost::none.
    // However, the miner and mining RPCs may not have populated this
    // value and will call `TestBlockValidity`. So, we act
    // conditionally.
    if (pindex->nChainSaplingValue) {
        if (*pindex->nChainSaplingValue < 0) {
            return state.DoS(100, error("%s: turnstile violation in Sapling shielded value pool: val: %d", __func__, *pindex->nChainSaplingValue),
                             REJECT_INVALID, "turnstile-violation-sapling-shielded-pool");
        }
    }

    bool fScriptChecks = pindex->nHeight >= Checkpoints::GetTotalBlocksEstimate();

    // If scripts won't be checked anyways, don't bother seeing if CLTV is activated
    bool fCLTVIsActivated = false;
    if (fScriptChecks && pindex->pprev) {
        fCLTVIsActivated = consensus.NetworkUpgradeActive(pindex->pprev->nHeight, Consensus::UPGRADE_BIP65);
    }

    CCheckQueueControl<CScriptCheck> control(fScriptChecks && nScriptCheckThreads ? &scriptcheckqueue : nullptr);

    int64_t nTimeStart = GetTimeMicros();
    CAmount nFees = 0;
    int nInputs = 0;
    unsigned int nSigOps = 0;
    CDiskTxPos pos(pindex->GetBlockPos(), GetSizeOfCompactSize(block.vtx.size()));
    std::vector<std::pair<uint256, CDiskTxPos> > vPos;
    std::vector<std::pair<CBigNum, uint256> > vSpends;
    vPos.reserve(block.vtx.size());
    CBlockUndo blockundo;
    blockundo.vtxundo.reserve(block.vtx.size() - 1);
    CAmount nValueOut = 0;
    CAmount nValueIn = 0;
    unsigned int nMaxBlockSigOps = MAX_BLOCK_SIGOPS_CURRENT;

    // Sapling
    SaplingMerkleTree sapling_tree;
    assert(view.GetSaplingAnchorAt(view.GetBestAnchor(), sapling_tree));

    std::vector<PrecomputedTransactionData> precomTxData;
    precomTxData.reserve(block.vtx.size()); // Required so that pointers to individual precomTxData don't get invalidated
    bool fInitialBlockDownload = IsInitialBlockDownload();
    bool fZerocoinMaintenance =  (block.nTime > sporkManager.GetSporkValue(SPORK_16_ZEROCOIN_MAINTENANCE_MODE));
    bool fSaplingMaintenance =  (block.nTime > sporkManager.GetSporkValue(SPORK_20_SAPLING_MAINTENANCE));
    for (unsigned int i = 0; i < block.vtx.size(); i++) {
        const CTransaction& tx = *block.vtx[i];

        nInputs += tx.vin.size();
        nSigOps += GetLegacySigOpCount(tx);
        if (nSigOps > nMaxBlockSigOps)
            return state.DoS(100, error("ConnectBlock() : too many sigops"), REJECT_INVALID, "bad-blk-sigops");

        // Check maintenance mode
        if (!fInitialBlockDownload) {
            if (fZerocoinMaintenance && tx.ContainsZerocoins())
                return state.DoS(100, error("%s : zerocoin transactions are currently in maintenance mode", __func__));
            if (fSaplingMaintenance && tx.IsShieldedTx())
                return state.DoS(100, error("%s : shielded transactions are currently in maintenance mode", __func__));
        }

        // If v5 is active, bye bye zerocoin
        if (isV5UpgradeEnforced && tx.ContainsZerocoins()) {
            return state.DoS(100, error("%s : v5 upgrade enforced, zerocoin disabled", __func__));
        }

        if (tx.HasZerocoinSpendInputs()) {
            auto opCoinSpendValues = ParseAndValidateZerocoinSpend(consensus, tx, pindex->nHeight, state);
            if (!opCoinSpendValues) {
                return false; // Invalidity/DoS is handled by ParseAndValidateZerocoinSpend.
            }
            nValueIn += opCoinSpendValues->value;
            vSpends.emplace_back(opCoinSpendValues->serial, tx.GetHash());
        } else if (!tx.IsCoinBase()) {
            if (!view.HaveInputs(tx)) {
                return state.DoS(100, false, REJECT_INVALID, "bad-txns-inputs-missingorspent");
            }
            // Sapling: are the sapling spends' requirements met in tx(valid anchors/nullifiers)?
            if (!view.HaveShieldedRequirements(tx))
                return state.DoS(100, error("%s: spends requirements not met", __func__),
                                 REJECT_INVALID, "bad-txns-sapling-requirements-not-met");

            // Add in sigops done by pay-to-script-hash inputs;
            // this is to prevent a "rogue miner" from creating
            // an incredibly-expensive-to-validate block.
            nSigOps += GetP2SHSigOpCount(tx, view);
            if (nSigOps > nMaxBlockSigOps)
                return state.DoS(100, error("ConnectBlock() : too many sigops"), REJECT_INVALID, "bad-blk-sigops");

        }

        // Cache the sig ser hashes
        precomTxData.emplace_back(tx);

        if (!tx.IsCoinBase()) {
            if (!tx.IsCoinStake())
                nFees += view.GetValueIn(tx) - tx.GetValueOut();
            nValueIn += view.GetValueIn(tx);

            std::vector<CScriptCheck> vChecks;
            unsigned int flags = SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_DERSIG;
            if (fCLTVIsActivated)
                flags |= SCRIPT_VERIFY_CHECKLOCKTIMEVERIFY;

            bool fCacheResults = fJustCheck; /* Don't cache results if we're actually connecting blocks (still consult the cache, though) */
            if (!CheckInputs(tx, state, view, fScriptChecks, flags, fCacheResults, precomTxData[i], nScriptCheckThreads ? &vChecks : NULL))
                return error("%s: Check inputs on %s failed with %s", __func__, tx.GetHash().ToString(), FormatStateMessage(state));
            control.Add(vChecks);
        }
        nValueOut += tx.GetValueOut();

        CTxUndo undoDummy;
        if (i > 0) {
            blockundo.vtxundo.emplace_back();
        }
        const bool fSkipInvalid = SkipInvalidUTXOS(pindex->nHeight);
        UpdateCoins(tx, view, i == 0 ? undoDummy : blockundo.vtxundo.back(), pindex->nHeight, fSkipInvalid);

        // Sapling update tree
        if (tx.IsShieldedTx() && !tx.sapData->vShieldedOutput.empty()) {
            for(const OutputDescription &outputDescription : tx.sapData->vShieldedOutput) {
                sapling_tree.append(outputDescription.cmu);
            }
        }

        vPos.emplace_back(tx.GetHash(), pos);
        pos.nTxOffset += ::GetSerializeSize(tx, CLIENT_VERSION);
    }

    // Push new tree anchor
    view.PushAnchor(sapling_tree);

    // Verify header correctness
    if (isV5UpgradeEnforced) {
        // If Sapling is active, block.hashFinalSaplingRoot must be the
        // same as the root of the Sapling tree
        if (block.hashFinalSaplingRoot != sapling_tree.root()) {
            return state.DoS(100,
                             error("ConnectBlock(): block's hashFinalSaplingRoot is incorrect (should be Sapling tree root)"),
                             REJECT_INVALID, "bad-sapling-root-in-block");
        }
    }

    // track mint amount info
    const int64_t nMint = (nValueOut - nValueIn) + nFees;

    int64_t nTime1 = GetTimeMicros();
    nTimeConnect += nTime1 - nTimeStart;
    LogPrint(BCLog::BENCH, "      - Connect %u transactions: %.2fms (%.3fms/tx, %.3fms/txin) [%.2fs]\n", (unsigned)block.vtx.size(), 0.001 * (nTime1 - nTimeStart), 0.001 * (nTime1 - nTimeStart) / block.vtx.size(), nInputs <= 1 ? 0 : 0.001 * (nTime1 - nTimeStart) / (nInputs - 1), nTimeConnect * 0.000001);

    //PoW phase redistributed fees to miner. PoS stage destroys fees.
    CAmount nExpectedMint = GetBlockValue(pindex->nHeight);
    if (!isPoSBlock)
        nExpectedMint += nFees;

    //Check that the block does not overmint
    CAmount nBudgetAmt = 0;     // If this is a superblock, amount to be paid to the winning proposal, otherwise 0
    if (!IsBlockValueValid(pindex->nHeight, nExpectedMint, nMint, nBudgetAmt)) {
        return state.DoS(100, error("%s: reward pays too much (actual=%s vs limit=%s)",
                                    __func__, FormatMoney(nMint), FormatMoney(nExpectedMint)),
                         REJECT_INVALID, "bad-blk-amount");
    }

    // Masternode/Budget payments
    // !TODO: after transition to DMN is complete, check this also during IBD
    if (!fInitialBlockDownload) {
        if (!IsBlockPayeeValid(block, pindex->pprev)) {
            mapRejectedBlocks.emplace(block.GetHash(), GetTime());
            return state.DoS(0, false, REJECT_INVALID, "bad-cb-payee", false, "Couldn't find masternode/budget payment");
        }
    }

    // After v6 enforcement: Check that the coinbase pays the exact amount
    if (isPoSBlock && isV6UpgradeEnforced && !IsCoinbaseValueValid(block.vtx[0], nBudgetAmt, state)) {
        // pass the state returned by the function above
        return false;
    }

    if (!control.Wait())
        return state.DoS(100, error("%s: CheckQueue failed", __func__), REJECT_INVALID, "block-validation-failed");
    int64_t nTime2 = GetTimeMicros();
    nTimeVerify += nTime2 - nTimeStart;
    LogPrint(BCLog::BENCH, "    - Verify %u txins: %.2fms (%.3fms/txin) [%.2fs]\n", nInputs - 1, 0.001 * (nTime2 - nTimeStart), nInputs <= 1 ? 0 : 0.001 * (nTime2 - nTimeStart) / (nInputs - 1), nTimeVerify * 0.000001);

    if (!ProcessSpecialTxsInBlock(block, pindex, state, fJustCheck)) {
        return error("%s: Special tx processing failed with %s", __func__, FormatStateMessage(state));
    }
    int64_t nTime3 = GetTimeMicros();
    nTimeProcessSpecial += nTime3 - nTime2;
    LogPrint(BCLog::BENCH, "    - Process special tx: %.2fms [%.2fs]\n", 0.001 * (nTime3 - nTime2), nTimeProcessSpecial * 0.000001);

    //IMPORTANT NOTE: Nothing before this point should actually store to disk (or even memory)
    if (fJustCheck)
        return true;

    // Write undo information to disk
    if (pindex->GetUndoPos().IsNull() || !pindex->IsValid(BLOCK_VALID_SCRIPTS)) {
        if (pindex->GetUndoPos().IsNull()) {
            FlatFilePos diskPosBlock;
            if (!FindUndoPos(state, pindex->nFile, diskPosBlock, ::GetSerializeSize(blockundo, CLIENT_VERSION) + 40))
                return error("ConnectBlock() : FindUndoPos failed");
            if (!UndoWriteToDisk(blockundo, diskPosBlock, pindex->pprev->GetBlockHash()))
                return AbortNode(state, "Failed to write undo data");

            // update nUndoPos in block index
            pindex->nUndoPos = diskPosBlock.nPos;
            pindex->nStatus |= BLOCK_HAVE_UNDO;
        }

        pindex->RaiseValidity(BLOCK_VALID_SCRIPTS);
        setDirtyBlockIndex.insert(pindex);
    }

    // Flush spend/mint info to disk
    if (!vSpends.empty() && !zerocoinDB->WriteCoinSpendBatch(vSpends))
        return AbortNode(state, "Failed to record coin serials to database");

    if (fTxIndex)
        if (!pblocktree->WriteTxIndex(vPos))
            return AbortNode(state, "Failed to write transaction index");

    // add this block to the view's block chain
    view.SetBestBlock(pindex->GetBlockHash());
    evoDb->WriteBestBlock(pindex->GetBlockHash());

    int64_t nTime4 = GetTimeMicros();
    nTimeIndex += nTime4 - nTime3;
    LogPrint(BCLog::BENCH, "    - Index writing: %.2fms [%.2fs]\n", 0.001 * (nTime4 - nTime3), nTimeIndex * 0.000001);

    if (consensus.NetworkUpgradeActive(pindex->nHeight, Consensus::UPGRADE_ZC_V2) &&
            pindex->nHeight < consensus.height_last_ZC_AccumCheckpoint) {
        // Legacy Zerocoin DB: If Accumulators Checkpoint is changed, database the checksums
        DataBaseAccChecksum(pindex, true);
    } else if (pindex->nHeight == consensus.height_last_ZC_AccumCheckpoint) {
        // After last Checkpoint block, wipe the checksum database
        zerocoinDB->WipeAccChecksums();
    }

    // 100 blocks after the last invalid out, clean the map contents
    if (pindex->nHeight == consensus.height_last_invalid_UTXO + 100) {
        invalid_out::setInvalidOutPoints.clear();
    }

    return true;
}

/**
 * Update the on-disk chain state.
 * The caches and indexes are flushed if either they're too large, forceWrite is set, or
 * fast is not set and it's been a while since the last write.
 * Full flush also updates the money supply from disk (except during shutdown)
 */
bool static FlushStateToDisk(CValidationState& state, FlushStateMode mode)
{
    int64_t nMempoolUsage = mempool.DynamicMemoryUsage();
    LOCK(cs_main);
    static int64_t nLastWrite = 0;
    static int64_t nLastFlush = 0;
    static int64_t nLastSetChain = 0;
    try {
        int64_t nNow = GetTimeMicros();
        // Avoid writing/flushing immediately after startup.
        if (nLastWrite == 0) {
            nLastWrite = nNow;
        }
        if (nLastFlush == 0) {
            nLastFlush = nNow;
        }
        if (nLastSetChain == 0) {
            nLastSetChain = nNow;
        }
        int64_t nMempoolSizeMax = gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000;
        int64_t cacheSize = pcoinsTip->DynamicMemoryUsage();
        cacheSize += evoDb->GetMemoryUsage();
        int64_t nTotalSpace = nCoinCacheUsage + std::max<int64_t>(nMempoolSizeMax - nMempoolUsage, 0);
        // The cache is large and we're within 10% and 10 MiB of the limit, but we have time now
        // (not in the middle of a block processing).
        bool fCacheLarge = mode == FLUSH_STATE_PERIODIC &&
                cacheSize > std::max((9 * nTotalSpace) / 10, nTotalSpace - MAX_BLOCK_COINSDB_USAGE * 1024 * 1024);
        // The cache is over the limit, we have to write now.
        bool fCacheCritical = mode == FLUSH_STATE_IF_NEEDED && (unsigned) cacheSize > nCoinCacheUsage;
        // It's been a while since we wrote the block index to disk.
        // Do this frequently, so we don't need to redownload after a crash.
        bool fPeriodicWrite = mode == FLUSH_STATE_PERIODIC && nNow > nLastWrite + (int64_t)DATABASE_WRITE_INTERVAL * 1000000;
        // It's been very long since we flushed the cache. Do this infrequently, to optimize cache usage.
        bool fPeriodicFlush = mode == FLUSH_STATE_PERIODIC && nNow > nLastFlush + (int64_t)DATABASE_FLUSH_INTERVAL * 1000000;
        // Combine all conditions that result in a full cache flush.
        bool fDoFullFlush = (mode == FLUSH_STATE_ALWAYS) || fCacheLarge || fCacheCritical || fPeriodicFlush;
        // Write blocks and block index to disk.
        if (fDoFullFlush || fPeriodicWrite) {
            // Depend on nMinDiskSpace to ensure we can write block index
            if (!CheckDiskSpace(GetBlocksDir())) {
                return AbortNode(state, "Disk space is low!", _("Error: Disk space is low!"));
            }
            // First make sure all block and undo data is flushed to disk.
            FlushBlockFile();
            // Then update all block file information (which may refer to block and undo files).
            {
                std::vector<std::pair<int, const CBlockFileInfo*> > vFiles;
                vFiles.reserve(setDirtyFileInfo.size());
                for (std::set<int>::iterator it = setDirtyFileInfo.begin(); it != setDirtyFileInfo.end(); ) {
                    vFiles.emplace_back(*it, &vinfoBlockFile[*it]);
                    setDirtyFileInfo.erase(it++);
                }
                std::vector<const CBlockIndex*> vBlocks;
                vBlocks.reserve(setDirtyBlockIndex.size());
                for (std::set<CBlockIndex*>::iterator it = setDirtyBlockIndex.begin(); it != setDirtyBlockIndex.end(); ) {
                    vBlocks.push_back(*it);
                    setDirtyBlockIndex.erase(it++);
                }
                if (!pblocktree->WriteBatchSync(vFiles, nLastBlockFile, vBlocks)) {
                    return AbortNode(state, "Files to write to block index database");
                }
            }
            nLastWrite = nNow;
        }

        // Flush best chain related state. This can only be done if the blocks / block index write was also done.
        if (fDoFullFlush) {
            // Typical Coin structures on disk are around 48 bytes in size.
            // Pushing a new one to the database can cause it to be written
            // twice (once in the log, and once in the tables). This is already
            // an overestimation, as most will delete an existing entry or
            // overwrite one. Still, use a conservative safety factor of 2.
            if (!CheckDiskSpace(GetDataDir(), 48 * 2 * 2 * pcoinsTip->GetCacheSize())) {
                return AbortNode(state, "Disk space is low!", _("Error: Disk space is low!"));
            }
            // Flush the chainstate (which may refer to block index entries).
            if (!pcoinsTip->Flush())
                return AbortNode(state, "Failed to write to coin database");
            if (!evoDb->CommitRootTransaction()) {
                return AbortNode(state, "Failed to commit EvoDB");
            }
            nLastFlush = nNow;
            // Update money supply on memory, reading data from disk
            if (!ShutdownRequested() && !IsInitialBlockDownload()) {
                MoneySupply.Update(pcoinsTip->GetTotalAmount(), chainActive.Height());
            }
        }
        if ((mode == FLUSH_STATE_ALWAYS || mode == FLUSH_STATE_PERIODIC) && nNow > nLastSetChain + (int64_t)DATABASE_WRITE_INTERVAL * 1000000) {
            // Update best block in wallet (so we can detect restored wallets).
            GetMainSignals().SetBestChain(chainActive.GetLocator());
            nLastSetChain = nNow;
        }

    } catch (const std::runtime_error& e) {
        return AbortNode(state, std::string("System error while flushing: ") + e.what());
    }
    return true;
}

void FlushStateToDisk()
{
    CValidationState state;
    FlushStateToDisk(state, FLUSH_STATE_ALWAYS);
}

/** Update chainActive and related internal data structures. */
void static UpdateTip(CBlockIndex* pindexNew)
{
    AssertLockHeld(cs_main);
    chainActive.SetTip(pindexNew);

    // New best block
    mempool.AddTransactionsUpdated(1);

    {
        LOCK(g_best_block_mutex);
        g_best_block = pindexNew->GetBlockHash();
        g_best_block_time = pindexNew->GetBlockTime();
        g_best_block_cv.notify_all();
    }

    const CBlockIndex* pChainTip = chainActive.Tip();
    LogPrintf("%s: new best=%s  height=%d version=%d  log2_work=%.16f  tx=%lu  date=%s progress=%f  cache=%.1fMiB(%utxo)  evodb_cache=%.1fMiB\n",
              __func__,
              pChainTip->GetBlockHash().GetHex(), pChainTip->nHeight, pChainTip->nVersion, log(pChainTip->nChainWork.getdouble()) / log(2.0), (unsigned long)pChainTip->nChainTx,
              FormatISO8601DateTime(pChainTip->GetBlockTime()),
              Checkpoints::GuessVerificationProgress(pChainTip), pcoinsTip->DynamicMemoryUsage() * (1.0 / (1<<20)), pcoinsTip->GetCacheSize(),
              evoDb->GetMemoryUsage() * (1.0 / (1<<20)));

    // Check the version of the last 100 blocks to see if we need to upgrade:
    static bool fWarned = false;
    if (!IsInitialBlockDownload() && !fWarned) {
        int nUpgraded = 0;
        const CBlockIndex* pindex = pChainTip;
        for (int i = 0; i < 100 && pindex != NULL; i++) {
            if (pindex->nVersion > CBlock::CURRENT_VERSION)
                ++nUpgraded;
            pindex = pindex->pprev;
        }
        if (nUpgraded > 0)
            LogPrintf("SetBestChain: %d of last 100 blocks above version %d\n", nUpgraded, (int)CBlock::CURRENT_VERSION);
        if (nUpgraded > 100 / 2) {
            std::string strWarning = _("Warning: This version is obsolete, upgrade required!");
            SetMiscWarning(strWarning);
            if (!fWarned) {
                AlertNotify(strWarning);
                fWarned = true;
            }
        }
    }
}

/** Disconnect chainActive's tip.
  * After calling, the mempool will be in an inconsistent state, with
  * transactions from disconnected blocks being added to disconnectpool.  You
  * should make the mempool consistent again by calling UpdateMempoolForReorg.
  * with cs_main held.
  *
  * If disconnectpool is NULL, then no disconnected transactions are added to
  * disconnectpool (note that the caller is responsible for mempool consistency
  * in any case).
  */
bool static DisconnectTip(CValidationState& state, const CChainParams& chainparams, DisconnectedBlockTransactions *disconnectpool)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(mempool.cs);
    CBlockIndex* pindexDelete = chainActive.Tip();
    assert(pindexDelete);
    // Read block from disk.
    std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>();
    CBlock& block = *pblock;
    if (!ReadBlockFromDisk(block, pindexDelete))
        return error("%s: Failed to read block", __func__);
    // Apply the block atomically to the chain state.
    const uint256& saplingAnchorBeforeDisconnect = pcoinsTip->GetBestAnchor();
    int64_t nStart = GetTimeMicros();
    {
        auto dbTx = evoDb->BeginTransaction();

        CCoinsViewCache view(pcoinsTip);
        assert(view.GetBestBlock() == pindexDelete->GetBlockHash());
        if (DisconnectBlock(block, pindexDelete, view) != DISCONNECT_OK)
            return error("DisconnectTip() : DisconnectBlock %s failed", pindexDelete->GetBlockHash().ToString());
        bool flushed = view.Flush();
        assert(flushed);
        dbTx->Commit();
    }
    LogPrint(BCLog::BENCH, "- Disconnect block: %.2fms\n", (GetTimeMicros() - nStart) * 0.001);
    const uint256& saplingAnchorAfterDisconnect = pcoinsTip->GetBestAnchor();
    // Write the chain state to disk, if necessary.
    if (!FlushStateToDisk(state, FLUSH_STATE_IF_NEEDED))
        return false;
    if (disconnectpool) {
        // Save transactions to re-add to mempool at end of reorg
        for (auto it = block.vtx.rbegin(); it != block.vtx.rend(); ++it) {
            disconnectpool->addTransaction(*it);
        }
        while (disconnectpool->DynamicMemoryUsage() > MAX_DISCONNECTED_TX_POOL_SIZE * 1000) {
            // Drop the earliest entry, and remove its children from the mempool.
            auto it = disconnectpool->queuedTx.get<insertion_order>().begin();
            mempool.removeRecursive(**it, MemPoolRemovalReason::REORG);
            disconnectpool->removeEntry(it);
        }
    }
    // Update MN manager cache
    // replace the cached hash of pindexDelete with the hash of the block
    // at depth CACHED_BLOCK_HASHES if it exists, or empty hash otherwise.
    if ((unsigned) pindexDelete->nHeight >= CACHED_BLOCK_HASHES) {
        mnodeman.CacheBlockHash(chainActive[pindexDelete->nHeight - CACHED_BLOCK_HASHES]);
    } else {
        mnodeman.UncacheBlockHash(pindexDelete);
    }
    // Evict from mempool if the anchor changes
    if (saplingAnchorBeforeDisconnect != saplingAnchorAfterDisconnect) {
        // The anchor may not change between block disconnects,
        // in which case we don't want to evict from the mempool yet!
        mempool.removeWithAnchor(saplingAnchorBeforeDisconnect);
    }
    // Update chainActive and related variables.
    UpdateTip(pindexDelete->pprev);
    // Let wallets know transactions went from 1-confirmed to
    // 0-confirmed or conflicted:
    GetMainSignals().BlockDisconnected(pblock, pindexDelete->GetBlockHash(), pindexDelete->nHeight, pindexDelete->GetBlockTime());

    return true;
}

static int64_t nTimeReadFromDisk = 0;
static int64_t nTimeConnectTotal = 0;
static int64_t nTimeFlush = 0;
static int64_t nTimeChainState = 0;
static int64_t nTimePostConnect = 0;

struct PerBlockConnectTrace {
    CBlockIndex* pindex = nullptr;
    std::shared_ptr<const CBlock> pblock;
    PerBlockConnectTrace() {}
};
/**
 * Used to track blocks whose transactions were applied to the UTXO state as a
 * part of a single ActivateBestChainStep call.
 *
 * This class is single-use, once you call GetBlocksConnected() you have to throw
 * it away and make a new one.
 */
class ConnectTrace {
private:
    std::vector<PerBlockConnectTrace> blocksConnected;

public:
    ConnectTrace() : blocksConnected(1) {}

    void BlockConnected(CBlockIndex* pindex, std::shared_ptr<const CBlock> pblock) {
        assert(!blocksConnected.back().pindex);
        assert(pindex);
        assert(pblock);
        blocksConnected.back().pindex = pindex;
        blocksConnected.back().pblock = std::move(pblock);
        blocksConnected.emplace_back();
    }

    std::vector<PerBlockConnectTrace>& GetBlocksConnected() {
        // We always keep one extra block at the end of our list because
        // blocks are added after all the conflicted transactions have
        // been filled in. Thus, the last entry should always be an empty
        // one waiting for the transactions from the next block. We pop
        // the last entry here to make sure the list we return is sane.
        assert(!blocksConnected.back().pindex);
        blocksConnected.pop_back();
        return blocksConnected;
    }
};

/**
 * Connect a new block to chainActive. pblock is either NULL or a pointer to a CBlock
 * corresponding to pindexNew, to bypass loading it again from disk.
 *
 * The block is added to connectTrace if connection succeeds.
 */
bool static ConnectTip(CValidationState& state, CBlockIndex* pindexNew, const std::shared_ptr<const CBlock>& pblock, ConnectTrace& connectTrace, DisconnectedBlockTransactions &disconnectpool)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(mempool.cs);
    assert(pindexNew->pprev == chainActive.Tip());

    // Read block from disk.
    int64_t nTime1 = GetTimeMicros();
    std::shared_ptr<const CBlock> pthisBlock;
    if (!pblock) {
        std::shared_ptr<CBlock> pblockNew = std::make_shared<CBlock>();
        if (!ReadBlockFromDisk(*pblockNew, pindexNew))
            return AbortNode(state, "Failed to read block");
        pthisBlock = pblockNew;
    } else {
        pthisBlock = pblock;
    }
    const CBlock& blockConnecting = *pthisBlock;

    // Apply the block atomically to the chain state.
    int64_t nTime2 = GetTimeMicros();
    nTimeReadFromDisk += nTime2 - nTime1;
    int64_t nTime3;
    LogPrint(BCLog::BENCH, "  - Load block from disk: %.2fms [%.2fs]\n", (nTime2 - nTime1) * 0.001, nTimeReadFromDisk * 0.000001);
    {
        auto dbTx = evoDb->BeginTransaction();

        CCoinsViewCache view(pcoinsTip);
        bool rv = ConnectBlock(blockConnecting, state, pindexNew, view, false);
        GetMainSignals().BlockChecked(blockConnecting, state);
        if (!rv) {
            if (state.IsInvalid())
                InvalidBlockFound(pindexNew, state);
            return error("%s: ConnectBlock %s failed, %s", __func__, pindexNew->GetBlockHash().ToString(), FormatStateMessage(state));
        }
        nTime3 = GetTimeMicros();
        nTimeConnectTotal += nTime3 - nTime2;
        LogPrint(BCLog::BENCH, "  - Connect total: %.2fms [%.2fs]\n", (nTime3 - nTime2) * 0.001, nTimeConnectTotal * 0.000001);
        bool flushed = view.Flush();
        assert(flushed);
        dbTx->Commit();
    }
    int64_t nTime4 = GetTimeMicros();
    nTimeFlush += nTime4 - nTime3;
    LogPrint(BCLog::BENCH, "  - Flush: %.2fms [%.2fs]\n", (nTime4 - nTime3) * 0.001, nTimeFlush * 0.000001);

    // Write the chain state to disk, if necessary. Always write to disk if this is the first of a new file.
    FlushStateMode flushMode = FLUSH_STATE_IF_NEEDED;
    if (pindexNew->pprev && (pindexNew->GetBlockPos().nFile != pindexNew->pprev->GetBlockPos().nFile))
        flushMode = FLUSH_STATE_ALWAYS;
    if (!FlushStateToDisk(state, flushMode))
        return false;
    int64_t nTime5 = GetTimeMicros();
    nTimeChainState += nTime5 - nTime4;
    LogPrint(BCLog::BENCH, "  - Writing chainstate: %.2fms [%.2fs]\n", (nTime5 - nTime4) * 0.001, nTimeChainState * 0.000001);

    // Remove conflicting transactions from the mempool.
    mempool.removeForBlock(blockConnecting.vtx, pindexNew->nHeight);
    disconnectpool.removeForBlock(blockConnecting.vtx);
    // Update chainActive & related variables.
    UpdateTip(pindexNew);
    // Update MN manager cache
    mnodeman.CacheBlockHash(pindexNew);
    mnodeman.CheckSpentCollaterals(blockConnecting.vtx);

    int64_t nTime6 = GetTimeMicros();
    nTimePostConnect += nTime6 - nTime5;
    nTimeTotal += nTime6 - nTime1;
    LogPrint(BCLog::BENCH, "  - Connect postprocess: %.2fms [%.2fs]\n", (nTime6 - nTime5) * 0.001, nTimePostConnect * 0.000001);
    LogPrint(BCLog::BENCH, "- Connect block: %.2fms [%.2fs]\n", (nTime6 - nTime1) * 0.001, nTimeTotal * 0.000001);

    connectTrace.BlockConnected(pindexNew, std::move(pthisBlock));
    return true;
}

/**
 * Return the tip of the chain with the most work in it, that isn't
 * known to be invalid (it's however far from certain to be valid).
 */
static CBlockIndex* FindMostWorkChain()
{
    do {
        CBlockIndex* pindexNew = NULL;

        // Find the best candidate header.
        {
            std::set<CBlockIndex*, CBlockIndexWorkComparator>::reverse_iterator it = setBlockIndexCandidates.rbegin();
            if (it == setBlockIndexCandidates.rend())
                return NULL;
            pindexNew = *it;
        }

        // Check whether all blocks on the path between the currently active chain and the candidate are valid.
        // Just going until the active chain is an optimization, as we know all blocks in it are valid already.
        CBlockIndex* pindexTest = pindexNew;
        bool fInvalidAncestor = false;
        while (pindexTest && !chainActive.Contains(pindexTest)) {
            assert(pindexTest->nChainTx || pindexTest->nHeight == 0);

            // Pruned nodes may have entries in setBlockIndexCandidates for
            // which block files have been deleted.  Remove those as candidates
            // for the most work chain if we come across them; we can't switch
            // to a chain unless we have all the non-active-chain parent blocks.
            bool fFailedChain = pindexTest->nStatus & BLOCK_FAILED_MASK;
            bool fMissingData = !(pindexTest->nStatus & BLOCK_HAVE_DATA);
            if (fFailedChain || fMissingData) {
                // Candidate chain is not usable (either invalid or missing data)
                if (fFailedChain && (pindexBestInvalid == NULL || pindexNew->nChainWork > pindexBestInvalid->nChainWork))
                    pindexBestInvalid = pindexNew;
                CBlockIndex* pindexFailed = pindexNew;
                // Remove the entire chain from the set.
                while (pindexTest != pindexFailed) {
                    if (fFailedChain) {
                        pindexFailed->nStatus |= BLOCK_FAILED_CHILD;
                    } else if (fMissingData) {
                        // If we're missing data, then add back to mapBlocksUnlinked,
                        // so that if the block arrives in the future we can try adding
                        // to setBlockIndexCandidates again.
                        mapBlocksUnlinked.emplace(pindexFailed->pprev, pindexFailed);
                    }
                    setBlockIndexCandidates.erase(pindexFailed);
                    pindexFailed = pindexFailed->pprev;
                }
                setBlockIndexCandidates.erase(pindexTest);
                fInvalidAncestor = true;
                break;
            }
            pindexTest = pindexTest->pprev;
        }
        if (!fInvalidAncestor)
            return pindexNew;
    } while (true);
}

/** Delete all entries in setBlockIndexCandidates that are worse than the current tip. */
static void PruneBlockIndexCandidates()
{
    // Note that we can't delete the current block itself, as we may need to return to it later in case a
    // reorganization to a better block fails.
    std::set<CBlockIndex*, CBlockIndexWorkComparator>::iterator it = setBlockIndexCandidates.begin();
    while (it != setBlockIndexCandidates.end() && setBlockIndexCandidates.value_comp()(*it, chainActive.Tip())) {
        setBlockIndexCandidates.erase(it++);
    }
    // Either the current tip or a successor of it we're working towards is left in setBlockIndexCandidates.
    assert(!setBlockIndexCandidates.empty());
}

/**
 * Try to make some progress towards making pindexMostWork the active block.
 * pblock is either NULL or a pointer to a CBlock corresponding to pindexMostWork.
 */
static bool ActivateBestChainStep(CValidationState& state, CBlockIndex* pindexMostWork, const std::shared_ptr<const CBlock>& pblock, bool& fInvalidFound, ConnectTrace& connectTrace)
{
    AssertLockHeld(cs_main);
    AssertLockHeld(mempool.cs);
    const CBlockIndex* pindexOldTip = chainActive.Tip();
    const CBlockIndex* pindexFork = chainActive.FindFork(pindexMostWork);

    // Disconnect active blocks which are no longer in the best chain.
    bool fBlocksDisconnected = false;
    DisconnectedBlockTransactions disconnectpool;
    while (chainActive.Tip() && chainActive.Tip() != pindexFork) {
        if (!DisconnectTip(state, Params(), &disconnectpool)) {
            // This is likely a fatal error, but keep the mempool consistent,
            // just in case. Only remove from the mempool in this case.
            UpdateMempoolForReorg(disconnectpool, false);

            // If we're unable to disconnect a block during normal operation,
            // then that is a failure of our local system -- we should abort
            // rather than stay on a less work chain.
            return AbortNode(state, "Failed to disconnect block; see debug.log for details");
        }
        fBlocksDisconnected = true;
    }

    // Build list of new blocks to connect.
    std::vector<CBlockIndex*> vpindexToConnect;
    bool fContinue = true;
    int nHeight = pindexFork ? pindexFork->nHeight : -1;
    while (fContinue && nHeight != pindexMostWork->nHeight) {
        // Don't iterate the entire list of potential improvements toward the best tip, as we likely only need
        // a few blocks along the way.
        int nTargetHeight = std::min(nHeight + 32, pindexMostWork->nHeight);
        vpindexToConnect.clear();
        vpindexToConnect.reserve(nTargetHeight - nHeight);
        CBlockIndex* pindexIter = pindexMostWork->GetAncestor(nTargetHeight);
        while (pindexIter && pindexIter->nHeight != nHeight) {
            vpindexToConnect.push_back(pindexIter);
            pindexIter = pindexIter->pprev;
        }
        nHeight = nTargetHeight;

        // Connect new blocks.
        for (CBlockIndex* pindexConnect : reverse_iterate(vpindexToConnect)) {
            if (!ConnectTip(state, pindexConnect, (pindexConnect == pindexMostWork) ? pblock : std::shared_ptr<const CBlock>(), connectTrace, disconnectpool)) {
                if (state.IsInvalid()) {
                    // The block violates a consensus rule.
                    if (!state.CorruptionPossible()) {
                        InvalidChainFound(vpindexToConnect.front());
                    }
                    state = CValidationState();
                    fInvalidFound = true;
                    fContinue = false;
                    break;
                } else {
                    // A system error occurred (disk space, database error, ...).
                    // Make the mempool consistent with the current tip, just in case
                    // any observers try to use it before shutdown.
                    UpdateMempoolForReorg(disconnectpool, false);
                    return false;
                }
            } else {
                PruneBlockIndexCandidates();
                if (!pindexOldTip || chainActive.Tip()->nChainWork > pindexOldTip->nChainWork) {
                    // We're in a better position than we were. Return temporarily to release the lock.
                    fContinue = false;
                    break;
                }
            }
        }
    }

    if (fBlocksDisconnected) {
        // If any blocks were disconnected, disconnectpool may be non empty.  Add
        // any disconnected transactions back to the mempool.
        UpdateMempoolForReorg(disconnectpool, true);
    }
    mempool.check(pcoinsTip);

    // Callbacks/notifications for a new best chain.
    if (fInvalidFound)
        CheckForkWarningConditionsOnNewFork(vpindexToConnect.back());
    else
        CheckForkWarningConditions();

    return true;
}

/**
 * Make the best chain active, in multiple steps. The result is either failure
 * or an activated best chain. pblock is either NULL or a pointer to a block
 * that is already loaded (to avoid loading it again from disk).
 */

bool ActivateBestChain(CValidationState& state, std::shared_ptr<const CBlock> pblock)
{
    // Note that while we're often called here from ProcessNewBlock, this is
    // far from a guarantee. Things in the P2P/RPC will often end up calling
    // us in the middle of ProcessNewBlock - do not assume pblock is set
    // sanely for performance or correctness!
    AssertLockNotHeld(cs_main);

    // ABC maintains a fair degree of expensive-to-calculate internal state
    // because this function periodically releases cs_main so that it does not lock up other threads for too long
    // during large connects - and to allow for e.g. the callback queue to drain
    // we use m_cs_chainstate to enforce mutual exclusion so that only one caller may execute this function at a time
    LOCK(m_cs_chainstate);

    CBlockIndex* pindexNewTip = nullptr;
    CBlockIndex* pindexMostWork = nullptr;
    do {
        boost::this_thread::interruption_point();

        if (GetMainSignals().CallbacksPending() > 10) {
            // Block until the validation queue drains. This should largely
            // never happen in normal operation, however may happen during
            // reindex, causing memory blowup  if we run too far ahead.
            SyncWithValidationInterfaceQueue();
        }

        {
            LOCK(cs_main);
            LOCK(mempool.cs); // Lock transaction pool for at least as long as it takes for connectTrace to be consumed
            CBlockIndex* starting_tip = chainActive.Tip();
            bool blocks_connected = false;
            do {
                // We absolutely may not unlock cs_main until we've made forward progress
                // (with the exception of shutdown due to hardware issues, low disk space, etc).
                ConnectTrace connectTrace; // Destructed before cs_main is unlocked

                if (pindexMostWork == nullptr) {
                    pindexMostWork = FindMostWorkChain();
                }

                // Whether we have anything to do at all.
                if (pindexMostWork == nullptr || pindexMostWork == chainActive.Tip()) {
                    break;
                }

                bool fInvalidFound = false;
                std::shared_ptr<const CBlock> nullBlockPtr;
                if (!ActivateBestChainStep(state, pindexMostWork, pblock && pblock->GetHash() == pindexMostWork->GetBlockHash() ? pblock : nullBlockPtr, fInvalidFound, connectTrace))
                    return false;
                blocks_connected = true;

                if (fInvalidFound) {
                    // Wipe cache, we may need another branch now.
                    pindexMostWork = nullptr;
                }
                pindexNewTip = chainActive.Tip();

                for (const PerBlockConnectTrace& trace : connectTrace.GetBlocksConnected()) {
                    assert(trace.pblock && trace.pindex);
                    GetMainSignals().BlockConnected(trace.pblock, trace.pindex);
                }
            } while (!chainActive.Tip() || (starting_tip && CBlockIndexWorkComparator()(chainActive.Tip(), starting_tip)));
            if (!blocks_connected) return true;

            const CBlockIndex* pindexFork = chainActive.FindFork(starting_tip);
            bool fInitialDownload = IsInitialBlockDownload();

            // Notify external listeners about the new tip.
            // Enqueue while holding cs_main to ensure that UpdatedBlockTip is called in the order in which blocks are connected
            if (pindexFork != pindexNewTip) {
                // Notify ValidationInterface subscribers
                GetMainSignals().UpdatedBlockTip(pindexNewTip, pindexFork, fInitialDownload);

                // Always notify the UI if a new block tip was connected
                uiInterface.NotifyBlockTip(fInitialDownload, pindexNewTip);
            }
        }

        // We check shutdown only after giving ActivateBestChainStep a chance to run once so that we
        // never shutdown before connecting the genesis block during LoadChainTip(). Previously this
        // caused an assert() failure during shutdown in such cases as the UTXO DB flushing checks
        // that the best block hash is non-null.
        if (ShutdownRequested())
            break;
    } while (pindexMostWork != chainActive.Tip());

    CheckBlockIndex();

    // Write changes periodically to disk, after relay.
    if (!FlushStateToDisk(state, FLUSH_STATE_PERIODIC)) {
        return false;
    }

    return true;
}

bool InvalidateBlock(CValidationState& state, const CChainParams& chainparams, CBlockIndex* pindex)
{
    AssertLockHeld(cs_main);
    // Mark the block itself as invalid.
    pindex->nStatus |= BLOCK_FAILED_VALID;
    setDirtyBlockIndex.insert(pindex);
    setBlockIndexCandidates.erase(pindex);

    LOCK(mempool.cs); // Lock for as long as disconnectpool is in scope to make sure UpdateMempoolForReorg is called after DisconnectTip without unlocking in between
    DisconnectedBlockTransactions disconnectpool;
    while (chainActive.Contains(pindex)) {
        CBlockIndex* pindexWalk = chainActive.Tip();
        pindexWalk->nStatus |= BLOCK_FAILED_CHILD;
        setDirtyBlockIndex.insert(pindexWalk);
        setBlockIndexCandidates.erase(pindexWalk);
        // ActivateBestChain considers blocks already in chainActive
        // unconditionally valid already, so force disconnect away from it.
        if (!DisconnectTip(state, chainparams, &disconnectpool)) {
            // It's probably hopeless to try to make the mempool consistent
            // here if DisconnectTip failed, but we can try.
            UpdateMempoolForReorg(disconnectpool, false);
            return false;
        }
    }

    // DisconnectTip will add transactions to disconnectpool; try to add these
    // back to the mempool.
    UpdateMempoolForReorg(disconnectpool, true);

    // The resulting new best tip may not be in setBlockIndexCandidates anymore, so
    // add it again.
    BlockMap::iterator it = mapBlockIndex.begin();
    while (it != mapBlockIndex.end()) {
        if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) && it->second->nChainTx && !setBlockIndexCandidates.value_comp()(it->second, chainActive.Tip())) {
            setBlockIndexCandidates.insert(it->second);
        }
        it++;
    }

    InvalidChainFound(pindex);
    return true;
}

bool ReconsiderBlock(CValidationState& state, CBlockIndex* pindex)
{
    AssertLockHeld(cs_main);

    int nHeight = pindex->nHeight;

    // Remove the invalidity flag from this block and all its descendants.
    BlockMap::iterator it = mapBlockIndex.begin();
    while (it != mapBlockIndex.end()) {
        if (!it->second->IsValid() && it->second->GetAncestor(nHeight) == pindex) {
            it->second->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(it->second);
            if (it->second->IsValid(BLOCK_VALID_TRANSACTIONS) && it->second->nChainTx && setBlockIndexCandidates.value_comp()(chainActive.Tip(), it->second)) {
                setBlockIndexCandidates.insert(it->second);
            }
            if (it->second == pindexBestInvalid) {
                // Reset invalid block marker if it was pointing to one of those.
                pindexBestInvalid = NULL;
            }
        }
        it++;
    }

    // Remove the invalidity flag from all ancestors too.
    while (pindex != NULL) {
        if (pindex->nStatus & BLOCK_FAILED_MASK) {
            pindex->nStatus &= ~BLOCK_FAILED_MASK;
            setDirtyBlockIndex.insert(pindex);
        }
        pindex = pindex->pprev;
    }
    return true;
}

CBlockIndex* AddToBlockIndex(const CBlock& block)
{
    // Check for duplicate
    uint256 hash = block.GetHash();
    BlockMap::iterator it = mapBlockIndex.find(hash);
    if (it != mapBlockIndex.end())
        return it->second;

    // Construct new block index object
    CBlockIndex* pindexNew = new CBlockIndex(block);
    assert(pindexNew);
    // We assign the sequence id to blocks only when the full data is available,
    // to avoid miners withholding blocks but broadcasting headers, to get a
    // competitive advantage.
    pindexNew->nSequenceId = 0;
    BlockMap::iterator mi = mapBlockIndex.emplace(hash, pindexNew).first;

    pindexNew->phashBlock = &((*mi).first);
    BlockMap::iterator miPrev = mapBlockIndex.find(block.hashPrevBlock);
    if (miPrev != mapBlockIndex.end()) {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
        pindexNew->BuildSkip();

        const Consensus::Params& consensus = Params().GetConsensus();
        if (!consensus.NetworkUpgradeActive(pindexNew->nHeight, Consensus::UPGRADE_V3_4)) {
            // compute and set new V1 stake modifier (entropy bits)
            pindexNew->SetNewStakeModifier();

        } else {
            // compute and set new V2 stake modifier (hash of prevout and prevModifier)
            pindexNew->SetNewStakeModifier(block.vtx[1]->vin[0].prevout.hash);
        }
    }
    pindexNew->nTimeMax = (pindexNew->pprev ? std::max(pindexNew->pprev->nTimeMax, pindexNew->nTime) : pindexNew->nTime);
    pindexNew->nChainWork = (pindexNew->pprev ? pindexNew->pprev->nChainWork : 0) + GetBlockProof(*pindexNew);
    pindexNew->RaiseValidity(BLOCK_VALID_TREE);
    if (pindexBestHeader == NULL || pindexBestHeader->nChainWork < pindexNew->nChainWork)
        pindexBestHeader = pindexNew;

    setDirtyBlockIndex.insert(pindexNew);

    return pindexNew;
}

/** Mark a block as having its data received and checked (up to BLOCK_VALID_TRANSACTIONS). */
bool ReceivedBlockTransactions(const CBlock& block, CValidationState& state, CBlockIndex* pindexNew, const FlatFilePos& pos)
{
    if (block.IsProofOfStake())
        pindexNew->SetProofOfStake();
    pindexNew->nTx = block.vtx.size();
    pindexNew->nChainTx = 0;

    // Sapling
    CAmount saplingValue = 0;
    for (const auto& tx : block.vtx) {
        if (tx->IsShieldedTx()) {
            // Negative valueBalance "takes" money from the transparent value pool
            // and adds it to the Sapling value pool. Positive valueBalance "gives"
            // money to the transparent value pool, removing from the Sapling value
            // pool. So we invert the sign here.
            saplingValue += -tx->sapData->valueBalance;
        }
    }
    pindexNew->nSaplingValue = saplingValue;
    pindexNew->nChainSaplingValue = nullopt;

    pindexNew->nFile = pos.nFile;
    pindexNew->nDataPos = pos.nPos;
    pindexNew->nUndoPos = 0;
    pindexNew->nStatus |= BLOCK_HAVE_DATA;
    pindexNew->RaiseValidity(BLOCK_VALID_TRANSACTIONS);
    setDirtyBlockIndex.insert(pindexNew);

    if (pindexNew->pprev == NULL || pindexNew->pprev->nChainTx) {
        // If pindexNew is the genesis block or all parents are BLOCK_VALID_TRANSACTIONS.
        std::deque<CBlockIndex*> queue;
        queue.push_back(pindexNew);

        // Recursively process any descendant blocks that now may be eligible to be connected.
        while (!queue.empty()) {
            CBlockIndex* pindex = queue.front();
            queue.pop_front();
            pindex->nChainTx = (pindex->pprev ? pindex->pprev->nChainTx : 0) + pindex->nTx;

            // Sapling, update chain value
            pindex->SetChainSaplingValue();

            {
                LOCK(cs_nBlockSequenceId);
                pindex->nSequenceId = nBlockSequenceId++;
            }
            if (chainActive.Tip() == NULL || !setBlockIndexCandidates.value_comp()(pindex, chainActive.Tip())) {
                setBlockIndexCandidates.insert(pindex);
            }
            std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> range = mapBlocksUnlinked.equal_range(pindex);
            while (range.first != range.second) {
                std::multimap<CBlockIndex*, CBlockIndex*>::iterator it = range.first;
                queue.push_back(it->second);
                range.first++;
                mapBlocksUnlinked.erase(it);
            }
        }
    } else {
        if (pindexNew->pprev && pindexNew->pprev->IsValid(BLOCK_VALID_TREE)) {
            mapBlocksUnlinked.emplace(pindexNew->pprev, pindexNew);
        }
    }

    return true;
}

bool FindBlockPos(CValidationState& state, FlatFilePos& pos, unsigned int nAddSize, unsigned int nHeight, uint64_t nTime, bool fKnown = false)
{
    LOCK(cs_LastBlockFile);

    unsigned int nFile = fKnown ? pos.nFile : nLastBlockFile;
    if (vinfoBlockFile.size() <= nFile) {
        vinfoBlockFile.resize(nFile + 1);
    }

    if (!fKnown) {
        while (vinfoBlockFile[nFile].nSize + nAddSize >= MAX_BLOCKFILE_SIZE) {
            nFile++;
            if (vinfoBlockFile.size() <= nFile) {
                vinfoBlockFile.resize(nFile + 1);
            }
        }
        pos.nFile = nFile;
        pos.nPos = vinfoBlockFile[nFile].nSize;
    }

    if ((int)nFile != nLastBlockFile) {
        if (!fKnown) {
            LogPrintf("Leaving block file %i: %s\n", nFile, vinfoBlockFile[nFile].ToString());
        }
        FlushBlockFile(!fKnown);
        nLastBlockFile = nFile;
    }

    vinfoBlockFile[nFile].AddBlock(nHeight, nTime);
    if (fKnown)
        vinfoBlockFile[nFile].nSize = std::max(pos.nPos + nAddSize, vinfoBlockFile[nFile].nSize);
    else
        vinfoBlockFile[nFile].nSize += nAddSize;

    if (!fKnown) {
        bool out_of_space;
        BlockFileSeq().Allocate(pos, nAddSize, out_of_space);
        if (out_of_space) {
            return AbortNode("Disk space is low!", _("Error: Disk space is low!"));
        }
        // future: add prunning flag check
    }

    setDirtyFileInfo.insert(nFile);
    return true;
}

bool FindUndoPos(CValidationState& state, int nFile, FlatFilePos& pos, unsigned int nAddSize)
{
    pos.nFile = nFile;

    LOCK(cs_LastBlockFile);

    pos.nPos = vinfoBlockFile[nFile].nUndoSize;
    vinfoBlockFile[nFile].nUndoSize += nAddSize;
    setDirtyFileInfo.insert(nFile);

    bool out_of_space;
    UndoFileSeq().Allocate(pos, nAddSize, out_of_space);
    if (out_of_space) {
        return AbortNode(state, "Disk space is low!", _("Error: Disk space is low!"));
    }
    // future: add prunning flag check

    return true;
}

bool CheckColdStakeFreeOutput(const CTransaction& tx, const int nHeight)
{
    assert(tx.IsCoinStake());
    // This check applies only to coinstakes spending a P2CS_LOF script.
    // The script-check ensures that all but the first and the last output
    // (if the coinstake has more than 3 outputs) have the same scriptPubKey.
    // If the second script is not a P2CS_LOF, then either this is a "regular"
    // P2PKH stake, or it fails the script verification.
    if (!tx.vout[1].scriptPubKey.IsPayToColdStakingLOF()) {
        return true;
    }
    // If the last output is different, then it can be either a masternode
    // or a budget proposal payment
    const unsigned int outs = tx.vout.size();
    const CTxOut& lastOut = tx.vout[outs-1];
    if (outs >=3 && lastOut.scriptPubKey != tx.vout[outs-2].scriptPubKey) {
        if (Params().GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_V6_0)) {
            // after v6.0, masternode and budgets are paid in the coinbase. No more free outputs allowed.
            return false;
        }
        if (lastOut.nValue == GetMasternodePayment())
            return true;

        // if mnsync is incomplete, we cannot verify if this is a budget block.
        // so we check that the staker is not transferring value to the free output
        if (!masternodeSync.IsSynced()) {
            // First try finding the previous transaction in database
            CTransactionRef txPrev; uint256 hashBlock;
            if (!GetTransaction(tx.vin[0].prevout.hash, txPrev, hashBlock, true))
                return error("%s : read txPrev failed: %s",  __func__, tx.vin[0].prevout.hash.GetHex());
            CAmount amtIn = txPrev->vout[tx.vin[0].prevout.n].nValue + GetBlockValue(nHeight);
            CAmount amtOut = 0;
            for (unsigned int i = 1; i < outs-1; i++) amtOut += tx.vout[i].nValue;
            if (amtOut != amtIn)
                return error("%s: non-free outputs value %d less than required %d", __func__, amtOut, amtIn);
            return true;
        }

        // Check that this is indeed a superblock.
        if (g_budgetman.IsBudgetPaymentBlock(nHeight)) {
            // if superblocks are not enabled, reject
            if (!sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS))
                return error("%s: superblocks are not enabled", __func__);
            return true;
        }

        // wrong free output
        return error("%s: Wrong cold staking outputs: vout[%d].scriptPubKey (%s) != vout[%d].scriptPubKey (%s) - value: %s",
                __func__, outs-1, HexStr(lastOut.scriptPubKey), outs-2, HexStr(tx.vout[outs-2].scriptPubKey), FormatMoney(lastOut.nValue).c_str());
    }

    return true;
}

// cumulative size of all shielded txes inside a block
static unsigned int GetTotalShieldedTxSize(const CBlock& block)
{
    unsigned int nSizeShielded = 0;
    for (const auto& tx : block.vtx) {
        if (tx->IsShieldedTx()) nSizeShielded += tx->GetTotalSize();
    }
    return nSizeShielded;
}

bool CheckBlock(const CBlock& block, CValidationState& state, bool fCheckPOW, bool fCheckMerkleRoot, bool fCheckSig)
{
    if (block.fChecked)
        return true;

    // These are checks that are independent of context.
    const bool IsPoS = block.IsProofOfStake();

    // Check that the header is valid (particularly PoW).  This is mostly
    // redundant with the call in AcceptBlockHeader.
    if (!IsPoS && fCheckPOW && !CheckProofOfWork(block.GetHash(), block.nBits))
        return state.DoS(50, false, REJECT_INVALID, "high-hash", false, "proof of work failed");

    // All potential-corruption validation must be done before we do any
    // transaction validation, as otherwise we may mark the header as invalid
    // because we receive the wrong transactions for it.

    // Check the merkle root.
    if (fCheckMerkleRoot) {
        bool mutated;
        uint256 hashMerkleRoot2 = BlockMerkleRoot(block, &mutated);
        if (block.hashMerkleRoot != hashMerkleRoot2)
            return state.DoS(100, false, REJECT_INVALID, "bad-txnmrklroot", true, "hashMerkleRoot mismatch");

        // Check for merkle tree malleability (CVE-2012-2459): repeating sequences
        // of transactions in a block without affecting the merkle root of a block,
        // while still invalidating it.
        if (mutated)
            return state.DoS(100, false, REJECT_INVALID, "bad-txns-duplicate", true, "duplicate transaction");
    }

    // Size limits
    unsigned int nMaxBlockSize = MAX_BLOCK_SIZE_CURRENT;
    const unsigned int nBlockSize = ::GetSerializeSize(block, PROTOCOL_VERSION);
    if (block.vtx.empty() || block.vtx.size() > nMaxBlockSize || nBlockSize > nMaxBlockSize)
        return state.DoS(100, false, REJECT_INVALID, "bad-blk-length", false, "size limits failed");

    // Check shielded txes limits (no need to check if the block size is already under 750kB)
    if (nBlockSize > MAX_BLOCK_SHIELDED_TXES_SIZE && GetTotalShieldedTxSize(block) > MAX_BLOCK_SHIELDED_TXES_SIZE)
        return state.DoS(100, false, REJECT_INVALID, "bad-blk-shielded-size", false, "shielded size limits failed");

    // First transaction must be coinbase, the rest must not be
    if (block.vtx.empty() || !block.vtx[0]->IsCoinBase())
        return state.DoS(100, false, REJECT_INVALID, "bad-cb-missing", false, "first tx is not coinbase");
    for (unsigned int i = 1; i < block.vtx.size(); i++)
        if (block.vtx[i]->IsCoinBase())
            return state.DoS(100, false, REJECT_INVALID, "bad-cb-multiple", false, "more than one coinbase");

    if (IsPoS) {
        // Second transaction must be coinstake, the rest must not be
        if (block.vtx.empty() || !block.vtx[1]->IsCoinStake())
            return state.DoS(100, false, REJECT_INVALID, "bad-cs-missing", false, "second tx is not coinstake");
        for (unsigned int i = 2; i < block.vtx.size(); i++)
            if (block.vtx[i]->IsCoinStake())
                return state.DoS(100, false, REJECT_INVALID, "bad-cs-multiple", false, "more than one coinstake");
    }

    // Cold Staking enforcement (true during sync - reject P2CS outputs when false)
    bool fColdStakingActive = true;

    // Zerocoin activation
    bool fZerocoinActive = block.GetBlockTime() > Params().GetConsensus().ZC_TimeStart;

    // masternode payments / budgets
    CBlockIndex* pindexPrev = chainActive.Tip();
    int nHeight = 0;
    if (pindexPrev != nullptr && block.hashPrevBlock != UINT256_ZERO) {
        if (pindexPrev->GetBlockHash() != block.hashPrevBlock) {
            //out of order
            auto mi = mapBlockIndex.find(block.hashPrevBlock);
            if (mi == mapBlockIndex.end()) {
                return state.Error("blk-out-of-order");
            }
            pindexPrev = mi->second;
        }
        nHeight = pindexPrev->nHeight + 1;

        // PIVX
        // It is entierly possible that we don't have enough data and this could fail
        // (i.e. the block could indeed be valid). Store the block for later consideration
        // but issue an initial reject message.
        // The case also exists that the sending peer could not have enough data to see
        // that this block is invalid, so don't issue an outright ban.
        if (nHeight != 0 && !IsInitialBlockDownload()) {
            // Last output of Cold-Stake is not abused
            if (IsPoS && !CheckColdStakeFreeOutput(*(block.vtx[1]), nHeight)) {
                mapRejectedBlocks.emplace(block.GetHash(), GetTime());
                return state.DoS(0, false, REJECT_INVALID, "bad-p2cs-outs", false, "invalid cold-stake output");
            }

            // set Cold Staking Spork
            fColdStakingActive = !sporkManager.IsSporkActive(SPORK_19_COLDSTAKING_MAINTENANCE);

        } else {
            LogPrintf("%s: Masternode/Budget payment checks skipped on sync\n", __func__);
        }
    }

    // Check transactions
    bool fSaplingActive = Params().GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_V5_0);
    std::vector<CBigNum> vBlockSerials;
    for (const auto& txIn : block.vtx) {
        const CTransaction& tx = *txIn;
        if (!CheckTransaction(tx, state, fColdStakingActive)) {
            return state.Invalid(false, state.GetRejectCode(), state.GetRejectReason(),
                    strprintf("Transaction check failed (tx hash %s) %s", tx.GetHash().ToString(), state.GetDebugMessage()));
        }

        // Non-contextual checks for special txes
        if (!CheckSpecialTxNoContext(tx, state)) {
            // pass the state returned by the function above
            return false;
        }

        // No need to check for zerocoin anymore after sapling, they are networkely disabled
        // and checkpoints are preventing the chain for any massive reorganization.
        if (fSaplingActive && tx.ContainsZerocoins()) {
            return state.DoS(100, error("%s : v5 upgrade enforced, zerocoin disabled", __func__),
                             REJECT_INVALID, "bad-blk-with-zc");
        }
    }

    unsigned int nSigOps = 0;
    for (const auto& tx : block.vtx) {
        nSigOps += GetLegacySigOpCount(*tx);
    }
    unsigned int nMaxBlockSigOps = fZerocoinActive ? MAX_BLOCK_SIGOPS_CURRENT : MAX_BLOCK_SIGOPS_LEGACY;
    if (nSigOps > nMaxBlockSigOps)
        return state.DoS(100, error("%s : out-of-bounds SigOpCount", __func__),
            REJECT_INVALID, "bad-blk-sigops", true);

    // Check PoS signature.
    if (fCheckSig && !CheckBlockSignature(block)) {
        return state.DoS(100, error("%s : bad proof-of-stake block signature", __func__),
                         REJECT_INVALID, "bad-PoS-sig", true);
    }

    if (fCheckPOW && fCheckMerkleRoot && fCheckSig)
        block.fChecked = true;

    return true;
}

bool CheckWork(const CBlock& block, const CBlockIndex* const pindexPrev)
{
    if (pindexPrev == NULL)
        return error("%s : null pindexPrev for block %s", __func__, block.GetHash().GetHex());

    unsigned int nBitsRequired = GetNextWorkRequired(pindexPrev, &block);

    if (!Params().IsRegTestNet() && block.IsProofOfWork() && (pindexPrev->nHeight + 1 <= 68589)) {
        double n1 = ConvertBitsToDouble(block.nBits);
        double n2 = ConvertBitsToDouble(nBitsRequired);

        if (std::abs(n1 - n2) > n1 * 0.5)
            return error("%s : incorrect proof of work (DGW pre-fork) - %f %f %f at %d", __func__, std::abs(n1 - n2), n1, n2, pindexPrev->nHeight + 1);

        return true;
    }

    if (block.nBits != nBitsRequired) {
        // Pivx Specific reference to the block with the wrong threshold was used.
        const Consensus::Params& consensus = Params().GetConsensus();
        if ((block.nTime == (uint32_t) consensus.nPivxBadBlockTime) &&
                (block.nBits == (uint32_t) consensus.nPivxBadBlockBits)) {
            // accept PIVX block minted with incorrect proof of work threshold
            return true;
        }

        return error("%s : incorrect proof of work at %d", __func__, pindexPrev->nHeight + 1);
    }

    return true;
}

bool CheckBlockTime(const CBlockHeader& block, CValidationState& state, CBlockIndex* const pindexPrev)
{
    // Not enforced on RegTest
    if (Params().IsRegTestNet())
        return true;

    const int64_t blockTime = block.GetBlockTime();
    const int blockHeight = pindexPrev->nHeight + 1;

    // Check blocktime against future drift (WANT: blk_time <= Now + MaxDrift)
    if (blockTime > pindexPrev->MaxFutureBlockTime())
        return state.Invalid(error("%s : block timestamp too far in the future", __func__), REJECT_INVALID, "time-too-new");

    // Check blocktime against prev (WANT: blk_time > MinPastBlockTime)
    if (blockTime <= pindexPrev->MinPastBlockTime())
        return state.DoS(50, error("%s : block timestamp too old", __func__), REJECT_INVALID, "time-too-old");

    // Check blocktime mask
    if (!Params().GetConsensus().IsValidBlockTimeStamp(blockTime, blockHeight))
        return state.DoS(100, error("%s : block timestamp mask not valid", __func__), REJECT_INVALID, "invalid-time-mask");

    // All good
    return true;
}

bool ContextualCheckBlockHeader(const CBlockHeader& block, CValidationState& state, CBlockIndex* const pindexPrev)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    uint256 hash = block.GetHash();

    if (hash == consensus.hashGenesisBlock)
        return true;

    assert(pindexPrev);

    const int nHeight = pindexPrev->nHeight + 1;
    const int chainHeight = chainActive.Height();

    //If this is a reorg, check that it is not too deep
    int nMaxReorgDepth = gArgs.GetArg("-maxreorg", DEFAULT_MAX_REORG_DEPTH);
    if (chainHeight - nHeight >= nMaxReorgDepth)
        return state.DoS(1, error("%s: forked chain older than max reorganization depth (height %d)", __func__, chainHeight - nHeight));

    // Check blocktime (past limit, future limit and mask)
    if (!CheckBlockTime(block, state, pindexPrev))
        return false;

    // Check that the block chain matches the known block chain up to a checkpoint
    if (!Checkpoints::CheckBlock(nHeight, hash))
        return state.DoS(100, error("%s : rejected by checkpoint lock-in at %d", __func__, nHeight),
            REJECT_CHECKPOINT, "checkpoint mismatch");

    // Don't accept any forks from the main chain prior to last checkpoint
    CBlockIndex* pcheckpoint = Checkpoints::GetLastCheckpoint();
    if (pcheckpoint && nHeight < pcheckpoint->nHeight)
        return state.DoS(0, error("%s : forked chain older than last checkpoint (height %d)", __func__, nHeight));

    // Reject outdated version blocks
    if ((block.nVersion < 3 && nHeight >= 1) ||
        (block.nVersion < 4 && consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_ZC)) ||
        (block.nVersion < 5 && consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_BIP65)) ||
        (block.nVersion < 6 && consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_V3_4)) ||
        (block.nVersion < 7 && consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_V4_0)) ||
        (block.nVersion < 8 && consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_V5_0)))
    {
        std::string stringErr = strprintf("rejected block version %d at height %d", block.nVersion, nHeight);
        return state.Invalid(false, REJECT_OBSOLETE, "bad-version", stringErr);
    }

    return true;
}

bool ContextualCheckBlock(const CBlock& block, CValidationState& state, CBlockIndex* const pindexPrev)
{
    const int nHeight = pindexPrev == nullptr ? 0 : pindexPrev->nHeight + 1;
    const CChainParams& chainparams = Params();

    // Check that all transactions are finalized
    for (const auto& tx : block.vtx) {

        // Check transaction contextually against consensus rules at block height
        if (!ContextualCheckTransaction(tx, state, chainparams, nHeight, true /* isMined */, IsInitialBlockDownload())) {
            return false;
        }

        if (!IsFinalTx(tx, nHeight, block.GetBlockTime())) {
            return state.DoS(10, false, REJECT_INVALID, "bad-txns-nonfinal", false, "non-final transaction");
        }
    }

    // Enforce block.nVersion=2 rule that the coinbase starts with serialized block height
    if (pindexPrev) { // pindexPrev is only null on the first block which is a version 1 block.
        CScript expect = CScript() << nHeight;
        if (block.vtx[0]->vin[0].scriptSig.size() < expect.size() ||
            !std::equal(expect.begin(), expect.end(), block.vtx[0]->vin[0].scriptSig.begin())) {
            return state.DoS(100, false, REJECT_INVALID, "bad-cb-height", false, "block height mismatch in coinbase");
        }
    }

    return true;
}

// Get the index of previous block of given CBlock
bool GetPrevIndex(const CBlock& block, CBlockIndex** pindexPrevRet, CValidationState& state)
{
    CBlockIndex*& pindexPrev = *pindexPrevRet;
    pindexPrev = nullptr;
    if (block.GetHash() != Params().GetConsensus().hashGenesisBlock) {
        BlockMap::iterator mi = mapBlockIndex.find(block.hashPrevBlock);
        if (mi == mapBlockIndex.end()) {
            return state.DoS(0, error("%s : prev block %s not found", __func__, block.hashPrevBlock.GetHex()), 0,
                             "prevblk-not-found");
        }
        pindexPrev = (*mi).second;
        if (pindexPrev->nStatus & BLOCK_FAILED_MASK) {
            //If this "invalid" block is an exact match from the checkpoints, then reconsider it
            if (Checkpoints::CheckBlock(pindexPrev->nHeight, block.hashPrevBlock, true)) {
                LogPrintf("%s : Reconsidering block %s height %d\n", __func__, block.hashPrevBlock.ToString(), pindexPrev->nHeight);
                CValidationState statePrev;
                ReconsiderBlock(statePrev, pindexPrev);
                if (statePrev.IsValid()) {
                    ActivateBestChain(statePrev);
                    return true;
                }
            }
            return state.DoS(100, error("%s : prev block %s is invalid, unable to add block %s", __func__, block.hashPrevBlock.GetHex(), block.GetHash().GetHex()),
                             REJECT_INVALID, "bad-prevblk");
        }
    }
    return true;
}

bool AcceptBlockHeader(const CBlock& block, CValidationState& state, CBlockIndex** ppindex, CBlockIndex* pindexPrev)
{
    AssertLockHeld(cs_main);
    // Check for duplicate
    const uint256& hash = block.GetHash();
    BlockMap::iterator miSelf = mapBlockIndex.find(hash);
    CBlockIndex* pindex = NULL;

    // TODO : ENABLE BLOCK CACHE IN SPECIFIC CASES
    if (miSelf != mapBlockIndex.end()) {
        // Block header is already known.
        pindex = miSelf->second;
        if (ppindex)
            *ppindex = pindex;
        if (pindex->nStatus & BLOCK_FAILED_MASK)
            return state.Invalid(error("%s : block is marked invalid", __func__), 0, "duplicate");
        return true;
    }

    // Get prev block index
    if (pindexPrev == nullptr && !GetPrevIndex(block, &pindexPrev, state)) {
        return false;
    }

    if (!ContextualCheckBlockHeader(block, state, pindexPrev))
        return error("%s: ContextualCheckBlockHeader failed for block %s: %s", __func__, hash.ToString(), FormatStateMessage(state));

    if (pindex == nullptr)
        pindex = AddToBlockIndex(block);

    if (ppindex)
        *ppindex = pindex;

    CheckBlockIndex();

    return true;
}

static bool AcceptBlock(const CBlock& block, CValidationState& state, CBlockIndex** ppindex, const FlatFilePos* dbp)
{
    AssertLockHeld(cs_main);

    CBlockIndex* pindexDummy = nullptr;
    CBlockIndex*& pindex = ppindex ? *ppindex : pindexDummy;

    const Consensus::Params& consensus = Params().GetConsensus();

    // Get prev block index
    CBlockIndex* pindexPrev = nullptr;
    if (!GetPrevIndex(block, &pindexPrev, state))
        return false;

    if (block.GetHash() != consensus.hashGenesisBlock && !CheckWork(block, pindexPrev))
        return state.DoS(100, false, REJECT_INVALID);

    bool isPoS = block.IsProofOfStake();
    if (isPoS) {
        std::string strError;
        if (!CheckProofOfStake(block, strError, pindexPrev))
            return state.DoS(100, error("%s: proof of stake check failed (%s)", __func__, strError));
    }

    if (!AcceptBlockHeader(block, state, &pindex, pindexPrev))
        return false;

    if (pindex->nStatus & BLOCK_HAVE_DATA) {
        // TODO: deal better with duplicate blocks.
        // return state.DoS(20, error("AcceptBlock() : already have block %d %s", pindex->nHeight, pindex->GetBlockHash().ToString()), REJECT_DUPLICATE, "duplicate");
        LogPrintf("AcceptBlock() : already have block %d %s", pindex->nHeight, pindex->GetBlockHash().ToString());
        return true;
    }

    if (!CheckBlock(block, state) || !ContextualCheckBlock(block, state, pindex->pprev)) {
        if (state.IsInvalid() && !state.CorruptionPossible()) {
            pindex->nStatus |= BLOCK_FAILED_VALID;
            setDirtyBlockIndex.insert(pindex);
        }
        return error("%s: %s", __func__, FormatStateMessage(state));
    }

    int nHeight = pindex->nHeight;
    int splitHeight = -1;

    if (isPoS) {
        LOCK(cs_main);

        // Blocks arrives in order, so if prev block is not the tip then we are on a fork.
        // Extra info: duplicated blocks are skipping this checks, so we don't have to worry about those here.
        bool isBlockFromFork = pindexPrev != nullptr && chainActive.Tip() != pindexPrev;

        // Coin stake
        const CTransaction &stakeTxIn = *block.vtx[1];

        // Inputs
        std::vector<CTxIn> pivInputs;
        std::vector<CTxIn> zPIVInputs;

        for (const CTxIn& stakeIn : stakeTxIn.vin) {
            if(stakeIn.IsZerocoinSpend()){
                zPIVInputs.push_back(stakeIn);
            }else{
                pivInputs.push_back(stakeIn);
            }
        }
        const bool hasPIVInputs = !pivInputs.empty();
        const bool hasZPIVInputs = !zPIVInputs.empty();

        // ZC started after PoS.
        // Check for serial double spent on the same block, TODO: Move this to the proper method..

        std::vector<CBigNum> inBlockSerials;
        for (const auto& txIn : block.vtx) {
            const CTransaction& tx = *txIn;
            for (const CTxIn& in: tx.vin) {
                if(consensus.NetworkUpgradeActive(nHeight, Consensus::UPGRADE_ZC)) {
                    bool isPublicSpend = in.IsZerocoinPublicSpend();
                    bool isPrivZerocoinSpend = in.IsZerocoinSpend();
                    if (isPrivZerocoinSpend || isPublicSpend) {

                        // Check enforcement
                        if (!CheckPublicCoinSpendEnforced(pindex->nHeight, isPublicSpend)){
                            return false;
                        }

                        libzerocoin::CoinSpend spend;
                        if (isPublicSpend) {
                            libzerocoin::ZerocoinParams* params = consensus.Zerocoin_Params(false);
                            PublicCoinSpend publicSpend(params);
                            if (!ZPIVModule::ParseZerocoinPublicSpend(in, tx, state, publicSpend)){
                                return false;
                            }
                            spend = publicSpend;
                        } else {
                            spend = TxInToZerocoinSpend(in);
                        }
                        // Check for serials double spending in the same block
                        if (std::find(inBlockSerials.begin(), inBlockSerials.end(), spend.getCoinSerialNumber()) !=
                            inBlockSerials.end()) {
                            return state.DoS(100, error("%s: serial double spent on the same block", __func__));
                        }
                        inBlockSerials.push_back(spend.getCoinSerialNumber());
                    }
                }
                if(tx.IsCoinStake()) continue;
                if(hasPIVInputs) {
                    // Check if coinstake input is double spent inside the same block
                    for (const CTxIn& pivIn : pivInputs)
                        if(pivIn.prevout == in.prevout)
                            // double spent coinstake input inside block
                            return error("%s: double spent coinstake input inside block", __func__);
                }

            }
        }
        inBlockSerials.clear();


        // Check whether is a fork or not
        if (isBlockFromFork) {

            // Start at the block we're adding on to
            CBlockIndex *prev = pindexPrev;

            CBlock bl;
            if (!ReadBlockFromDisk(bl, prev))
                return error("%s: previous block %s not on disk", __func__, prev->GetBlockHash().GetHex());

            std::vector<CBigNum> vBlockSerials;
            int readBlock = 0;
            // Go backwards on the forked chain up to the split
            while (!chainActive.Contains(prev)) {

                // Increase amount of read blocks
                readBlock++;
                // Check if the forked chain is longer than the max reorg limit
                if (readBlock == gArgs.GetArg("-maxreorg", DEFAULT_MAX_REORG_DEPTH)) {
                    // TODO: Remove this chain from disk.
                    return error("%s: forked chain longer than maximum reorg limit", __func__);
                }

                // Loop through every tx of this block
                for (const auto& txIn : bl.vtx) {
                    const CTransaction& t = *txIn;
                    // Loop through every input of this tx
                    for (const CTxIn& in: t.vin) {
                        // If this input is a zerocoin spend, and the coinstake has zerocoin inputs
                        // then store the serials for later check
                        if(hasZPIVInputs && in.IsZerocoinSpend())
                            vBlockSerials.push_back(TxInToZerocoinSpend(in).getCoinSerialNumber());

                        // Loop through every input of the staking tx
                        if (hasPIVInputs) {
                            for (const CTxIn& stakeIn : pivInputs)
                                // check if the tx input is double spending any coinstake input
                                if (stakeIn.prevout == in.prevout)
                                    return state.DoS(100, error("%s: input already spent on a previous block", __func__));
                        }
                    }
                }

                // Prev block
                prev = prev->pprev;
                if (!ReadBlockFromDisk(bl, prev))
                    // Previous block not on disk
                    return error("%s: previous block %s not on disk", __func__, prev->GetBlockHash().GetHex());

            }

            // Split height
            splitHeight = prev->nHeight;

            // Now that this loop if completed. Check if we have zPIV inputs.
            if(hasZPIVInputs) {
                for (const CTxIn& zPivInput : zPIVInputs) {
                    libzerocoin::CoinSpend spend = TxInToZerocoinSpend(zPivInput);

                    // First check if the serials were not already spent on the forked blocks.
                    CBigNum coinSerial = spend.getCoinSerialNumber();
                    for(const CBigNum& serial : vBlockSerials){
                        if(serial == coinSerial){
                            return state.DoS(100, error("%s: serial double spent on fork", __func__));
                        }
                    }

                    // Now check if the serial exists before the chain split.
                    int nHeightTx = 0;
                    if (IsSerialInBlockchain(spend.getCoinSerialNumber(), nHeightTx)) {
                        // if the height is nHeightTx > chainSplit means that the spent occurred after the chain split
                        if(nHeightTx <= splitHeight)
                            return state.DoS(100, error("%s: serial double spent on main chain", __func__));
                    }

                    if (!ContextualCheckZerocoinSpendNoSerialCheck(stakeTxIn, &spend, pindex->nHeight))
                        return state.DoS(100,error("%s: forked chain ContextualCheckZerocoinSpend failed for tx %s", __func__,
                                                   stakeTxIn.GetHash().GetHex()), REJECT_INVALID, "bad-txns-invalid-zpiv");

                }
            }

        }


        // If the stake is not a zPoS then let's check if the inputs were spent on the main chain
        const CCoinsViewCache coins(pcoinsTip);
        if(!stakeTxIn.HasZerocoinSpendInputs()) {
            for (const CTxIn& in: stakeTxIn.vin) {
                const Coin& coin = coins.AccessCoin(in.prevout);
                if(coin.IsSpent() && !isBlockFromFork){
                    // No coins on the main chain
                    return error("%s: coin stake inputs not-available/already-spent on main chain, received height %d vs current %d", __func__, nHeight, chainActive.Height());
                }
            }
        } else {
            if(!isBlockFromFork)
                for (const CTxIn& zPivInput : zPIVInputs) {
                        libzerocoin::CoinSpend spend = TxInToZerocoinSpend(zPivInput);
                        if (!ContextualCheckZerocoinSpend(stakeTxIn, &spend, pindex->nHeight))
                            return state.DoS(100,error("%s: main chain ContextualCheckZerocoinSpend failed for tx %s", __func__,
                                    stakeTxIn.GetHash().GetHex()), REJECT_INVALID, "bad-txns-invalid-zpiv");
                }

        }

    }

    // Write block to history file
    try {
        unsigned int nBlockSize = ::GetSerializeSize(block, CLIENT_VERSION);
        FlatFilePos blockPos;
        if (dbp != nullptr)
            blockPos = *dbp;
        if (!FindBlockPos(state, blockPos, nBlockSize + 8, nHeight, block.GetBlockTime(), dbp != nullptr))
            return error("%s : FindBlockPos failed", __func__);
        if (dbp == nullptr)
            if (!WriteBlockToDisk(block, blockPos))
                return AbortNode(state, "Failed to write block");
        if (!ReceivedBlockTransactions(block, state, pindex, blockPos))
            return error("%s : ReceivedBlockTransactions failed", __func__);
    } catch (const std::runtime_error& e) {
        return AbortNode(state, std::string("System error: ") + e.what());
    }

    return true;
}

bool ProcessNewBlock(const std::shared_ptr<const CBlock>& pblock, const FlatFilePos* dbp)
{
    AssertLockNotHeld(cs_main);

    // Preliminary checks
    int64_t nStartTime = GetTimeMillis();
    int newHeight = 0;

    {
        // CheckBlock requires cs_main lock
        LOCK(cs_main);
        CValidationState state;
        if (!CheckBlock(*pblock, state)) {
            GetMainSignals().BlockChecked(*pblock, state);
            return error ("%s : CheckBlock FAILED for block %s, %s", __func__, pblock->GetHash().GetHex(), FormatStateMessage(state));
        }

        // Store to disk
        CBlockIndex* pindex = nullptr;
        bool ret = AcceptBlock(*pblock, state, &pindex, dbp);
        CheckBlockIndex();
        if (!ret) {
            GetMainSignals().BlockChecked(*pblock, state);
            return error("%s : AcceptBlock FAILED", __func__);
        }
        newHeight = pindex->nHeight;
    }

    CValidationState state; // Only used to report errors, not invalidity - ignore it
    if (!ActivateBestChain(state, pblock))
        return error("%s : ActivateBestChain failed", __func__);

    if (!fLiteMode) {
        mnodeman.SetBestHeight(newHeight);
        g_budgetman.NewBlock(newHeight);
        if (masternodeSync.RequestedMasternodeAssets > MASTERNODE_SYNC_LIST) {
            masternodePayments.ProcessBlock(newHeight + 10);
        }
    }

    LogPrintf("%s : ACCEPTED Block %ld in %ld milliseconds with size=%d\n", __func__, newHeight, GetTimeMillis() - nStartTime,
              GetSerializeSize(*pblock, CLIENT_VERSION));

    return true;
}

bool TestBlockValidity(CValidationState& state, const CBlock& block, CBlockIndex* const pindexPrev, bool fCheckPOW, bool fCheckMerkleRoot, bool fCheckBlockSig)
{
    AssertLockHeld(cs_main);
    assert(pindexPrev);
    if (pindexPrev != chainActive.Tip()) {
        LogPrintf("%s : No longer working on chain tip\n", __func__);
        return false;
    }

    CCoinsViewCache viewNew(pcoinsTip);
    CBlockIndex indexDummy(block);
    indexDummy.pprev = pindexPrev;
    indexDummy.nHeight = pindexPrev->nHeight + 1;

    // begin tx and let it rollback
    auto dbTx = evoDb->BeginTransaction();

    // NOTE: CheckBlockHeader is called by CheckBlock
    if (!ContextualCheckBlockHeader(block, state, pindexPrev))
        return error("%s: ContextualCheckBlockHeader failed: %s", __func__, FormatStateMessage(state));
    if (!CheckBlock(block, state, fCheckPOW, fCheckMerkleRoot, fCheckBlockSig))
        return error("%s: CheckBlock failed: %s", __func__, FormatStateMessage(state));
    if (!ContextualCheckBlock(block, state, pindexPrev))
        return error("%s: ContextualCheckBlock failed: %s", __func__, FormatStateMessage(state));
    if (!ConnectBlock(block, state, &indexDummy, viewNew, true))
        return false;
    assert(state.IsValid());

    return true;
}

static FlatFileSeq BlockFileSeq()
{
    return FlatFileSeq(GetBlocksDir(), "blk", BLOCKFILE_CHUNK_SIZE);
}

static FlatFileSeq UndoFileSeq()
{
    return FlatFileSeq(GetBlocksDir(), "rev", UNDOFILE_CHUNK_SIZE);
}

FILE* OpenBlockFile(const FlatFilePos& pos, bool fReadOnly)
{
    return BlockFileSeq().Open(pos, fReadOnly);
}

FILE* OpenUndoFile(const FlatFilePos& pos, bool fReadOnly)
{
    return UndoFileSeq().Open(pos, fReadOnly);
}

fs::path GetBlockPosFilename(const FlatFilePos &pos)
{
    return BlockFileSeq().FileName(pos);
}

CBlockIndex* InsertBlockIndex(uint256 hash)
{
    if (hash.IsNull())
        return NULL;

    // Return existing
    BlockMap::iterator mi = mapBlockIndex.find(hash);
    if (mi != mapBlockIndex.end())
        return (*mi).second;

    // Create new
    CBlockIndex* pindexNew = new CBlockIndex();
    if (!pindexNew)
        throw std::runtime_error("LoadBlockIndex() : new CBlockIndex failed");
    mi = mapBlockIndex.emplace(hash, pindexNew).first;

    pindexNew->phashBlock = &((*mi).first);

    return pindexNew;
}

bool static LoadBlockIndexDB(std::string& strError)
{
    if (!pblocktree->LoadBlockIndexGuts(InsertBlockIndex))
        return false;

    boost::this_thread::interruption_point();

    // Calculate nChainWork
    std::vector<std::pair<int, CBlockIndex*> > vSortedByHeight;
    vSortedByHeight.reserve(mapBlockIndex.size());
    for (const std::pair<const uint256, CBlockIndex*>& item : mapBlockIndex) {
        CBlockIndex* pindex = item.second;
        vSortedByHeight.emplace_back(pindex->nHeight, pindex);
    }
    std::sort(vSortedByHeight.begin(), vSortedByHeight.end());
    for (const std::pair<int, CBlockIndex*>& item : vSortedByHeight) {
        // Stop if shutdown was requested
        if (ShutdownRequested()) return false;

        CBlockIndex* pindex = item.second;
        pindex->nChainWork = (pindex->pprev ? pindex->pprev->nChainWork : 0) + GetBlockProof(*pindex);
        pindex->nTimeMax = (pindex->pprev ? std::max(pindex->pprev->nTimeMax, pindex->nTime) : pindex->nTime);
        if (pindex->nStatus & BLOCK_HAVE_DATA) {
            if (pindex->pprev) {
                if (pindex->pprev->nChainTx) {
                    pindex->nChainTx = pindex->pprev->nChainTx + pindex->nTx;
                    // Sapling, calculate chain index value
                    if (pindex->pprev->nChainSaplingValue) {
                        pindex->nChainSaplingValue = *pindex->pprev->nChainSaplingValue + pindex->nSaplingValue;
                    } else {
                        pindex->nChainSaplingValue = nullopt;
                    }

                } else {
                    pindex->nChainTx = 0;
                    pindex->nChainSaplingValue = nullopt;
                    mapBlocksUnlinked.emplace(pindex->pprev, pindex);
                }
            } else {
                pindex->nChainTx = pindex->nTx;
                pindex->nChainSaplingValue = pindex->nSaplingValue;
            }
        }
        if (pindex->IsValid(BLOCK_VALID_TRANSACTIONS) && (pindex->nChainTx || pindex->pprev == NULL))
            setBlockIndexCandidates.insert(pindex);
        if (pindex->nStatus & BLOCK_FAILED_MASK && (!pindexBestInvalid || pindex->nChainWork > pindexBestInvalid->nChainWork))
            pindexBestInvalid = pindex;
        if (pindex->pprev)
            pindex->BuildSkip();
        if (pindex->IsValid(BLOCK_VALID_TREE) && (pindexBestHeader == NULL || CBlockIndexWorkComparator()(pindexBestHeader, pindex)))
            pindexBestHeader = pindex;
    }

    // Load block file info
    pblocktree->ReadLastBlockFile(nLastBlockFile);
    vinfoBlockFile.resize(nLastBlockFile + 1);
    LogPrintf("%s: last block file = %i\n", __func__, nLastBlockFile);
    for (int nFile = 0; nFile <= nLastBlockFile; nFile++) {
        pblocktree->ReadBlockFileInfo(nFile, vinfoBlockFile[nFile]);
    }
    LogPrintf("%s: last block file info: %s\n", __func__, vinfoBlockFile[nLastBlockFile].ToString());
    for (int nFile = nLastBlockFile + 1; true; nFile++) {
        CBlockFileInfo info;
        if (pblocktree->ReadBlockFileInfo(nFile, info)) {
            vinfoBlockFile.push_back(info);
        } else {
            break;
        }
    }

    // Check presence of blk files
    LogPrintf("Checking all blk files are present...\n");
    std::set<int> setBlkDataFiles;
    for (const std::pair<const uint256, CBlockIndex*>& item : mapBlockIndex) {
        CBlockIndex* pindex = item.second;
        if (pindex->nStatus & BLOCK_HAVE_DATA) {
            setBlkDataFiles.insert(pindex->nFile);
        }
    }
    for (std::set<int>::iterator it = setBlkDataFiles.begin(); it != setBlkDataFiles.end(); it++) {
        FlatFilePos pos(*it, 0);
        if (CAutoFile(OpenBlockFile(pos, true), SER_DISK, CLIENT_VERSION).IsNull()) {
            return false;
        }
    }

    //Check if the shutdown procedure was followed on last client exit
    bool fLastShutdownWasPrepared = true;
    pblocktree->ReadFlag("shutdown", fLastShutdownWasPrepared);
    LogPrintf("%s: Last shutdown was prepared: %s\n", __func__, fLastShutdownWasPrepared);

    // Check whether we need to continue reindexing
    bool fReindexing = false;
    pblocktree->ReadReindexing(fReindexing);
    if (fReindexing) fReindex = true;

    // Check whether we have a transaction index
    pblocktree->ReadFlag("txindex", fTxIndex);
    LogPrintf("LoadBlockIndexDB(): transaction index %s\n", fTxIndex ? "enabled" : "disabled");

    // If this is written true before the next client init, then we know the shutdown process failed
    pblocktree->WriteFlag("shutdown", false);

    return true;
}

bool LoadChainTip(const CChainParams& chainparams)
{
    if (chainActive.Tip() && chainActive.Tip()->GetBlockHash() == pcoinsTip->GetBestBlock()) return true;

    if (pcoinsTip->GetBestBlock().IsNull() && mapBlockIndex.size() == 1) {
        // In case we just added the genesis block, connect it now, so
        // that we always have a chainActive.Tip() when we return.
        LogPrintf("%s: Connecting genesis block...\n", __func__);
        CValidationState state;
        if (!ActivateBestChain(state)) {
            return false;
        }
    }

    // Load pointer to end of best chain
    BlockMap::iterator it = mapBlockIndex.find(pcoinsTip->GetBestBlock());
    if (it == mapBlockIndex.end()) {
        return false;
    }
    chainActive.SetTip(it->second);

    PruneBlockIndexCandidates();

    const CBlockIndex* pChainTip = chainActive.Tip();

    LogPrintf("Loaded best chain: hashBestChain=%s height=%d date=%s progress=%f\n",
            pChainTip->GetBlockHash().GetHex(), pChainTip->nHeight,
            FormatISO8601DateTime(pChainTip->GetBlockTime()),
            Checkpoints::GuessVerificationProgress(pChainTip));
    return true;
}

CVerifyDB::CVerifyDB()
{
    uiInterface.ShowProgress(_("Verifying blocks..."), 0);
}

CVerifyDB::~CVerifyDB()
{
    uiInterface.ShowProgress("", 100);
}

bool CVerifyDB::VerifyDB(CCoinsView* coinsview, int nCheckLevel, int nCheckDepth)
{
    LOCK(cs_main);
    if (chainActive.Tip() == NULL || chainActive.Tip()->pprev == NULL)
        return true;

    const int chainHeight = chainActive.Height();

    // begin tx and let it rollback
    auto dbTx = evoDb->BeginTransaction();

    // Verify blocks in the best chain
    if (nCheckDepth <= 0)
        nCheckDepth = 1000000000; // suffices until the year 19000
    if (nCheckDepth > chainHeight)
        nCheckDepth = chainHeight;
    nCheckLevel = std::max(0, std::min(4, nCheckLevel));
    LogPrintf("Verifying last %i blocks at level %i\n", nCheckDepth, nCheckLevel);
    CCoinsViewCache coins(coinsview);
    CBlockIndex* pindexState = chainActive.Tip();
    CBlockIndex* pindexFailure = NULL;
    int nGoodTransactions = 0;
    int reportDone = 0;
    LogPrintf("[0%%]...");
    CValidationState state;
    for (CBlockIndex* pindex = chainActive.Tip(); pindex && pindex->pprev; pindex = pindex->pprev) {
        boost::this_thread::interruption_point();
        int percentageDone = std::max(1, std::min(99, (int)(((double)(chainActive.Height() - pindex->nHeight)) / (double)nCheckDepth * (nCheckLevel >= 4 ? 50 : 100))));
        if (reportDone < percentageDone/10) {
            // report every 10% step
            LogPrintf("[%d%%]...", percentageDone);
            reportDone = percentageDone/10;
        }
        uiInterface.ShowProgress(_("Verifying blocks..."), percentageDone);
        if (pindex->nHeight < chainHeight - nCheckDepth)
            break;
        CBlock block;
        // check level 0: read from disk
        if (!ReadBlockFromDisk(block, pindex))
            return error("%s: *** ReadBlockFromDisk failed at %d, hash=%s", __func__, pindex->nHeight, pindex->GetBlockHash().ToString());
        // check level 1: verify block validity
        if (nCheckLevel >= 1 && !CheckBlock(block, state))
            return error("%s: *** found bad block at %d, hash=%s (%s)\n", __func__, pindex->nHeight, pindex->GetBlockHash().ToString(), FormatStateMessage(state));
        // check level 2: verify undo validity
        if (nCheckLevel >= 2 && pindex) {
            CBlockUndo undo;
            FlatFilePos pos = pindex->GetUndoPos();
            if (!pos.IsNull()) {
                if (!UndoReadFromDisk(undo, pos, pindex->pprev->GetBlockHash()))
                    return error("%s: *** found bad undo data at %d, hash=%s\n", __func__, pindex->nHeight, pindex->GetBlockHash().ToString());
            }
        }
        // check level 3: check for inconsistencies during memory-only disconnect of tip blocks
        if (nCheckLevel >= 3 && pindex == pindexState && (coins.DynamicMemoryUsage() + pcoinsTip->DynamicMemoryUsage()) <= nCoinCacheUsage) {
            assert(coins.GetBestBlock() == pindex->GetBlockHash());
            DisconnectResult res = DisconnectBlock(block, pindex, coins);
            if (res == DISCONNECT_FAILED) {
                return error("%s: *** irrecoverable inconsistency in block data at %d, hash=%s", __func__,
                             pindex->nHeight, pindex->GetBlockHash().ToString());
            }
            pindexState = pindex->pprev;
            if (res == DISCONNECT_UNCLEAN) {
                nGoodTransactions = 0;
                pindexFailure = pindex;
            } else {
                nGoodTransactions += block.vtx.size();
            }
        }
        if (ShutdownRequested())
            return true;
    }
    if (pindexFailure)
        return error("%s: *** coin database inconsistencies found (last %i blocks, %i good transactions before that)\n", __func__, chainHeight - pindexFailure->nHeight + 1, nGoodTransactions);

    // check level 4: try reconnecting blocks
    if (nCheckLevel >= 4) {
        CBlockIndex* pindex = pindexState;
        while (pindex != chainActive.Tip()) {
            boost::this_thread::interruption_point();
            uiInterface.ShowProgress(_("Verifying blocks..."), std::max(1, std::min(99, 100 - (int)(((double)(chainHeight - pindex->nHeight)) / (double)nCheckDepth * 50))));
            pindex = chainActive.Next(pindex);
            CBlock block;
            if (!ReadBlockFromDisk(block, pindex))
                return error("%s: *** ReadBlockFromDisk failed at %d, hash=%s", __func__, pindex->nHeight, pindex->GetBlockHash().ToString());
            if (!ConnectBlock(block, state, pindex, coins, false))
                return error("%s: *** found unconnectable block at %d, hash=%s", __func__, pindex->nHeight, pindex->GetBlockHash().ToString());
        }
    }
    LogPrintf("[DONE].\n");
    LogPrintf("No coin database inconsistencies in last %i blocks (%i transactions)\n", chainHeight - pindexState->nHeight, nGoodTransactions);

    return true;
}

/** Apply the effects of a block on the utxo cache, ignoring that it may already have been applied. */
static bool RollforwardBlock(const CBlockIndex* pindex, CCoinsViewCache& inputs, const CChainParams& params)
{
    // TODO: merge with ConnectBlock
    CBlock block;
    if (!ReadBlockFromDisk(block, pindex)) {
        return error("ReplayBlock(): ReadBlockFromDisk failed at %d, hash=%s", pindex->nHeight, pindex->GetBlockHash().ToString());
    }

    const bool fSkipInvalid = SkipInvalidUTXOS(pindex->nHeight);

    for (const CTransactionRef& tx : block.vtx) {
        if (!tx->IsCoinBase()) {
            for (const CTxIn &txin : tx->vin) {
                inputs.SpendCoin(txin.prevout);
            }
        }

        // Pass check = true as every addition may be an overwrite.
        AddCoins(inputs, *tx, pindex->nHeight, true, fSkipInvalid);
    }

    CValidationState state;
    if (!ProcessSpecialTxsInBlock(block, pindex, state, false /*fJustCheck*/)) {
        return error("%s: Special tx processing failed for block %s with %s",
                     __func__, pindex->GetBlockHash().ToString(), FormatStateMessage(state));
    }

    return true;
}

bool ReplayBlocks(const CChainParams& params, CCoinsView* view)
{
    LOCK(cs_main);

    CCoinsViewCache cache(view);

    std::vector<uint256> hashHeads = view->GetHeadBlocks();
    if (hashHeads.empty()) return true; // We're already in a consistent state.
    if (hashHeads.size() != 2) return error("%s: unknown inconsistent state", __func__);

    uiInterface.ShowProgress(_("Replaying blocks..."), 0);
    LogPrintf("Replaying blocks\n");

    const CBlockIndex* pindexOld = nullptr;  // Old tip during the interrupted flush.
    const CBlockIndex* pindexNew;            // New tip during the interrupted flush.
    const CBlockIndex* pindexFork = nullptr; // Latest block common to both the old and the new tip.

    auto itIndexNew = mapBlockIndex.find(hashHeads[0]);
    if (itIndexNew == mapBlockIndex.end()) {
        return error("%s: reorganization to unknown block requested", __func__);
    }
    pindexNew = itIndexNew->second;

    if (!hashHeads[1].IsNull()) { // The old tip is allowed to be 0, indicating it's the first flush.
        auto it = mapBlockIndex.find(hashHeads[1]);
        if (it == mapBlockIndex.end()) {
            return error("%s: reorganization from unknown block requested", __func__);
        }
        pindexOld = it->second;
        pindexFork = LastCommonAncestor(pindexOld, pindexNew);
        assert(pindexFork != nullptr);
    }

    // Rollback along the old branch.
    while (pindexOld != pindexFork) {
        if (pindexOld->nHeight > 0) { // Never disconnect the genesis block.
            CBlock block;
            if (!ReadBlockFromDisk(block, pindexOld)) {
                return error("RollbackBlock(): ReadBlockFromDisk() failed at %d, hash=%s", pindexOld->nHeight, pindexOld->GetBlockHash().ToString());
            }
            LogPrintf("Rolling back %s (%i)\n", pindexOld->GetBlockHash().ToString(), pindexOld->nHeight);
            DisconnectResult res = DisconnectBlock(block, pindexOld, cache);
            if (res == DISCONNECT_FAILED) {
                return error("RollbackBlock(): DisconnectBlock failed at %d, hash=%s", pindexOld->nHeight, pindexOld->GetBlockHash().ToString());
            }
            // If DISCONNECT_UNCLEAN is returned, it means a non-existing UTXO was deleted, or an existing UTXO was
            // overwritten. It corresponds to cases where the block-to-be-disconnect never had all its operations
            // applied to the UTXO set. However, as both writing a UTXO and deleting a UTXO are idempotent operations,
            // the result is still a version of the UTXO set with the effects of that block undone.
        }
        pindexOld = pindexOld->pprev;
    }

    // Roll forward from the forking point to the new tip.
    int nForkHeight = pindexFork ? pindexFork->nHeight : 0;
    for (int nHeight = nForkHeight + 1; nHeight <= pindexNew->nHeight; ++nHeight) {
        const CBlockIndex* pindex = pindexNew->GetAncestor(nHeight);
        LogPrintf("Rolling forward %s (%i)\n", pindex->GetBlockHash().ToString(), nHeight);
        if (!RollforwardBlock(pindex, cache, params)) return false;
    }

    cache.SetBestBlock(pindexNew->GetBlockHash());
    evoDb->WriteBestBlock(pindexNew->GetBlockHash());
    cache.Flush();
    uiInterface.ShowProgress("", 100);
    return true;
}

// May NOT be used after any connections are up as much
// of the peer-processing logic assumes a consistent
// block index state
void UnloadBlockIndex()
{
    LOCK(cs_main);
    setBlockIndexCandidates.clear();
    chainActive.SetTip(NULL);
    pindexBestInvalid = NULL;
    pindexBestHeader = NULL;
    mempool.clear();
    mapBlocksUnlinked.clear();
    vinfoBlockFile.clear();
    nLastBlockFile = 0;
    nBlockSequenceId = 1;
    setDirtyBlockIndex.clear();
    setDirtyFileInfo.clear();

    for (BlockMap::value_type& entry : mapBlockIndex) {
        delete entry.second;
    }
    mapBlockIndex.clear();
}

bool LoadBlockIndex(std::string& strError)
{
    bool needs_init = fReindex;
    if (!fReindex) {
        if (!LoadBlockIndexDB(strError))
            return false;
        needs_init = mapBlockIndex.empty();
    }

    if (needs_init) {
        // Everything here is for *new* reindex/DBs. Thus, though
        // LoadBlockIndexDB may have set fReindex if we shut down
        // mid-reindex previously, we don't check fReindex and
        // instead only check it prior to LoadBlockIndexDB to set
        // needs_init.

        LogPrintf("Initializing databases...\n");
        // Use the provided setting for -txindex in the new database
        fTxIndex = gArgs.GetBoolArg("-txindex", DEFAULT_TXINDEX);
        pblocktree->WriteFlag("txindex", fTxIndex);
    }
    return true;
}


bool LoadGenesisBlock()
{
    LOCK(cs_main);

    // Check whether we're already initialized by checking for genesis in
    // mapBlockIndex. Note that we can't use chainActive here, since it is
    // set based on the coins db, not the block index db, which is the only
    // thing loaded at this point.
    if (mapBlockIndex.count(Params().GenesisBlock().GetHash()))
        return true;

    try {
        CBlock& block = const_cast<CBlock&>(Params().GenesisBlock());
        // Start new block file
        unsigned int nBlockSize = ::GetSerializeSize(block, CLIENT_VERSION);
        FlatFilePos blockPos;
        CValidationState state;
        if (!FindBlockPos(state, blockPos, nBlockSize + 8, 0, block.GetBlockTime()))
            return error("%s: FindBlockPos failed", __func__);
        if (!WriteBlockToDisk(block, blockPos))
            return error("%s: writing genesis block to disk failed", __func__);
        CBlockIndex *pindex = AddToBlockIndex(block);
        if (!ReceivedBlockTransactions(block, state, pindex, blockPos))
            return error("%s: genesis block not accepted", __func__);
    } catch (const std::runtime_error& e) {
         return error("%s: failed to write genesis block: %s", __func__, e.what());
     }

    return true;
}


bool LoadExternalBlockFile(FILE* fileIn, FlatFilePos* dbp)
{
    // Map of disk positions for blocks with unknown parent (only used for reindex)
    static std::multimap<uint256, FlatFilePos> mapBlocksUnknownParent;
    int64_t nStart = GetTimeMillis();

    // Block checked event listener
    BlockStateCatcher stateCatcher(UINT256_ZERO);
    stateCatcher.registerEvent();

    int nLoaded = 0;
    try {
        // This takes over fileIn and calls fclose() on it in the CBufferedFile destructor
        CBufferedFile blkdat(fileIn, 2 * MAX_BLOCK_SIZE_CURRENT, MAX_BLOCK_SIZE_CURRENT + 8, SER_DISK, CLIENT_VERSION);
        uint64_t nRewind = blkdat.GetPos();
        while (!blkdat.eof()) {
            boost::this_thread::interruption_point();

            blkdat.SetPos(nRewind);
            nRewind++;         // start one byte further next time, in case of failure
            blkdat.SetLimit(); // remove former limit
            unsigned int nSize = 0;
            try {
                // locate a header
                unsigned char buf[MESSAGE_START_SIZE];
                blkdat.FindByte(Params().MessageStart()[0]);
                nRewind = blkdat.GetPos()+1;
                blkdat >> buf;
                if (memcmp(buf, Params().MessageStart(), MESSAGE_START_SIZE))
                    continue;
                // read size
                blkdat >> nSize;
                if (nSize < 80 || nSize > MAX_BLOCK_SIZE_CURRENT)
                    continue;
            } catch (const std::exception&) {
                // no valid block header found; don't complain
                break;
            }
            try {
                // read block
                uint64_t nBlockPos = blkdat.GetPos();
                if (dbp)
                    dbp->nPos = nBlockPos;
                blkdat.SetLimit(nBlockPos + nSize);
                blkdat.SetPos(nBlockPos);
                CBlock block;
                blkdat >> block;
                nRewind = blkdat.GetPos();

                // detect out of order blocks, and store them for later
                uint256 hash = block.GetHash();
                if (hash != Params().GetConsensus().hashGenesisBlock && mapBlockIndex.find(block.hashPrevBlock) == mapBlockIndex.end()) {
                    LogPrint(BCLog::REINDEX, "%s: Out of order block %s, parent %s not known\n", __func__,
                            hash.GetHex(), block.hashPrevBlock.GetHex());
                    if (dbp)
                        mapBlocksUnknownParent.emplace(block.hashPrevBlock, *dbp);
                    continue;
                }

                // process in case the block isn't known yet
                if (mapBlockIndex.count(hash) == 0 || (mapBlockIndex[hash]->nStatus & BLOCK_HAVE_DATA) == 0) {
                    std::shared_ptr<const CBlock> block_ptr = std::make_shared<const CBlock>(block);
                    stateCatcher.setBlockHash(block_ptr->GetHash());
                    if (ProcessNewBlock(block_ptr, dbp)) {
                        nLoaded++;
                    }
                    if (stateCatcher.stateErrorFound()) {
                        break;
                    }
                } else if (hash != Params().GetConsensus().hashGenesisBlock && mapBlockIndex[hash]->nHeight % 1000 == 0) {
                    LogPrint(BCLog::REINDEX, "Block Import: already had block %s at height %d\n", hash.ToString(), mapBlockIndex[hash]->nHeight);
                }

                // Recursively process earlier encountered successors of this block
                std::deque<uint256> queue;
                queue.push_back(hash);
                while (!queue.empty()) {
                    uint256 head = queue.front();
                    queue.pop_front();
                    std::pair<std::multimap<uint256, FlatFilePos>::iterator, std::multimap<uint256, FlatFilePos>::iterator> range = mapBlocksUnknownParent.equal_range(head);
                    while (range.first != range.second) {
                        std::multimap<uint256, FlatFilePos>::iterator it = range.first;
                        if (ReadBlockFromDisk(block, it->second)) {
                            LogPrint(BCLog::REINDEX, "%s: Processing out of order child %s of %s\n", __func__, block.GetHash().ToString(),
                                head.ToString());
                            std::shared_ptr<const CBlock> block_ptr = std::make_shared<const CBlock>(block);
                            if (ProcessNewBlock(block_ptr, &it->second)) {
                                nLoaded++;
                                queue.emplace_back(block.GetHash());
                            }
                        }
                        range.first++;
                        mapBlocksUnknownParent.erase(it);
                    }
                }
            } catch (const std::exception& e) {
                LogPrintf("%s : Deserialize or I/O error - %s", __func__, e.what());
            }
        }
    } catch (const std::runtime_error& e) {
        AbortNode(std::string("System error: ") + e.what());
    }
    if (nLoaded > 0)
        LogPrintf("Loaded %i blocks from external file in %dms\n", nLoaded, GetTimeMillis() - nStart);
    return nLoaded > 0;
}

void static CheckBlockIndex()
{
    if (!fCheckBlockIndex) {
        return;
    }

    LOCK(cs_main);

    // During a reindex, we read the genesis block and call CheckBlockIndex before ActivateBestChain,
    // so we have the genesis block in mapBlockIndex but no active chain.  (A few of the tests when
    // iterating the block tree require that chainActive has been initialized.)
    if (chainActive.Height() < 0) {
        assert(mapBlockIndex.size() <= 1);
        return;
    }

    // Build forward-pointing map of the entire block tree.
    std::multimap<CBlockIndex*, CBlockIndex*> forward;
    for (BlockMap::iterator it = mapBlockIndex.begin(); it != mapBlockIndex.end(); it++) {
        forward.emplace(it->second->pprev, it->second);
    }

    assert(forward.size() == mapBlockIndex.size());

    std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> rangeGenesis = forward.equal_range(NULL);
    CBlockIndex* pindex = rangeGenesis.first->second;
    rangeGenesis.first++;
    assert(rangeGenesis.first == rangeGenesis.second); // There is only one index entry with parent NULL.

    // Iterate over the entire block tree, using depth-first search.
    // Along the way, remember whether there are blocks on the path from genesis
    // block being explored which are the first to have certain properties.
    size_t nNodes = 0;
    int nHeight = 0;
    CBlockIndex* pindexFirstInvalid = NULL;         // Oldest ancestor of pindex which is invalid.
    CBlockIndex* pindexFirstMissing = NULL;         // Oldest ancestor of pindex which does not have BLOCK_HAVE_DATA.
    CBlockIndex* pindexFirstNotTreeValid = NULL;    // Oldest ancestor of pindex which does not have BLOCK_VALID_TREE (regardless of being valid or not).
    CBlockIndex* pindexFirstNotChainValid = NULL;   // Oldest ancestor of pindex which does not have BLOCK_VALID_CHAIN (regardless of being valid or not).
    CBlockIndex* pindexFirstNotScriptsValid = NULL; // Oldest ancestor of pindex which does not have BLOCK_VALID_SCRIPTS (regardless of being valid or not).
    while (pindex != NULL) {
        nNodes++;
        if (pindexFirstInvalid == NULL && pindex->nStatus & BLOCK_FAILED_VALID) pindexFirstInvalid = pindex;
        if (pindexFirstMissing == NULL && !(pindex->nStatus & BLOCK_HAVE_DATA)) pindexFirstMissing = pindex;
        if (pindex->pprev != NULL && pindexFirstNotTreeValid == NULL && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_TREE) pindexFirstNotTreeValid = pindex;
        if (pindex->pprev != NULL && pindexFirstNotChainValid == NULL && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_CHAIN) pindexFirstNotChainValid = pindex;
        if (pindex->pprev != NULL && pindexFirstNotScriptsValid == NULL && (pindex->nStatus & BLOCK_VALID_MASK) < BLOCK_VALID_SCRIPTS) pindexFirstNotScriptsValid = pindex;

        // Begin: actual consistency checks.
        if (pindex->pprev == NULL) {
            // Genesis block checks.
            assert(pindex->GetBlockHash() == Params().GetConsensus().hashGenesisBlock); // Genesis block's hash must match.
            assert(pindex == chainActive.Genesis());                       // The current active chain's genesis block must be this block.
        }
        // HAVE_DATA is equivalent to VALID_TRANSACTIONS and equivalent to nTx > 0 (we stored the number of transactions in the block)
        assert(!(pindex->nStatus & BLOCK_HAVE_DATA) == (pindex->nTx == 0));
        assert(((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TRANSACTIONS) == (pindex->nTx > 0));
        if (pindex->nChainTx == 0) assert(pindex->nSequenceId == 0); // nSequenceId can't be set for blocks that aren't linked
        // All parents having data is equivalent to all parents being VALID_TRANSACTIONS, which is equivalent to nChainTx being set.
        assert((pindexFirstMissing != NULL) == (pindex->nChainTx == 0));                                             // nChainTx == 0 is used to signal that all parent block's transaction data is available.
        assert(pindex->nHeight == nHeight);                                                                          // nHeight must be consistent.
        assert(pindex->pprev == NULL || pindex->nChainWork >= pindex->pprev->nChainWork);                            // For every block except the genesis block, the chainwork must be larger than the parent's.
        assert(nHeight < 2 || (pindex->pskip && (pindex->pskip->nHeight < nHeight)));                                // The pskip pointer must point back for all but the first 2 blocks.
        assert(pindexFirstNotTreeValid == NULL);                                                                     // All mapBlockIndex entries must at least be TREE valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_TREE) assert(pindexFirstNotTreeValid == NULL);       // TREE valid implies all parents are TREE valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_CHAIN) assert(pindexFirstNotChainValid == NULL);     // CHAIN valid implies all parents are CHAIN valid
        if ((pindex->nStatus & BLOCK_VALID_MASK) >= BLOCK_VALID_SCRIPTS) assert(pindexFirstNotScriptsValid == NULL); // SCRIPTS valid implies all parents are SCRIPTS valid
        if (pindexFirstInvalid == NULL) {
            // Checks for not-invalid blocks.
            assert((pindex->nStatus & BLOCK_FAILED_MASK) == 0); // The failed mask cannot be set for blocks without invalid parents.
        }
        if (!CBlockIndexWorkComparator()(pindex, chainActive.Tip()) && pindexFirstMissing == NULL) {
            if (pindexFirstInvalid == NULL) { // If this block sorts at least as good as the current tip and is valid, it must be in setBlockIndexCandidates.
                assert(setBlockIndexCandidates.count(pindex));
            }
        } else { // If this block sorts worse than the current tip, it cannot be in setBlockIndexCandidates.
            assert(setBlockIndexCandidates.count(pindex) == 0);
        }
        // Check whether this block is in mapBlocksUnlinked.
        std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> rangeUnlinked = mapBlocksUnlinked.equal_range(pindex->pprev);
        bool foundInUnlinked = false;
        while (rangeUnlinked.first != rangeUnlinked.second) {
            assert(rangeUnlinked.first->first == pindex->pprev);
            if (rangeUnlinked.first->second == pindex) {
                foundInUnlinked = true;
                break;
            }
            rangeUnlinked.first++;
        }
        if (pindex->pprev && pindex->nStatus & BLOCK_HAVE_DATA && pindexFirstMissing != NULL) {
            if (pindexFirstInvalid == NULL) { // If this block has block data available, some parent doesn't, and has no invalid parents, it must be in mapBlocksUnlinked.
                assert(foundInUnlinked);
            }
        } else { // If this block does not have block data available, or all parents do, it cannot be in mapBlocksUnlinked.
            assert(!foundInUnlinked);
        }
        // assert(pindex->GetBlockHash() == pindex->GetBlockHeader().GetHash()); // Perhaps too slow
        // End: actual consistency checks.

        // Try descending into the first subnode.
        std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> range = forward.equal_range(pindex);
        if (range.first != range.second) {
            // A subnode was found.
            pindex = range.first->second;
            nHeight++;
            continue;
        }
        // This is a leaf node.
        // Move upwards until we reach a node of which we have not yet visited the last child.
        while (pindex) {
            // We are going to either move to a parent or a sibling of pindex.
            // If pindex was the first with a certain property, unset the corresponding variable.
            if (pindex == pindexFirstInvalid) pindexFirstInvalid = NULL;
            if (pindex == pindexFirstMissing) pindexFirstMissing = NULL;
            if (pindex == pindexFirstNotTreeValid) pindexFirstNotTreeValid = NULL;
            if (pindex == pindexFirstNotChainValid) pindexFirstNotChainValid = NULL;
            if (pindex == pindexFirstNotScriptsValid) pindexFirstNotScriptsValid = NULL;
            // Find our parent.
            CBlockIndex* pindexPar = pindex->pprev;
            // Find which child we just visited.
            std::pair<std::multimap<CBlockIndex*, CBlockIndex*>::iterator, std::multimap<CBlockIndex*, CBlockIndex*>::iterator> rangePar = forward.equal_range(pindexPar);
            while (rangePar.first->second != pindex) {
                assert(rangePar.first != rangePar.second); // Our parent must have at least the node we're coming from as child.
                rangePar.first++;
            }
            // Proceed to the next one.
            rangePar.first++;
            if (rangePar.first != rangePar.second) {
                // Move to the sibling.
                pindex = rangePar.first->second;
                break;
            } else {
                // Move up further.
                pindex = pindexPar;
                nHeight--;
                continue;
            }
        }
    }

    // Check that we actually traversed the entire map.
    assert(nNodes == forward.size());
}

// Note: whenever a protocol update is needed toggle between both implementations (comment out the formerly active one)
//       so we can leave the existing clients untouched (old SPORK will stay on so they don't see even older clients).
//       Those old clients won't react to the changes of the other (new) SPORK because at the time of their implementation
//       it was the one which was commented out
int ActiveProtocol()
{
    // SPORK_14 is used for 70922 (v5.2.0)
    if (sporkManager.IsSporkActive(SPORK_14_NEW_PROTOCOL_ENFORCEMENT))
            return MIN_PEER_PROTO_VERSION_AFTER_ENFORCEMENT;

    // SPORK_15 was used for 70921 (v5.0.1), commented out now.
    //if (sporkManager.IsSporkActive(SPORK_15_NEW_PROTOCOL_ENFORCEMENT_2))
    //        return MIN_PEER_PROTO_VERSION_AFTER_ENFORCEMENT;

    return MIN_PEER_PROTO_VERSION_BEFORE_ENFORCEMENT;
}

std::string CBlockFileInfo::ToString() const
{
    return strprintf("CBlockFileInfo(blocks=%u, size=%u, heights=%u...%u, time=%s...%s)", nBlocks, nSize, nHeightFirst, nHeightLast, FormatISO8601Date(nTimeFirst), FormatISO8601Date(nTimeLast));
}

CBlockFileInfo* GetBlockFileInfo(size_t n)
{
    return &vinfoBlockFile.at(n);
}

static const uint64_t MEMPOOL_DUMP_VERSION = 1;

bool LoadMempool(CTxMemPool& pool)
{
    int64_t nExpiryTimeout = gArgs.GetArg("-mempoolexpiry", DEFAULT_MEMPOOL_EXPIRY) * 60 * 60;
    FILE* filestr = fopen((GetDataDir() / "mempool.dat").string().c_str(), "r");
    CAutoFile file(filestr, SER_DISK, CLIENT_VERSION);
    if (file.IsNull()) {
        LogPrintf("Failed to open mempool file from disk. Continuing anyway.\n");
        return false;
    }

    int64_t count = 0;
    int64_t skipped = 0;
    int64_t failed = 0;
    int64_t nNow = GetTime();

    try {
        uint64_t version;
        file >> version;
        if (version != MEMPOOL_DUMP_VERSION) {
            return false;
        }
        uint64_t num;
        file >> num;
        while (num--) {
            CTransactionRef tx;
            int64_t nTime;
            int64_t nFeeDelta;
            file >> tx;
            file >> nTime;
            file >> nFeeDelta;

            CAmount amountdelta = nFeeDelta;
            if (amountdelta) {
                pool.PrioritiseTransaction(tx->GetHash(), amountdelta);
            }
            CValidationState state;
            if (nTime + nExpiryTimeout > nNow) {
                LOCK(cs_main);
                AcceptToMemoryPoolWithTime(pool, state, tx, true, NULL, nTime);
                if (state.IsValid()) {
                    ++count;
                } else {
                    ++failed;
                }
            } else {
                ++skipped;
            }
            if (ShutdownRequested())
                return false;
        }
        std::map<uint256, CAmount> mapDeltas;
        file >> mapDeltas;

        for (const auto& i : mapDeltas) {
            pool.PrioritiseTransaction(i.first, i.second);
        }
    } catch (const std::exception& e) {
        LogPrintf("Failed to deserialize mempool data on disk: %s. Continuing anyway.\n", e.what());
        return false;
    }

    LogPrintf("Imported mempool transactions from disk: %i successes, %i failed, %i expired\n", count, failed, skipped);
    return true;
}

bool DumpMempool(const CTxMemPool& pool)
{
    int64_t start = GetTimeMicros();

    std::map<uint256, CAmount> mapDeltas;
    std::vector<TxMempoolInfo> vinfo;

    static Mutex dump_mutex;
    LOCK(dump_mutex);

    {
        LOCK(pool.cs);
        for (const auto &i : pool.mapDeltas) {
            mapDeltas[i.first] = i.second;
        }
        vinfo = pool.infoAll();
    }

    int64_t mid = GetTimeMicros();

    try {
        FILE* filestr = fopen((GetDataDir() / "mempool.dat.new").string().c_str(), "w");
        if (!filestr) {
            return false;
        }

        CAutoFile file(filestr, SER_DISK, CLIENT_VERSION);

        uint64_t version = MEMPOOL_DUMP_VERSION;
        file << version;

        file << (uint64_t)vinfo.size();
        for (const auto& i : vinfo) {
            file << i.tx;
            file << (int64_t)i.nTime;
            file << (int64_t)i.nFeeDelta;
            mapDeltas.erase(i.tx->GetHash());
        }

        file << mapDeltas;
        if (!FileCommit(file.Get()))
            throw std::runtime_error("FileCommit failed");
        file.fclose();
        if (!RenameOver(GetDataDir() / "mempool.dat.new", GetDataDir() / "mempool.dat")) {
            throw std::runtime_error("Rename failed");
        }
        int64_t last = GetTimeMicros();
        LogPrintf("Dumped mempool: %gs to copy, %gs to dump\n", (mid-start)*0.000001, (last-mid)*0.000001);
    } catch (const std::exception& e) {
        LogPrintf("Failed to dump mempool: %s. Continuing anyway.\n", e.what());
        return false;
    }
    return true;
}

class CMainCleanup
{
public:
    CMainCleanup() {}
    ~CMainCleanup()
    {
        // block headers
        BlockMap::iterator it1 = mapBlockIndex.begin();
        for (; it1 != mapBlockIndex.end(); it1++)
            delete (*it1).second;
        mapBlockIndex.clear();
    }
} instance_of_cmaincleanup;

