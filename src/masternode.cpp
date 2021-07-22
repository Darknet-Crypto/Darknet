// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternode.h"

#include "addrman.h"
#include "init.h"
#include "masternode-sync.h"
#include "masternodeman.h"
#include "netbase.h"
#include "sync.h"
#include "wallet/wallet.h"

#define MASTERNODE_MIN_CONFIRMATIONS_REGTEST 1
#define MASTERNODE_MIN_MNP_SECONDS_REGTEST 90
#define MASTERNODE_MIN_MNB_SECONDS_REGTEST 25
#define MASTERNODE_PING_SECONDS_REGTEST 25
#define MASTERNODE_EXPIRATION_SECONDS_REGTEST 12 * 60
#define MASTERNODE_REMOVAL_SECONDS_REGTEST 13 * 60

#define MASTERNODE_MIN_CONFIRMATIONS 15
#define MASTERNODE_MIN_MNP_SECONDS (10 * 60)
#define MASTERNODE_MIN_MNB_SECONDS (5 * 60)
#define MASTERNODE_PING_SECONDS (5 * 60)
#define MASTERNODE_EXPIRATION_SECONDS (120 * 60)
#define MASTERNODE_REMOVAL_SECONDS (130 * 60)
#define MASTERNODE_CHECK_SECONDS 5

// keep track of the scanning errors I've seen
std::map<uint256, int> mapSeenMasternodeScanningErrors;


int MasternodeMinPingSeconds()
{
    return Params().IsRegTestNet() ? MASTERNODE_MIN_MNP_SECONDS_REGTEST : MASTERNODE_MIN_MNP_SECONDS;
}

int MasternodeBroadcastSeconds()
{
    return Params().IsRegTestNet() ? MASTERNODE_MIN_MNB_SECONDS_REGTEST : MASTERNODE_MIN_MNB_SECONDS;
}

int MasternodeCollateralMinConf()
{
    return Params().IsRegTestNet() ? MASTERNODE_MIN_CONFIRMATIONS_REGTEST : MASTERNODE_MIN_CONFIRMATIONS;
}

int MasternodePingSeconds()
{
    return Params().IsRegTestNet() ? MASTERNODE_PING_SECONDS_REGTEST : MASTERNODE_PING_SECONDS;
}

int MasternodeExpirationSeconds()
{
    return Params().IsRegTestNet() ? MASTERNODE_EXPIRATION_SECONDS_REGTEST : MASTERNODE_EXPIRATION_SECONDS;
}

int MasternodeRemovalSeconds()
{
    return Params().IsRegTestNet() ? MASTERNODE_REMOVAL_SECONDS_REGTEST : MASTERNODE_REMOVAL_SECONDS;
}


CMasternode::CMasternode() :
        CSignedMessage()
{
    LOCK(cs);
    vin = CTxIn();
    addr = CService();
    pubKeyCollateralAddress = CPubKey();
    pubKeyMasternode = CPubKey();
    sigTime = 0;
    lastPing = CMasternodePing();
    protocolVersion = PROTOCOL_VERSION;
    nScanningErrorCount = 0;
    nLastScanningErrorBlockHeight = 0;
    mnPayeeScript.clear();
}

CMasternode::CMasternode(const CMasternode& other) :
        CSignedMessage(other)
{
    LOCK(cs);
    vin = other.vin;
    addr = other.addr;
    pubKeyCollateralAddress = other.pubKeyCollateralAddress;
    pubKeyMasternode = other.pubKeyMasternode;
    sigTime = other.sigTime;
    lastPing = other.lastPing;
    protocolVersion = other.protocolVersion;
    nScanningErrorCount = other.nScanningErrorCount;
    nLastScanningErrorBlockHeight = other.nLastScanningErrorBlockHeight;
    mnPayeeScript = other.mnPayeeScript;
}

