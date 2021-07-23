// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2011-2013 The PPCoin developers
// Copyright (c) 2013-2014 The NovaCoin Developers
// Copyright (c) 2014-2018 The BlackCoin Developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "miner.h"

#include "amount.h"
#include "blockassembler.h"
#include "consensus/params.h"
#include "masternode-sync.h"
#include "net.h"
#include "policy/feerate.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "timedata.h"
#include "util/blockstatecatcher.h"
#include "util/system.h"
#include "utilmoneystr.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif
#include "invalid.h"
#include "policy/policy.h"

#include <boost/thread.hpp>

#ifdef ENABLE_WALLET
//////////////////////////////////////////////////////////////////////////////
//
// Internal miner
//
double dHashesPerSec = 0.0;
int64_t nHPSTimerStart = 0;

std::unique_ptr<CBlockTemplate> CreateNewBlockWithKey(std::unique_ptr<CReserveKey>& reservekey, CWallet* pwallet)
{
    CPubKey pubkey;
    if (!reservekey->GetReservedKey(pubkey)) return nullptr;
    return CreateNewBlockWithScript(GetScriptForDestination(pubkey.GetID()), pwallet);
}

std::unique_ptr<CBlockTemplate> CreateNewBlockWithScript(const CScript& coinbaseScript, CWallet* pwallet)
{
    const int nHeightNext = chainActive.Tip()->nHeight + 1;

    // If we're building a late PoW block, don't continue
    // PoS blocks are built directly with CreateNewBlock
    if (Params().GetConsensus().NetworkUpgradeActive(nHeightNext, Consensus::UPGRADE_POS)) {
        LogPrintf("%s: Aborting PoW block creation during PoS phase\n", __func__);
        // sleep 1/2 a block time so we don't go into a tight loop.
        MilliSleep((Params().GetConsensus().nTargetSpacing * 1000) >> 1);
        return nullptr;
    }

    return BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(coinbaseScript, pwallet, false);
}

bool ProcessBlockFound(const std::shared_ptr<const CBlock>& pblock, CWallet& wallet, std::unique_ptr<CReserveKey>& reservekey)
{
    LogPrintf("%s\n", pblock->ToString());
    LogPrintf("generated %s\n", FormatMoney(pblock->vtx[0]->vout[0].nValue));

    // Found a solution
    {
        WAIT_LOCK(g_best_block_mutex, lock);
        if (pblock->hashPrevBlock != g_best_block)
            return error("PIVXMiner : generated block is stale");
    }

    // Remove key from key pool
    if (reservekey)
        reservekey->KeepKey();

    // Process this block the same as if we had received it from another node
    BlockStateCatcher sc(pblock->GetHash());
    sc.registerEvent();
    bool res = ProcessNewBlock(pblock, nullptr);
    if (!res || sc.stateErrorFound()) {
        return error("PIVXMiner : ProcessNewBlock, block not accepted");
    }

    g_connman->ForEachNode([&pblock](CNode* node)
    {
        node->PushInventory(CInv(MSG_BLOCK, pblock->GetHash()));
    });

    return true;
}

bool fGenerateBitcoins = false;
bool fStakeableCoins = false;

void CheckForCoins(CWallet* pwallet, std::vector<CStakeableOutput>* availableCoins)
{
    if (!pwallet || !pwallet->pStakerStatus)
        return;

    // control the amount of times the client will check for mintable coins (every block)
    {
        WAIT_LOCK(g_best_block_mutex, lock);
        if (g_best_block == pwallet->pStakerStatus->GetLastHash())
            return;
    }
    fStakeableCoins = pwallet->StakeableCoins(availableCoins);
}

