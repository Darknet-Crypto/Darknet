// Copyright (c) 2014-2016 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activemasternode.h"

#include "addrman.h"
#include "evo/providertx.h"
#include "masternode-sync.h"
#include "masternode.h"
#include "masternodeconfig.h"
#include "masternodeman.h"
#include "messagesigner.h"
#include "netbase.h"
#include "protocol.h"

// Keep track of the active Masternode
CActiveDeterministicMasternodeManager* activeMasternodeManager{nullptr};

static bool GetLocalAddress(CService& addrRet)
{
    // First try to find whatever local address is specified by externalip option
    bool fFound = GetLocal(addrRet) && CActiveDeterministicMasternodeManager::IsValidNetAddr(addrRet);
    if (!fFound && Params().IsRegTestNet()) {
        if (Lookup("127.0.0.1", addrRet, GetListenPort(), false)) {
            fFound = true;
        }
    }
    if(!fFound) {
        // If we have some peers, let's try to find our local address from one of them
        g_connman->ForEachNodeContinueIf([&fFound, &addrRet](CNode* pnode) {
            if (pnode->addr.IsIPv4())
                fFound = GetLocal(addrRet, &pnode->addr) && CActiveDeterministicMasternodeManager::IsValidNetAddr(addrRet);
            return !fFound;
        });
    }
    return fFound;
}

std::string CActiveDeterministicMasternodeManager::GetStatus() const
{
    switch (state) {
        case MASTERNODE_WAITING_FOR_PROTX:    return "Waiting for ProTx to appear on-chain";
        case MASTERNODE_POSE_BANNED:          return "Masternode was PoSe banned";
        case MASTERNODE_REMOVED:              return "Masternode removed from list";
        case MASTERNODE_OPERATOR_KEY_CHANGED: return "Operator key changed or revoked";
        case MASTERNODE_PROTX_IP_CHANGED:     return "IP address specified in ProTx changed";
        case MASTERNODE_READY:                return "Ready";
        case MASTERNODE_ERROR:                return "Error. " + strError;
        default:                              return "Unknown";
    }
}

OperationResult CActiveDeterministicMasternodeManager::SetOperatorKey(const std::string& strMNOperatorPrivKey)
{

    LOCK(cs_main); // Lock cs_main so the node doesn't perform any action while we setup the Masternode
    LogPrintf("Initializing deterministic masternode...\n");
    if (strMNOperatorPrivKey.empty()) {
        return errorOut("ERROR: Masternode operator priv key cannot be empty.");
    }
    if (!CMessageSigner::GetKeysFromSecret(strMNOperatorPrivKey, info.keyOperator, info.keyIDOperator)) {
        return errorOut(_("Invalid mnoperatorprivatekey. Please see the documentation."));
    }
    return OperationResult(true);
}

OperationResult CActiveDeterministicMasternodeManager::GetOperatorKey(CKey& key, CKeyID& keyID, CDeterministicMNCPtr& dmn) const
{
    if (!IsReady()) {
        return errorOut("Active masternode not ready");
    }
    dmn = deterministicMNManager->GetListAtChainTip().GetValidMN(info.proTxHash);
    if (!dmn) {
        return errorOut(strprintf("Active masternode %s not registered or PoSe banned", info.proTxHash.ToString()));
    }
    if (info.keyIDOperator != dmn->pdmnState->keyIDOperator) {
        return errorOut("Active masternode operator key changed or revoked");
    }
    // return keys
    key = info.keyOperator;
    keyID = info.keyIDOperator;
    return OperationResult(true);
}

