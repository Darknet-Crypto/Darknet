// Copyright (c) 2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "tiertwo_networksync.h"

#include "spork.h"  // for sporkManager
#include "masternode-sync.h"
#include "masternodeman.h" // for mnodeman
#include "masternode-budget.h"
#include "netmessagemaker.h"
#include "streams.h"  // for CDataStream

#define SEEN_MESSAGE_SPAM_THRESHOLD 10

// Update in-flight message status if needed
bool TierTwoSyncMan::UpdatePeerSyncState(const NodeId& id, const char* msg, const int nextSyncStatus)
{
    LOCK(cs_peersSyncState);
    auto it = peersSyncState.find(id);
    if (it != peersSyncState.end()) {
        auto& peerData = it->second;
        const auto& msgMapIt = peerData.mapMsgData.find(msg);
        if (msgMapIt != peerData.mapMsgData.end()) {
            // exists, let's update the received status and the sync state.

            // future: these boolean will not be needed once the peer syncState status gets implemented.
            msgMapIt->second.second = true;
            LogPrintf("%s: %s message updated peer sync state\n", __func__, msgMapIt->first);

            // todo: this should only happen if more than N peers have sent the data.
            // move overall tier two sync state to the next one if needed.
            syncParent->RequestedMasternodeAssets = nextSyncStatus;
            return true;
        }
    }
    return false;
}

int TierTwoSyncMan::AddSeenMessageCount(CNode* pfrom)
{
    AssertLockNotHeld(cs_peersSyncState);
    LOCK(cs_peersSyncState);
    const auto& it = peersSyncState.find(pfrom->GetId());
    return it != peersSyncState.end() ?
           it->second.AddSeenMessagesCount() : 0;
}

bool TierTwoSyncMan::MessageDispatcher(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (strCommand == NetMsgType::GETSPORKS) {
        // send sporks
        sporkManager.ProcessGetSporks(pfrom, strCommand, vRecv);
        return true;
    }

    if (strCommand == NetMsgType::GETMNLIST) {
        // Get Masternode list or specific entry
        mnodeman.ProcessGetMNList(pfrom, strCommand, vRecv);
        return true;
    }

    if (strCommand == NetMsgType::SPORK) {
        // as there is no completion message, this is using a SPORK_INVALID as final message for now.
        // which is just a hack, should be replaced with another message, guard it until the protocol gets deployed on mainnet and
        // add compatibility with the previous protocol as well.
        sporkManager.ProcessSporkMsg(pfrom, strCommand, vRecv);
        // Update in-flight message status if needed
        UpdatePeerSyncState(pfrom->GetId(), NetMsgType::GETSPORKS, MASTERNODE_SYNC_LIST);
        return true;
    }

    if (strCommand == NetMsgType::SYNCSTATUSCOUNT) {
        // Nothing to do.
        if (syncParent->RequestedMasternodeAssets >= MASTERNODE_SYNC_FINISHED) return true;

        // Sync status count
        int nItemID;
        int nCount;
        vRecv >> nItemID >> nCount;

        // this means we will receive no further communication on the first sync
        switch (nItemID) {
            case MASTERNODE_SYNC_LIST: {
                UpdatePeerSyncState(pfrom->GetId(), NetMsgType::GETMNLIST, MASTERNODE_SYNC_MNW);
                return true;
            }
            case MASTERNODE_SYNC_MNW: {
                UpdatePeerSyncState(pfrom->GetId(), NetMsgType::GETMNWINNERS, MASTERNODE_SYNC_BUDGET);
                return true;
            }
            case MASTERNODE_SYNC_BUDGET_PROP: {
                // TODO: This could be a MASTERNODE_SYNC_BUDGET_FIN as well, possibly should decouple the finalization budget sync
                //  from the MASTERNODE_SYNC_BUDGET_PROP (both are under the BUDGETVOTESYNC message)
                UpdatePeerSyncState(pfrom->GetId(), NetMsgType::BUDGETVOTESYNC, MASTERNODE_SYNC_FINISHED);
                LogPrintf("SYNC FINISHED!\n");
                return true;
            }
        }
    }

    if (strCommand == NetMsgType::BUDGETPROPOSAL) {
        // Masternode Proposal
        CBudgetProposal proposal;
        if (!proposal.LoadBroadcast(vRecv)) {
            // !TODO: we should probably call misbehaving here
            return false;
        }

        // Check if we already have seen this proposal
        if (!seenProposalsItems.tryAppendItem(proposal.GetHash())) {
            int seenMessagesCount = AddSeenMessageCount(pfrom);
            if (seenMessagesCount > SEEN_MESSAGE_SPAM_THRESHOLD) {
                // todo: add misbehaving if the peer sent this message several times already..
            }
            return false;
        }

        // todo: add misbevahing..
        budget.ProcessProposalMsg(proposal);
    }

    if (strCommand == NetMsgType::FINALBUDGET) {
        CFinalizedBudget finalbudget;
        if (!finalbudget.LoadBroadcast(vRecv)) {
            // !TODO: we should probably call misbehaving here
            return false;
        }

        // Check if we already have seen this budget finalization
        if (!seenBudgetItems.tryAppendItem(finalbudget.GetHash())) {
            int seenMessagesCount = AddSeenMessageCount(pfrom);
            if (seenMessagesCount > SEEN_MESSAGE_SPAM_THRESHOLD) {
                // todo: add misbehaving if the peer sent this message several times already..
            }
        }

        // todo: add misbevahing..
        budget.ProcessBudgetFinalizationMsg(finalbudget);
    }

    return false;
}