void BitcoinMiner(CWallet* pwallet, bool fProofOfStake)
{
    LogPrintf("PIVXMiner started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);
    util::ThreadRename("pivx-miner");
    const Consensus::Params& consensus = Params().GetConsensus();
    const int64_t nSpacingMillis = consensus.nTargetSpacing * 1000;

    // Each thread has its own key and counter
    std::unique_ptr<CReserveKey> pReservekey = fProofOfStake ? nullptr : std::make_unique<CReserveKey>(pwallet);

    // Available UTXO set
    std::vector<CStakeableOutput> availableCoins;
    unsigned int nExtraNonce = 0;

    while (fGenerateBitcoins || fProofOfStake) {
        CBlockIndex* pindexPrev = GetChainTip();
        if (!pindexPrev) {
            MilliSleep(nSpacingMillis);       // sleep a block
            continue;
        }
        if (fProofOfStake) {
            if (!consensus.NetworkUpgradeActive(pindexPrev->nHeight + 1, Consensus::UPGRADE_POS)) {
                // The last PoW block hasn't even been mined yet.
                MilliSleep(nSpacingMillis);       // sleep a block
                continue;
            }

            // update fStakeableCoins
            CheckForCoins(pwallet, &availableCoins);

            while ((g_connman && g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL) == 0 && Params().MiningRequiresPeers())
                    || pwallet->IsLocked() || !fStakeableCoins || masternodeSync.NotCompleted()) {
                MilliSleep(5000);
                // Do another check here to ensure fStakeableCoins is updated
                if (!fStakeableCoins) CheckForCoins(pwallet, &availableCoins);
            }

            //search our map of hashed blocks, see if bestblock has been hashed yet
            if (pwallet->pStakerStatus &&
                    pwallet->pStakerStatus->GetLastHash() == pindexPrev->GetBlockHash() &&
                    pwallet->pStakerStatus->GetLastTime() >= GetCurrentTimeSlot()) {
                MilliSleep(2000);
                continue;
            }

        } else if (pindexPrev->nHeight > 6 && consensus.NetworkUpgradeActive(pindexPrev->nHeight - 6, Consensus::UPGRADE_POS)) {
            // Late PoW: run for a little while longer, just in case there is a rewind on the chain.
            LogPrintf("%s: Exiting PoW Mining Thread at height: %d\n", __func__, pindexPrev->nHeight);
            return;
       }

        //
        // Create new block
        //
        unsigned int nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();

        std::unique_ptr<CBlockTemplate> pblocktemplate((fProofOfStake ?
                                                        BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(CScript(), pwallet, true, &availableCoins) :
                                                        CreateNewBlockWithKey(pReservekey, pwallet)));
        if (!pblocktemplate) continue;
        std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>(pblocktemplate->block);

        // POS - block found: process it
        if (fProofOfStake) {
            LogPrintf("%s : proof-of-stake block was signed %s \n", __func__, pblock->GetHash().ToString().c_str());
            SetThreadPriority(THREAD_PRIORITY_NORMAL);
            if (!ProcessBlockFound(pblock, *pwallet, pReservekey)) {
                LogPrintf("%s: New block orphaned\n", __func__);
                continue;
            }
            SetThreadPriority(THREAD_PRIORITY_LOWEST);
            continue;
        }

        // POW - miner main
        IncrementExtraNonce(pblock, pindexPrev->nHeight + 1, nExtraNonce);

        LogPrintf("Running PIVXMiner with %u transactions in block (%u bytes)\n", pblock->vtx.size(),
            ::GetSerializeSize(*pblock, PROTOCOL_VERSION));

        //
        // Search
        //
        int64_t nStart = GetTime();
        arith_uint256& hashTarget = arith_uint256().SetCompact(pblock->nBits);
        while (true) {
            unsigned int nHashesDone = 0;

            arith_uint256 hash;
            while (true) {
                hash = UintToArith256(pblock->GetHash());
                if (hash <= hashTarget) {
                    // Found a solution
                    SetThreadPriority(THREAD_PRIORITY_NORMAL);
                    LogPrintf("%s:\n", __func__);
                    LogPrintf("proof-of-work found  \n  hash: %s  \ntarget: %s\n", hash.GetHex(), hashTarget.GetHex());
                    ProcessBlockFound(pblock, *pwallet, pReservekey);
                    SetThreadPriority(THREAD_PRIORITY_LOWEST);

                    // In regression test mode, stop mining after a block is found. This
                    // allows developers to controllably generate a block on demand.
                    if (Params().IsRegTestNet())
                        throw boost::thread_interrupted();

                    break;
                }
                pblock->nNonce += 1;
                nHashesDone += 1;
                if ((pblock->nNonce & 0xFF) == 0)
                    break;
            }

            // Meter hashes/sec
            static int64_t nHashCounter;
            if (nHPSTimerStart == 0) {
                nHPSTimerStart = GetTimeMillis();
                nHashCounter = 0;
            } else
                nHashCounter += nHashesDone;
            if (GetTimeMillis() - nHPSTimerStart > 4000) {
                static RecursiveMutex cs;
                {
                    LOCK(cs);
                    if (GetTimeMillis() - nHPSTimerStart > 4000) {
                        dHashesPerSec = 1000.0 * nHashCounter / (GetTimeMillis() - nHPSTimerStart);
                        nHPSTimerStart = GetTimeMillis();
                        nHashCounter = 0;
                        static int64_t nLogTime;
                        if (GetTime() - nLogTime > 30 * 60) {
                            nLogTime = GetTime();
                            LogPrintf("hashmeter %6.0f khash/s\n", dHashesPerSec / 1000.0);
                        }
                    }
                }
            }

            // Check for stop or if block needs to be rebuilt
            boost::this_thread::interruption_point();
            if (    (g_connman && g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL) == 0 && Params().MiningRequiresPeers()) || // Regtest mode doesn't require peers
                    (pblock->nNonce >= 0xffff0000) ||
                    (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 60) ||
                    (pindexPrev != chainActive.Tip())
                ) break;

            // Update nTime every few seconds
            UpdateTime(pblock.get(), consensus, pindexPrev);
            if (Params().GetConsensus().fPowAllowMinDifficultyBlocks) {
                // Changing pblock->nTime can change work required on testnet:
                hashTarget.SetCompact(pblock->nBits);
            }

        }
    }
}

