// Copyright (c) 2014-2016 The Dash developers
// Copyright (c) 2016-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "spork.h"

#include "netmessagemaker.h"
#include "net_processing.h"
#include "sporkdb.h"
#include "validation.h"

#include <iostream>

#define MAKE_SPORK_DEF(name, defaultValue) CSporkDef(name, defaultValue, #name)

std::vector<CSporkDef> sporkDefs = {
    MAKE_SPORK_DEF(SPORK_5_MAX_VALUE,                       1000),          // 1000 PIV
    MAKE_SPORK_DEF(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT,  4070908800ULL), // OFF
    MAKE_SPORK_DEF(SPORK_9_MASTERNODE_BUDGET_ENFORCEMENT,   4070908800ULL), // OFF
    MAKE_SPORK_DEF(SPORK_13_ENABLE_SUPERBLOCKS,             4070908800ULL), // OFF
    MAKE_SPORK_DEF(SPORK_14_NEW_PROTOCOL_ENFORCEMENT,       4070908800ULL), // OFF
    MAKE_SPORK_DEF(SPORK_15_NEW_PROTOCOL_ENFORCEMENT_2,     4070908800ULL), // OFF
    MAKE_SPORK_DEF(SPORK_16_ZEROCOIN_MAINTENANCE_MODE,      4070908800ULL), // OFF
    MAKE_SPORK_DEF(SPORK_18_ZEROCOIN_PUBLICSPEND_V4,        4070908800ULL), // OFF
    MAKE_SPORK_DEF(SPORK_19_COLDSTAKING_MAINTENANCE,        4070908800ULL), // OFF
    MAKE_SPORK_DEF(SPORK_20_SAPLING_MAINTENANCE,            4070908800ULL), // OFF
    MAKE_SPORK_DEF(SPORK_21_LEGACY_MNS_MAX_HEIGHT,          4070908800ULL), // OFF
};

CSporkManager sporkManager;
std::map<uint256, CSporkMessage> mapSporks;

CSporkManager::CSporkManager()
{
    for (auto& sporkDef : sporkDefs) {
        sporkDefsById.emplace(sporkDef.sporkId, &sporkDef);
        sporkDefsByName.emplace(sporkDef.name, &sporkDef);
    }
}

void CSporkManager::Clear()
{
    strMasterPrivKey = "";
    mapSporksActive.clear();
}

// PIVX: on startup load spork values from previous session if they exist in the sporkDB
void CSporkManager::LoadSporksFromDB()
{
    for (const auto& sporkDef : sporkDefs) {
        // attempt to read spork from sporkDB
        CSporkMessage spork;
        if (!pSporkDB->ReadSpork(sporkDef.sporkId, spork)) {
            LogPrintf("%s : no previous value for %s found in database\n", __func__, sporkDef.name);
            continue;
        }

        // TODO: Temporary workaround for v5.0 clients to ensure up-to-date protocol version spork
        if (spork.nSporkID == SPORK_15_NEW_PROTOCOL_ENFORCEMENT_2) {
            LogPrintf("%s : Spork 15 signed at %d\n", __func__, spork.nTimeSigned);
            // 1578338986 is the timestamp that spork 15 was last signed at for mainnet for the previous
            // protocol bump. If the timestamp in the DB is equal or lower than this, we know that
            // the value is stale and should ignore it to prevent un-necessary disconnections in the
            // version handshake process. This value is also suitable for testnet as the timestamp
            // for this spork on that network was signed shortly after this.
            if (spork.nTimeSigned <= 1578338986 ) {
                LogPrintf("%s : Stale spork 15 detected, clearing...\n", __func__);
                CSporkManager::Clear();
                return;
            }
        }

        // add spork to memory
        AddOrUpdateSporkMessage(spork);

        std::time_t result = spork.nValue;
        // If SPORK Value is greater than 1,000,000 assume it's actually a Date and then convert to a more readable format
        std::string sporkName = sporkManager.GetSporkNameByID(spork.nSporkID);
        if (spork.nValue > 1000000) {
            char* res = std::ctime(&result);
            LogPrintf("%s : loaded spork %s with value %d : %s\n", __func__, sporkName.c_str(), spork.nValue,
                      ((res) ? res : "no time") );
        } else {
            LogPrintf("%s : loaded spork %s with value %d\n", __func__,
                      sporkName, spork.nValue);
        }
    }
}