void CActiveDeterministicMasternodeManager::Init()
{
    // set masternode arg if called from RPC
    if (!fMasterNode) {
        gArgs.ForceSetArg("-masternode", "1");
        fMasterNode = true;
    }

    if (!deterministicMNManager->IsDIP3Enforced()) {
        state = MASTERNODE_ERROR;
        strError = "Evo upgrade is not active yet.";
        LogPrintf("%s -- ERROR: %s\n", __func__, strError);
        return;
    }

    LOCK(cs_main);

    // Check that our local network configuration is correct
    if (!fListen) {
        // listen option is probably overwritten by smth else, no good
        state = MASTERNODE_ERROR;
        strError = "Masternode must accept connections from outside. Make sure listen configuration option is not overwritten by some another parameter.";
        LogPrintf("%s ERROR: %s\n", __func__, strError);
        return;
    }

    if (!GetLocalAddress(info.service)) {
        state = MASTERNODE_ERROR;
        strError = "Can't detect valid external address. Please consider using the externalip configuration option if problem persists. Make sure to use IPv4 address only.";
        LogPrintf("%s ERROR: %s\n", __func__, strError);
        return;
    }

    CDeterministicMNList mnList = deterministicMNManager->GetListAtChainTip();

    CDeterministicMNCPtr dmn = mnList.GetMNByOperatorKey(info.keyIDOperator);
    if (!dmn) {
        // MN not appeared on the chain yet
        return;
    }

    if (!mnList.IsMNValid(dmn->proTxHash)) {
        if (mnList.IsMNPoSeBanned(dmn->proTxHash)) {
            state = MASTERNODE_POSE_BANNED;
        } else {
            state = MASTERNODE_REMOVED;
        }
        return;
    }

    LogPrintf("%s: proTxHash=%s, proTx=%s\n", __func__, dmn->proTxHash.ToString(), dmn->ToString());

    info.proTxHash = dmn->proTxHash;

    if (info.service != dmn->pdmnState->addr) {
        state = MASTERNODE_ERROR;
        strError = strprintf("Local address %s does not match the address from ProTx (%s)",
                             info.service.ToStringIPPort(), dmn->pdmnState->addr.ToStringIPPort());
        LogPrintf("%s ERROR: %s\n", __func__, strError);
        return;
    }

    if (!Params().IsRegTestNet()) {
        // Check socket connectivity
        const std::string& strService = info.service.ToString();
        LogPrintf("%s: Checking inbound connection to '%s'\n", __func__, strService);
        SOCKET hSocket;
        bool fConnected = ConnectSocket(info.service, hSocket, nConnectTimeout) && IsSelectableSocket(hSocket);
        CloseSocket(hSocket);

        if (!fConnected) {
            state = MASTERNODE_ERROR;
            LogPrintf("%s ERROR: Could not connect to %s\n", __func__, strService);
            return;
        }
    }

    state = MASTERNODE_READY;
}

void CActiveDeterministicMasternodeManager::Reset(masternode_state_t _state)
{
    state = _state;
    SetNullProTx();
    // MN might have reappeared in same block with a new ProTx
    Init();
}

void CActiveDeterministicMasternodeManager::UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload)
{
    if (fInitialDownload)
        return;

    if (!fMasterNode || !deterministicMNManager->IsDIP3Enforced())
        return;

    if (state == MASTERNODE_READY) {
        auto oldMNList = deterministicMNManager->GetListForBlock(pindexNew->pprev);
        auto newMNList = deterministicMNManager->GetListForBlock(pindexNew);
        if (!newMNList.IsMNValid(info.proTxHash)) {
            // MN disappeared from MN list
            Reset(MASTERNODE_REMOVED);
            return;
        }

        auto oldDmn = oldMNList.GetMN(info.proTxHash);
        auto newDmn = newMNList.GetMN(info.proTxHash);
        if (newDmn->pdmnState->keyIDOperator != oldDmn->pdmnState->keyIDOperator) {
            // MN operator key changed or revoked
            Reset(MASTERNODE_OPERATOR_KEY_CHANGED);
            return;
        }

        if (newDmn->pdmnState->addr != oldDmn->pdmnState->addr) {
            // MN IP changed
            Reset(MASTERNODE_PROTX_IP_CHANGED);
            return;
        }
    } else {
        // MN might have (re)appeared with a new ProTx or we've found some peers
        // and figured out our local address
        Init();
    }
}

