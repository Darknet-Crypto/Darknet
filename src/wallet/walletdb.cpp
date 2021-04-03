// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "wallet/walletdb.h"

#include "fs.h"

#include "base58.h"
#include "protocol.h"
#include "sapling/key_io_sapling.h"
#include "serialize.h"
#include "sync.h"
#include "util.h"
#include "utiltime.h"
#include "wallet/wallet.h"

#include <atomic>
#include <string>

#include <boost/thread.hpp>

static std::atomic<unsigned int> nWalletDBUpdateCounter;

//
// CWalletDB
//

bool CWalletDB::WriteName(const std::string& strAddress, const std::string& strName)
{
    nWalletDBUpdateCounter++;
    return batch.Write(std::make_pair(std::string("name"), strAddress), strName);
}

bool CWalletDB::EraseName(const std::string& strAddress)
{
    // This should only be used for sending addresses, never for receiving addresses,
    // receiving addresses must always have an address book entry if they're not change return.
    nWalletDBUpdateCounter++;
    return batch.Erase(std::make_pair(std::string("name"), strAddress));
}

bool CWalletDB::WritePurpose(const std::string& strAddress, const std::string& strPurpose)
{
    nWalletDBUpdateCounter++;
    return batch.Write(std::make_pair(std::string("purpose"), strAddress), strPurpose);
}

bool CWalletDB::ErasePurpose(const std::string& strPurpose)
{
    nWalletDBUpdateCounter++;
    return batch.Erase(std::make_pair(std::string("purpose"), strPurpose));
}

bool CWalletDB::WriteTx(const CWalletTx& wtx)
{
    nWalletDBUpdateCounter++;
    return batch.Write(std::make_pair(std::string("tx"), wtx.GetHash()), wtx);
}

bool CWalletDB::EraseTx(uint256 hash)
{
    nWalletDBUpdateCounter++;
    return batch.Erase(std::make_pair(std::string("tx"), hash));
}

bool CWalletDB::WriteKey(const CPubKey& vchPubKey, const CPrivKey& vchPrivKey, const CKeyMetadata& keyMeta)
{
    nWalletDBUpdateCounter++;

    if (!batch.Write(std::make_pair(std::string("keymeta"), vchPubKey),
               keyMeta, false))
        return false;

    // hash pubkey/privkey to accelerate wallet load
    std::vector<unsigned char> vchKey;
    vchKey.reserve(vchPubKey.size() + vchPrivKey.size());
    vchKey.insert(vchKey.end(), vchPubKey.begin(), vchPubKey.end());
    vchKey.insert(vchKey.end(), vchPrivKey.begin(), vchPrivKey.end());

    return batch.Write(std::make_pair(std::string("key"), vchPubKey), std::make_pair(vchPrivKey, Hash(vchKey.begin(), vchKey.end())), false);
}

bool CWalletDB::WriteCryptedKey(const CPubKey& vchPubKey,
    const std::vector<unsigned char>& vchCryptedSecret,
    const CKeyMetadata& keyMeta)
{
    const bool fEraseUnencryptedKey = true;
    nWalletDBUpdateCounter++;

    if (!batch.Write(std::make_pair(std::string("keymeta"), vchPubKey),
            keyMeta))
        return false;

    if (!batch.Write(std::make_pair(std::string("ckey"), vchPubKey), vchCryptedSecret, false))
        return false;
    if (fEraseUnencryptedKey) {
        batch.Erase(std::make_pair(std::string("key"), vchPubKey));
        batch.Erase(std::make_pair(std::string("wkey"), vchPubKey));
    }
    return true;
}

bool CWalletDB::WriteSaplingZKey(const libzcash::SaplingIncomingViewingKey &ivk,
                                 const libzcash::SaplingExtendedSpendingKey &key,
                                 const CKeyMetadata &keyMeta)
{
    nWalletDBUpdateCounter++;

    if (!batch.Write(std::make_pair(std::string("sapzkeymeta"), ivk), keyMeta))
        return false;

    return batch.Write(std::make_pair(std::string("sapzkey"), ivk), key, false);
}

bool CWalletDB::WriteSaplingPaymentAddress(
        const libzcash::SaplingPaymentAddress &addr,
        const libzcash::SaplingIncomingViewingKey &ivk)
{
    nWalletDBUpdateCounter++;

    return batch.Write(std::make_pair(std::string("sapzaddr"), addr), ivk, false);
}

bool CWalletDB::WriteCryptedSaplingZKey(
        const libzcash::SaplingExtendedFullViewingKey &extfvk,
        const std::vector<unsigned char>& vchCryptedSecret,
        const CKeyMetadata &keyMeta)
{
    const bool fEraseUnencryptedKey = true;
    nWalletDBUpdateCounter++;
    auto ivk = extfvk.fvk.in_viewing_key();

    if (!batch.Write(std::make_pair(std::string("sapzkeymeta"), ivk), keyMeta))
        return false;

    if (!batch.Write(std::make_pair(std::string("csapzkey"), ivk), std::make_pair(extfvk, vchCryptedSecret), false))
        return false;

    if (fEraseUnencryptedKey) {
        batch.Erase(std::make_pair(std::string("sapzkey"), ivk));
    }
    return true;
}

bool CWalletDB::WriteSaplingCommonOVK(const uint256& ovk)
{
    nWalletDBUpdateCounter++;
    return batch.Write(std::string("commonovk"), ovk);
}

