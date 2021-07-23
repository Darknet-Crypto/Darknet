// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NET_H
#define BITCOIN_NET_H

#include "addrdb.h"
#include "addrman.h"
#include "bloom.h"
#include "compat.h"
#include "fs.h"
#include "hash.h"
#include "limitedmap.h"
#include "netaddress.h"
#include "protocol.h"
#include "random.h"
#include "streams.h"
#include "sync.h"
#include "uint256.h"
#include "utilstrencodings.h"
#include "threadinterrupt.h"

#include <atomic>
#include <deque>
#include <stdint.h>
#include <thread>
#include <memory>
#include <condition_variable>

#ifndef WIN32
#include <arpa/inet.h>
#endif

class CAddrMan;
class CBlockIndex;
class CScheduler;
class CNode;

/** Time between pings automatically sent out for latency probing and keepalive (in seconds). */
static const int PING_INTERVAL = 2 * 60;
/** Time after which to disconnect, after waiting for a ping response (or inactivity). */
static const int TIMEOUT_INTERVAL = 20 * 60;
/** Run the feeler connection loop once every 2 minutes or 120 seconds. **/
static const int FEELER_INTERVAL = 120;
/** The maximum number of entries in an 'inv' protocol message */
static const unsigned int MAX_INV_SZ = 50000;
/** The maximum number of entries in a locator */
static const unsigned int MAX_LOCATOR_SZ = 101;
/** The maximum number of new addresses to accumulate before announcing. */
static const unsigned int MAX_ADDR_TO_SEND = 1000;
/** Maximum length of incoming protocol messages (no message over 2 MiB is currently acceptable). */
static const unsigned int MAX_PROTOCOL_MESSAGE_LENGTH = 2 * 1024 * 1024;
/** Maximum length of strSubVer in `version` message */
static const unsigned int MAX_SUBVERSION_LENGTH = 256;
/** Maximum number of outgoing nodes */
static const int MAX_OUTBOUND_CONNECTIONS = 16;
/** -listen default */
static const bool DEFAULT_LISTEN = true;
/** The maximum number of entries in mapAskFor */
static const size_t MAPASKFOR_MAX_SZ = MAX_INV_SZ;
/** The maximum number of entries in setAskFor (larger due to getdata latency)*/
static const size_t SETASKFOR_MAX_SZ = 2 * MAX_INV_SZ;
/** The maximum number of peer connections to maintain. */
static const unsigned int DEFAULT_MAX_PEER_CONNECTIONS = 125;
/** Disconnected peers are added to setOffsetDisconnectedPeers only if node has less than ENOUGH_CONNECTIONS */
#define ENOUGH_CONNECTIONS 2
/** Maximum number of peers added to setOffsetDisconnectedPeers before triggering a warning */
#define MAX_TIMEOFFSET_DISCONNECTIONS 16

static const ServiceFlags REQUIRED_SERVICES = NODE_NETWORK;

static const bool DEFAULT_FORCEDNSSEED = false;
static const size_t DEFAULT_MAXRECEIVEBUFFER = 5 * 1000;
static const size_t DEFAULT_MAXSENDBUFFER    = 1 * 1000;

// NOTE: When adjusting this, update rpcnet:setban's help ("24h")
static const unsigned int DEFAULT_MISBEHAVING_BANTIME = 60 * 60 * 24;  // Default 24-hour ban

typedef int NodeId;

struct AddedNodeInfo
{
    std::string strAddedNode;
    CService resolvedAddress;
    bool fConnected;
    bool fInbound;

    AddedNodeInfo(const std::string& _strAddedNode, const CService& _resolvedAddress, bool _fConnected, bool _fInbound):
        strAddedNode(_strAddedNode),
        resolvedAddress(_resolvedAddress),
        fConnected(_fConnected),
        fInbound(_fInbound)
    {}
};

class CTransaction;
class CNodeStats;
class CClientUIInterface;

