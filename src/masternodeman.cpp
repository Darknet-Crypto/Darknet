// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "masternodeman.h"

#include "addrman.h"
#include "evo/deterministicmns.h"
#include "fs.h"
#include "masternode-payments.h"
#include "masternode-sync.h"
#include "masternode.h"
#include "messagesigner.h"
#include "netbase.h"
#include "netmessagemaker.h"
#include "net_processing.h"
#include "spork.h"

#include <boost/thread/thread.hpp>

#define MN_WINNER_MINIMUM_AGE 8000    // Age in seconds. This should be > MASTERNODE_REMOVAL_SECONDS to avoid misconfigured new nodes in the list.

/** Masternode manager */
CMasternodeMan mnodeman;
/** Keep track of the active Masternode */
CActiveMasternode activeMasternode;

struct CompareScoreMN {
    template <typename T>
    bool operator()(const std::pair<int64_t, T>& t1,
        const std::pair<int64_t, T>& t2) const
    {
        return t1.first < t2.first;
    }
};

//
// CMasternodeDB
//

static const int MASTERNODE_DB_VERSION = 1;

CMasternodeDB::CMasternodeDB()
{
    pathMN = GetDataDir() / "mncache.dat";
    strMagicMessage = "MasternodeCache";
}

bool CMasternodeDB::Write(const CMasternodeMan& mnodemanToSave)
{
    int64_t nStart = GetTimeMillis();

    // serialize, checksum data up to that point, then append checksum
    CDataStream ssMasternodes(SER_DISK, CLIENT_VERSION);
    ssMasternodes << MASTERNODE_DB_VERSION;
    ssMasternodes << strMagicMessage;                   // masternode cache file specific magic message
    ssMasternodes << Params().MessageStart(); // network specific magic number
    ssMasternodes << mnodemanToSave;
    uint256 hash = Hash(ssMasternodes.begin(), ssMasternodes.end());
    ssMasternodes << hash;

    // open output file, and associate with CAutoFile
    FILE* file = fsbridge::fopen(pathMN, "wb");
    CAutoFile fileout(file, SER_DISK, CLIENT_VERSION);
    if (fileout.IsNull())
        return error("%s : Failed to open file %s", __func__, pathMN.string());

    // Write and commit header, data
    try {
        fileout << ssMasternodes;
    } catch (const std::exception& e) {
        return error("%s : Serialize or I/O error - %s", __func__, e.what());
    }
    //    FileCommit(fileout);
    fileout.fclose();

    LogPrint(BCLog::MASTERNODE,"Written info to mncache.dat  %dms\n", GetTimeMillis() - nStart);
    LogPrint(BCLog::MASTERNODE,"  %s\n", mnodemanToSave.ToString());

    return true;
}

CMasternodeDB::ReadResult CMasternodeDB::Read(CMasternodeMan& mnodemanToLoad)
{
    int64_t nStart = GetTimeMillis();
    // open input file, and associate with CAutoFile
    FILE* file = fsbridge::fopen(pathMN, "rb");
    CAutoFile filein(file, SER_DISK, CLIENT_VERSION);
    if (filein.IsNull()) {
        error("%s : Failed to open file %s", __func__, pathMN.string());
        return FileError;
    }

    // use file size to size memory buffer
    int fileSize = fs::file_size(pathMN);
    int dataSize = fileSize - sizeof(uint256);
    // Don't try to resize to a negative number if file is small
    if (dataSize < 0)
        dataSize = 0;
    std::vector<unsigned char> vchData;
    vchData.resize(dataSize);
    uint256 hashIn;

    // read data and checksum from file
    try {
        filein.read((char*)vchData.data(), dataSize);
        filein >> hashIn;
    } catch (const std::exception& e) {
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return HashReadError;
    }
    filein.fclose();

    CDataStream ssMasternodes(vchData, SER_DISK, CLIENT_VERSION);

    // verify stored checksum matches input data
    uint256 hashTmp = Hash(ssMasternodes.begin(), ssMasternodes.end());
    if (hashIn != hashTmp) {
        error("%s : Checksum mismatch, data corrupted", __func__);
        return IncorrectHash;
    }

    int version;
    std::string strMagicMessageTmp;
    try {
        // de-serialize file header
        ssMasternodes >> version;
        ssMasternodes >> strMagicMessageTmp;

        // ... verify the message matches predefined one
        if (strMagicMessage != strMagicMessageTmp) {
            error("%s : Invalid masternode cache magic message", __func__);
            return IncorrectMagicMessage;
        }

        // de-serialize file header (network specific magic number) and ..
        std::vector<unsigned char> pchMsgTmp(4);
        ssMasternodes >> MakeSpan(pchMsgTmp);

        // ... verify the network matches ours
        if (memcmp(pchMsgTmp.data(), Params().MessageStart(), pchMsgTmp.size()) != 0) {
            error("%s : Invalid network magic number", __func__);
            return IncorrectMagicNumber;
        }
        // de-serialize data into CMasternodeMan object
        ssMasternodes >> mnodemanToLoad;
    } catch (const std::exception& e) {
        mnodemanToLoad.Clear();
        error("%s : Deserialize or I/O error - %s", __func__, e.what());
        return IncorrectFormat;
    }

    LogPrint(BCLog::MASTERNODE,"Loaded info from mncache.dat (dbversion=%d) %dms\n", version, GetTimeMillis() - nStart);
    LogPrint(BCLog::MASTERNODE,"  %s\n", mnodemanToLoad.ToString());

    return Ok;
}