void CSporkManager::ProcessSpork(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (fLiteMode) return; // disable all masternode related functionality

    if (strCommand == NetMsgType::SPORK) {
        int banScore = ProcessSporkMsg(vRecv);
        if (banScore > 0) {
            LOCK(cs_main);
            Misbehaving(pfrom->GetId(), banScore);
        }
    }
    if (strCommand == NetMsgType::GETSPORKS) {
        ProcessGetSporks(pfrom, strCommand, vRecv);
    }
}

int CSporkManager::ProcessSporkMsg(CDataStream& vRecv)
{
    CSporkMessage spork;
    vRecv >> spork;
    return ProcessSporkMsg(spork);
}

int CSporkManager::ProcessSporkMsg(CSporkMessage& spork)
{
    // Ignore spork messages about unknown/deleted sporks
    std::string strSpork = sporkManager.GetSporkNameByID(spork.nSporkID);
    if (strSpork == "Unknown") return 0;

    // Do not accept sporks signed way too far into the future
    if (spork.nTimeSigned > GetAdjustedTime() + 2 * 60 * 60) {
        LogPrint(BCLog::SPORKS, "%s : ERROR: too far into the future\n", __func__);
        return 100;
    }

    // reject old signature version
    if (spork.nMessVersion != MessageVersion::MESS_VER_HASH) {
        LogPrint(BCLog::SPORKS, "%s : nMessVersion=%d not accepted anymore\n", __func__, spork.nMessVersion);
        return 0;
    }

    std::string sporkName = sporkManager.GetSporkNameByID(spork.nSporkID);
    std::string strStatus;
    {
        LOCK(cs);
        if (mapSporksActive.count(spork.nSporkID)) {
            // spork is active
            if (mapSporksActive[spork.nSporkID].nTimeSigned >= spork.nTimeSigned) {
                // spork in memory has been signed more recently
                LogPrint(BCLog::SPORKS, "%s : spork %d (%s) in memory is more recent: %d >= %d\n", __func__,
                          spork.nSporkID, sporkName,
                          mapSporksActive[spork.nSporkID].nTimeSigned, spork.nTimeSigned);
                return 0;
            } else {
                // update active spork
                strStatus = "updated";
            }
        } else {
            // spork is not active
            strStatus = "new";
        }
    }

    const bool fRequireNew = spork.nTimeSigned >= Params().GetConsensus().nTime_EnforceNewSporkKey;
    bool fValidSig = spork.CheckSignature(spork.GetPublicKey().GetID());
    if (!fValidSig && !fRequireNew) {
        // See if window is open that allows for old spork key to sign messages
        if (GetAdjustedTime() < Params().GetConsensus().nTime_RejectOldSporkKey) {
            CPubKey pubkeyold = spork.GetPublicKeyOld();
            fValidSig = spork.CheckSignature(pubkeyold.GetID());
        }
    }

    if (!fValidSig) {
        LogPrint(BCLog::SPORKS, "%s : Invalid Signature\n", __func__);
        return 100;
    }

    // Log valid spork value change
    LogPrintf("%s : got %s spork %d (%s) with value %d (signed at %d)\n", __func__,
              strStatus, spork.nSporkID, sporkName, spork.nValue, spork.nTimeSigned);

    AddOrUpdateSporkMessage(spork);
    spork.Relay();

    // PIVX: add to spork database.
    pSporkDB->WriteSpork(spork.nSporkID, spork);
    // All good.
    return 0;
}

void CSporkManager::ProcessGetSporks(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    LOCK(cs);

    std::map<SporkId, CSporkMessage>::iterator it = mapSporksActive.begin();

    while (it != mapSporksActive.end()) {
        g_connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::SPORK, it->second));
        it++;
    }

    // end message
    if (Params().IsRegTestNet()) {
        // For now, only use it on regtest.
        CSporkMessage msg(SPORK_INVALID, 0, 0);
        g_connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::SPORK, msg));
    }
}