struct CSerializedNetMsg
{
    CSerializedNetMsg() = default;
    CSerializedNetMsg(CSerializedNetMsg&&) = default;
    CSerializedNetMsg& operator=(CSerializedNetMsg&&) = default;
    // No copying, only moves.
    CSerializedNetMsg(const CSerializedNetMsg& msg) = delete;
    CSerializedNetMsg& operator=(const CSerializedNetMsg&) = delete;

    std::vector<unsigned char> data;
    std::string command;
};

class NetEventsInterface;
class CConnman
{
public:

    enum NumConnections {
        CONNECTIONS_NONE = 0,
        CONNECTIONS_IN = (1U << 0),
        CONNECTIONS_OUT = (1U << 1),
        CONNECTIONS_ALL = (CONNECTIONS_IN | CONNECTIONS_OUT),
    };

    struct Options
    {
        ServiceFlags nLocalServices = NODE_NONE;
        ServiceFlags nRelevantServices = NODE_NONE;
        int nMaxConnections = 0;
        int nMaxOutbound = 0;
        int nMaxFeeler = 0;
        int nBestHeight = 0;
        CClientUIInterface* uiInterface = nullptr;
        NetEventsInterface* m_msgproc = nullptr;
        unsigned int nSendBufferMaxSize = 0;
        unsigned int nReceiveFloodSize = 0;
    };
    CConnman(uint64_t seed0, uint64_t seed1);
    ~CConnman();
    bool Start(CScheduler& scheduler, std::string& strNodeError, Options options);
    void Stop();
    void Interrupt();
    bool BindListenPort(const CService &bindAddr, std::string& strError, bool fWhitelisted = false);
    bool OpenNetworkConnection(const CAddress& addrConnect, bool fCountFailure, CSemaphoreGrant* grantOutbound = NULL, const char* strDest = NULL, bool fOneShot = false, bool fFeeler = false);
    bool CheckIncomingNonce(uint64_t nonce);

    bool ForNode(NodeId id, std::function<bool(CNode* pnode)> func);

    void PushMessage(CNode* pnode, CSerializedNetMsg&& msg);

    template<typename Callable>
    bool ForEachNodeContinueIf(Callable&& func)
    {
        LOCK(cs_vNodes);
        for (auto&& node : vNodes)
            if (NodeFullyConnected(node)) {
                if (!func(node))
                    return false;
            }
        return true;
    };

    template<typename Callable>
    void ForEachNode(Callable&& func)
    {
        LOCK(cs_vNodes);
        for (auto&& node : vNodes) {
            if (NodeFullyConnected(node))
                func(node);
        }
    };

    template<typename Callable>
    void ForEachNode(Callable&& func) const
    {
        LOCK(cs_vNodes);
        for (auto&& node : vNodes) {
            if (NodeFullyConnected(node))
                func(node);
        }
    };

    template<typename Callable, typename CallableAfter>
    void ForEachNodeThen(Callable&& pre, CallableAfter&& post)
    {
        LOCK(cs_vNodes);
        for (auto&& node : vNodes) {
            if (NodeFullyConnected(node))
                pre(node);
        }
        post();
    };

    template<typename Callable, typename CallableAfter>
    void ForEachNodeThen(Callable&& pre, CallableAfter&& post) const
    {
        LOCK(cs_vNodes);
        for (auto&& node : vNodes) {
            if (NodeFullyConnected(node))
                pre(node);
        }
        post();
    };

    void RelayInv(CInv& inv);
    bool IsNodeConnected(const CAddress& addr);
    // Retrieves a connected peer (if connection success). Used only to check peer address availability for now.
    CNode* ConnectNode(CAddress addrConnect);

    // Addrman functions
    size_t GetAddressCount() const;
    void SetServices(const CService &addr, ServiceFlags nServices);
    void MarkAddressGood(const CAddress& addr);
    void AddNewAddress(const CAddress& addr, const CAddress& addrFrom, int64_t nTimePenalty = 0);
    void AddNewAddresses(const std::vector<CAddress>& vAddr, const CAddress& addrFrom, int64_t nTimePenalty = 0);
    std::vector<CAddress> GetAddresses();