void DumpMasternodes()
{
    int64_t nStart = GetTimeMillis();

    CMasternodeDB mndb;
    LogPrint(BCLog::MASTERNODE,"Writing info to mncache.dat...\n");
    mndb.Write(mnodeman);

    LogPrint(BCLog::MASTERNODE,"Masternode dump finished  %dms\n", GetTimeMillis() - nStart);
}

CMasternodeMan::CMasternodeMan():
        cvLastBlockHashes(CACHED_BLOCK_HASHES, UINT256_ZERO),
        nDsqCount(0)
{}

bool CMasternodeMan::Add(CMasternode& mn)
{
    // Skip after legacy obsolete. !TODO: remove when transition to DMN is complete
    if (deterministicMNManager->LegacyMNObsolete()) {
        return false;
    }

    if (deterministicMNManager->GetListAtChainTip().HasMNByCollateral(mn.vin.prevout)) {
        LogPrint(BCLog::MASTERNODE, "ERROR: Not Adding Masternode %s as the collateral is already registered with a DMN\n",
                mn.vin.prevout.ToString());
        return false;
    }

    LOCK(cs);

    if (!mn.IsAvailableState())
        return false;

    const auto& it = mapMasternodes.find(mn.vin.prevout);
    if (it == mapMasternodes.end()) {
        LogPrint(BCLog::MASTERNODE, "Adding new Masternode %s\n", mn.vin.prevout.ToString());
        mapMasternodes.emplace(mn.vin.prevout, std::make_shared<CMasternode>(mn));
        LogPrint(BCLog::MASTERNODE, "Masternode added. New total count: %d\n", mapMasternodes.size());
        return true;
    }

    return false;
}

void CMasternodeMan::AskForMN(CNode* pnode, const CTxIn& vin)
{
    // Skip after legacy obsolete. !TODO: remove when transition to DMN is complete
    if (deterministicMNManager->LegacyMNObsolete()) {
        return;
    }

    std::map<COutPoint, int64_t>::iterator i = mWeAskedForMasternodeListEntry.find(vin.prevout);
    if (i != mWeAskedForMasternodeListEntry.end()) {
        int64_t t = (*i).second;
        if (GetTime() < t) return; // we've asked recently
    }

    // ask for the mnb info once from the node that sent mnp

    LogPrint(BCLog::MASTERNODE, "CMasternodeMan::AskForMN - Asking node for missing entry, vin: %s\n", vin.prevout.hash.ToString());
    g_connman->PushMessage(pnode, CNetMsgMaker(pnode->GetSendVersion()).Make(NetMsgType::GETMNLIST, vin));
    int64_t askAgain = GetTime() + MasternodeMinPingSeconds();
    mWeAskedForMasternodeListEntry[vin.prevout] = askAgain;
}

