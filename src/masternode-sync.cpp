// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// clang-format off
#include "activemasternode.h"
#include "budget/budgetmanager.h"
#include "evo/deterministicmns.h"
#include "masternode-sync.h"
#include "masternode-payments.h"
#include "masternode.h"
#include "masternodeman.h"
#include "netmessagemaker.h"
#include "spork.h"
#include "util/system.h"
#include "addrman.h"
// clang-format on

class CMasternodeSync;
CMasternodeSync masternodeSync;

CMasternodeSync::CMasternodeSync()
{
    Reset();
}

bool CMasternodeSync::IsSynced()
{
    return RequestedMasternodeAssets == MASTERNODE_SYNC_FINISHED;
}

bool CMasternodeSync::IsSporkListSynced()
{
    return RequestedMasternodeAssets > MASTERNODE_SYNC_SPORKS;
}

bool CMasternodeSync::IsMasternodeListSynced()
{
    return RequestedMasternodeAssets > MASTERNODE_SYNC_LIST;
}

bool CMasternodeSync::NotCompleted()
{
    return (!IsSynced() && (
            !IsSporkListSynced() ||
            sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT) ||
            sporkManager.IsSporkActive(SPORK_9_MASTERNODE_BUDGET_ENFORCEMENT) ||
            sporkManager.IsSporkActive(SPORK_13_ENABLE_SUPERBLOCKS)));
}

bool CMasternodeSync::IsBlockchainSynced()
{
    int64_t now = GetTime();

    // if the last call to this function was more than 60 minutes ago (client was in sleep mode) reset the sync process
    if (now > lastProcess + 60 * 60) {
        Reset();
        fBlockchainSynced = false;
    }
    lastProcess = now;

    if (fBlockchainSynced) return true;

    if (fImporting || fReindex) return false;

    int64_t blockTime = 0;
    {
        TRY_LOCK(g_best_block_mutex, lock);
        if (!lock) return false;
        blockTime = g_best_block_time;
    }

    if (blockTime + 60 * 60 < lastProcess)
        return false;

    fBlockchainSynced = true;

    return true;
}

bool CMasternodeSync::IsBlockchainSyncedReadOnly() const
{
    return fBlockchainSynced;
}

void CMasternodeSync::Reset()
{
    fBlockchainSynced = false;
    lastProcess = 0;
    lastMasternodeList = 0;
    lastMasternodeWinner = 0;
    lastBudgetItem = 0;
    mapSeenSyncMNB.clear();
    mapSeenSyncMNW.clear();
    mapSeenSyncBudget.clear();
    lastFailure = 0;
    nCountFailures = 0;
    sumMasternodeList = 0;
    sumMasternodeWinner = 0;
    sumBudgetItemProp = 0;
    sumBudgetItemFin = 0;
    countMasternodeList = 0;
    countMasternodeWinner = 0;
    countBudgetItemProp = 0;
    countBudgetItemFin = 0;
    RequestedMasternodeAssets = MASTERNODE_SYNC_INITIAL;
    RequestedMasternodeAttempt = 0;
    nAssetSyncStarted = GetTime();
}

void CMasternodeSync::AddedMasternodeList(const uint256& hash)
{
    if (mnodeman.mapSeenMasternodeBroadcast.count(hash)) {
        if (mapSeenSyncMNB[hash] < MASTERNODE_SYNC_THRESHOLD) {
            lastMasternodeList = GetTime();
            mapSeenSyncMNB[hash]++;
        }
    } else {
        lastMasternodeList = GetTime();
        mapSeenSyncMNB.emplace(hash, 1);
    }
}

void CMasternodeSync::AddedMasternodeWinner(const uint256& hash)
{
    if (masternodePayments.mapMasternodePayeeVotes.count(hash)) {
        if (mapSeenSyncMNW[hash] < MASTERNODE_SYNC_THRESHOLD) {
            lastMasternodeWinner = GetTime();
            mapSeenSyncMNW[hash]++;
        }
    } else {
        lastMasternodeWinner = GetTime();
        mapSeenSyncMNW.emplace(hash, 1);
    }
}