CMasternode::CMasternode(const CDeterministicMNCPtr& dmn, int64_t registeredTime, const uint256& registeredHash) :
        CSignedMessage()
{
    LOCK(cs);
    vin = CTxIn(dmn->collateralOutpoint);
    addr = dmn->pdmnState->addr;
    pubKeyCollateralAddress = CPubKey();
    pubKeyMasternode = CPubKey();
    sigTime = registeredTime;
    lastPing = CMasternodePing(vin, registeredHash, registeredTime);
    protocolVersion = PROTOCOL_VERSION;
    nScanningErrorCount = 0;
    nLastScanningErrorBlockHeight = 0;
    mnPayeeScript = dmn->pdmnState->scriptPayout;
}

uint256 CMasternode::GetSignatureHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << nMessVersion;
    ss << addr;
    ss << sigTime;
    ss << pubKeyCollateralAddress;
    ss << pubKeyMasternode;
    ss << protocolVersion;
    return ss.GetHash();
}

std::string CMasternode::GetStrMessage() const
{
    return (addr.ToString() +
            std::to_string(sigTime) +
            pubKeyCollateralAddress.GetID().ToString() +
            pubKeyMasternode.GetID().ToString() +
            std::to_string(protocolVersion)
    );
}

//
// When a new masternode broadcast is sent, update our information
//
bool CMasternode::UpdateFromNewBroadcast(CMasternodeBroadcast& mnb)
{
    if (mnb.sigTime > sigTime) {
        // TODO: lock cs. Need to be careful as mnb.lastPing.CheckAndUpdate locks cs_main internally.
        pubKeyMasternode = mnb.pubKeyMasternode;
        pubKeyCollateralAddress = mnb.pubKeyCollateralAddress;
        sigTime = mnb.sigTime;
        vchSig = mnb.vchSig;
        protocolVersion = mnb.protocolVersion;
        addr = mnb.addr;
        int nDoS = 0;
        if (mnb.lastPing.IsNull() || (!mnb.lastPing.IsNull() && mnb.lastPing.CheckAndUpdate(nDoS, false))) {
            lastPing = mnb.lastPing;
            mnodeman.mapSeenMasternodePing.emplace(lastPing.GetHash(), lastPing);
        }
        return true;
    }
    return false;
}

//
// Deterministically calculate a given "score" for a Masternode depending on how close it's hash is to
// the proof of work for that block. The further away they are the better, the furthest will win the election
// and get paid this block
//
arith_uint256 CMasternode::CalculateScore(const uint256& hash) const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << hash;
    const arith_uint256& hash2 = UintToArith256(ss.GetHash());

    CHashWriter ss2(SER_GETHASH, PROTOCOL_VERSION);
    ss2 << hash;
    const arith_uint256& aux = UintToArith256(vin.prevout.hash) + vin.prevout.n;
    ss2 << aux;
    const arith_uint256& hash3 = UintToArith256(ss2.GetHash());

    return (hash3 > hash2 ? hash3 - hash2 : hash2 - hash3);
}

CMasternode::state CMasternode::GetActiveState() const
{
    LOCK(cs);
    if (fCollateralSpent) {
        return MASTERNODE_VIN_SPENT;
    }
    if (!IsPingedWithin(MasternodeRemovalSeconds())) {
        return MASTERNODE_REMOVE;
    }
    if (!IsPingedWithin(MasternodeExpirationSeconds())) {
        return MASTERNODE_EXPIRED;
    }
    if(lastPing.sigTime - sigTime < MasternodeMinPingSeconds()){
        return MASTERNODE_PRE_ENABLED;
    }
    return MASTERNODE_ENABLED;
}

bool CMasternode::IsValidNetAddr() const
{
    // TODO: regtest is fine with any addresses for now,
    // should probably be a bit smarter if one day we start to implement tests for this
    return Params().IsRegTestNet() ||
           (IsReachable(addr) && addr.IsRoutable());
}

bool CMasternode::IsInputAssociatedWithPubkey() const
{
    CScript payee;
    payee = GetScriptForDestination(pubKeyCollateralAddress.GetID());

    CTransactionRef txVin;
    uint256 hash;
    if(GetTransaction(vin.prevout.hash, txVin, hash, true)) {
        for (const CTxOut& out : txVin->vout) {
            if (out.nValue == Params().GetConsensus().nMNCollateralAmt &&
                out.scriptPubKey == payee)
                return true;
        }
    }

    return false;
}