int CMasternodeMan::CheckAndRemove(bool forceExpiredRemoval)
{
    // Skip after legacy obsolete. !TODO: remove when transition to DMN is complete
    if (deterministicMNManager->LegacyMNObsolete()) {
        LogPrint(BCLog::MASTERNODE, "Removing all legacy mn due to SPORK 21\n");
        Clear();
        return 0;
    }

    LOCK(cs);

    //remove inactive and outdated (or replaced by DMN)
    auto it = mapMasternodes.begin();
    while (it != mapMasternodes.end()) {
        MasternodeRef& mn = it->second;
        auto activeState = mn->GetActiveState();
        if (activeState == CMasternode::MASTERNODE_REMOVE ||
            activeState == CMasternode::MASTERNODE_VIN_SPENT ||
            (forceExpiredRemoval && activeState == CMasternode::MASTERNODE_EXPIRED) ||
            mn->protocolVersion < ActiveProtocol()) {
            LogPrint(BCLog::MASTERNODE, "Removing inactive (legacy) Masternode %s\n", it->first.ToString());
            //erase all of the broadcasts we've seen from this vin
            // -- if we missed a few pings and the node was removed, this will allow is to get it back without them
            //    sending a brand new mnb
            std::map<uint256, CMasternodeBroadcast>::iterator it3 = mapSeenMasternodeBroadcast.begin();
            while (it3 != mapSeenMasternodeBroadcast.end()) {
                if (it3->second.vin == it->second->vin) {
                    masternodeSync.mapSeenSyncMNB.erase((*it3).first);
                    it3 = mapSeenMasternodeBroadcast.erase(it3);
                } else {
                    ++it3;
                }
            }

            // allow us to ask for this masternode again if we see another ping
            std::map<COutPoint, int64_t>::iterator it2 = mWeAskedForMasternodeListEntry.begin();
            while (it2 != mWeAskedForMasternodeListEntry.end()) {
                if (it2->first == it->first) {
                    it2 = mWeAskedForMasternodeListEntry.erase(it2);
                } else {
                    ++it2;
                }
            }

            it = mapMasternodes.erase(it);
            LogPrint(BCLog::MASTERNODE, "Masternode removed.\n");
        } else {
            ++it;
        }
    }
    LogPrint(BCLog::MASTERNODE, "New total masternode count: %d\n", mapMasternodes.size());

    // check who's asked for the Masternode list
    std::map<CNetAddr, int64_t>::iterator it1 = mAskedUsForMasternodeList.begin();
    while (it1 != mAskedUsForMasternodeList.end()) {
        if ((*it1).second < GetTime()) {
            it1 = mAskedUsForMasternodeList.erase(it1);
        } else {
            ++it1;
        }
    }

    // check who we asked for the Masternode list
    it1 = mWeAskedForMasternodeList.begin();
    while (it1 != mWeAskedForMasternodeList.end()) {
        if ((*it1).second < GetTime()) {
            it1 = mWeAskedForMasternodeList.erase(it1);
        } else {
            ++it1;
        }
    }

    // check which Masternodes we've asked for
    std::map<COutPoint, int64_t>::iterator it2 = mWeAskedForMasternodeListEntry.begin();
    while (it2 != mWeAskedForMasternodeListEntry.end()) {
        if ((*it2).second < GetTime()) {
            it2 = mWeAskedForMasternodeListEntry.erase(it2);
        } else {
            ++it2;
        }
    }

    // remove expired mapSeenMasternodeBroadcast
    std::map<uint256, CMasternodeBroadcast>::iterator it3 = mapSeenMasternodeBroadcast.begin();
    while (it3 != mapSeenMasternodeBroadcast.end()) {
        if ((*it3).second.lastPing.sigTime < GetTime() - (MasternodeRemovalSeconds() * 2)) {
            masternodeSync.mapSeenSyncMNB.erase((*it3).second.GetHash());
            it3 = mapSeenMasternodeBroadcast.erase(it3);
        } else {
            ++it3;
        }
    }

    // remove expired mapSeenMasternodePing
    std::map<uint256, CMasternodePing>::iterator it4 = mapSeenMasternodePing.begin();
    while (it4 != mapSeenMasternodePing.end()) {
        if ((*it4).second.sigTime < GetTime() - (MasternodeRemovalSeconds() * 2)) {
            it4 = mapSeenMasternodePing.erase(it4);
        } else {
            ++it4;
        }
    }

    return mapMasternodes.size();
}

void CMasternodeMan::Clear()
{
    LOCK(cs);
    mapMasternodes.clear();
    mAskedUsForMasternodeList.clear();
    mWeAskedForMasternodeList.clear();
    mWeAskedForMasternodeListEntry.clear();
    mapSeenMasternodeBroadcast.clear();
    mapSeenMasternodePing.clear();
    nDsqCount = 0;
}

int CMasternodeMan::stable_size() const
{
    int nStable_size = 0;
    int nMinProtocol = ActiveProtocol();

    for (const auto& it : mapMasternodes) {
        const MasternodeRef& mn = it.second;
        if (mn->protocolVersion < nMinProtocol) {
            continue; // Skip obsolete versions
        }
        if (sporkManager.IsSporkActive (SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT)) {
            if (GetAdjustedTime() - mn->sigTime < MN_WINNER_MINIMUM_AGE) {
                continue; // Skip masternodes younger than (default) 8000 sec (MUST be > MASTERNODE_REMOVAL_SECONDS)
            }
        }

        if (!mn->IsEnabled ())
            continue; // Skip not-enabled masternodes

        nStable_size++;
    }

    return nStable_size;
}

int CMasternodeMan::CountEnabled(int protocolVersion) const
{
    int i = 0;
    protocolVersion = protocolVersion == -1 ? ActiveProtocol() : protocolVersion;

    for (const auto& it : mapMasternodes) {
        const MasternodeRef& mn = it.second;
        if (mn->protocolVersion < protocolVersion || !mn->IsEnabled()) continue;
        i++;
    }

    return i;
}

int CMasternodeMan::CountNetworks(int& ipv4, int& ipv6, int& onion) const
{
    LOCK(cs);
    for (const auto& it : mapMasternodes) {
        const MasternodeRef& mn = it.second;
        std::string strHost;
        int port;
        SplitHostPort(mn->addr.ToString(), port, strHost);
        CNetAddr node;
        LookupHost(strHost.c_str(), node, false);
        int nNetwork = node.GetNetwork();
        switch (nNetwork) {
            case 1 :
                ipv4++;
                break;
            case 2 :
                ipv6++;
                break;
            case 3 :
                onion++;
                break;
        }
    }
    return mapMasternodes.size();
}