bool CSporkManager::UpdateSpork(SporkId nSporkID, int64_t nValue)
{
    CSporkMessage spork(nSporkID, nValue, GetTime());

    if (spork.Sign(strMasterPrivKey)) {
        spork.Relay();
        AddOrUpdateSporkMessage(spork);
        return true;
    }

    return false;
}

void CSporkManager::AddOrUpdateSporkMessage(const CSporkMessage& spork)
{
    LOCK(cs);
    mapSporks[spork.GetHash()] = spork;
    mapSporksActive[spork.nSporkID] = spork;
}

// grab the spork value, and see if it's off
bool CSporkManager::IsSporkActive(SporkId nSporkID)
{
    return GetSporkValue(nSporkID) < GetAdjustedTime();
}

// grab the value of the spork on the network, or the default
int64_t CSporkManager::GetSporkValue(SporkId nSporkID)
{
    LOCK(cs);

    if (mapSporksActive.count(nSporkID)) {
        return mapSporksActive[nSporkID].nValue;

    } else {
        auto it = sporkDefsById.find(nSporkID);
        if (it != sporkDefsById.end()) {
            return it->second->defaultValue;
        } else {
            LogPrintf("%s : Unknown Spork %d\n", __func__, nSporkID);
        }
    }

    return -1;
}

SporkId CSporkManager::GetSporkIDByName(std::string strName)
{
    auto it = sporkDefsByName.find(strName);
    if (it == sporkDefsByName.end()) {
        LogPrintf("%s : Unknown Spork name '%s'\n", __func__, strName);
        return SPORK_INVALID;
    }
    return it->second->sporkId;
}

std::string CSporkManager::GetSporkNameByID(SporkId nSporkID)
{
    auto it = sporkDefsById.find(nSporkID);
    if (it == sporkDefsById.end()) {
        LogPrint(BCLog::SPORKS, "%s : Unknown Spork ID %d\n", __func__, nSporkID);
        return "Unknown";
    }
    return it->second->name;
}

bool CSporkManager::SetPrivKey(std::string strPrivKey)
{
    CSporkMessage spork;

    spork.Sign(strPrivKey);

    bool fValidSig = spork.CheckSignature(spork.GetPublicKey().GetID());
    if (!fValidSig) {
        // See if window is open that allows for old spork key to sign messages
        if (GetAdjustedTime() < Params().GetConsensus().nTime_RejectOldSporkKey) {
            CPubKey pubkeyold = spork.GetPublicKeyOld();
            fValidSig = spork.CheckSignature(pubkeyold.GetID());
        }
    }
    if (fValidSig) {
        LOCK(cs);
        // Test signing successful, proceed
        LogPrintf("%s : Successfully initialized as spork signer\n", __func__);
        strMasterPrivKey = strPrivKey;
        return true;
    }

    return false;
}

std::string CSporkManager::ToString() const
{
    LOCK(cs);
    return strprintf("Sporks: %llu", mapSporksActive.size());
}

uint256 CSporkMessage::GetSignatureHash() const
{
    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << nMessVersion;
    ss << nSporkID;
    ss << nValue;
    ss << nTimeSigned;
    return ss.GetHash();
}

std::string CSporkMessage::GetStrMessage() const
{
    return std::to_string(nSporkID) +
            std::to_string(nValue) +
            std::to_string(nTimeSigned);
}

const CPubKey CSporkMessage::GetPublicKey() const
{
    return CPubKey(ParseHex(Params().GetConsensus().strSporkPubKey));
}

const CPubKey CSporkMessage::GetPublicKeyOld() const
{
    return CPubKey(ParseHex(Params().GetConsensus().strSporkPubKeyOld));
}

void CSporkMessage::Relay()
{
    CInv inv(MSG_SPORK, GetHash());
    g_connman->RelayInv(inv);
}