bool CWalletDB::ReadSaplingCommonOVK(uint256& ovkRet)
{
    return batch.Read(std::string("commonovk"), ovkRet);
}

bool CWalletDB::WriteWitnessCacheSize(int64_t nWitnessCacheSize)
{
    nWalletDBUpdateCounter++;
    return batch.Write(std::string("witnesscachesize"), nWitnessCacheSize);
}

bool CWalletDB::WriteMasterKey(unsigned int nID, const CMasterKey& kMasterKey)
{
    nWalletDBUpdateCounter++;
    return batch.Write(std::make_pair(std::string("mkey"), nID), kMasterKey, true);
}

bool CWalletDB::WriteCScript(const uint160& hash, const CScript& redeemScript)
{
    nWalletDBUpdateCounter++;
    return batch.Write(std::make_pair(std::string("cscript"), hash), redeemScript, false);
}

bool CWalletDB::WriteWatchOnly(const CScript& dest)
{
    nWalletDBUpdateCounter++;
    return batch.Write(std::make_pair(std::string("watchs"), dest), '1');
}

bool CWalletDB::EraseWatchOnly(const CScript& dest)
{

    nWalletDBUpdateCounter++;
    return batch.Erase(std::make_pair(std::string("watchs"), dest));
}

bool CWalletDB::WriteMultiSig(const CScript& dest)
{
    nWalletDBUpdateCounter++;
    return batch.Write(std::make_pair(std::string("multisig"), dest), '1');
}

bool CWalletDB::EraseMultiSig(const CScript& dest)
{
    nWalletDBUpdateCounter++;
    return batch.Erase(std::make_pair(std::string("multisig"), dest));
}

bool CWalletDB::WriteBestBlock(const CBlockLocator& locator)
{
    nWalletDBUpdateCounter++;
    batch.Write(std::string("bestblock"), CBlockLocator()); // Write empty block locator so versions that require a merkle branch automatically rescan
    return batch.Write(std::string("bestblock_nomerkle"), locator);
}

bool CWalletDB::ReadBestBlock(CBlockLocator& locator)
{
    if (batch.Read(std::string("bestblock"), locator) && !locator.vHave.empty()) return true;
    return batch.Read(std::string("bestblock_nomerkle"), locator);
}

bool CWalletDB::WriteOrderPosNext(int64_t nOrderPosNext)
{
    nWalletDBUpdateCounter++;
    return batch.Write(std::string("orderposnext"), nOrderPosNext);
}

bool CWalletDB::WriteStakeSplitThreshold(const CAmount& nStakeSplitThreshold)
{
    nWalletDBUpdateCounter++;
    return batch.Write(std::string("stakeSplitThreshold"), nStakeSplitThreshold);
}

bool CWalletDB::WriteUseCustomFee(bool fUse)
{
    nWalletDBUpdateCounter++;
    return batch.Write(std::string("fUseCustomFee"), fUse);
}

bool CWalletDB::WriteCustomFeeValue(const CAmount& nFee)
{
    nWalletDBUpdateCounter++;
    return batch.Write(std::string("nCustomFee"), nFee);
}

bool CWalletDB::WriteMultiSend(std::vector<std::pair<std::string, int> > vMultiSend)
{
    return false;
    /* disable multisend
    nWalletDBUpdateCounter++;
    bool ret = true;
    for (unsigned int i = 0; i < vMultiSend.size(); i++) {
        std::pair<std::string, int> pMultiSend;
        pMultiSend = vMultiSend[i];
        if (!Write(std::make_pair(std::string("multisend"), i), pMultiSend, true))
            ret = false;
    }
    return ret;
    */
}

bool CWalletDB::EraseMultiSend(std::vector<std::pair<std::string, int> > vMultiSend)
{
    return false;
    /* disable multisend
    nWalletDBUpdateCounter++;
    bool ret = true;
    for (unsigned int i = 0; i < vMultiSend.size(); i++) {
        std::pair<std::string, int> pMultiSend;
        pMultiSend = vMultiSend[i];
        if (!Erase(std::make_pair(std::string("multisend"), i)))
            ret = false;
    }
    return ret;
    */
}

bool CWalletDB::WriteMSettings(bool fMultiSendStake, bool fMultiSendMasternode, int nLastMultiSendHeight)
{
    return false;
    /* disable multisend
    nWalletDBUpdateCounter++;
    std::pair<bool, bool> enabledMS(fMultiSendStake, fMultiSendMasternode);
    std::pair<std::pair<bool, bool>, int> pSettings(enabledMS, nLastMultiSendHeight);

    return Write(std::string("msettingsv2"), pSettings, true);
    */
}

bool CWalletDB::WriteMSDisabledAddresses(std::vector<std::string> vDisabledAddresses)
{
    return false;
    /* disable multisend
    nWalletDBUpdateCounter++;
    bool ret = true;
    for (unsigned int i = 0; i < vDisabledAddresses.size(); i++) {
        if (!Write(std::make_pair(std::string("mdisabled"), i), vDisabledAddresses[i]))
            ret = false;
    }
    return ret;
    */
}