void CMasternodeMan::DsegUpdate(CNode* pnode)
{
    // Skip after legacy obsolete. !TODO: remove when transition to DMN is complete
    if (deterministicMNManager->LegacyMNObsolete()) {
        return;
    }

    LOCK(cs);

    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (!(pnode->addr.IsRFC1918() || pnode->addr.IsLocal())) {
            std::map<CNetAddr, int64_t>::iterator it = mWeAskedForMasternodeList.find(pnode->addr);
            if (it != mWeAskedForMasternodeList.end()) {
                if (GetTime() < (*it).second) {
                    LogPrint(BCLog::MASTERNODE, "dseg - we already asked peer %i for the list; skipping...\n", pnode->GetId());
                    return;
                }
            }
        }
    }

    g_connman->PushMessage(pnode, CNetMsgMaker(pnode->GetSendVersion()).Make(NetMsgType::GETMNLIST, CTxIn()));
    int64_t askAgain = GetTime() + MASTERNODES_DSEG_SECONDS;
    mWeAskedForMasternodeList[pnode->addr] = askAgain;
}

CMasternode* CMasternodeMan::Find(const COutPoint& collateralOut)
{
    LOCK(cs);
    auto it = mapMasternodes.find(collateralOut);
    return it != mapMasternodes.end() ? it->second.get() : nullptr;
}

const CMasternode* CMasternodeMan::Find(const COutPoint& collateralOut) const
{
    LOCK(cs);
    auto const& it = mapMasternodes.find(collateralOut);
    return it != mapMasternodes.end() ? it->second.get() : nullptr;
}

CMasternode* CMasternodeMan::Find(const CPubKey& pubKeyMasternode)
{
    LOCK(cs);

    for (auto& it : mapMasternodes) {
        MasternodeRef& mn = it.second;
        if (mn->pubKeyMasternode == pubKeyMasternode)
            return mn.get();
    }
    return nullptr;
}

void CMasternodeMan::CheckSpentCollaterals(const std::vector<CTransactionRef>& vtx)
{
    // Skip after legacy obsolete. !TODO: remove when transition to DMN is complete
    if (deterministicMNManager->LegacyMNObsolete()) {
        return;
    }

    LOCK(cs);
    for (const auto& tx : vtx) {
        for (const auto& in : tx->vin) {
            auto it = mapMasternodes.find(in.prevout);
            if (it != mapMasternodes.end()) {
                it->second->SetSpent();
            }
        }
    }
}

static bool canScheduleMN(bool fFilterSigTime, const MasternodeRef& mn, int minProtocol,
                          int nMnCount, int nBlockHeight)
{
    // check protocol version
    if (mn->protocolVersion < minProtocol) return false;

    // it's in the list (up to 8 entries ahead of current block to allow propagation) -- so let's skip it
    if (masternodePayments.IsScheduled(*mn, nBlockHeight)) return false;

    // it's too new, wait for a cycle
    if (fFilterSigTime && mn->sigTime + (nMnCount * 2.6 * 60) > GetAdjustedTime()) return false;

    // make sure it has as many confirmations as there are masternodes
    if (pcoinsTip->GetCoinDepthAtHeight(mn->vin.prevout, nBlockHeight) < nMnCount) return false;

    return true;
}

//
// Deterministically select the oldest/best masternode to pay on the network
//
MasternodeRef CMasternodeMan::GetNextMasternodeInQueueForPayment(int nBlockHeight, bool fFilterSigTime, int& nCount, const CBlockIndex* pChainTip) const
{
    // Skip after legacy obsolete. !TODO: remove when transition to DMN is complete
    if (deterministicMNManager->LegacyMNObsolete()) {
        LogPrintf("%s: ERROR - called after legacy system disabled\n", __func__);
        return nullptr;
    }

    AssertLockNotHeld(cs_main);
    const CBlockIndex* BlockReading = (pChainTip == nullptr ? GetChainTip() : pChainTip);
    if (!BlockReading) return nullptr;

    MasternodeRef pBestMasternode = nullptr;
    std::vector<std::pair<int64_t, MasternodeRef> > vecMasternodeLastPaid;

    CDeterministicMNList mnList;
    if (deterministicMNManager->IsDIP3Enforced()) {
        mnList = deterministicMNManager->GetListAtChainTip();
    }

    /*
        Make a vector with all of the last paid times
    */
    int minProtocol = ActiveProtocol();
    int nMnCount = mnList.GetValidMNsCount();
    {
        LOCK(cs);
        nMnCount += CountEnabled();
        for (const auto& it : mapMasternodes) {
            if (!it.second->IsEnabled()) continue;
            if (canScheduleMN(fFilterSigTime, it.second, minProtocol, nMnCount, nBlockHeight)) {
                vecMasternodeLastPaid.emplace_back(SecondsSincePayment(it.second, BlockReading), it.second);
            }
        }
    }
    // Add deterministic masternodes to the vector
    mnList.ForEachMN(true, [&](const CDeterministicMNCPtr& dmn) {
        const MasternodeRef mn = MakeMasternodeRefForDMN(dmn);
        if (canScheduleMN(fFilterSigTime, mn, minProtocol, nMnCount, nBlockHeight)) {
            vecMasternodeLastPaid.emplace_back(SecondsSincePayment(mn, BlockReading), mn);
        }
    });

    nCount = (int)vecMasternodeLastPaid.size();

    //when the network is in the process of upgrading, don't penalize nodes that recently restarted
    if (fFilterSigTime && nCount < nMnCount / 3) return GetNextMasternodeInQueueForPayment(nBlockHeight, false, nCount, BlockReading);

    // Sort them high to low
    sort(vecMasternodeLastPaid.rbegin(), vecMasternodeLastPaid.rend(), CompareScoreMN());

    // Look at 1/10 of the oldest nodes (by last payment), calculate their scores and pay the best one
    //  -- This doesn't look at who is being paid in the +8-10 blocks, allowing for double payments very rarely
    //  -- 1/100 payments should be a double payment on mainnet - (1/(3000/10))*2
    //  -- (chance per block * chances before IsScheduled will fire)
    int nTenthNetwork = nMnCount / 10;
    int nCountTenth = 0;
    arith_uint256 nHigh = ARITH_UINT256_ZERO;
    const uint256& hash = GetHashAtHeight(nBlockHeight - 101);
    for (const auto& s: vecMasternodeLastPaid) {
        const MasternodeRef pmn = s.second;
        if (!pmn) break;

        const arith_uint256& n = pmn->CalculateScore(hash);
        if (n > nHigh) {
            nHigh = n;
            pBestMasternode = pmn;
        }
        nCountTenth++;
        if (nCountTenth >= nTenthNetwork) break;
    }
    return pBestMasternode;
}