void CMasternodeSync::AddedBudgetItem(const uint256& hash)
{
    if (g_budgetman.HaveProposal(hash) ||
            g_budgetman.HaveSeenProposalVote(hash) ||
            g_budgetman.HaveFinalizedBudget(hash) ||
            g_budgetman.HaveSeenFinalizedBudgetVote(hash)) {
        if (mapSeenSyncBudget[hash] < MASTERNODE_SYNC_THRESHOLD) {
            lastBudgetItem = GetTime();
            mapSeenSyncBudget[hash]++;
        }
    } else {
        lastBudgetItem = GetTime();
        mapSeenSyncBudget.emplace(hash, 1);
    }
}

bool CMasternodeSync::IsBudgetPropEmpty()
{
    return sumBudgetItemProp == 0 && countBudgetItemProp > 0;
}

bool CMasternodeSync::IsBudgetFinEmpty()
{
    return sumBudgetItemFin == 0 && countBudgetItemFin > 0;
}

int CMasternodeSync::GetNextAsset(int currentAsset)
{
    if (currentAsset > MASTERNODE_SYNC_FINISHED) {
        LogPrintf("%s - invalid asset %d\n", __func__, currentAsset);
        return MASTERNODE_SYNC_FAILED;
    }
    switch (currentAsset) {
    case (MASTERNODE_SYNC_INITIAL):
    case (MASTERNODE_SYNC_FAILED):
        return MASTERNODE_SYNC_SPORKS;
    case (MASTERNODE_SYNC_SPORKS):
        return deterministicMNManager->LegacyMNObsolete() ? MASTERNODE_SYNC_BUDGET : MASTERNODE_SYNC_LIST;
    case (MASTERNODE_SYNC_LIST):
        return deterministicMNManager->LegacyMNObsolete() ? MASTERNODE_SYNC_BUDGET : MASTERNODE_SYNC_MNW;
    case (MASTERNODE_SYNC_MNW):
        return MASTERNODE_SYNC_BUDGET;
    case (MASTERNODE_SYNC_BUDGET):
    default:
        return MASTERNODE_SYNC_FINISHED;
    }
}

void CMasternodeSync::SwitchToNextAsset()
{
    const int nextAsset = GetNextAsset(RequestedMasternodeAssets);
    if (nextAsset == MASTERNODE_SYNC_FINISHED) {
        LogPrintf("%s - Sync has finished\n", __func__);
    } else if (nextAsset == MASTERNODE_SYNC_FAILED) {
        ClearFulfilledRequest();
    }
    RequestedMasternodeAssets = nextAsset;
    RequestedMasternodeAttempt = 0;
    nAssetSyncStarted = GetTime();
}

std::string CMasternodeSync::GetSyncStatus()
{
    switch (masternodeSync.RequestedMasternodeAssets) {
    case MASTERNODE_SYNC_INITIAL:
        return _("MNs synchronization pending...");
    case MASTERNODE_SYNC_SPORKS:
        return _("Synchronizing sporks...");
    case MASTERNODE_SYNC_LIST:
        return _("Synchronizing masternodes...");
    case MASTERNODE_SYNC_MNW:
        return _("Synchronizing masternode winners...");
    case MASTERNODE_SYNC_BUDGET:
        return _("Synchronizing budgets...");
    case MASTERNODE_SYNC_FAILED:
        return _("Synchronization failed");
    case MASTERNODE_SYNC_FINISHED:
        return _("Synchronization finished");
    }
    return "";
}

void CMasternodeSync::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (strCommand == NetMsgType::SYNCSTATUSCOUNT) { //Sync status count
        int nItemID;
        int nCount;
        vRecv >> nItemID >> nCount;

        if (RequestedMasternodeAssets >= MASTERNODE_SYNC_FINISHED) return;

        //this means we will receive no further communication
        switch (nItemID) {
        case (MASTERNODE_SYNC_LIST):
            if (nItemID != RequestedMasternodeAssets) return;
            sumMasternodeList += nCount;
            countMasternodeList++;
            break;
        case (MASTERNODE_SYNC_MNW):
            if (nItemID != RequestedMasternodeAssets) return;
            sumMasternodeWinner += nCount;
            countMasternodeWinner++;
            break;
        case (MASTERNODE_SYNC_BUDGET_PROP):
            if (RequestedMasternodeAssets != MASTERNODE_SYNC_BUDGET) return;
            sumBudgetItemProp += nCount;
            countBudgetItemProp++;
            break;
        case (MASTERNODE_SYNC_BUDGET_FIN):
            if (RequestedMasternodeAssets != MASTERNODE_SYNC_BUDGET) return;
            sumBudgetItemFin += nCount;
            countBudgetItemFin++;
            break;
        }

        LogPrint(BCLog::MASTERNODE, "CMasternodeSync:ProcessMessage - ssc - got inventory count %d %d\n", nItemID, nCount);
    }
}