CMasternodeBroadcast::CMasternodeBroadcast() :
        CMasternode()
{ }

CMasternodeBroadcast::CMasternodeBroadcast(CService newAddr, CTxIn newVin, CPubKey pubKeyCollateralAddressNew, CPubKey pubKeyMasternodeNew, int protocolVersionIn, const CMasternodePing& _lastPing) :
        CMasternode()
{
    vin = newVin;
    addr = newAddr;
    pubKeyCollateralAddress = pubKeyCollateralAddressNew;
    pubKeyMasternode = pubKeyMasternodeNew;
    protocolVersion = protocolVersionIn;
    lastPing = _lastPing;
    sigTime = lastPing.sigTime;
}

CMasternodeBroadcast::CMasternodeBroadcast(const CMasternode& mn) :
        CMasternode(mn)
{ }

bool CMasternodeBroadcast::Create(const std::string& strService,
                                  const std::string& strKeyMasternode,
                                  const std::string& strTxHash,
                                  const std::string& strOutputIndex,
                                  std::string& strErrorRet,
                                  CMasternodeBroadcast& mnbRet,
                                  bool fOffline)
{
    CTxIn txin;
    CPubKey pubKeyCollateralAddressNew;
    CKey keyCollateralAddressNew;
    CPubKey pubKeyMasternodeNew;
    CKey keyMasternodeNew;

    //need correct blocks to send ping
    if (!fOffline && !masternodeSync.IsBlockchainSynced()) {
        strErrorRet = "Sync in progress. Must wait until sync is complete to start Masternode";
        LogPrint(BCLog::MASTERNODE,"CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    if (!CMessageSigner::GetKeysFromSecret(strKeyMasternode, keyMasternodeNew, pubKeyMasternodeNew)) {
        strErrorRet = strprintf("Invalid masternode key %s", strKeyMasternode);
        LogPrint(BCLog::MASTERNODE,"CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        return false;
    }

    std::string strError;
    // Use wallet-0 here. Legacy mnb creation can be removed after transition to DMN
    if (vpwallets.empty() || !vpwallets[0]->GetMasternodeVinAndKeys(txin, pubKeyCollateralAddressNew, keyCollateralAddressNew, strTxHash, strOutputIndex, strError)) {
        strErrorRet = strError; // GetMasternodeVinAndKeys logs this error. Only returned for GUI error notification.
        LogPrint(BCLog::MASTERNODE,"CMasternodeBroadcast::Create -- %s\n", strprintf("Could not allocate txin %s:%s for masternode %s", strTxHash, strOutputIndex, strService));
        return false;
    }

    int nPort = 0;
    int nDefaultPort = Params().GetDefaultPort();
    std::string strHost;
    SplitHostPort(strService, nPort, strHost);
    if (nPort == 0) nPort = nDefaultPort;
    CService _service(LookupNumeric(strHost.c_str(), nPort));

    // The service needs the correct default port to work properly
    if (!CheckDefaultPort(_service, strErrorRet, "CMasternodeBroadcast::Create"))
        return false;

    return Create(txin, _service, keyCollateralAddressNew, pubKeyCollateralAddressNew, keyMasternodeNew, pubKeyMasternodeNew, strErrorRet, mnbRet);
}

bool CMasternodeBroadcast::Create(const CTxIn& txin,
                                  const CService& service,
                                  const CKey& keyCollateralAddressNew,
                                  const CPubKey& pubKeyCollateralAddressNew,
                                  const CKey& keyMasternodeNew,
                                  const CPubKey& pubKeyMasternodeNew,
                                  std::string& strErrorRet,
                                  CMasternodeBroadcast& mnbRet)
{
    // wait for reindex and/or import to finish
    if (fImporting || fReindex) return false;

    LogPrint(BCLog::MASTERNODE, "CMasternodeBroadcast::Create -- pubKeyCollateralAddressNew = %s, pubKeyMasternodeNew.GetID() = %s\n",
             EncodeDestination(pubKeyCollateralAddressNew.GetID()),
        pubKeyMasternodeNew.GetID().ToString());

    // Get block hash to ping (TODO: move outside of this function)
    const uint256& nBlockHashToPing = mnodeman.GetBlockHashToPing();
    CMasternodePing mnp(txin, nBlockHashToPing, GetAdjustedTime());
    if (!mnp.Sign(keyMasternodeNew, pubKeyMasternodeNew.GetID())) {
        strErrorRet = strprintf("Failed to sign ping, masternode=%s", txin.prevout.hash.ToString());
        LogPrint(BCLog::MASTERNODE,"CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CMasternodeBroadcast();
        return false;
    }

    mnbRet = CMasternodeBroadcast(service, txin, pubKeyCollateralAddressNew, pubKeyMasternodeNew, PROTOCOL_VERSION, mnp);

    if (!mnbRet.IsValidNetAddr()) {
        strErrorRet = strprintf("Invalid IP address %s, masternode=%s", mnbRet.addr.ToStringIP (), txin.prevout.hash.ToString());
        LogPrint(BCLog::MASTERNODE,"CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CMasternodeBroadcast();
        return false;
    }

    if (!mnbRet.Sign(keyCollateralAddressNew, pubKeyCollateralAddressNew)) {
        strErrorRet = strprintf("Failed to sign broadcast, masternode=%s", txin.prevout.hash.ToString());
        LogPrint(BCLog::MASTERNODE,"CMasternodeBroadcast::Create -- %s\n", strErrorRet);
        mnbRet = CMasternodeBroadcast();
        return false;
    }

    return true;
}

bool CMasternodeBroadcast::Sign(const CKey& key, const CPubKey& pubKey)
{
    std::string strError = "";
    nMessVersion = MessageVersion::MESS_VER_HASH;
    const std::string strMessage = GetSignatureHash().GetHex();

    if (!CMessageSigner::SignMessage(strMessage, vchSig, key)) {
        return error("%s : SignMessage() (nMessVersion=%d) failed", __func__, nMessVersion);
    }

    if (!CMessageSigner::VerifyMessage(pubKey, vchSig, strMessage, strError)) {
        return error("%s : VerifyMessage() (nMessVersion=%d) failed, error: %s\n",
                __func__, nMessVersion, strError);
    }

    return true;
}

bool CMasternodeBroadcast::Sign(const std::string strSignKey)
{
    CKey key;
    CPubKey pubkey;

    if (!CMessageSigner::GetKeysFromSecret(strSignKey, key, pubkey)) {
        return error("%s : Invalid strSignKey", __func__);
    }

    return Sign(key, pubkey);
}

bool CMasternodeBroadcast::CheckSignature() const
{
    std::string strError = "";
    std::string strMessage = (
                            nMessVersion == MessageVersion::MESS_VER_HASH ?
                            GetSignatureHash().GetHex() :
                            GetStrMessage()
                            );

    if(!CMessageSigner::VerifyMessage(pubKeyCollateralAddress, vchSig, strMessage, strError))
        return error("%s : VerifyMessage (nMessVersion=%d) failed: %s", __func__, nMessVersion, strError);

    return true;
}

bool CMasternodeBroadcast::CheckDefaultPort(CService service, std::string& strErrorRet, const std::string& strContext)
{
    int nDefaultPort = Params().GetDefaultPort();

    if (service.GetPort() != nDefaultPort && !Params().IsRegTestNet()) {
        strErrorRet = strprintf("Invalid port %u for masternode %s, only %d is supported on %s-net.",
            service.GetPort(), service.ToString(), nDefaultPort, Params().NetworkIDString());
        LogPrintf("%s - %s\n", strContext, strErrorRet);
        return false;
    }

    return true;
}

bool CMasternodeBroadcast::CheckAndUpdate(int& nDos)
{
    // make sure signature isn't in the future (past is OK)
    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrint(BCLog::MASTERNODE,"mnb - Signature rejected, too far into the future %s\n", vin.prevout.hash.ToString());
        nDos = 1;
        return false;
    }

    if (protocolVersion < ActiveProtocol()) {
        LogPrint(BCLog::MASTERNODE,"mnb - ignoring outdated Masternode %s protocol version %d\n", vin.prevout.hash.ToString(), protocolVersion);
        return false;
    }

    CScript pubkeyScript;
    pubkeyScript = GetScriptForDestination(pubKeyCollateralAddress.GetID());

    if (pubkeyScript.size() != 25) {
        LogPrint(BCLog::MASTERNODE,"mnb - pubkey the wrong size\n");
        nDos = 100;
        return false;
    }

    CScript pubkeyScript2;
    pubkeyScript2 = GetScriptForDestination(pubKeyMasternode.GetID());

    if (pubkeyScript2.size() != 25) {
        LogPrint(BCLog::MASTERNODE,"mnb - pubkey2 the wrong size\n");
        nDos = 100;
        return false;
    }

    if (!vin.scriptSig.empty()) {
        LogPrint(BCLog::MASTERNODE,"mnb - Ignore Not Empty ScriptSig %s\n", vin.prevout.hash.ToString());
        return false;
    }

    std::string strError = "";
    if (!CheckSignature())
    {
        // don't ban for old masternodes, their sigs could be broken because of the bug
        nDos = protocolVersion < MIN_PEER_MNANNOUNCE ? 0 : 100;
        return error("%s : Got bad Masternode address signature", __func__);
    }

    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (addr.GetPort() != 51472) return false;
    } else if (addr.GetPort() == 51472)
        return false;

    // incorrect ping or its sigTime
    if(lastPing.IsNull() || !lastPing.CheckAndUpdate(nDos, false, true)) {
        return false;
    }

    //search existing Masternode list, this is where we update existing Masternodes with new mnb broadcasts
    CMasternode* pmn = mnodeman.Find(vin.prevout);

    // no such masternode, nothing to update
    if (pmn == NULL) return true;

    // this broadcast is older or equal than the one that we already have - it's bad and should never happen
    // unless someone is doing something fishy
    // (mapSeenMasternodeBroadcast in CMasternodeMan::ProcessMessage should filter legit duplicates)
    if(pmn->sigTime >= sigTime) {
        return error("%s : Bad sigTime %d for Masternode %20s %105s (existing broadcast is at %d)",
                      __func__, sigTime, addr.ToString(), vin.ToString(), pmn->sigTime);
    }

    // masternode is not enabled yet/already, nothing to update
    if (!pmn->IsEnabled()) return true;

    // mn.pubkey = pubkey, IsVinAssociatedWithPubkey is validated once below,
    //   after that they just need to match
    if (pmn->pubKeyCollateralAddress == pubKeyCollateralAddress && !pmn->IsBroadcastedWithin(MasternodeBroadcastSeconds())) {
        //take the newest entry
        LogPrint(BCLog::MASTERNODE,"mnb - Got updated entry for %s\n", vin.prevout.hash.ToString());
        if (pmn->UpdateFromNewBroadcast((*this))) {
            if (pmn->IsEnabled()) Relay();
        }
        masternodeSync.AddedMasternodeList(GetHash());
    }

    return true;
}

bool CMasternodeBroadcast::CheckInputsAndAdd(int nChainHeight, int& nDoS)
{
    // we are a masternode with the same vin (i.e. already activated) and this mnb is ours (matches our Masternode privkey)
    // so nothing to do here for us
    if (fMasterNode && activeMasternode.vin != nullopt &&
            vin.prevout == activeMasternode.vin->prevout &&
            pubKeyMasternode == activeMasternode.pubKeyMasternode &&
            activeMasternode.GetStatus() == ACTIVE_MASTERNODE_STARTED) {
        return true;
    }

    // incorrect ping or its sigTime
    if(lastPing.IsNull() || !lastPing.CheckAndUpdate(nDoS, false, true)) {
        return false;
    }

    // search existing Masternode list
    CMasternode* pmn = mnodeman.Find(vin.prevout);
    if (pmn != NULL) {
        // nothing to do here if we already know about this masternode and it's enabled
        if (pmn->IsEnabled()) return true;
        // if it's not enabled, remove old MN first and continue
        else
            mnodeman.Remove(pmn->vin.prevout);
    }

    const Coin& collateralUtxo = pcoinsTip->AccessCoin(vin.prevout);
    if (collateralUtxo.IsSpent()) {
        LogPrint(BCLog::MASTERNODE,"mnb - vin %s spent\n", vin.prevout.ToString());
        return false;
    }

    LogPrint(BCLog::MASTERNODE, "mnb - Accepted Masternode entry\n");
    const int utxoHeight = collateralUtxo.nHeight;
    int collateralUtxoDepth = nChainHeight - utxoHeight + 1;
    if (collateralUtxoDepth < MasternodeCollateralMinConf()) {
        LogPrint(BCLog::MASTERNODE,"mnb - Input must have at least %d confirmations\n", MasternodeCollateralMinConf());
        // maybe we miss few blocks, let this mnb to be checked again later
        mnodeman.mapSeenMasternodeBroadcast.erase(GetHash());
        masternodeSync.mapSeenSyncMNB.erase(GetHash());
        return false;
    }

    // verify that sig time is legit in past
    // should be at least not earlier than block when 1000 PIV tx got MASTERNODE_MIN_CONFIRMATIONS
    CBlockIndex* pConfIndex = WITH_LOCK(cs_main, return chainActive[utxoHeight + MasternodeCollateralMinConf() - 1]); // block where tx got MASTERNODE_MIN_CONFIRMATIONS
    if (pConfIndex->GetBlockTime() > sigTime) {
        LogPrint(BCLog::MASTERNODE,"mnb - Bad sigTime %d for Masternode %s (%i conf block is at %d)\n",
            sigTime, vin.prevout.hash.ToString(), MasternodeCollateralMinConf(), pConfIndex->GetBlockTime());
        return false;
    }

    LogPrint(BCLog::MASTERNODE,"mnb - Got NEW Masternode entry - %s - %lli \n", vin.prevout.hash.ToString(), sigTime);
    CMasternode mn(*this);
    mnodeman.Add(mn);

    // if it matches our Masternode privkey, then we've been remotely activated
    if (pubKeyMasternode == activeMasternode.pubKeyMasternode && protocolVersion == PROTOCOL_VERSION) {
        activeMasternode.EnableHotColdMasterNode(vin, addr);
    }

    bool isLocal = (addr.IsRFC1918() || addr.IsLocal()) && !Params().IsRegTestNet();

    if (!isLocal) Relay();

    return true;
}

void CMasternodeBroadcast::Relay()
{
    CInv inv(MSG_MASTERNODE_ANNOUNCE, GetHash());
    g_connman->RelayInv(inv);
}

uint256 CMasternodeBroadcast::GetHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << sigTime;
    ss << pubKeyCollateralAddress;
    return ss.GetHash();
}

CMasternodePing::CMasternodePing() :
        CSignedMessage(),
        vin(),
        blockHash(),
        sigTime(0)
{ }

CMasternodePing::CMasternodePing(const CTxIn& newVin, const uint256& nBlockHash, uint64_t _sigTime) :
        CSignedMessage(),
        vin(newVin),
        blockHash(nBlockHash),
        sigTime(_sigTime)
{ }

uint256 CMasternodePing::GetHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << vin;
    if (nMessVersion == MessageVersion::MESS_VER_HASH) ss << blockHash;
    ss << sigTime;
    return ss.GetHash();
}