MasternodeRef CMasternodeMan::GetCurrentMasterNode(const uint256& hash) const
{
    int minProtocol = ActiveProtocol();
    int64_t score = 0;
    MasternodeRef winner = nullptr;

    // scan for winner
    for (const auto& it : mapMasternodes) {
        const MasternodeRef& mn = it.second;
        if (mn->protocolVersion < minProtocol || !mn->IsEnabled()) continue;
        // calculate the score of the masternode
        const int64_t n = mn->CalculateScore(hash).GetCompact(false);
        // determine the winner
        if (n > score) {
            score = n;
            winner = mn;
        }
    }

    // scan also dmns
    if (deterministicMNManager->IsDIP3Enforced()) {
        auto mnList = deterministicMNManager->GetListAtChainTip();
        mnList.ForEachMN(true, [&](const CDeterministicMNCPtr& dmn) {
            const MasternodeRef mn = MakeMasternodeRefForDMN(dmn);
            // calculate the score of the masternode
            const int64_t n = mn->CalculateScore(hash).GetCompact(false);
            // determine the winner
            if (n > score) {
                score = n;
                winner = mn;
            }
        });
    }

    return winner;
}

std::vector<std::pair<MasternodeRef, int>> CMasternodeMan::GetMnScores(int nLast) const
{
    std::vector<std::pair<MasternodeRef, int>> ret;
    int nChainHeight = GetBestHeight();
    if (nChainHeight < 0) return ret;

    for (int nHeight = nChainHeight - nLast; nHeight < nChainHeight + 20; nHeight++) {
        const uint256& hash = GetHashAtHeight(nHeight - 101);
        MasternodeRef winner = GetCurrentMasterNode(hash);
        if (winner) {
            ret.emplace_back(winner, nHeight);
        }
    }
    return ret;
}

int CMasternodeMan::GetMasternodeRank(const CTxIn& vin, int64_t nBlockHeight) const
{
    const uint256& hash = GetHashAtHeight(nBlockHeight - 1);
    // height outside range
    if (hash == UINT256_ZERO) return -1;

    // scan for winner
    int minProtocol = ActiveProtocol();
    std::vector<std::pair<int64_t, CTxIn> > vecMasternodeScores;
    {
        LOCK(cs);
        for (const auto& it : mapMasternodes) {
            const MasternodeRef& mn = it.second;
            if (!mn->IsEnabled()) {
                continue; // Skip not enabled
            }
            if (mn->protocolVersion < minProtocol) {
                LogPrint(BCLog::MASTERNODE,"Skipping Masternode with obsolete version %d\n", mn->protocolVersion);
                continue; // Skip obsolete versions
            }
            if (sporkManager.IsSporkActive(SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT) &&
                    GetAdjustedTime() - mn->sigTime < MN_WINNER_MINIMUM_AGE) {
                continue; // Skip masternodes younger than (default) 1 hour
            }
            vecMasternodeScores.emplace_back(mn->CalculateScore(hash).GetCompact(false), mn->vin);
        }
    }

    // scan also dmns
    if (deterministicMNManager->IsDIP3Enforced()) {
        auto mnList = deterministicMNManager->GetListAtChainTip();
        mnList.ForEachMN(true, [&](const CDeterministicMNCPtr& dmn) {
            const MasternodeRef mn = MakeMasternodeRefForDMN(dmn);
            vecMasternodeScores.emplace_back(mn->CalculateScore(hash).GetCompact(false), mn->vin);
        });
    }

    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareScoreMN());

    int rank = 0;
    for (std::pair<int64_t, CTxIn> & s : vecMasternodeScores) {
        rank++;
        if (s.second.prevout == vin.prevout) {
            return rank;
        }
    }

    return -1;
}