bool CWalletDB::EraseMSDisabledAddresses(std::vector<std::string> vDisabledAddresses)
{
    return false;
    /* disable multisend
    nWalletDBUpdateCounter++;
    bool ret = true;
    for (unsigned int i = 0; i < vDisabledAddresses.size(); i++) {
        if (!batch.Erase(std::make_pair(std::string("mdisabled"), i)))
            ret = false;
    }
    return ret;
    */
}

bool CWalletDB::WriteAutoCombineSettings(bool fEnable, CAmount nCombineThreshold)
{
    nWalletDBUpdateCounter++;
    std::pair<bool, CAmount> pSettings;
    pSettings.first = fEnable;
    pSettings.second = nCombineThreshold;
    return batch.Write(std::string("autocombinesettings"), pSettings, true);
}

bool CWalletDB::ReadPool(int64_t nPool, CKeyPool& keypool)
{
    return batch.Read(std::make_pair(std::string("pool"), nPool), keypool);
}

bool CWalletDB::WritePool(int64_t nPool, const CKeyPool& keypool)
{
    nWalletDBUpdateCounter++;
    return batch.Write(std::make_pair(std::string("pool"), nPool), keypool);
}

bool CWalletDB::ErasePool(int64_t nPool)
{
    nWalletDBUpdateCounter++;
    return batch.Erase(std::make_pair(std::string("pool"), nPool));
}

bool CWalletDB::WriteMinVersion(int nVersion)
{
    return batch.Write(std::string("minversion"), nVersion);
}

bool CWalletDB::WriteHDChain(const CHDChain& chain)
{
    nWalletDBUpdateCounter++;
    std::string key = std::string("hdchain");
    if (chain.chainType == HDChain::ChainCounterType::Sapling)
        key += std::string("_sap");
    return batch.Write(key, chain);
}

DBErrors CWalletDB::ReorderTransactions(CWallet* pwallet)
{
    LOCK(pwallet->cs_wallet);
    // Old wallets didn't have any defined order for transactions
    // Probably a bad idea to change the output of this

    // First: get all CWalletTx into a sorted-by-time multimap.
    typedef std::multimap<int64_t, CWalletTx*> TxItems;
    TxItems txByTime;

    for (std::map<uint256, CWalletTx>::iterator it = pwallet->mapWallet.begin(); it != pwallet->mapWallet.end(); ++it) {
        CWalletTx* wtx = &((*it).second);
        txByTime.insert(std::make_pair(wtx->nTimeReceived, wtx));
    }

    int64_t& nOrderPosNext = pwallet->nOrderPosNext;
    nOrderPosNext = 0;
    std::vector<int64_t> nOrderPosOffsets;
    for (TxItems::iterator it = txByTime.begin(); it != txByTime.end(); ++it) {
        CWalletTx *const pwtx = (*it).second;
        int64_t& nOrderPos = pwtx->nOrderPos;

        if (nOrderPos == -1) {
            nOrderPos = nOrderPosNext++;
            nOrderPosOffsets.push_back(nOrderPos);

            if (!WriteTx(*pwtx))
                return DB_LOAD_FAIL;
        } else {
            int64_t nOrderPosOff = 0;
            for (const int64_t& nOffsetStart : nOrderPosOffsets) {
                if (nOrderPos >= nOffsetStart)
                    ++nOrderPosOff;
            }
            nOrderPos += nOrderPosOff;
            nOrderPosNext = std::max(nOrderPosNext, nOrderPos + 1);

            if (!nOrderPosOff)
                continue;

            // Since we're changing the order, write it back
            if (!WriteTx(*pwtx))
                return DB_LOAD_FAIL;
        }
    }
    WriteOrderPosNext(nOrderPosNext);

    return DB_LOAD_OK;
}

class CWalletScanState
{
public:
    unsigned int nKeys;
    unsigned int nCKeys;
    unsigned int nKeyMeta;
    unsigned int nZKeys;
    unsigned int nZKeyMeta;
    unsigned int nSapZAddrs;
    bool fIsEncrypted;
    bool fAnyUnordered;
    int nFileVersion;
    std::vector<uint256> vWalletUpgrade;

    CWalletScanState()
    {
        nKeys = nCKeys = nKeyMeta = nZKeys = nZKeyMeta = nSapZAddrs = 0;
        fIsEncrypted = false;
        fAnyUnordered = false;
        nFileVersion = 0;
    }
};