void static ThreadBitcoinMiner(void* parg)
{
    boost::this_thread::interruption_point();
    CWallet* pwallet = (CWallet*)parg;
    try {
        BitcoinMiner(pwallet, false);
        boost::this_thread::interruption_point();
    } catch (const std::exception& e) {
        LogPrintf("PIVXMiner exception");
    } catch (...) {
        LogPrintf("PIVXMiner exception");
    }

    LogPrintf("PIVXMiner exiting\n");
}

void GenerateBitcoins(bool fGenerate, CWallet* pwallet, int nThreads)
{
    static boost::thread_group* minerThreads = NULL;
    fGenerateBitcoins = fGenerate;

    if (minerThreads != NULL) {
        minerThreads->interrupt_all();
        delete minerThreads;
        minerThreads = NULL;
    }

    if (nThreads == 0 || !fGenerate)
        return;

    minerThreads = new boost::thread_group();
    for (int i = 0; i < nThreads; i++)
        minerThreads->create_thread(std::bind(&ThreadBitcoinMiner, pwallet));
}

void ThreadStakeMinter()
{
    boost::this_thread::interruption_point();
    LogPrintf("ThreadStakeMinter started. Using wallet-0\n");
    CWallet* pwallet = vpwallets[0];
    try {
        BitcoinMiner(pwallet, true);
        boost::this_thread::interruption_point();
    } catch (const std::exception& e) {
        LogPrintf("ThreadStakeMinter() exception \n");
    } catch (...) {
        LogPrintf("ThreadStakeMinter() error \n");
    }
    LogPrintf("ThreadStakeMinter exiting,\n");
}

#endif // ENABLE_WALLET