std::vector<std::pair<int64_t, MasternodeRef>> CMasternodeMan::GetMasternodeRanks(int nBlockHeight) const
{
    std::vector<std::pair<int64_t, MasternodeRef>> vecMasternodeScores;
    const uint256& hash = GetHashAtHeight(nBlockHeight - 1);
    // height outside range
    if (hash == UINT256_ZERO) return vecMasternodeScores;
    {
        LOCK(cs);
        // scan for winner
        for (const auto& it : mapMasternodes) {
            const MasternodeRef mn = it.second;
            const uint32_t score = mn->IsEnabled() ? mn->CalculateScore(hash).GetCompact(false) : 9999;

            vecMasternodeScores.emplace_back(score, mn);
        }
    }
    // scan also dmns
    if (deterministicMNManager->IsDIP3Enforced()) {
        auto mnList = deterministicMNManager->GetListAtChainTip();
        mnList.ForEachMN(false, [&](const CDeterministicMNCPtr& dmn) {
            const MasternodeRef mn = MakeMasternodeRefForDMN(dmn);
            const uint32_t score = mnList.IsMNValid(dmn) ? mn->CalculateScore(hash).GetCompact(false) : 9999;

            vecMasternodeScores.emplace_back(score, mn);
        });
    }
    sort(vecMasternodeScores.rbegin(), vecMasternodeScores.rend(), CompareScoreMN());
    return vecMasternodeScores;
}

int CMasternodeMan::ProcessMNBroadcast(CNode* pfrom, CMasternodeBroadcast& mnb)
{
    const uint256& mnbHash = mnb.GetHash();
    if (mapSeenMasternodeBroadcast.count(mnbHash)) { //seen
        masternodeSync.AddedMasternodeList(mnbHash);
        return 0;
    }

    int nDoS = 0;
    if (!mnb.CheckAndUpdate(nDoS)) {
        return nDoS;
    }

    // make sure the vout that was signed is related to the transaction that spawned the Masternode
    //  - this is expensive, so it's only done once per Masternode
    if (!mnb.IsInputAssociatedWithPubkey()) {
        LogPrintf("CMasternodeMan::ProcessMessage() : mnb - Got mismatched pubkey and vin\n");
        return 33;
    }

    // now that did the basic mnb checks, can add it.
    mapSeenMasternodeBroadcast.emplace(mnbHash, mnb);

    // make sure it's still unspent
    //  - this is checked later by .check() in many places and by ThreadCheckObfuScationPool()
    if (mnb.CheckInputsAndAdd(GetBestHeight(), nDoS)) {
        // use this as a peer
        g_connman->AddNewAddress(CAddress(mnb.addr, NODE_NETWORK), pfrom->addr, 2 * 60 * 60);
        masternodeSync.AddedMasternodeList(mnbHash);
    } else {
        LogPrint(BCLog::MASTERNODE,"mnb - Rejected Masternode entry %s\n", mnb.vin.prevout.hash.ToString());
        return nDoS;
    }
    // All good
    return 0;
}

int CMasternodeMan::ProcessMNPing(CNode* pfrom, CMasternodePing& mnp)
{
    const uint256& mnpHash = mnp.GetHash();
    if (mapSeenMasternodePing.count(mnpHash)) return 0; //seen

    int nDoS = 0;
    if (mnp.CheckAndUpdate(nDoS)) return 0;

    if (nDoS > 0) {
        // if anything significant failed, mark that node
        return nDoS;
    } else {
        // if nothing significant failed, search existing Masternode list
        CMasternode* pmn = Find(mnp.vin.prevout);
        // if it's known, don't ask for the mnb, just return
        if (pmn != NULL) return 0;
    }

    // something significant is broken or mn is unknown,
    // we might have to ask for a masternode entry once
    AskForMN(pfrom, mnp.vin);

    // All good
    return 0;
}