    // Denial-of-service detection/prevention
    // The idea is to detect peers that are behaving
    // badly and disconnect/ban them, but do it in a
    // one-coding-mistake-won't-shatter-the-entire-network
    // way.
    // IMPORTANT:  There should be nothing I can give a
    // node that it will forward on that will make that
    // node's peers drop it. If there is, an attacker
    // can isolate a node and/or try to split the network.
    // Dropping a node for sending stuff that is invalid
    // now but might be valid in a later version is also
    // dangerous, because it can cause a network split
    // between nodes running old code and nodes running
    // new code.
    void Ban(const CNetAddr& netAddr, const BanReason& reason, int64_t bantimeoffset = 0, bool sinceUnixEpoch = false);
    void Ban(const CSubNet& subNet, const BanReason& reason, int64_t bantimeoffset = 0, bool sinceUnixEpoch = false);
    void ClearBanned(); // needed for unit testing
    bool IsBanned(CNetAddr ip);
    bool IsBanned(CSubNet subnet);
    bool Unban(const CNetAddr &ip);
    bool Unban(const CSubNet &ip);
    void GetBanned(banmap_t &banmap);
    void SetBanned(const banmap_t &banmap);

    void AddOneShot(const std::string& strDest);

    bool AddNode(const std::string& node);
    bool RemoveAddedNode(const std::string& node);
    std::vector<AddedNodeInfo> GetAddedNodeInfo();

    size_t GetNodeCount(NumConnections num);
    void GetNodeStats(std::vector<CNodeStats>& vstats);
    bool DisconnectNode(const std::string& node);
    bool DisconnectNode(NodeId id);

    unsigned int GetSendBufferSize() const;

    void AddWhitelistedRange(const CSubNet& subnet);

    ServiceFlags GetLocalServices() const;

    uint64_t GetTotalBytesRecv();
    uint64_t GetTotalBytesSent();

    void SetBestHeight(int height);
    int GetBestHeight() const;

    /** Get a unique deterministic randomizer. */
    CSipHasher GetDeterministicRandomizer(uint64_t id);

    unsigned int GetReceiveFloodSize() const;
private:
    struct ListenSocket {
        SOCKET socket;
        bool whitelisted;

        ListenSocket(SOCKET socket_, bool whitelisted_) : socket(socket_), whitelisted(whitelisted_) {}
    };

    void ThreadOpenAddedConnections();
    void ProcessOneShot();
    void ThreadOpenConnections();
    void ThreadMessageHandler();
    void AcceptConnection(const ListenSocket& hListenSocket);
    void ThreadSocketHandler();
    void ThreadDNSAddressSeed();

    void WakeMessageHandler();

    uint64_t CalculateKeyedNetGroup(const CAddress& ad);

    CNode* FindNode(const CNetAddr& ip);
    CNode* FindNode(const CSubNet& subNet);
    CNode* FindNode(const std::string& addrName);
    CNode* FindNode(const CService& addr);

    bool AttemptToEvictConnection(bool fPreferNewConnection);
    CNode* ConnectNode(CAddress addrConnect, const char* pszDest, bool fCountFailure);
    bool IsWhitelistedRange(const CNetAddr &addr);

    void DeleteNode(CNode* pnode);

    NodeId GetNewNodeId();

    size_t SocketSendData(CNode *pnode);
    //!check is the banlist has unwritten changes
    bool BannedSetIsDirty();
    //!set the "dirty" flag for the banlist
    void SetBannedSetDirty(bool dirty=true);
    //!clean unused entries (if bantime has expired)
    void SweepBanned();
    void DumpAddresses();
    void DumpData();
    void DumpBanlist();

    // Network stats
    void RecordBytesRecv(uint64_t bytes);
    void RecordBytesSent(uint64_t bytes);

    // Whether the node should be passed out in ForEach* callbacks
    static bool NodeFullyConnected(const CNode* pnode);

    // Network usage totals
    RecursiveMutex cs_totalBytesRecv;
    RecursiveMutex cs_totalBytesSent;
    uint64_t nTotalBytesRecv{0};
    uint64_t nTotalBytesSent{0};