void CMasternodeSync::ClearFulfilledRequest()
{
    g_connman->ForEachNode([](CNode* pnode) {
        pnode->ClearFulfilledRequest("getspork");
        pnode->ClearFulfilledRequest("mnsync");
        pnode->ClearFulfilledRequest("mnwsync");
        pnode->ClearFulfilledRequest("busync");
    });
}

void CMasternodeSync::Process()
{
    static int tick = 0;
    const bool isRegTestNet = Params().IsRegTestNet();

    if (tick++ % MASTERNODE_SYNC_TIMEOUT != 0) return;

    if (IsSynced()) {
        /*
            Resync if we lose all masternodes from sleep/wake or failure to sync originally
        */
        if (mnodeman.CountEnabled() == 0 && !isRegTestNet) {
            Reset();
        } else
            return;
    }

    //try syncing again
    if (RequestedMasternodeAssets == MASTERNODE_SYNC_FAILED && lastFailure + (1 * 60) < GetTime()) {
        Reset();
    } else if (RequestedMasternodeAssets == MASTERNODE_SYNC_FAILED) {
        return;
    }

    if (RequestedMasternodeAssets == MASTERNODE_SYNC_INITIAL) SwitchToNextAsset();

    // sporks synced but blockchain is not, wait until we're almost at a recent block to continue
    if (!IsBlockchainSynced() &&
        RequestedMasternodeAssets > MASTERNODE_SYNC_SPORKS) return;

    // Skip after legacy obsolete. !TODO: remove when transition to DMN is complete
    bool fLegacyMnObsolete = deterministicMNManager->LegacyMNObsolete();

    CMasternodeSync* sync = this;

    // New sync architecture, regtest only for now.
    if (isRegTestNet) {
        g_connman->ForEachNode([sync](CNode* pnode){
            return sync->SyncRegtest(pnode);
        });
        return;
    }

    // Mainnet sync
    g_connman->ForEachNodeContinueIf([sync, fLegacyMnObsolete](CNode* pnode){
        return sync->SyncWithNode(pnode, fLegacyMnObsolete);
    });
}