int CMasternodeMan::ProcessGetMNList(CNode* pfrom, CTxIn& vin)
{
    if (vin.IsNull()) { //only should ask for this once
        //local network
        bool isLocal = (pfrom->addr.IsRFC1918() || pfrom->addr.IsLocal());

        if (!isLocal && Params().NetworkIDString() == CBaseChainParams::MAIN) {
            std::map<CNetAddr, int64_t>::iterator i = mAskedUsForMasternodeList.find(pfrom->addr);
            if (i != mAskedUsForMasternodeList.end()) {
                int64_t t = (*i).second;
                if (GetTime() < t) {
                    LogPrintf("CMasternodeMan::ProcessMessage() : dseg - peer already asked me for the list\n");
                    return 34;
                }
            }
            int64_t askAgain = GetTime() + MASTERNODES_DSEG_SECONDS;
            mAskedUsForMasternodeList[pfrom->addr] = askAgain;
        }
    } //else, asking for a specific node which is ok

    int nInvCount = 0;
    {
        LOCK(cs);
        for (auto& it : mapMasternodes) {
            MasternodeRef& mn = it.second;
            if (mn->addr.IsRFC1918()) continue; //local network

            if (mn->IsEnabled()) {
                LogPrint(BCLog::MASTERNODE, "dseg - Sending Masternode entry - %s \n", mn->vin.prevout.hash.ToString());
                if (vin.IsNull() || vin == mn->vin) {
                    CMasternodeBroadcast mnb = CMasternodeBroadcast(*mn);
                    uint256 hash = mnb.GetHash();
                    pfrom->PushInventory(CInv(MSG_MASTERNODE_ANNOUNCE, hash));
                    nInvCount++;

                    if (!mapSeenMasternodeBroadcast.count(hash)) mapSeenMasternodeBroadcast.emplace(hash, mnb);

                    if (vin == mn->vin) {
                        LogPrint(BCLog::MASTERNODE, "dseg - Sent 1 Masternode entry to peer %i\n", pfrom->GetId());
                        return 0;
                    }
                }
            }
        }
    }

    if (vin.IsNull()) {
        g_connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::SYNCSTATUSCOUNT, MASTERNODE_SYNC_LIST, nInvCount));
        LogPrint(BCLog::MASTERNODE, "dseg - Sent %d Masternode entries to peer %i\n", nInvCount, pfrom->GetId());
    }

    // All good
    return 0;
}

void CMasternodeMan::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    int banScore = ProcessMessageInner(pfrom, strCommand, vRecv);
    if (banScore > 0) {
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), banScore);
    }
}

int CMasternodeMan::ProcessMessageInner(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    if (fLiteMode) return 0; //disable all Masternode related functionality
    if (!masternodeSync.IsBlockchainSynced()) return 0;

    // Skip after legacy obsolete. !TODO: remove when transition to DMN is complete
    if (deterministicMNManager->LegacyMNObsolete()) {
        LogPrint(BCLog::MASTERNODE, "%s: skip obsolete message %s\n", __func__, strCommand);
        return 0;
    }

    LOCK(cs_process_message);

    if (strCommand == NetMsgType::MNBROADCAST) {
        CMasternodeBroadcast mnb;
        vRecv >> mnb;
        return ProcessMNBroadcast(pfrom, mnb);

    } else if (strCommand == NetMsgType::MNPING) {
        //Masternode Ping
        CMasternodePing mnp;
        vRecv >> mnp;
        LogPrint(BCLog::MNPING, "mnp - Masternode ping, vin: %s\n", mnp.vin.prevout.hash.ToString());
        return ProcessMNPing(pfrom, mnp);

    } else if (strCommand == NetMsgType::GETMNLIST) {
        //Get Masternode list or specific entry
        CTxIn vin;
        vRecv >> vin;
        return ProcessGetMNList(pfrom, vin);
    }
    // Nothing to report
    return 0;
}

void CMasternodeMan::Remove(const COutPoint& collateralOut)
{
    LOCK(cs);
    const auto it = mapMasternodes.find(collateralOut);
    if (it != mapMasternodes.end()) {
        mapMasternodes.erase(it);
    }
}

void CMasternodeMan::UpdateMasternodeList(CMasternodeBroadcast& mnb)
{
    // Skip after legacy obsolete. !TODO: remove when transition to DMN is complete
    if (deterministicMNManager->LegacyMNObsolete()) {
        LogPrint(BCLog::MASTERNODE, "Removing all legacy mn due to SPORK 21\n");
        return;
    }

    mapSeenMasternodePing.emplace(mnb.lastPing.GetHash(), mnb.lastPing);
    mapSeenMasternodeBroadcast.emplace(mnb.GetHash(), mnb);
    masternodeSync.AddedMasternodeList(mnb.GetHash());

    LogPrint(BCLog::MASTERNODE,"%s -- masternode=%s\n", __func__, mnb.vin.prevout.ToString());

    CMasternode* pmn = Find(mnb.vin.prevout);
    if (pmn == NULL) {
        CMasternode mn(mnb);
        Add(mn);
    } else {
        pmn->UpdateFromNewBroadcast(mnb);
    }
}

int64_t CMasternodeMan::SecondsSincePayment(const MasternodeRef& mn, const CBlockIndex* BlockReading) const
{
    int64_t sec = (GetAdjustedTime() - GetLastPaid(mn, BlockReading));
    int64_t month = 60 * 60 * 24 * 30;
    if (sec < month) return sec; //if it's less than 30 days, give seconds

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << mn->vin;
    ss << mn->sigTime;
    const arith_uint256& hash = UintToArith256(ss.GetHash());

    // return some deterministic value for unknown/unpaid but force it to be more than 30 days old
    return month + hash.GetCompact(false);
}