bool CActiveDeterministicMasternodeManager::IsValidNetAddr(const CService& addrIn)
{
    // TODO: check IPv6 and TOR addresses
    return Params().IsRegTestNet() || (addrIn.IsIPv4() && IsReachable(addrIn) && addrIn.IsRoutable());
}


/********* LEGACY *********/

OperationResult initMasternode(const std::string& _strMasterNodePrivKey, const std::string& _strMasterNodeAddr, bool isFromInit)
{
    if (!isFromInit && fMasterNode) {
        return errorOut( "ERROR: Masternode already initialized.");
    }

    LOCK(cs_main); // Lock cs_main so the node doesn't perform any action while we setup the Masternode
    LogPrintf("Initializing masternode, addr %s..\n", _strMasterNodeAddr.c_str());

    if (_strMasterNodePrivKey.empty()) {
        return errorOut("ERROR: Masternode priv key cannot be empty.");
    }

    if (_strMasterNodeAddr.empty()) {
        return errorOut("ERROR: Empty masternodeaddr");
    }

    // Global params set
    strMasterNodeAddr = _strMasterNodeAddr;

    // Address parsing.
    const CChainParams& params = Params();
    int nPort = 0;
    int nDefaultPort = params.GetDefaultPort();
    std::string strHost;
    SplitHostPort(strMasterNodeAddr, nPort, strHost);

    // Allow for the port number to be omitted here and just double check
    // that if a port is supplied, it matches the required default port.
    if (nPort == 0) nPort = nDefaultPort;
    if (nPort != nDefaultPort && !params.IsRegTestNet()) {
        return errorOut(strprintf(_("Invalid -masternodeaddr port %d, only %d is supported on %s-net."),
                                           nPort, nDefaultPort, Params().NetworkIDString()));
    }
    CService addrTest(LookupNumeric(strHost.c_str(), nPort));
    if (!addrTest.IsValid()) {
        return errorOut(strprintf(_("Invalid -masternodeaddr address: %s"), strMasterNodeAddr));
    }

    // Peer port needs to match the masternode public one for IPv4 and IPv6.
    // Onion can run in other ports because those are behind a hidden service which has the public port fixed to the default port.
    if (nPort != GetListenPort() && !addrTest.IsTor()) {
        return errorOut(strprintf(_("Invalid -masternodeaddr port %d, isn't the same as the peer port %d"),
                                  nPort, GetListenPort()));
    }

    CKey key;
    CPubKey pubkey;
    if (!CMessageSigner::GetKeysFromSecret(_strMasterNodePrivKey, key, pubkey)) {
        return errorOut(_("Invalid masternodeprivkey. Please see the documentation."));
    }

    activeMasternode.pubKeyMasternode = pubkey;
    activeMasternode.privKeyMasternode = key;
    activeMasternode.service = addrTest;
    fMasterNode = true;

    if (masternodeSync.IsBlockchainSynced()) {
        // Check if the masternode already exists in the list
        CMasternode* pmn = mnodeman.Find(pubkey);
        if (pmn) activeMasternode.EnableHotColdMasterNode(pmn->vin, pmn->addr);
    }

    return OperationResult(true);
}