    // Whitelisted ranges. Any node connecting from these is automatically
    // whitelisted (as well as those connecting to whitelisted binds).
    std::vector<CSubNet> vWhitelistedRange;
    RecursiveMutex cs_vWhitelistedRange;

    unsigned int nSendBufferMaxSize{0};
    unsigned int nReceiveFloodSize{0};

    std::vector<ListenSocket> vhListenSocket;
    banmap_t setBanned;
    RecursiveMutex cs_setBanned;
    bool setBannedIsDirty{false};
    bool fAddressesInitialized{false};
    CAddrMan addrman;
    std::deque<std::string> vOneShots;
    RecursiveMutex cs_vOneShots;
    std::vector<std::string> vAddedNodes;
    RecursiveMutex cs_vAddedNodes;
    std::vector<CNode*> vNodes;
    std::list<CNode*> vNodesDisconnected;
    mutable RecursiveMutex cs_vNodes;
    std::atomic<NodeId> nLastNodeId;

    /** Services this instance offers */
    ServiceFlags nLocalServices{NODE_NONE};

    /** Services this instance cares about */
    ServiceFlags nRelevantServices{NODE_NONE};

    CSemaphore *semOutbound{nullptr};
    int nMaxConnections{0};
    int nMaxOutbound{0};
    int nMaxFeeler{0};
    std::atomic<int> nBestHeight;
    CClientUIInterface* clientInterface{nullptr};
    NetEventsInterface* m_msgproc{nullptr};

    /** SipHasher seeds for deterministic randomness */
    const uint64_t nSeed0{0}, nSeed1{0};

    /** flag for waking the message processor. */
    bool fMsgProcWake{false};

    std::condition_variable condMsgProc;
    std::mutex mutexMsgProc;
    std::atomic<bool> flagInterruptMsgProc;

    CThreadInterrupt interruptNet;

    std::thread threadDNSAddressSeed;
    std::thread threadSocketHandler;
    std::thread threadOpenAddedConnections;
    std::thread threadOpenConnections;
    std::thread threadMessageHandler;
};
extern std::unique_ptr<CConnman> g_connman;
void Discover();
unsigned short GetListenPort();
bool BindListenPort(const CService& bindAddr, std::string& strError, bool fWhitelisted = false);
void CheckOffsetDisconnectedPeers(const CNetAddr& ip);

struct CombinerAll {
    typedef bool result_type;

    template <typename I>
    bool operator()(I first, I last) const
    {
        while (first != last) {
            if (!(*first)) return false;
            ++first;
        }
        return true;
    }
};

enum {
    LOCAL_NONE,   // unknown
    LOCAL_IF,     // address a local interface listens on
    LOCAL_BIND,   // address explicit bound to
    LOCAL_MAPPED, // address reported by UPnP or NAT-PMP
    LOCAL_MANUAL, // address explicitly specified (-externalip=)

    LOCAL_MAX
};

bool IsPeerAddrLocalGood(CNode* pnode);
void AdvertiseLocal(CNode* pnode);
void SetLimited(enum Network net, bool fLimited = true);
bool IsLimited(enum Network net);
bool IsLimited(const CNetAddr& addr);
bool AddLocal(const CService& addr, int nScore = LOCAL_NONE);
bool AddLocal(const CNetAddr& addr, int nScore = LOCAL_NONE);
bool RemoveLocal(const CService& addr);
bool SeenLocal(const CService& addr);
bool IsLocal(const CService& addr);
bool GetLocal(CService& addr, const CNetAddr* paddrPeer = NULL);
bool IsReachable(enum Network net);
bool IsReachable(const CNetAddr& addr);
CAddress GetLocalAddress(const CNetAddr* paddrPeer, ServiceFlags nLocalServices);

bool validateMasternodeIP(const std::string& addrStr);          // valid, reachable and routable address


extern bool fDiscover;
extern bool fListen;

extern limitedmap<CInv, int64_t> mapAlreadyAskedFor;

/** Subversion as sent to the P2P network in `version` messages */
extern std::string strSubVersion;

struct LocalServiceInfo {
    int nScore;
    int nPort;
};