int64_t CMasternodeMan::GetLastPaid(const MasternodeRef& mn, const CBlockIndex* BlockReading) const
{
    if (BlockReading == nullptr) return false;

    const CScript& mnpayee = mn->GetPayeeScript();

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << mn->vin;
    ss << mn->sigTime;
    uint256 hash = ss.GetHash();

    // use a deterministic offset to break a tie -- 2.5 minutes
    int64_t nOffset = UintToArith256(hash).GetCompact(false) % 150;

    int nMnCount = CountEnabled() * 1.25;
    for (int n = 0; n < nMnCount; n++) {
        const auto& it = masternodePayments.mapMasternodeBlocks.find(BlockReading->nHeight);
        if (it != masternodePayments.mapMasternodeBlocks.end()) {
            // Search for this payee, with at least 2 votes. This will aid in consensus
            // allowing the network to converge on the same payees quickly, then keep the same schedule.
            if (it->second.HasPayeeWithVotes(mnpayee, 2))
                return BlockReading->nTime + nOffset;
        }
        BlockReading = BlockReading->pprev;

        if (BlockReading == nullptr || BlockReading->nHeight <= 0) {
            break;
        }
    }

    return 0;
}

std::string CMasternodeMan::ToString() const
{
    std::ostringstream info;
    info << "Masternodes: " << (int)mapMasternodes.size()
         << ", peers who asked us for Masternode list: " << (int)mAskedUsForMasternodeList.size()
         << ", peers we asked for Masternode list: " << (int)mWeAskedForMasternodeList.size()
         << ", entries in Masternode list we asked for: " << (int)mWeAskedForMasternodeListEntry.size();
    return info.str();
}

void CMasternodeMan::CacheBlockHash(const CBlockIndex* pindex)
{
    cvLastBlockHashes.Set(pindex->nHeight, pindex->GetBlockHash());
}

void CMasternodeMan::UncacheBlockHash(const CBlockIndex* pindex)
{
    cvLastBlockHashes.Set(pindex->nHeight, UINT256_ZERO);
}

uint256 CMasternodeMan::GetHashAtHeight(int nHeight) const
{
    // return zero if outside bounds
    if (nHeight < 0) {
        LogPrint(BCLog::MASTERNODE, "%s: Negative height. Returning 0\n",  __func__);
        return UINT256_ZERO;
    }
    int nCurrentHeight = GetBestHeight();
    if (nHeight > nCurrentHeight) {
        LogPrint(BCLog::MASTERNODE, "%s: height %d over current height %d. Returning 0\n",
                __func__, nHeight, nCurrentHeight);
        return UINT256_ZERO;
    }

    if (nHeight > nCurrentHeight - (int) CACHED_BLOCK_HASHES) {
        // Use cached hash
        return cvLastBlockHashes.Get(nHeight);
    } else {
        // Use chainActive
        LOCK(cs_main);
        return chainActive[nHeight]->GetBlockHash();
    }
}

bool CMasternodeMan::IsWithinDepth(const uint256& nHash, int depth) const
{
    // Sanity checks
    if (nHash.IsNull()) {
        return error("%s: Called with null hash\n", __func__);
    }
    if (depth < 0 || (unsigned) depth >= CACHED_BLOCK_HASHES) {
        return error("%s: Invalid depth %d. Cached block hashes: %d\n", __func__, depth, CACHED_BLOCK_HASHES);
    }
    // Check last depth blocks to find one with matching hash
    const int nCurrentHeight = GetBestHeight();
    int nStopHeight = std::max(0, nCurrentHeight - depth);
    for (int i = nCurrentHeight; i >= nStopHeight; i--) {
        if (GetHashAtHeight(i) == nHash)
            return true;
    }
    return false;
}

void ThreadCheckMasternodes()
{
    if (fLiteMode) return; //disable all Masternode related functionality

    // Make this thread recognisable as the wallet flushing thread
    util::ThreadRename("pivx-masternodeman");
    LogPrintf("Masternodes thread started\n");

    unsigned int c = 0;

    try {
        // first clean up stale masternode payments data
        masternodePayments.CleanPaymentList(mnodeman.CheckAndRemove(), mnodeman.GetBestHeight());

        while (true) {

            if (ShutdownRequested()) {
                break;
            }

            MilliSleep(1000);
            boost::this_thread::interruption_point();

            // try to sync from all available nodes, one step at a time
            masternodeSync.Process();

            if (masternodeSync.IsBlockchainSynced()) {
                c++;

                // check if we should activate or ping every few minutes,
                // start right after sync is considered to be done
                if (c % (MasternodePingSeconds()/2) == 0)
                    activeMasternode.ManageStatus();

                if (c % (MasternodePingSeconds()/5) == 0) {
                    masternodePayments.CleanPaymentList(mnodeman.CheckAndRemove(), mnodeman.GetBestHeight());
                }
            }
        }
    } catch (boost::thread_interrupted&) {
        // nothing, thread interrupted.
    }
}