//
// Bootup the Masternode, look for a 10000 PIVX input and register on the network
//
void CActiveMasternode::ManageStatus()
{
    if (!fMasterNode) return;
    if (activeMasternodeManager != nullptr) {
        // Deterministic masternode
        return;
    }

    // !TODO: Legacy masternodes - remove after enforcement
    LogPrint(BCLog::MASTERNODE, "CActiveMasternode::ManageStatus() - Begin\n");

    // If a DMN has been registered with same collateral, disable me.
    CMasternode* pmn = mnodeman.Find(pubKeyMasternode);
    if (pmn && deterministicMNManager->GetListAtChainTip().HasMNByCollateral(pmn->vin.prevout)) {
        LogPrintf("%s: Disabling active legacy Masternode %s as the collateral is now registered with a DMN\n",
                         __func__, pmn->vin.prevout.ToString());
        status = ACTIVE_MASTERNODE_NOT_CAPABLE;
        notCapableReason = "Collateral registered with DMN";
        return;
    }

    //need correct blocks to send ping
    if (!Params().IsRegTestNet() && !masternodeSync.IsBlockchainSynced()) {
        status = ACTIVE_MASTERNODE_SYNC_IN_PROCESS;
        LogPrintf("CActiveMasternode::ManageStatus() - %s\n", GetStatusMessage());
        return;
    }

    if (status == ACTIVE_MASTERNODE_SYNC_IN_PROCESS) status = ACTIVE_MASTERNODE_INITIAL;

    if (status == ACTIVE_MASTERNODE_INITIAL || (pmn && status == ACTIVE_MASTERNODE_NOT_CAPABLE)) {
        if (pmn) {
            if (pmn->protocolVersion != PROTOCOL_VERSION) {
                LogPrintf("%s: ERROR Trying to start a masternode running an old protocol version, "
                          "the controller and masternode wallets need to be running the latest release version.\n", __func__);
                return;
            }
            // Update vin and service
            EnableHotColdMasterNode(pmn->vin, pmn->addr);
        }
    }

    if (status != ACTIVE_MASTERNODE_STARTED) {
        // Set defaults
        status = ACTIVE_MASTERNODE_NOT_CAPABLE;
        notCapableReason = "";

        LogPrintf("%s - Checking inbound connection for masternode to '%s'\n", __func__ , service.ToString());

        CAddress addr(service, NODE_NETWORK);
        if (!g_connman->IsNodeConnected(addr)) {
            CNode* node = g_connman->ConnectNode(addr);
            if (!node) {
                notCapableReason =
                        "Masternode address:port connection availability test failed, could not open a connection to the public masternode address (" +
                        service.ToString() + ")";
                LogPrintf("%s - not capable: %s\n", __func__, notCapableReason);
            } else {
                // don't leak allocated object in memory
                delete node;
            }
            return;
        }

        notCapableReason = "Waiting for start message from controller.";
        return;
    }

    //send to all peers
    std::string errorMessage;
    if (!SendMasternodePing(errorMessage)) {
        LogPrintf("CActiveMasternode::ManageStatus() - Error on Ping: %s\n", errorMessage);
    }
}

void CActiveMasternode::ResetStatus()
{
    status = ACTIVE_MASTERNODE_INITIAL;
    ManageStatus();
}

std::string CActiveMasternode::GetStatusMessage() const
{
    switch (status) {
    case ACTIVE_MASTERNODE_INITIAL:
        return "Node just started, not yet activated";
    case ACTIVE_MASTERNODE_SYNC_IN_PROCESS:
        return "Sync in progress. Must wait until sync is complete to start Masternode";
    case ACTIVE_MASTERNODE_NOT_CAPABLE:
        return "Not capable masternode: " + notCapableReason;
    case ACTIVE_MASTERNODE_STARTED:
        return "Masternode successfully started";
    default:
        return "unknown";
    }
}

