// Copyright (c) 2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef TIERTWO_NETWORKSYNC_H
#define TIERTWO_NETWORKSYNC_H

#include "net.h" // for NodeId

#include <map>

class CMasternodeSync;

struct PeerMonitor {
    // Number of already seen messages received by the peer
    int seenMessagesCount{0};
};

class TierTwoPeerData {
public:
    explicit TierTwoPeerData() {};
    // map of message --> last request timestamp, bool hasResponseArrived.
    std::map<const char*, std::pair<int64_t, bool>> mapMsgData;

    int AddSeenMessagesCount() {
        return WITH_LOCK(cs_monitor, return ++monitor.seenMessagesCount; );
    }

    PeerMonitor GetPeerMonitor() { return WITH_LOCK(cs_monitor, return monitor; ); }
private:
    PeerMonitor monitor;
    Mutex cs_monitor;

};

template <typename T>
class ProtectedVector : public std::vector<T>
{
private:
    Mutex cs;

    bool has(const T& t) {
        return std::any_of(this->begin(), this->end(), [t](const T& _t){ return t == _t; } );
    }

public:
    // returns false if the item is already on the vector
    bool tryAppendItem(const T& t) {
        LOCK(cs);
        if (has(t)) return false;
        this->emplace_back(t);
        return true;
    }
    bool contains(const T& t) {
        return WITH_LOCK(cs, return has(t););
    }

    template<typename Callable>
    void ForEachItem(Callable&& func)
    {
        LOCK(cs);
        for (auto it = this->begin(); it != this->end(); ++it) {
            func(*it);
        }
    };
};

// Class in charge of managing the tier two synchronization.
class TierTwoSyncMan
{
private:
    // Old sync class, needed for now.
    CMasternodeSync* syncParent{nullptr};

    // Sync node state
    // map of nodeID --> TierTwoPeerData
    std::map<NodeId, TierTwoPeerData> peersSyncState;
    Mutex cs_peersSyncState;
    // Dead peers that requires an state cleanup.
    ProtectedVector<NodeId> peersToRemove;

    template <typename... Args>
    void RequestDataTo(CNode* pnode, const char* msg, bool forceRequest, Args&&... args);

    template <typename... Args>
    void PushMessage(CNode* pnode, const char* msg, Args&&... args);

    // update peer sync state data
    bool UpdatePeerSyncState(const NodeId& id, const char* msg, const int nextSyncStatus);

    // Check if an update is needed
    void CheckAndUpdateSyncStatus();
    int AddSeenMessageCount(CNode* pfrom);

    ProtectedVector<uint256>* GetSeenItemsVector(int type);

    // DoS spam filtering protection, guarded by its own internal mutex.
    // For now, it only protects us from seen proposals and budgets.
    ProtectedVector<uint256> seenBudgetItems;
    ProtectedVector<uint256> seenProposalsItems;

public:

    explicit TierTwoSyncMan(CMasternodeSync* _syncParent) : syncParent(_syncParent) {}

    // Sync message dispatcher
    bool MessageDispatcher(CNode* pfrom, std::string& strCommand, CDataStream& vRecv);

    void Process();
    void CleanupPeers();
    void SyncRegtest(CNode* pnode);
    void PeerFinalized(NodeId nodeid) { peersToRemove.tryAppendItem(nodeid); };
    // todo: Connect me..
    void RegisterNodeSignals(CNodeSignals& nodeSignals);
    void UnregisterNodeSignals(CNodeSignals& nodeSignals);

    // Check if we already have seen the item
    bool AlreadyHave(const uint256& hash, int type);
    bool TryAppendItem(const uint256& hash, int type);

};

#endif //TIERTWO_NETWORKSYNC_H