extern RecursiveMutex cs_mapLocalHost;
extern std::map<CNetAddr, LocalServiceInfo> mapLocalHost;
typedef std::map<std::string, uint64_t> mapMsgCmdSize; //command, total bytes

class CNodeStats
{
public:
    NodeId nodeid;
    ServiceFlags nServices;
    int64_t nLastSend;
    int64_t nLastRecv;
    int64_t nTimeConnected;
    int64_t nTimeOffset;
    std::string addrName;
    int nVersion;
    std::string cleanSubVer;
    bool fInbound;
    int nStartingHeight;
    uint64_t nSendBytes;
    mapMsgCmdSize mapSendBytesPerMsgCmd;
    uint64_t nRecvBytes;
    mapMsgCmdSize mapRecvBytesPerMsgCmd;
    bool fWhitelisted;
    double dPingTime;
    double dPingWait;
    std::string addrLocal;
};


class CNetMessage
{
private:
    mutable CHash256 hasher;
    mutable uint256 data_hash;
public:
    bool in_data; // parsing header (false) or data (true)

    CDataStream hdrbuf; // partially received header
    CMessageHeader hdr; // complete header
    unsigned int nHdrPos;

    CDataStream vRecv; // received message data
    unsigned int nDataPos;

    int64_t nTime; // time (in microseconds) of message receipt.

    CNetMessage(const CMessageHeader::MessageStartChars& pchMessageStartIn, int nTypeIn, int nVersionIn) : hdrbuf(nTypeIn, nVersionIn), hdr(pchMessageStartIn), vRecv(nTypeIn, nVersionIn) {
        hdrbuf.resize(24);
        in_data = false;
        nHdrPos = 0;
        nDataPos = 0;
        nTime = 0;
    }

    bool complete() const
    {
        if (!in_data)
            return false;
        return (hdr.nMessageSize == nDataPos);
    }

    const uint256& GetMessageHash() const;

    void SetVersion(int nVersionIn)
    {
        hdrbuf.SetVersion(nVersionIn);
        vRecv.SetVersion(nVersionIn);
    }

    int readHeader(const char* pch, unsigned int nBytes);
    int readData(const char* pch, unsigned int nBytes);
};


/** Information about a peer */
class CNode
{
    friend class CConnman;
public:
    // socket
    std::atomic<ServiceFlags> nServices;
    ServiceFlags nServicesExpected;
    SOCKET hSocket;
    size_t nSendSize;   // total size of all vSendMsg entries
    size_t nSendOffset; // offset inside the first vSendMsg already sent
    uint64_t nSendBytes;
    std::deque<std::vector<unsigned char>> vSendMsg;
    RecursiveMutex cs_vSend;
    RecursiveMutex cs_hSocket;
    RecursiveMutex cs_vRecv;

    RecursiveMutex cs_vProcessMsg;
    std::list<CNetMessage> vProcessMsg;
    size_t nProcessQueueSize;

    RecursiveMutex cs_sendProcessing;

    std::deque<CInv> vRecvGetData;
    uint64_t nRecvBytes;
    std::atomic<int> nRecvVersion;

    std::atomic<int64_t> nLastSend;
    std::atomic<int64_t> nLastRecv;
    const int64_t nTimeConnected;
    std::atomic<int64_t> nTimeOffset;
    const CAddress addr;
    std::atomic<int> nVersion;
    // strSubVer is whatever byte array we read from the wire. However, this field is intended
    // to be printed out, displayed to humans in various forms and so on. So we sanitize it and
    // store the sanitized version in cleanSubVer. The original should be used when dealing with
    // the network or wire types and the cleaned string used when displayed or logged.
    std::string strSubVer, cleanSubVer;
    RecursiveMutex cs_SubVer; // used for both cleanSubVer and strSubVer
    bool fWhitelisted; // This peer can bypass DoS banning.
    bool fFeeler;      // If true this node is being used as a short lived feeler.
    bool fOneShot;
    bool fClient;
    const bool fInbound;
    bool fNetworkNode;
    std::atomic_bool fSuccessfullyConnected;
    std::atomic_bool fDisconnect;
    // We use fRelayTxes for two purposes -
    // a) it allows us to not relay tx invs before receiving the peer's version message
    // b) the peer may tell us in their version message that we should not relay tx invs
    //    until they have initialized their bloom filter.
    bool fRelayTxes; //protected by cs_filter
    CSemaphoreGrant grantOutbound;
    RecursiveMutex cs_filter;
    CBloomFilter* pfilter;
    std::atomic<int> nRefCount;
    const NodeId id;