std::string CMasternodePing::GetStrMessage() const
{
    return vin.ToString() + blockHash.ToString() + std::to_string(sigTime);
}

bool CMasternodePing::CheckAndUpdate(int& nDos, bool fRequireAvailable, bool fCheckSigTimeOnly)
{
    if (sigTime > GetAdjustedTime() + 60 * 60) {
        LogPrint(BCLog::MNPING,"%s: Signature rejected, too far into the future %s\n", __func__, vin.prevout.hash.ToString());
        nDos = 30;
        return false;
    }

    if (sigTime <= GetAdjustedTime() - 60 * 60) {
        LogPrint(BCLog::MNPING,"%s: Signature rejected, too far into the past %s - %d %d \n", __func__, vin.prevout.hash.ToString(), sigTime, GetAdjustedTime());
        nDos = 30;
        return false;
    }

    // Check if the ping block hash exists and it's within 24 blocks from the tip
    if (!mnodeman.IsWithinDepth(blockHash, 2 * MNPING_DEPTH)) {
        LogPrint(BCLog::MNPING,"%s: Masternode %s block hash %s is too old or has an invalid block hash\n",
                                        __func__, vin.prevout.hash.ToString(), blockHash.ToString());
        // don't ban peers relaying stale data before the active protocol enforcement
        nDos = (ActiveProtocol() < MIN_PEER_CACHEDVERSION ? 0 : 33);
        return false;
    }

    // see if we have this Masternode
    CMasternode* pmn = mnodeman.Find(vin.prevout);
    const bool isMasternodeFound = (pmn != nullptr);
    const bool isSignatureValid = (isMasternodeFound && CheckSignature(pmn->pubKeyMasternode.GetID()));

    if(fCheckSigTimeOnly) {
        if (isMasternodeFound && !isSignatureValid) {
            nDos = 33;
            return false;
        }
        return true;
    }

    LogPrint(BCLog::MNPING, "%s: New Ping - %s - %s - %lli\n", __func__, GetHash().ToString(), blockHash.ToString(), sigTime);

    if (isMasternodeFound && pmn->protocolVersion >= ActiveProtocol()) {

        // Update ping only if the masternode is in available state (pre-enabled or enabled)
        if (fRequireAvailable && !pmn->IsAvailableState()) {
            nDos = 20;
            return false;
        }

        // update only if there is no known ping for this masternode or
        // last ping was more then MASTERNODE_MIN_MNP_SECONDS-60 ago comparing to this one
        if (!pmn->IsPingedWithin(MasternodeMinPingSeconds() - 60, sigTime)) {
            if (!isSignatureValid) {
                nDos = 33;
                return false;
            }

            // ping have passed the basic checks, can be updated now
            mnodeman.mapSeenMasternodePing.emplace(GetHash(), *this);

            // SetLastPing locks masternode cs. Be careful with the lock ordering.
            pmn->SetLastPing(*this);

            //mnodeman.mapSeenMasternodeBroadcast.lastPing is probably outdated, so we'll update it
            CMasternodeBroadcast mnb(*pmn);
            const uint256& hash = mnb.GetHash();
            if (mnodeman.mapSeenMasternodeBroadcast.count(hash)) {
                mnodeman.mapSeenMasternodeBroadcast[hash].lastPing = *this;
            }

            if (!pmn->IsEnabled()) return false;

            LogPrint(BCLog::MNPING, "%s: Masternode ping accepted, vin: %s\n", __func__, vin.prevout.hash.ToString());

            Relay();
            return true;
        }
        LogPrint(BCLog::MNPING, "%s: Masternode ping arrived too early, vin: %s\n", __func__, vin.prevout.hash.ToString());
        //nDos = 1; //disable, this is happening frequently and causing banned peers
        return false;
    }
    LogPrint(BCLog::MNPING, "%s: Couldn't find compatible Masternode entry, vin: %s\n", __func__, vin.prevout.hash.ToString());

    return false;
}

void CMasternodePing::Relay()
{
    CInv inv(MSG_MASTERNODE_PING, GetHash());
    g_connman->RelayInv(inv);
}

MasternodeRef MakeMasternodeRefForDMN(const CDeterministicMNCPtr& dmn)
{
    // create legacy masternode for DMN
    int refHeight = std::max(dmn->pdmnState->nRegisteredHeight, dmn->pdmnState->nPoSeRevivedHeight);
    const CBlockIndex* pindex = WITH_LOCK(cs_main, return mapBlockIndex.at(chainActive[refHeight]->GetBlockHash()); );
    return std::make_shared<CMasternode>(CMasternode(dmn, pindex->GetBlockTime(), pindex->GetBlockHash()));
}