bool CActiveMasternode::SendMasternodePing(std::string& errorMessage)
{
    if (vin == nullopt) {
        errorMessage = "Active Masternode not initialized";
        return false;
    }

    if (status != ACTIVE_MASTERNODE_STARTED) {
        errorMessage = "Masternode is not in a running status";
        return false;
    }

    if (!privKeyMasternode.IsValid() || !pubKeyMasternode.IsValid()) {
        errorMessage = "Error upon masternode key.\n";
        return false;
    }

    LogPrintf("CActiveMasternode::SendMasternodePing() - Relay Masternode Ping vin = %s\n", vin->ToString());

    const uint256& nBlockHash = mnodeman.GetBlockHashToPing();
    CMasternodePing mnp(*vin, nBlockHash, GetAdjustedTime());
    if (!mnp.Sign(privKeyMasternode, pubKeyMasternode.GetID())) {
        errorMessage = "Couldn't sign Masternode Ping";
        return false;
    }

    // Update lastPing for our masternode in Masternode list
    CMasternode* pmn = mnodeman.Find(vin->prevout);
    if (pmn != NULL) {
        if (pmn->IsPingedWithin(MasternodePingSeconds(), mnp.sigTime)) {
            errorMessage = "Too early to send Masternode Ping";
            return false;
        }

        // SetLastPing locks the masternode cs, be careful with the lock order.
        pmn->SetLastPing(mnp);
        mnodeman.mapSeenMasternodePing.emplace(mnp.GetHash(), mnp);

        //mnodeman.mapSeenMasternodeBroadcast.lastPing is probably outdated, so we'll update it
        CMasternodeBroadcast mnb(*pmn);
        uint256 hash = mnb.GetHash();
        if (mnodeman.mapSeenMasternodeBroadcast.count(hash)) {
            // SetLastPing locks the masternode cs, be careful with the lock order.
            // TODO: check why are we double setting the last ping here..
            mnodeman.mapSeenMasternodeBroadcast[hash].SetLastPing(mnp);
        }

        mnp.Relay();
        return true;

    } else {
        // Seems like we are trying to send a ping while the Masternode is not registered in the network
        errorMessage = "Masternode List doesn't include our Masternode, shutting down Masternode pinging service! " + vin->ToString();
        status = ACTIVE_MASTERNODE_NOT_CAPABLE;
        notCapableReason = errorMessage;
        return false;
    }
}

// when starting a Masternode, this can enable to run as a hot wallet with no funds
bool CActiveMasternode::EnableHotColdMasterNode(CTxIn& newVin, CService& newService)
{
    if (!fMasterNode) return false;

    status = ACTIVE_MASTERNODE_STARTED;

    //The values below are needed for signing mnping messages going forward
    vin = newVin;
    service = newService;

    LogPrintf("CActiveMasternode::EnableHotColdMasterNode() - Enabled! You may shut down the cold daemon.\n");

    return true;
}

void CActiveMasternode::GetKeys(CKey& _privKeyMasternode, CPubKey& _pubKeyMasternode)
{
    if (!privKeyMasternode.IsValid() || !pubKeyMasternode.IsValid()) {
        throw std::runtime_error("Error trying to get masternode keys");
    }
    _privKeyMasternode = privKeyMasternode;
    _pubKeyMasternode = pubKeyMasternode;
}

bool GetActiveMasternodeKeys(CKey& key, CKeyID& keyID, CTxIn& vin)
{
    if (activeMasternodeManager != nullptr) {
        // deterministic mn
        CDeterministicMNCPtr dmn;
        auto res = activeMasternodeManager->GetOperatorKey(key, keyID, dmn);
        if (!res) {
            LogPrint(BCLog::MNBUDGET,"%s: %s\n", __func__, res.getError());
            return false;
        }
        vin = CTxIn(dmn->collateralOutpoint);
        return true;
    }

    // legacy mn
    if (activeMasternode.vin == nullopt) {
        LogPrint(BCLog::MNBUDGET,"%s: Active Masternode not initialized\n", __func__);
        return false;
    }
    if (activeMasternode.GetStatus() != ACTIVE_MASTERNODE_STARTED) {
        LogPrint(BCLog::MNBUDGET,"%s: MN not started (%s)\n", __func__, activeMasternode.GetStatusMessage());
        return false;
    }
    CPubKey mnPubKey;
    activeMasternode.GetKeys(key, mnPubKey);
    keyID = mnPubKey.GetID();
    vin = *activeMasternode.vin;
    return true;
}