bool CMasternodeSync::SyncWithNode(CNode* pnode, bool fLegacyMnObsolete)
{
    CNetMsgMaker msgMaker(pnode->GetSendVersion());

    //set to synced
    if (RequestedMasternodeAssets == MASTERNODE_SYNC_SPORKS) {
        if (pnode->HasFulfilledRequest("getspork")) return true;
        pnode->FulfilledRequest("getspork");

        g_connman->PushMessage(pnode, msgMaker.Make(NetMsgType::GETSPORKS)); //get current network sporks
        if (RequestedMasternodeAttempt >= 2) SwitchToNextAsset();
        RequestedMasternodeAttempt++;
        return false;
    }

    if (pnode->nVersion >= ActiveProtocol()) {
        if (RequestedMasternodeAssets == MASTERNODE_SYNC_LIST) {
            if (fLegacyMnObsolete) {
                SwitchToNextAsset();
                return false;
            }

            LogPrint(BCLog::MASTERNODE, "CMasternodeSync::Process() - lastMasternodeList %lld (GetTime() - MASTERNODE_SYNC_TIMEOUT) %lld\n", lastMasternodeList, GetTime() - MASTERNODE_SYNC_TIMEOUT);
            if (lastMasternodeList > 0 && lastMasternodeList < GetTime() - MASTERNODE_SYNC_TIMEOUT * 2 && RequestedMasternodeAttempt >= MASTERNODE_SYNC_THRESHOLD) { //hasn't received a new item in the last five seconds, so we'll move to the
                SwitchToNextAsset();
                return false;
            }

            if (pnode->HasFulfilledRequest("mnsync")) return true;
            pnode->FulfilledRequest("mnsync");

            // timeout
            if (lastMasternodeList == 0 &&
                (RequestedMasternodeAttempt >= MASTERNODE_SYNC_THRESHOLD * 3 || GetTime() - nAssetSyncStarted > MASTERNODE_SYNC_TIMEOUT * 5)) {
                if (sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT)) {
                    LogPrintf("CMasternodeSync::Process - ERROR - Sync has failed on %s, will retry later\n", "MASTERNODE_SYNC_LIST");
                    RequestedMasternodeAssets = MASTERNODE_SYNC_FAILED;
                    RequestedMasternodeAttempt = 0;
                    lastFailure = GetTime();
                    nCountFailures++;
                } else {
                    SwitchToNextAsset();
                }
                return false;
            }

            if (RequestedMasternodeAttempt >= MASTERNODE_SYNC_THRESHOLD * 3) return false;

            mnodeman.DsegUpdate(pnode);
            RequestedMasternodeAttempt++;
            return false;
        }

        if (RequestedMasternodeAssets == MASTERNODE_SYNC_MNW) {
            if (fLegacyMnObsolete) {
                SwitchToNextAsset();
                return false;
            }

            if (lastMasternodeWinner > 0 && lastMasternodeWinner < GetTime() - MASTERNODE_SYNC_TIMEOUT * 2 && RequestedMasternodeAttempt >= MASTERNODE_SYNC_THRESHOLD) { //hasn't received a new item in the last five seconds, so we'll move to the
                SwitchToNextAsset();
                return false;
            }

            if (pnode->HasFulfilledRequest("mnwsync")) return true;
            pnode->FulfilledRequest("mnwsync");

            // timeout
            if (lastMasternodeWinner == 0 &&
                (RequestedMasternodeAttempt >= MASTERNODE_SYNC_THRESHOLD * 3 || GetTime() - nAssetSyncStarted > MASTERNODE_SYNC_TIMEOUT * 5)) {
                if (sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT)) {
                    LogPrintf("CMasternodeSync::Process - ERROR - Sync has failed on %s, will retry later\n", "MASTERNODE_SYNC_MNW");
                    RequestedMasternodeAssets = MASTERNODE_SYNC_FAILED;
                    RequestedMasternodeAttempt = 0;
                    lastFailure = GetTime();
                    nCountFailures++;
                } else {
                    SwitchToNextAsset();
                }
                return false;
            }

            if (RequestedMasternodeAttempt >= MASTERNODE_SYNC_THRESHOLD * 3) return false;

            int nMnCount = mnodeman.CountEnabled();
            g_connman->PushMessage(pnode, msgMaker.Make(NetMsgType::GETMNWINNERS, nMnCount)); //sync payees
            RequestedMasternodeAttempt++;
            return false;
        }

        if (RequestedMasternodeAssets == MASTERNODE_SYNC_BUDGET) {
            // We'll start rejecting votes if we accidentally get set as synced too soon
            if (lastBudgetItem > 0 && lastBudgetItem < GetTime() - MASTERNODE_SYNC_TIMEOUT * 2 && RequestedMasternodeAttempt >= MASTERNODE_SYNC_THRESHOLD) {
                // Hasn't received a new item in the last five seconds, so we'll move to the
                SwitchToNextAsset();

                // Try to activate our masternode if possible
                activeMasternode.ManageStatus();
                return false;
            }

            // timeout
            if (lastBudgetItem == 0 &&
                (RequestedMasternodeAttempt >= MASTERNODE_SYNC_THRESHOLD * 3 || GetTime() - nAssetSyncStarted > MASTERNODE_SYNC_TIMEOUT * 5)) {
                // maybe there is no budgets at all, so just finish syncing
                SwitchToNextAsset();
                activeMasternode.ManageStatus();
                return false;
            }

            if (pnode->HasFulfilledRequest("busync")) return true;
            pnode->FulfilledRequest("busync");

            if (RequestedMasternodeAttempt >= MASTERNODE_SYNC_THRESHOLD * 3) return false;

            uint256 n;
            g_connman->PushMessage(pnode, msgMaker.Make(NetMsgType::BUDGETVOTESYNC, n)); //sync masternode votes
            RequestedMasternodeAttempt++;
            return false;
        }
    }

    return true;
}