template <typename... Args>
void TierTwoSyncMan::PushMessage(CNode* pnode, const char* msg, Args&&... args)
{
    g_connman->PushMessage(pnode, CNetMsgMaker(pnode->GetSendVersion()).Make(msg, std::forward<Args>(args)...));
}

template <typename... Args>
void TierTwoSyncMan::RequestDataTo(CNode* pnode, const char* msg, bool forceRequest, Args&&... args)
{
    AssertLockNotHeld(cs_peersSyncState);
    bool exist = WITH_LOCK(cs_peersSyncState, return peersSyncState.count(pnode->id));
    if (!exist || forceRequest) {
        // send the message
        PushMessage(pnode, msg, std::forward<Args>(args)...);

        // Update peer sync state
        LOCK(cs_peersSyncState);
        // Erase it if this is a forced request. todo: check this again.. should be removing the entire peer state.
        if (exist) peersSyncState.erase(pnode->id);

        // Add data to the tier two peers sync state
        const auto& it = peersSyncState.emplace(std::piecewise_construct,
                               std::forward_as_tuple(pnode->id), std::forward_as_tuple());
        it.first->second.mapMsgData.emplace(msg, std::make_pair(GetTime(), false));
        return;
    } else {
        bool reRequestData = false;
        {
            LOCK(cs_peersSyncState);
            TierTwoPeerData& peerData = peersSyncState.at(pnode->id);
            const auto& msgMapIt = peerData.mapMsgData.find(msg);

            // Check if we have sent the message or not
            if (msgMapIt == peerData.mapMsgData.end()) {
                // message doesn't exist, push it and add it to the map.
                PushMessage(pnode, msg, std::forward<Args>(args)...);
                peerData.mapMsgData.emplace(msg, std::make_pair(GetTime(), false));
            } else {
                // message sent, next step: need to check if it was already answered or not.
                // And, if needed, request it again every certain amount of time.

                // Check if the node answered the message or not
                if (!msgMapIt->second.second) {
                    int64_t lastRequestTime = msgMapIt->second.first;
                    if (lastRequestTime + 600 < GetTime()) {
                        reRequestData = true;
                    }
                }
            }
        }

        // ten minutes passed. Let's ask it again.
        if (reRequestData) {
            RequestDataTo(pnode, msg, true, std::forward<Args>(args)...);
        }
    }
}

void TierTwoSyncMan::Process()
{
    // First cleanup old peers
    CleanupPeers();

    // Try to sync
    auto sync = this;
    g_connman->ForEachNode([sync](CNode* pnode){
        return sync->SyncRegtest(pnode);
    });
}

void TierTwoSyncMan::CleanupPeers()
{
    auto sync = this;
    peersToRemove.ForEachItem([sync](NodeId id){
        LOCK(sync->cs_peersSyncState);
        sync->peersSyncState.erase(id);
    });
}

void TierTwoSyncMan::SyncRegtest(CNode* pnode)
{
    // Initial sync, verify that the other peer answered to all of the messages successfully
    if (syncParent->RequestedMasternodeAssets == MASTERNODE_SYNC_SPORKS) {
        RequestDataTo(pnode, NetMsgType::GETSPORKS, false);
    } else if (syncParent->RequestedMasternodeAssets == MASTERNODE_SYNC_LIST) {
        RequestDataTo(pnode, NetMsgType::GETMNLIST, false, CTxIn());
    } else if (syncParent->RequestedMasternodeAssets == MASTERNODE_SYNC_MNW) {
        RequestDataTo(pnode, NetMsgType::GETMNWINNERS, false, mnodeman.CountEnabled());
    } else if (syncParent->RequestedMasternodeAssets == MASTERNODE_SYNC_BUDGET) {
        // sync masternode votes
        RequestDataTo(pnode, NetMsgType::BUDGETVOTESYNC, false, uint256());
    } else if (syncParent->RequestedMasternodeAssets == MASTERNODE_SYNC_FINISHED) {
        LogPrintf("REGTEST SYNC FINISHED!\n");
    }
}

void FinalizeNode(TierTwoSyncMan* syncMan, NodeId nodeid, bool& fUpdateConnectionTime)
{
    syncMan->PeerFinalized(nodeid);
}

void TierTwoSyncMan::RegisterNodeSignals(CNodeSignals& nodeSignals)
{
    nodeSignals.FinalizeNode.connect(boost::bind(FinalizeNode, this, _1, _2));
}

void TierTwoSyncMan::UnregisterNodeSignals(CNodeSignals& nodeSignals)
{
    nodeSignals.FinalizeNode.disconnect(boost::bind(FinalizeNode, this, _1, _2));
}

bool TierTwoSyncMan::AlreadyHave(const uint256& hash, int type)
{
    return GetSeenItemsVector(type)->contains(hash);
}

// TODO: Connect me
bool TierTwoSyncMan::TryAppendItem(const uint256& hash, int type)
{
    return GetSeenItemsVector(type)->tryAppendItem(hash);
}

ProtectedVector<uint256>* TierTwoSyncMan::GetSeenItemsVector(int type)
{
    switch (type) {
        case MSG_BUDGET_PROPOSAL:
            return &seenProposalsItems;
        case MSG_BUDGET_FINALIZED:
            return &seenBudgetItems;
        default:
            throw std::runtime_error("Invalid type");
    }
}