bool ReadKeyValue(CWallet* pwallet, CDataStream& ssKey, CDataStream& ssValue, CWalletScanState& wss, std::string& strType, std::string& strErr)
{
    try {
        // Unserialize
        // Taking advantage of the fact that pair serialization
        // is just the two items serialized one after the other
        ssKey >> strType;
        if (strType == "name") {
            std::string strAddress;
            ssKey >> strAddress;
            std::string strName;
            ssValue >> strName;
            pwallet->LoadAddressBookName(Standard::DecodeDestination(strAddress), strName);
        } else if (strType == "purpose") {
            std::string strAddress;
            ssKey >> strAddress;
            std::string strPurpose;
            ssValue >> strPurpose;
            pwallet->LoadAddressBookPurpose(Standard::DecodeDestination(strAddress), strPurpose);
        } else if (strType == "tx") {
            uint256 hash;
            ssKey >> hash;
            CWalletTx wtx(nullptr /* pwallet */, MakeTransactionRef());
            ssValue >> wtx;
            if (wtx.GetHash() != hash)
                return false;

            // Undo serialize changes in 31600
            if (31404 <= wtx.fTimeReceivedIsTxTime && wtx.fTimeReceivedIsTxTime <= 31703) {
                if (!ssValue.empty()) {
                    char fTmp;
                    char fUnused;
                    std::string unused_string;
                    ssValue >> fTmp >> fUnused >> unused_string;
                    strErr = strprintf("LoadWallet() upgrading tx ver=%d %d %s",
                        wtx.fTimeReceivedIsTxTime, fTmp, hash.ToString());
                    wtx.fTimeReceivedIsTxTime = fTmp;
                } else {
                    strErr = strprintf("LoadWallet() repairing tx ver=%d %s", wtx.fTimeReceivedIsTxTime, hash.ToString());
                    wtx.fTimeReceivedIsTxTime = 0;
                }
                wss.vWalletUpgrade.push_back(hash);
            }

            if (wtx.nOrderPos == -1)
                wss.fAnyUnordered = true;

            pwallet->LoadToWallet(wtx);
        } else if (strType == "watchs") {
            CScript script;
            ssKey >> script;
            char fYes;
            ssValue >> fYes;
            if (fYes == '1')
                pwallet->LoadWatchOnly(script);

            // Watch-only addresses have no birthday information for now,
            // so set the wallet birthday to the beginning of time.
            pwallet->nTimeFirstKey = 1;
        } else if (strType == "key" || strType == "wkey") {
            CPubKey vchPubKey;
            ssKey >> vchPubKey;
            if (!vchPubKey.IsValid()) {
                strErr = "Error reading wallet database: CPubKey corrupt";
                return false;
            }
            CKey key;
            CPrivKey pkey;
            uint256 hash;

            if (strType == "key") {
                wss.nKeys++;
                ssValue >> pkey;
            } else {
                CWalletKey wkey;
                ssValue >> wkey;
                pkey = wkey.vchPrivKey;
            }

            // Old wallets store keys as "key" [pubkey] => [privkey]
            // ... which was slow for wallets with lots of keys, because the public key is re-derived from the private key
            // using EC operations as a checksum.
            // Newer wallets store keys as "key"[pubkey] => [privkey][hash(pubkey,privkey)], which is much faster while
            // remaining backwards-compatible.
            try {
                ssValue >> hash;
            } catch (...) {
            }

            bool fSkipCheck = false;

            if (!hash.IsNull()) {
                // hash pubkey/privkey to accelerate wallet load
                std::vector<unsigned char> vchKey;
                vchKey.reserve(vchPubKey.size() + pkey.size());
                vchKey.insert(vchKey.end(), vchPubKey.begin(), vchPubKey.end());
                vchKey.insert(vchKey.end(), pkey.begin(), pkey.end());

                if (Hash(vchKey.begin(), vchKey.end()) != hash) {
                    strErr = "Error reading wallet database: CPubKey/CPrivKey corrupt";
                    return false;
                }

                fSkipCheck = true;
            }

            if (!key.Load(pkey, vchPubKey, fSkipCheck)) {
                strErr = "Error reading wallet database: CPrivKey corrupt";
                return false;
            }
            if (!pwallet->LoadKey(key, vchPubKey)) {
                strErr = "Error reading wallet database: LoadKey failed";
                return false;
            }
        } else if (strType == "mkey") {
            unsigned int nID;
            ssKey >> nID;
            CMasterKey kMasterKey;
            ssValue >> kMasterKey;
            if (pwallet->mapMasterKeys.count(nID) != 0) {
                strErr = strprintf("Error reading wallet database: duplicate CMasterKey id %u", nID);
                return false;
            }
            pwallet->mapMasterKeys[nID] = kMasterKey;
            if (pwallet->nMasterKeyMaxID < nID)
                pwallet->nMasterKeyMaxID = nID;
        } else if (strType == "ckey") {
            CPubKey vchPubKey;
            ssKey >> vchPubKey;
            std::vector<unsigned char> vchPrivKey;
            ssValue >> vchPrivKey;
            wss.nCKeys++;

            if (!pwallet->LoadCryptedKey(vchPubKey, vchPrivKey)) {
                strErr = "Error reading wallet database: LoadCryptedKey failed";
                return false;
            }
            wss.fIsEncrypted = true;
        } else if (strType == "keymeta") {
            CPubKey vchPubKey;
            ssKey >> vchPubKey;
            CKeyMetadata keyMeta;
            ssValue >> keyMeta;
            wss.nKeyMeta++;

            pwallet->LoadKeyMetadata(vchPubKey, keyMeta);

            // find earliest key creation time, as wallet birthday
            if (!pwallet->nTimeFirstKey ||
                (keyMeta.nCreateTime < pwallet->nTimeFirstKey))
                pwallet->nTimeFirstKey = keyMeta.nCreateTime;
        } else if (strType == "defaultkey") {
            // We don't want or need the default key, but if there is one set,
            // we want to make sure that it is valid so that we can detect corruption
            CPubKey vchPubKey;
            ssValue >> vchPubKey;
            if (!vchPubKey.IsValid()) {
                strErr = "Error reading wallet database: Default Key corrupt";
                return false;
            }
        } else if (strType == "pool") {
            int64_t nIndex;
            ssKey >> nIndex;
            CKeyPool keypool;
            ssValue >> keypool;
            pwallet->GetScriptPubKeyMan()->LoadKeyPool(nIndex, keypool);
        } else if (strType == "version") {
            ssValue >> wss.nFileVersion;
            if (wss.nFileVersion == 10300)
                wss.nFileVersion = 300;
        } else if (strType == "cscript") {
            uint160 hash;
            ssKey >> hash;
            CScript script;
            ssValue >> script;
            if (!pwallet->LoadCScript(script)) {
                strErr = "Error reading wallet database: LoadCScript failed";
                return false;
            }
        } else if (strType == "orderposnext") {
            ssValue >> pwallet->nOrderPosNext;
        } else if (strType == "stakeSplitThreshold") {
            ssValue >> pwallet->nStakeSplitThreshold;
            // originally saved as integer
            if (pwallet->nStakeSplitThreshold < COIN)
                pwallet->nStakeSplitThreshold *= COIN;
        } else if (strType == "fUseCustomFee") {
            ssValue >> pwallet->fUseCustomFee;
        } else if (strType == "nCustomFee") {
            ssValue >> pwallet->nCustomFee;
        /* disable multisend
        } else if (strType == "multisend") {
            unsigned int i;
            ssKey >> i;
            std::pair<std::string, int> pMultiSend;
            ssValue >> pMultiSend;
            if (CBitcoinAddress(pMultiSend.first).IsValid()) {
                pwallet->vMultiSend.push_back(pMultiSend);
            }
        } else if (strType == "msettingsv2") {
            std::pair<std::pair<bool, bool>, int> pSettings;
            ssValue >> pSettings;
            pwallet->fMultiSendStake = pSettings.first.first;
            pwallet->fMultiSendMasternodeReward = pSettings.first.second;
            pwallet->nLastMultiSendHeight = pSettings.second;
        } else if (strType == "mdisabled") {
            std::string strDisabledAddress;
            ssValue >> strDisabledAddress;
            pwallet->vDisabledAddresses.push_back(strDisabledAddress);
            */
        } else if (strType == "autocombinesettings") {
            std::pair<bool, CAmount> pSettings;
            ssValue >> pSettings;
            pwallet->fCombineDust = pSettings.first;
            pwallet->nAutoCombineThreshold = pSettings.second;
        } else if (strType == "destdata") {
            std::string strAddress, strKey, strValue;
            ssKey >> strAddress;
            ssKey >> strKey;
            ssValue >> strValue;
            if (!pwallet->LoadDestData(DecodeDestination(strAddress), strKey, strValue)) {
                strErr = "Error reading wallet database: LoadDestData failed";
                return false;
            }
        } else if (strType == "hdchain") { // Regular key chain counter
            CHDChain chain;
            ssValue >> chain;
            pwallet->GetScriptPubKeyMan()->SetHDChain(chain, true);
        } else if (strType == "hdchain_sap") {
            CHDChain chain;
            ssValue >> chain;
            pwallet->GetSaplingScriptPubKeyMan()->SetHDChain(chain, true);
        } else if (strType == "sapzkey") {
            libzcash::SaplingIncomingViewingKey ivk;
            ssKey >> ivk;
            libzcash::SaplingExtendedSpendingKey key;
            ssValue >> key;
            if (!pwallet->LoadSaplingZKey(key)) {
                strErr = "Error reading wallet database: LoadSaplingZKey failed";
                return false;
            }
            //add checks for integrity
            wss.nZKeys++;
        } else if (strType == "commonovk") {
            uint256 ovk;
            ssValue >> ovk;
            pwallet->GetSaplingScriptPubKeyMan()->setCommonOVK(ovk);
        } else if (strType == "csapzkey") {
            libzcash::SaplingIncomingViewingKey ivk;
            ssKey >> ivk;
            libzcash::SaplingExtendedFullViewingKey extfvk;
            ssValue >> extfvk;
            std::vector<unsigned char> vchCryptedSecret;
            ssValue >> vchCryptedSecret;
            wss.nCKeys++;

            if (!pwallet->LoadCryptedSaplingZKey(extfvk, vchCryptedSecret)) {
                strErr = "Error reading wallet database: LoadCryptedSaplingZKey failed";
                return false;
            }
            wss.fIsEncrypted = true;
        } else if (strType == "sapzkeymeta") {
            libzcash::SaplingIncomingViewingKey ivk;
            ssKey >> ivk;
            CKeyMetadata keyMeta;
            ssValue >> keyMeta;

            wss.nZKeyMeta++;

            pwallet->LoadSaplingZKeyMetadata(ivk, keyMeta);
        } else if (strType == "sapzaddr") {
            libzcash::SaplingPaymentAddress addr;
            ssKey >> addr;
            libzcash::SaplingIncomingViewingKey ivk;
            ssValue >> ivk;

            wss.nSapZAddrs++;

            if (!pwallet->LoadSaplingPaymentAddress(addr, ivk)) {
                strErr = "Error reading wallet database: LoadSaplingPaymentAddress failed";
                return false;
            }
        } else if (strType == "witnesscachesize") {
            ssValue >> pwallet->GetSaplingScriptPubKeyMan()->nWitnessCacheSize;
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool CWalletDB::IsKeyType(const std::string& strType)
{
    return (strType == "key" || strType == "wkey" ||
            strType == "mkey" || strType == "ckey" ||
            strType == "sapzkey" || strType == "csapzkey");
}

DBErrors CWalletDB::LoadWallet(CWallet* pwallet)
{
    CWalletScanState wss;
    bool fNoncriticalErrors = false;
    DBErrors result = DB_LOAD_OK;

    LOCK(pwallet->cs_wallet);
    try {
        int nMinVersion = 0;
        if (batch.Read((std::string) "minversion", nMinVersion)) {
            if (nMinVersion > CLIENT_VERSION)
                return DB_TOO_NEW;
            pwallet->LoadMinVersion(nMinVersion);
        }

        // Get cursor
        Dbc* pcursor = batch.GetCursor();
        if (!pcursor) {
            LogPrintf("Error getting wallet database cursor\n");
            return DB_CORRUPT;
        }

        while (true) {
            // Read next record
            CDataStream ssKey(SER_DISK, CLIENT_VERSION);
            CDataStream ssValue(SER_DISK, CLIENT_VERSION);
            int ret = batch.ReadAtCursor(pcursor, ssKey, ssValue);
            if (ret == DB_NOTFOUND)
                break;
            else if (ret != 0) {
                LogPrintf("Error reading next record from wallet database\n");
                return DB_CORRUPT;
            }

            // Try to be tolerant of single corrupt records:
            std::string strType, strErr;
            if (!ReadKeyValue(pwallet, ssKey, ssValue, wss, strType, strErr)) {
                // losing keys is considered a catastrophic error, anything else
                // we assume the user can live with:
                if (IsKeyType(strType) || strType == "defaultkey")
                    result = DB_CORRUPT;
                else {
                    // Leave other errors alone, if we try to fix them we might make things worse.
                    fNoncriticalErrors = true; // ... but do warn the user there is something wrong.
                    if (strType == "tx")
                        // Rescan if there is a bad transaction record:
                        gArgs.SoftSetBoolArg("-rescan", true);
                }
            }
            if (!strErr.empty())
                LogPrintf("%s\n", strErr);
        }
        pcursor->close();
    } catch (const boost::thread_interrupted&) {
        throw;
    } catch (...) {
        result = DB_CORRUPT;
    }

    if (fNoncriticalErrors && result == DB_LOAD_OK)
        result = DB_NONCRITICAL_ERROR;

    // Any wallet corruption at all: skip any rewriting or
    // upgrading, we don't want to make it worse.
    if (result != DB_LOAD_OK)
        return result;

    LogPrintf("nFileVersion = %d\n", wss.nFileVersion);

    LogPrintf("Keys: %u plaintext, %u encrypted, %u w/ metadata, %u total\n",
        wss.nKeys, wss.nCKeys, wss.nKeyMeta, wss.nKeys + wss.nCKeys);

    LogPrintf("ZKeys: %u plaintext, -- encrypted, %u w/metadata, %u total\n",
              wss.nZKeys, wss.nZKeyMeta, wss.nZKeys + 0);

    // nTimeFirstKey is only reliable if all keys have metadata
    if ((wss.nKeys + wss.nCKeys) != wss.nKeyMeta)
        pwallet->nTimeFirstKey = 1; // 0 would be considered 'no value'

    for (const uint256& hash : wss.vWalletUpgrade)
        WriteTx(pwallet->mapWallet.at(hash));

    // Rewrite encrypted wallets of versions 0.4.0 and 0.5.0rc:
    if (wss.fIsEncrypted && (wss.nFileVersion == 40000 || wss.nFileVersion == 50000))
        return DB_NEED_REWRITE;

    if (wss.nFileVersion < CLIENT_VERSION) // Update
        WriteVersion(CLIENT_VERSION);

    if (wss.fAnyUnordered)
        result = ReorderTransactions(pwallet);

    return result;
}

DBErrors CWalletDB::FindWalletTx(CWallet* pwallet, std::vector<uint256>& vTxHash, std::vector<CWalletTx>& vWtx)
{
    bool fNoncriticalErrors = false;
    DBErrors result = DB_LOAD_OK;

    try {
        LOCK(pwallet->cs_wallet);
        int nMinVersion = 0;
        if (batch.Read((std::string) "minversion", nMinVersion)) {
            if (nMinVersion > CLIENT_VERSION)
                return DB_TOO_NEW;
            pwallet->LoadMinVersion(nMinVersion);
        }

        // Get cursor
        Dbc* pcursor = batch.GetCursor();
        if (!pcursor) {
            LogPrintf("Error getting wallet database cursor\n");
            return DB_CORRUPT;
        }

        while (true) {
            // Read next record
            CDataStream ssKey(SER_DISK, CLIENT_VERSION);
            CDataStream ssValue(SER_DISK, CLIENT_VERSION);
            int ret = batch.ReadAtCursor(pcursor, ssKey, ssValue);
            if (ret == DB_NOTFOUND)
                break;
            else if (ret != 0) {
                LogPrintf("Error reading next record from wallet database\n");
                return DB_CORRUPT;
            }

            std::string strType;
            ssKey >> strType;
            if (strType == "tx") {
                uint256 hash;
                ssKey >> hash;

                CWalletTx wtx(nullptr /* pwallet */, MakeTransactionRef());
                ssValue >> wtx;

                vTxHash.push_back(hash);
                vWtx.push_back(wtx);
            }
        }
        pcursor->close();
    } catch (const boost::thread_interrupted&) {
        throw;
    } catch (...) {
        result = DB_CORRUPT;
    }

    if (fNoncriticalErrors && result == DB_LOAD_OK)
        result = DB_NONCRITICAL_ERROR;

    return result;
}

DBErrors CWalletDB::ZapWalletTx(CWallet* pwallet, std::vector<CWalletTx>& vWtx)
{
    // build list of wallet TXs
    std::vector<uint256> vTxHash;
    DBErrors err = FindWalletTx(pwallet, vTxHash, vWtx);
    if (err != DB_LOAD_OK)
        return err;

    // erase each wallet TX
    for (uint256& hash : vTxHash) {
        if (!EraseTx(hash))
            return DB_CORRUPT;
    }

    return DB_LOAD_OK;
}

void MaybeCompactWalletDB()
{
    static std::atomic<bool> fOneThread;
    if (fOneThread.exchange(true)) {
        return;
    }
    if (!gArgs.GetBoolArg("-flushwallet", DEFAULT_FLUSHWALLET)) {
        return;
    }

    static unsigned int nLastSeen = CWalletDB::GetUpdateCounter();
    static unsigned int nLastFlushed = CWalletDB::GetUpdateCounter();
    static int64_t nLastWalletUpdate = GetTime();

    if (nLastSeen != CWalletDB::GetUpdateCounter()) {
        nLastSeen = CWalletDB::GetUpdateCounter();
        nLastWalletUpdate = GetTime();
    }

    if (nLastFlushed != CWalletDB::GetUpdateCounter() && GetTime() - nLastWalletUpdate >= 2) {
        if (CDB::PeriodicFlush(pwalletMain->GetDBHandle())) {
            nLastFlushed = CWalletDB::GetUpdateCounter();
        }
    }
    fOneThread = false;
}

void NotifyBacked(const CWallet& wallet, bool fSuccess, std::string strMessage)
{
    LogPrintf("%s\n", strMessage);
    wallet.NotifyWalletBacked(fSuccess, strMessage);
}

// Returns first the pathCustom, second the pathWithFile.
std::pair<fs::path, fs::path> GetBackupPath(const CWallet& wallet)
{
    fs::path pathCustom;
    fs::path pathWithFile = gArgs.GetArg("-backuppath", "");
    if(!pathWithFile.empty()) {
        if(!pathWithFile.has_extension()) {
            pathCustom = pathWithFile;
            pathWithFile /= wallet.GetUniqueWalletBackupName();
        } else {
            pathCustom = pathWithFile.parent_path();
        }
        try {
            fs::create_directories(pathCustom);
        } catch (const fs::filesystem_error& e) {
            NotifyBacked(wallet, false, strprintf("%s\n", e.what()));
            pathCustom = "";
        }
    }
    return {pathCustom, pathWithFile};
}

void MultiBackup(const CWallet& wallet, fs::path pathCustom, fs::path pathWithFile, const fs::path& pathSrc)
{
    int nThreshold = gArgs.GetArg("-custombackupthreshold", DEFAULT_CUSTOMBACKUPTHRESHOLD);
    if (nThreshold > 0) {

        typedef std::multimap<std::time_t, fs::path> folder_set_t;
        folder_set_t folderSet;
        fs::directory_iterator end_iter;

        pathCustom.make_preferred();
        // Build map of backup files for current(!) wallet sorted by last write time

        fs::path currentFile;
        for (fs::directory_iterator dir_iter(pathCustom); dir_iter != end_iter; ++dir_iter) {
            // Only check regular files
            if (fs::is_regular_file(dir_iter->status())) {
                currentFile = dir_iter->path().filename();
                // Only add the backups for the current wallet, e.g. wallet.dat.*
                if (dir_iter->path().stem().string() == wallet.GetDBHandle().GetName()) {
                    folderSet.insert(folder_set_t::value_type(fs::last_write_time(dir_iter->path()), *dir_iter));
                }
            }
        }

        int counter = 0; //TODO: add seconds to avoid naming conflicts
        for (auto entry : folderSet) {
            counter++;
            if(entry.second == pathWithFile) {
                pathWithFile += "(1)";
            }
        }

        if (counter >= nThreshold) {
            std::time_t oldestBackup = 0;
            for(auto entry : folderSet) {
                if(oldestBackup == 0 || entry.first < oldestBackup) {
                    oldestBackup = entry.first;
                }
            }

            try {
                auto entry = folderSet.find(oldestBackup);
                if (entry != folderSet.end()) {
                    fs::remove(entry->second);
                    LogPrintf("Old backup deleted: %s\n", (*entry).second);
                }
            } catch (const fs::filesystem_error& error) {
                std::string strMessage = strprintf("Failed to delete backup %s\n", error.what());
                NotifyBacked(wallet, false, strMessage);
            }
        }
    }
    AttemptBackupWallet(wallet, pathSrc.string(), pathWithFile.string());
}

bool BackupWallet(const CWallet& wallet, const fs::path& strDest)
{
    const auto& pathsPair = GetBackupPath(wallet);
    fs::path pathCustom = pathsPair.first;
    fs::path pathWithFile = pathsPair.second;

    std::string strFile = wallet.GetDBHandle().GetName();
    while (true) {
        {
            LOCK(bitdb.cs_db);
            if (!bitdb.mapFileUseCount.count(strFile) || bitdb.mapFileUseCount[strFile] == 0) {
                // Flush log data to the dat file
                bitdb.CloseDb(strFile);
                bitdb.CheckpointLSN(strFile);
                bitdb.mapFileUseCount.erase(strFile);

                // Copy wallet file
                fs::path pathDest(strDest);
                fs::path pathSrc = GetDataDir() / strFile;
                if (is_directory(pathDest)) {
                    if(!exists(pathDest)) create_directory(pathDest);
                    pathDest /= strFile;
                }
                bool defaultPath = AttemptBackupWallet(wallet, pathSrc.string(), pathDest.string());

                if(defaultPath && !pathCustom.empty()) {
                    MultiBackup(wallet, pathCustom, pathWithFile, pathSrc);
                }

                return defaultPath;
            }
        }
        MilliSleep(100);
    }
    return false;
}

bool AttemptBackupWallet(const CWallet& wallet, const fs::path& pathSrc, const fs::path& pathDest)
{
    bool retStatus;
    std::string strMessage;
    try {
        if (fs::equivalent(pathSrc, pathDest)) {
            LogPrintf("cannot backup to wallet source file %s\n", pathDest.string());
            return false;
        }
#if BOOST_VERSION >= 107400
        fs::copy_file(pathSrc.c_str(), pathDest, fs::copy_options::overwrite_existing);
#elif BOOST_VERSION >= 105800 /* BOOST_LIB_VERSION 1_58 */
        fs::copy_file(pathSrc.c_str(), pathDest, fs::copy_option::overwrite_if_exists);
#else
        std::ifstream src(pathSrc.c_str(),  std::ios::binary | std::ios::in);
        std::ofstream dst(pathDest.c_str(), std::ios::binary | std::ios::out | std::ios::trunc);
        dst << src.rdbuf();
        dst.flush();
        src.close();
        dst.close();
#endif
        strMessage = strprintf("copied %s to %s\n", wallet.GetDBHandle().GetName(), pathDest.string());
        LogPrintf("%s : %s\n", __func__, strMessage);
        retStatus = true;
    } catch (const fs::filesystem_error& e) {
        retStatus = false;
        strMessage = strprintf("%s\n", e.what());
        LogPrintf("%s : %s\n", __func__, strMessage);
    }
    NotifyBacked(wallet, retStatus, strMessage);
    return retStatus;
}

//
// Try to (very carefully!) recover wallet file if there is a problem.
//
bool CWalletDB::Recover(const std::string& filename, void *callbackDataIn, bool (*recoverKVcallback)(void* callbackData, CDataStream ssKey, CDataStream ssValue))
{
    return CDB::Recover(filename, callbackDataIn, recoverKVcallback);
}

bool CWalletDB::Recover(const std::string& filename)
{
    // recover without a key filter callback
    // results in recovering all record types
    return CWalletDB::Recover(filename, nullptr, nullptr);
}

bool CWalletDB::RecoverKeysOnlyFilter(void *callbackData, CDataStream ssKey, CDataStream ssValue)
{
    CWallet *dummyWallet = reinterpret_cast<CWallet*>(callbackData);
    CWalletScanState dummyWss;
    std::string strType, strErr;
    bool fReadOK;
    {
        // Required in LoadKeyMetadata():
        LOCK(dummyWallet->cs_wallet);
        fReadOK = ReadKeyValue(dummyWallet, ssKey, ssValue,
                               dummyWss, strType, strErr);
    }
    if (!IsKeyType(strType) && strType != "hdchain")
        return false;
    if (!fReadOK)
    {
        LogPrintf("WARNING: CWalletDB::Recover skipping %s: %s\n", strType, strErr);
        return false;
    }

    return true;
}

bool CWalletDB::VerifyEnvironment(const std::string& walletFile, const fs::path& dataDir, std::string& errorStr)
{
    return CDB::VerifyEnvironment(walletFile, dataDir, errorStr);
}

bool CWalletDB::VerifyDatabaseFile(const std::string& walletFile, const fs::path& dataDir, std::string& warningStr, std::string& errorStr)
{
    return CDB::VerifyDatabaseFile(walletFile, dataDir, warningStr, errorStr, CWalletDB::Recover);
}

bool CWalletDB::WriteDestData(const std::string& address, const std::string& key, const std::string& value)
{
    nWalletDBUpdateCounter++;
    return batch.Write(std::make_pair(std::string("destdata"), std::make_pair(address, key)), value);
}

bool CWalletDB::EraseDestData(const std::string& address, const std::string& key)
{
    nWalletDBUpdateCounter++;
    return batch.Erase(std::make_pair(std::string("destdata"), std::make_pair(address, key)));
}

void CWalletDB::IncrementUpdateCounter()
{
    nWalletDBUpdateCounter++;
}

unsigned int CWalletDB::GetUpdateCounter()
{
    return nWalletDBUpdateCounter;
}

bool CWalletDB::TxnBegin()
{
    return batch.TxnBegin();
}

bool CWalletDB::TxnCommit()
{
    return batch.TxnCommit();
}

bool CWalletDB::TxnAbort()
{
    return batch.TxnAbort();
}

bool CWalletDB::ReadVersion(int& nVersion)
{
    return batch.ReadVersion(nVersion);
}

bool CWalletDB::WriteVersion(int nVersion)
{
    return batch.WriteVersion(nVersion);
}