    const uint64_t nKeyedNetGroup;
    std::atomic_bool fPauseRecv;
    std::atomic_bool fPauseSend;
protected:
    mapMsgCmdSize mapSendBytesPerMsgCmd;
    mapMsgCmdSize mapRecvBytesPerMsgCmd;

    std::vector<std::string> vecRequestsFulfilled; //keep track of what client has asked for

public:
    uint256 hashContinue;
    std::atomic<int> nStartingHeight;

    // flood relay
    std::vector<CAddress> vAddrToSend;
    CRollingBloomFilter addrKnown;
    bool fGetAddr;
    std::set<uint256> setKnown;
    int64_t nNextAddrSend;
    int64_t nNextLocalAddrSend;

    // inventory based relay
    CRollingBloomFilter filterInventoryKnown;
    // Set of transaction ids we still have to announce.
    // They are sorted by the mempool before relay, so the order is not important.
    std::set<uint256> setInventoryTxToSend;
    // List of block ids we still have announce.
    // There is no final sorting before sending, as they are always sent immediately
    // and in the order requested.
    std::vector<uint256> vInventoryBlockToSend;
    // Set of tier two messages ids we still have to announce.
    std::vector<CInv> vInventoryTierTwoToSend;
    RecursiveMutex cs_inventory;
    std::multimap<int64_t, CInv> mapAskFor;
    std::set<uint256> setAskFor;
    std::vector<uint256> vBlockRequested;
    std::chrono::microseconds nNextInvSend{0};
    // Used for BIP35 mempool sending, also protected by cs_inventory
    bool fSendMempool;

    // Last time a "MEMPOOL" request was serviced.
    std::atomic<int64_t> timeLastMempoolReq{0};

    // Ping time measurement:
    // The pong reply we're expecting, or 0 if no pong expected.
    std::atomic<uint64_t> nPingNonceSent;
    // Time (in usec) the last ping was sent, or 0 if no ping was ever sent.
    std::atomic<int64_t> nPingUsecStart;
    // Last measured round-trip time.
    std::atomic<int64_t> nPingUsecTime;
    // Best measured round-trip time.
    std::atomic<int64_t> nMinPingUsecTime;
    // Whether a ping is requested.
    std::atomic<bool> fPingQueued;

    CNode(NodeId id, ServiceFlags nLocalServicesIn, int nMyStartingHeightIn, SOCKET hSocketIn, const CAddress& addrIn, uint64_t nKeyedNetGroupIn, uint64_t nLocalHostNonceIn, const std::string& addrNameIn = "", bool fInboundIn = false);
    ~CNode();
    CNode(const CNode&) = delete;
    CNode& operator=(const CNode&) = delete;

private:
    const uint64_t nLocalHostNonce;
    // Services offered to this peer
    const ServiceFlags nLocalServices;
    const int nMyStartingHeight;
    int nSendVersion;
    std::list<CNetMessage> vRecvMsg;  // Used only by SocketHandler thread

    mutable RecursiveMutex cs_addrName;
    std::string addrName;

    CService addrLocal;
    mutable RecursiveMutex cs_addrLocal;
public:
    NodeId GetId() const
    {
        return id;
    }

    uint64_t GetLocalNonce() const {
      return nLocalHostNonce;
    }

    int GetMyStartingHeight() const {
      return nMyStartingHeight;
    }

    int GetRefCount()
    {
        assert(nRefCount >= 0);
        return nRefCount;
    }

    unsigned int GetTotalRecvSize()
    {
        unsigned int total = 0;
        for (const CNetMessage& msg : vRecvMsg)
            total += msg.vRecv.size() + 24;
        return total;
    }

    bool ReceiveMsgBytes(const char* pch, unsigned int nBytes, bool& complete);

