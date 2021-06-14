// Copyright (c) 2014-2016 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef ACTIVEMASTERNODE_H
#define ACTIVEMASTERNODE_H

#include "init.h"
#include "key.h"
#include "evo/deterministicmns.h"
#include "masternode.h"
#include "net.h"
#include "operationresult.h"
#include "sync.h"
#include "validationinterface.h"
#include "wallet/wallet.h"

class CActiveDeterministicMasternodeManager;
class CBLSPublicKey;
class CBLSSecretKey;

#define ACTIVE_MASTERNODE_INITIAL 0 // initial state
#define ACTIVE_MASTERNODE_SYNC_IN_PROCESS 1
#define ACTIVE_MASTERNODE_NOT_CAPABLE 3
#define ACTIVE_MASTERNODE_STARTED 4

extern CActiveDeterministicMasternodeManager* activeMasternodeManager;

struct CActiveMasternodeInfo
{
    // Keys for the active Masternode
    CBLSPublicKey pubKeyOperator;
    CBLSSecretKey keyOperator;
    // Initialized while registering Masternode
    uint256 proTxHash{UINT256_ZERO};
    CService service;
};

class CActiveDeterministicMasternodeManager : public CValidationInterface
{
public:
    enum masternode_state_t {
        MASTERNODE_WAITING_FOR_PROTX,
        MASTERNODE_POSE_BANNED,
        MASTERNODE_REMOVED,
        MASTERNODE_OPERATOR_KEY_CHANGED,
        MASTERNODE_PROTX_IP_CHANGED,
        MASTERNODE_READY,
        MASTERNODE_ERROR,
    };

private:
    masternode_state_t state{MASTERNODE_WAITING_FOR_PROTX};
    std::string strError;
    CActiveMasternodeInfo info;

public:
    virtual ~CActiveDeterministicMasternodeManager() = default;
    virtual void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload);

    void Init();
    void Reset(masternode_state_t _state);
    // Sets the Deterministic Masternode Operator's private/public key
    OperationResult SetOperatorKey(const std::string& strMNOperatorPrivKey);
    // If the active masternode is ready, and the keyID matches with the registered one,
    // return private key, keyID, and pointer to dmn.
    OperationResult GetOperatorKey(CBLSSecretKey& key, CBLSPublicKey& pubkey, CDeterministicMNCPtr& dmn) const;
    void SetNullProTx() { info.proTxHash = UINT256_ZERO; }

    const CActiveMasternodeInfo* GetInfo() const { return &info; }
    masternode_state_t GetState() const { return state; }
    std::string GetStatus() const;
    bool IsReady() const { return state == MASTERNODE_READY; }

    static bool IsValidNetAddr(const CService& addrIn);
};

// Responsible for initializing the masternode
OperationResult initMasternode(const std::string& strMasterNodePrivKey, const std::string& strMasterNodeAddr, bool isFromInit);


// Responsible for activating the Masternode and pinging the network (legacy MN list)
class CActiveMasternode
{
private:
    int status;
    std::string notCapableReason;

public:

    CActiveMasternode()
    {
        vin = nullopt;
        status = ACTIVE_MASTERNODE_INITIAL;
    }

    // Initialized by init.cpp
    // Keys for the main Masternode
    CPubKey pubKeyMasternode;
    CKey privKeyMasternode;

    // Initialized while registering Masternode
    Optional<CTxIn> vin;
    CService service;

    /// Manage status of main Masternode
    void ManageStatus();
    void ResetStatus();
    std::string GetStatusMessage() const;
    int GetStatus() const { return status; }

    /// Ping Masternode
    bool SendMasternodePing(std::string& errorMessage);
    /// Enable cold wallet mode (run a Masternode with no funds)
    bool EnableHotColdMasterNode(CTxIn& vin, CService& addr);

    void GetKeys(CKey& privKeyMasternode, CPubKey& pubKeyMasternode);
};

// Compatibility code: get keys for either legacy or deterministic masternode
bool GetActiveMasternodeKeys(CKey& key, CKeyID& keyID, CTxIn& vin, CBLSSecretKey& blsKey);
bool GetActiveDMNKeys(CBLSSecretKey& key, CBLSPublicKey& pubkey, CTxIn& vin);

#endif