    void SetRecvVersion(int nVersionIn)
    {
        nRecvVersion = nVersionIn;
    }
    int GetRecvVersion()
    {
        return nRecvVersion;
    }
    void SetSendVersion(int nVersionIn);
    int GetSendVersion() const;

    CService GetAddrLocal() const;
    //! May not be called more than once
    void SetAddrLocal(const CService& addrLocalIn);

    CNode* AddRef()
    {
        nRefCount++;
        return this;
    }

    void Release()
    {
        nRefCount--;
    }


    void AddAddressKnown(const CAddress& _addr)
    {
        addrKnown.insert(_addr.GetKey());
    }

    void PushAddress(const CAddress& _addr, FastRandomContext &insecure_rand)
    {
        // Known checking here is only to save space from duplicates.
        // SendMessages will filter it again for knowns that were added
        // after addresses were pushed.
        if (_addr.IsValid() && !addrKnown.contains(_addr.GetKey())) {
            if (vAddrToSend.size() >= MAX_ADDR_TO_SEND) {
                vAddrToSend[insecure_rand.randrange(vAddrToSend.size())] = _addr;
            } else {
                vAddrToSend.push_back(_addr);
            }
        }
    }


    void AddInventoryKnown(const CInv& inv)
    {
        {
            LOCK(cs_inventory);
            filterInventoryKnown.insert(inv.hash);
        }
    }

    void PushInventory(const CInv& inv)
    {
        LOCK(cs_inventory);
        if (inv.type == MSG_TX) {
            if (!filterInventoryKnown.contains(inv.hash)) {
                setInventoryTxToSend.insert(inv.hash);
            }
        } else if (inv.type == MSG_BLOCK) {
            vInventoryBlockToSend.push_back(inv.hash);
        } else {
            vInventoryTierTwoToSend.emplace_back(inv);
        }
    }

    void AskFor(const CInv& inv);

    bool HasFulfilledRequest(std::string strRequest)
    {
        for (std::string& type : vecRequestsFulfilled) {
            if (type == strRequest) return true;
        }
        return false;
    }

    void ClearFulfilledRequest(std::string strRequest)
    {
        std::vector<std::string>::iterator it = vecRequestsFulfilled.begin();
        while (it != vecRequestsFulfilled.end()) {
            if ((*it) == strRequest) {
                vecRequestsFulfilled.erase(it);
                return;
            }
            ++it;
        }
    }

    void FulfilledRequest(std::string strRequest)
    {
        if (HasFulfilledRequest(strRequest)) return;
        vecRequestsFulfilled.push_back(strRequest);
    }

    void CloseSocketDisconnect();
    bool DisconnectOldProtocol(int nVersionIn, int nVersionRequired);

    void copyStats(CNodeStats& stats);

    ServiceFlags GetLocalServices() const
    {
        return nLocalServices;
    }

    std::string GetAddrName() const;
    //! Sets the addrName only if it was not previously set
    void MaybeSetAddrName(const std::string& addrNameIn);
};

class CExplicitNetCleanup
{
public:
    static void callCleanup();
};

/**
 * Interface for message handling
 */
class NetEventsInterface
{
public:
    virtual bool ProcessMessages(CNode* pnode, std::atomic<bool>& interrupt) = 0;
    virtual bool SendMessages(CNode* pnode, std::atomic<bool>& interrupt) EXCLUSIVE_LOCKS_REQUIRED(pnode->cs_sendProcessing) = 0;
    virtual void InitializeNode(CNode* pnode) = 0;
    virtual void FinalizeNode(NodeId id, bool& update_connection_time) = 0;
};

/** Return a timestamp in the future (in microseconds) for exponentially distributed events. */
int64_t PoissonNextSend(int64_t nNow, int average_interval_seconds);

/** Wrapper to return mockable type */
inline std::chrono::microseconds PoissonNextSend(std::chrono::microseconds now, std::chrono::seconds average_interval)
{
    return std::chrono::microseconds{PoissonNextSend(now.count(), average_interval.count())};
}

#endif // BITCOIN_NET_H
