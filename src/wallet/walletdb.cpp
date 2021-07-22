// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2021 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "wallet/walletdb.h"

#include "fs.h"

#include "key_io.h"
#include "protocol.h"
#include "reverse_iterate.h"
#include "sapling/key_io_sapling.h"
#include "serialize.h"
#include "sync.h"
#include "util/system.h"
#include "utiltime.h"
#include "wallet/wallet.h"

#include <atomic>

#include <boost/thread.hpp>

namespace DBKeys {
    const std::string BESTBLOCK_NOMERKLE{"bestblock_nomerkle"};
    const std::string BESTBLOCK{"bestblock"};
    const std::string CRYPTED_KEY{"ckey"};
    const std::string CSCRIPT{"cscript"};
    const std::string DEFAULTKEY{"defaultkey"};
    const std::string DESTDATA{"destdata"};
    const std::string HDCHAIN{"hdchain"};
    const std::string KEYMETA{"keymeta"};
    const std::string KEY{"key"};
    const std::string MASTER_KEY{"mkey"};
    const std::string MINVERSION{"minversion"};
    const std::string NAME{"name"};
    const std::string ORDERPOSNEXT{"orderposnext"};
    const std::string POOL{"pool"};
    const std::string PURPOSE{"purpose"};
    const std::string TX{"tx"};
    const std::string VERSION{"version"};
    const std::string WATCHS{"watchs"};

    // Sapling
    const std::string SAP_KEYMETA{"sapzkeymeta"};
    const std::string SAP_KEY{"sapzkey"};
    const std::string SAP_KEY_CRIPTED{"csapzkey"};
    const std::string SAP_ADDR{"sapzaddr"};
    const std::string SAP_COMMON_OVK{"commonovk"};
    const std::string SAP_HDCHAIN{"hdchain_sap"};
    const std::string SAP_WITNESS_CACHE_SIZE{"witnesscachesize"};

    // Wallet custom settings
    const std::string AUTOCOMBINE{"autocombinesettings"};
    const std::string STAKE_SPLIT_THRESHOLD{"stakeSplitThreshold"};
    const std::string USE_CUSTOM_FEE{"fUseCustomFee"};
    const std::string CUSTOM_FEE_VALUE{"nCustomFee"};

} // namespace DBKeys


//
// CWalletDB
//

bool CWalletDB::WriteName(const std::string& strAddress, const std::string& strName)
{
    return WriteIC(std::make_pair(std::string(DBKeys::NAME), strAddress), strName);
}

bool CWalletDB::EraseName(const std::string& strAddress)
{
    // This should only be used for sending addresses, never for receiving addresses,
    // receiving addresses must always have an address book entry if they're not change return.
    return EraseIC(std::make_pair(std::string(DBKeys::NAME), strAddress));
}

bool CWalletDB::WritePurpose(const std::string& strAddress, const std::string& strPurpose)
{
    return WriteIC(std::make_pair(std::string(DBKeys::PURPOSE), strAddress), strPurpose);
}

bool CWalletDB::ErasePurpose(const std::string& strPurpose)
{
    return EraseIC(std::make_pair(std::string(DBKeys::PURPOSE), strPurpose));
}

bool CWalletDB::WriteTx(const CWalletTx& wtx)
{
    return WriteIC(std::make_pair(std::string(DBKeys::TX), wtx.GetHash()), wtx);
}

bool CWalletDB::EraseTx(uint256 hash)
{
    return EraseIC(std::make_pair(std::string(DBKeys::TX), hash));
}

bool CWalletDB::WriteKey(const CPubKey& vchPubKey, const CPrivKey& vchPrivKey, const CKeyMetadata& keyMeta)
{
    if (!WriteIC(std::make_pair(std::string(DBKeys::KEYMETA), vchPubKey), keyMeta, false)) {
        return false;
    }

    // hash pubkey/privkey to accelerate wallet load
    std::vector<unsigned char> vchKey;
    vchKey.reserve(vchPubKey.size() + vchPrivKey.size());
    vchKey.insert(vchKey.end(), vchPubKey.begin(), vchPubKey.end());
    vchKey.insert(vchKey.end(), vchPrivKey.begin(), vchPrivKey.end());

    return WriteIC(std::make_pair(std::string(DBKeys::KEY), vchPubKey), std::make_pair(vchPrivKey, Hash(vchKey.begin(), vchKey.end())), false);
}

bool CWalletDB::WriteCryptedKey(const CPubKey& vchPubKey,
                                const std::vector<unsigned char>& vchCryptedSecret,
                                const CKeyMetadata& keyMeta)
{
    const bool fEraseUnencryptedKey = true;

    if (!WriteIC(std::make_pair(std::string(DBKeys::KEYMETA), vchPubKey), keyMeta)) {
        return false;
    }

    if (!WriteIC(std::make_pair(std::string(DBKeys::CRYPTED_KEY), vchPubKey), vchCryptedSecret, false)) {
        return false;
    }
    if (fEraseUnencryptedKey) {
        EraseIC(std::make_pair(std::string(DBKeys::KEY), vchPubKey));
    }

    return true;
}

bool CWalletDB::WriteSaplingZKey(const libzcash::SaplingIncomingViewingKey &ivk,
                                 const libzcash::SaplingExtendedSpendingKey &key,
                                 const CKeyMetadata &keyMeta)
{
    if (!WriteIC(std::make_pair(std::string(DBKeys::SAP_KEYMETA), ivk), keyMeta)) {
        return false;
    }

    return WriteIC(std::make_pair(std::string(DBKeys::SAP_KEY), ivk), key, false);
}

bool CWalletDB::WriteSaplingPaymentAddress(const libzcash::SaplingPaymentAddress &addr,
                                           const libzcash::SaplingIncomingViewingKey &ivk)
{
    return WriteIC(std::make_pair(std::string(DBKeys::SAP_ADDR), addr), ivk, false);
}

bool CWalletDB::WriteCryptedSaplingZKey(const libzcash::SaplingExtendedFullViewingKey &extfvk,
                                        const std::vector<unsigned char>& vchCryptedSecret,
                                        const CKeyMetadata &keyMeta)
{
    const bool fEraseUnencryptedKey = true;
    auto ivk = extfvk.fvk.in_viewing_key();

    if (!WriteIC(std::make_pair(std::string(DBKeys::SAP_KEYMETA), ivk), keyMeta)) {
        return false;
    }

    if (!WriteIC(std::make_pair(std::string(DBKeys::SAP_KEY_CRIPTED), ivk),
                 std::make_pair(extfvk, vchCryptedSecret), false)) {
        return false;
    }

    if (fEraseUnencryptedKey) {
        EraseIC(std::make_pair(std::string(DBKeys::SAP_KEY), ivk));
    }
    return true;
}

bool CWalletDB::WriteSaplingCommonOVK(const uint256& ovk)
{
    return WriteIC(std::string(DBKeys::SAP_COMMON_OVK), ovk);
}

bool CWalletDB::ReadSaplingCommonOVK(uint256& ovkRet)
{
    return batch.Read(std::string(DBKeys::SAP_COMMON_OVK), ovkRet);
}

bool CWalletDB::WriteWitnessCacheSize(int64_t nWitnessCacheSize)
{
    return WriteIC(std::string(DBKeys::SAP_WITNESS_CACHE_SIZE), nWitnessCacheSize);
}

bool CWalletDB::WriteMasterKey(unsigned int nID, const CMasterKey& kMasterKey)
{
    return WriteIC(std::make_pair(std::string(DBKeys::MASTER_KEY), nID), kMasterKey, true);
}

bool CWalletDB::WriteCScript(const uint160& hash, const CScript& redeemScript)
{
    return WriteIC(std::make_pair(std::string(DBKeys::CSCRIPT), hash), redeemScript, false);
}

bool CWalletDB::WriteWatchOnly(const CScript& dest)
{
    return WriteIC(std::make_pair(std::string(DBKeys::WATCHS), dest), '1');
}

bool CWalletDB::EraseWatchOnly(const CScript& dest)
{
    return EraseIC(std::make_pair(std::string(DBKeys::WATCHS), dest));
}

bool CWalletDB::WriteBestBlock(const CBlockLocator& locator)
{
    WriteIC(std::string(DBKeys::BESTBLOCK), CBlockLocator()); // Write empty block locator so versions that require a merkle branch automatically rescan
    return WriteIC(std::string(DBKeys::BESTBLOCK_NOMERKLE), locator);
}

bool CWalletDB::ReadBestBlock(CBlockLocator& locator)
{
    if (batch.Read(std::string(DBKeys::BESTBLOCK), locator) && !locator.vHave.empty()) {
        return true;
    }
    return batch.Read(std::string(DBKeys::BESTBLOCK_NOMERKLE), locator);
}

bool CWalletDB::WriteOrderPosNext(int64_t nOrderPosNext)
{
    return WriteIC(std::string(DBKeys::ORDERPOSNEXT), nOrderPosNext);
}

bool CWalletDB::WriteStakeSplitThreshold(const CAmount& nStakeSplitThreshold)
{
    return WriteIC(std::string(DBKeys::STAKE_SPLIT_THRESHOLD), nStakeSplitThreshold);
}

bool CWalletDB::WriteUseCustomFee(bool fUse)
{
    return WriteIC(std::string(DBKeys::USE_CUSTOM_FEE), fUse);
}

bool CWalletDB::WriteCustomFeeValue(const CAmount& nFee)
{
    return WriteIC(std::string(DBKeys::CUSTOM_FEE_VALUE), nFee);
}

bool CWalletDB::WriteAutoCombineSettings(bool fEnable, CAmount nCombineThreshold)
{
    std::pair<bool, CAmount> pSettings;
    pSettings.first = fEnable;
    pSettings.second = nCombineThreshold;
    return WriteIC(std::string(DBKeys::AUTOCOMBINE), pSettings, true);
}

bool CWalletDB::ReadPool(int64_t nPool, CKeyPool& keypool)
{
    return batch.Read(std::make_pair(std::string(DBKeys::POOL), nPool), keypool);
}

bool CWalletDB::WritePool(int64_t nPool, const CKeyPool& keypool)
{
    return WriteIC(std::make_pair(std::string(DBKeys::POOL), nPool), keypool);
}

bool CWalletDB::ErasePool(int64_t nPool)
{
    return EraseIC(std::make_pair(std::string(DBKeys::POOL), nPool));
}

bool CWalletDB::WriteMinVersion(int nVersion)
{
    return WriteIC(std::string(DBKeys::MINVERSION), nVersion);
}

bool CWalletDB::WriteHDChain(const CHDChain& chain)
{
    std::string key = chain.chainType == HDChain::ChainCounterType::Sapling ?
                        DBKeys::SAP_HDCHAIN : DBKeys::HDCHAIN;
    return WriteIC(key, chain);
}

DBErrors CWalletDB::ReorderTransactions(CWallet* pwallet)
{
    LOCK(pwallet->cs_wallet);
    // Old wallets didn't have any defined order for transactions
    // Probably a bad idea to change the output of this

    // First: get all CWalletTx into a sorted-by-time multimap.
    typedef std::multimap<int64_t, CWalletTx*> TxItems;
    TxItems txByTime;

    for (auto it = pwallet->mapWallet.begin(); it != pwallet->mapWallet.end(); ++it) {
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

            if (!WriteTx(*pwtx)) return DB_LOAD_FAIL;

        } else {
            int64_t nOrderPosOff = 0;
            for (const int64_t& nOffsetStart : nOrderPosOffsets) {
                if (nOrderPos >= nOffsetStart)
                    ++nOrderPosOff;
            }
            nOrderPos += nOrderPosOff;
            nOrderPosNext = std::max(nOrderPosNext, nOrderPos + 1);

            if (!nOrderPosOff) continue;

            // Since we're changing the order, write it back
            if (!WriteTx(*pwtx)) return DB_LOAD_FAIL;
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
        if (strType == DBKeys::NAME) {
            std::string strAddress;
            ssKey >> strAddress;
            std::string strName;
            ssValue >> strName;
            pwallet->LoadAddressBookName(Standard::DecodeDestination(strAddress), strName);
        } else if (strType == DBKeys::PURPOSE) {
            std::string strAddress;
            ssKey >> strAddress;
            std::string strPurpose;
            ssValue >> strPurpose;
            pwallet->LoadAddressBookPurpose(Standard::DecodeDestination(strAddress), strPurpose);
        } else if (strType == DBKeys::TX) {
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
        } else if (strType == DBKeys::WATCHS) {
            CScript script;
            ssKey >> script;
            char fYes;
            ssValue >> fYes;
            if (fYes == '1')
                pwallet->LoadWatchOnly(script);

            // Watch-only addresses have no birthday information for now,
            // so set the wallet birthday to the beginning of time.
            pwallet->nTimeFirstKey = 1;
        } else if (strType == DBKeys::KEY) {
            CPubKey vchPubKey;
            ssKey >> vchPubKey;
            if (!vchPubKey.IsValid()) {
                strErr = "Error reading wallet database: CPubKey corrupt";
                return false;
            }
            CKey key;
            CPrivKey pkey;
            uint256 hash;
            wss.nKeys++;
            ssValue >> pkey;

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
        } else if (strType == DBKeys::MASTER_KEY) {
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
        } else if (strType == DBKeys::CRYPTED_KEY) {
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
        } else if (strType == DBKeys::KEYMETA) {
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
        } else if (strType == DBKeys::DEFAULTKEY) {
            // We don't want or need the default key, but if there is one set,
            // we want to make sure that it is valid so that we can detect corruption
            CPubKey vchPubKey;
            ssValue >> vchPubKey;
            if (!vchPubKey.IsValid()) {
                strErr = "Error reading wallet database: Default Key corrupt";
                return false;
            }
        } else if (strType == DBKeys::POOL) {
            int64_t nIndex;
            ssKey >> nIndex;
            CKeyPool keypool;
            ssValue >> keypool;
            pwallet->GetScriptPubKeyMan()->LoadKeyPool(nIndex, keypool);
        } else if (strType == DBKeys::VERSION) {
            ssValue >> wss.nFileVersion;
            if (wss.nFileVersion == 10300)
                wss.nFileVersion = 300;
        } else if (strType == DBKeys::CSCRIPT) {
            uint160 hash;
            ssKey >> hash;
            CScript script;
            ssValue >> script;
            if (!pwallet->LoadCScript(script)) {
                strErr = "Error reading wallet database: LoadCScript failed";
                return false;
            }
        } else if (strType == DBKeys::ORDERPOSNEXT) {
            ssValue >> pwallet->nOrderPosNext;
        } else if (strType == DBKeys::STAKE_SPLIT_THRESHOLD) {
            ssValue >> pwallet->nStakeSplitThreshold;
            // originally saved as integer
            if (pwallet->nStakeSplitThreshold < COIN)
                pwallet->nStakeSplitThreshold *= COIN;
        } else if (strType == DBKeys::USE_CUSTOM_FEE) {
            ssValue >> pwallet->fUseCustomFee;
        } else if (strType == DBKeys::CUSTOM_FEE_VALUE) {
            ssValue >> pwallet->nCustomFee;
        } else if (strType == DBKeys::AUTOCOMBINE) {
            std::pair<bool, CAmount> pSettings;
            ssValue >> pSettings;
            pwallet->fCombineDust = pSettings.first;
            pwallet->nAutoCombineThreshold = pSettings.second;
            // originally saved as integer
            if (pwallet->nAutoCombineThreshold < COIN)
                pwallet->nAutoCombineThreshold *= COIN;
        } else if (strType == DBKeys::DESTDATA) {
            std::string strAddress, strKey, strValue;
            ssKey >> strAddress;
            ssKey >> strKey;
            ssValue >> strValue;
            if (!pwallet->LoadDestData(DecodeDestination(strAddress), strKey, strValue)) {
                strErr = "Error reading wallet database: LoadDestData failed";
                return false;
            }
        } else if (strType == DBKeys::HDCHAIN) { // Regular key chain counter
            CHDChain chain;
            ssValue >> chain;
            pwallet->GetScriptPubKeyMan()->SetHDChain(chain, true);
        } else if (strType == DBKeys::SAP_HDCHAIN) {
            CHDChain chain;
            ssValue >> chain;
            pwallet->GetSaplingScriptPubKeyMan()->SetHDChain(chain, true);
        } else if (strType == DBKeys::SAP_KEY) {
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
        } else if (strType == DBKeys::SAP_COMMON_OVK) {
            uint256 ovk;
            ssValue >> ovk;
            pwallet->GetSaplingScriptPubKeyMan()->setCommonOVK(ovk);
        } else if (strType == DBKeys::SAP_KEY_CRIPTED) {
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
        } else if (strType == DBKeys::SAP_KEYMETA) {
            libzcash::SaplingIncomingViewingKey ivk;
            ssKey >> ivk;
            CKeyMetadata keyMeta;
            ssValue >> keyMeta;

            wss.nZKeyMeta++;

            pwallet->LoadSaplingZKeyMetadata(ivk, keyMeta);
        } else if (strType == DBKeys::SAP_ADDR) {
            libzcash::SaplingPaymentAddress addr;
            ssKey >> addr;
            libzcash::SaplingIncomingViewingKey ivk;
            ssValue >> ivk;

            wss.nSapZAddrs++;

            if (!pwallet->LoadSaplingPaymentAddress(addr, ivk)) {
                strErr = "Error reading wallet database: LoadSaplingPaymentAddress failed";
                return false;
            }
        } else if (strType == DBKeys::SAP_WITNESS_CACHE_SIZE) {
            ssValue >> pwallet->GetSaplingScriptPubKeyMan()->nWitnessCacheSize;
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool CWalletDB::IsKeyType(const std::string& strType)
{
    return (strType == DBKeys::KEY ||
            strType == DBKeys::MASTER_KEY || strType == DBKeys::CRYPTED_KEY ||
            strType == DBKeys::SAP_KEY || strType == DBKeys::SAP_KEY_CRIPTED);
}

DBErrors CWalletDB::LoadWallet(CWallet* pwallet)
{
    CWalletScanState wss;
    bool fNoncriticalErrors = false;
    DBErrors result = DB_LOAD_OK;

    LOCK(pwallet->cs_wallet);
    try {
        int nMinVersion = 0;
        if (batch.Read((std::string) DBKeys::MINVERSION, nMinVersion)) {
            if (nMinVersion > CLIENT_VERSION) {
                return DB_TOO_NEW;
            }
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
            if (ret == DB_NOTFOUND) {
                break;
            } else if (ret != 0) {
                LogPrintf("Error reading next record from wallet database\n");
                return DB_CORRUPT;
            }

            // Try to be tolerant of single corrupt records:
            std::string strType, strErr;
            if (!ReadKeyValue(pwallet, ssKey, ssValue, wss, strType, strErr)) {
                // losing keys is considered a catastrophic error, anything else
                // we assume the user can live with:
                if (IsKeyType(strType) || strType == DBKeys::DEFAULTKEY) {
                    result = DB_CORRUPT;
                } else {
                    // Leave other errors alone, if we try to fix them we might make things worse.
                    fNoncriticalErrors = true; // ... but do warn the user there is something wrong.
                    if (strType == DBKeys::TX)
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

    for (const uint256& hash : wss.vWalletUpgrade) {
        WriteTx(pwallet->mapWallet.at(hash));
    }

    // Rewrite encrypted wallets of versions 0.4.0 and 0.5.0rc:
    if (wss.fIsEncrypted && (wss.nFileVersion == 40000 || wss.nFileVersion == 50000)) {
        return DB_NEED_REWRITE;
    }

    if (wss.nFileVersion < CLIENT_VERSION) { // Update
        WriteVersion(CLIENT_VERSION);
    }

    if (wss.fAnyUnordered) {
        result = ReorderTransactions(pwallet);
    }

    return result;
}

DBErrors CWalletDB::FindWalletTx(CWallet* pwallet, std::vector<uint256>& vTxHash, std::vector<CWalletTx>& vWtx)
{
    bool fNoncriticalErrors = false;
    DBErrors result = DB_LOAD_OK;

    try {
        LOCK(pwallet->cs_wallet);
        int nMinVersion = 0;
        if (batch.Read((std::string) DBKeys::MINVERSION, nMinVersion)) {
            if (nMinVersion > CLIENT_VERSION) {
                return DB_TOO_NEW;
            }
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
            if (ret == DB_NOTFOUND) {
                break;
            } else if (ret != 0) {
                LogPrintf("Error reading next record from wallet database\n");
                return DB_CORRUPT;
            }

            std::string strType;
            ssKey >> strType;
            if (strType == DBKeys::TX) {
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
    if (err != DB_LOAD_OK) {
        return err;
    }

    // erase each wallet TX
    for (uint256& hash : vTxHash) {
        if (!EraseTx(hash)) return DB_CORRUPT;
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

    for (CWalletRef pwallet : vpwallets) {
        CWalletDBWrapper& dbh = pwallet->GetDBHandle();

        unsigned int nUpdateCounter = dbh.nUpdateCounter;
        if (dbh.nLastSeen != nUpdateCounter) {
            dbh.nLastSeen = nUpdateCounter;
            dbh.nLastWalletUpdate = GetTime();
        }

        if (dbh.nLastFlushed != nUpdateCounter && GetTime() - dbh.nLastWalletUpdate >= 2) {
            if (CDB::PeriodicFlush(dbh)) {
                dbh.nLastFlushed = nUpdateCounter;
            }
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

typedef std::multimap<std::time_t, fs::path> folder_set_t;
static folder_set_t buildBackupsMapSortedByLastWrite(const std::string& strWalletFile, const fs::path& backupsDir)
{
    folder_set_t folder_set;
    fs::directory_iterator end_iter;
    // Build map of backup files for current(!) wallet sorted by last write time
    for (fs::directory_iterator dir_iter(backupsDir); dir_iter != end_iter; ++dir_iter) {
        // Only check regular files
        if (fs::is_regular_file(dir_iter->status())) {
            // Only add the backups for the current wallet, e.g. wallet.dat.*
            if(dir_iter->path().stem().string() == strWalletFile) {
                folder_set.insert(folder_set_t::value_type(fs::last_write_time(dir_iter->path()), *dir_iter));
            }
        }
    }
    return folder_set;
}

static bool cleanWalletBackups(folder_set_t& folder_set, int nWalletBackups, std::string& strBackupWarning)
{
    // Loop backward through backup files and keep the N newest ones (1 <= N <= 10)
    int counter = 0;
    for (const std::pair<const std::time_t, fs::path>& file : reverse_iterate(folder_set)) {
        counter++;
        if (counter > nWalletBackups) {
            // More than nWalletBackups backups: delete oldest one(s)
            try {
                fs::remove(file.second);
                LogPrintf("Old backup deleted: %s\n", file.second);
            } catch (fs::filesystem_error &error) {
                strBackupWarning = strprintf(_("Failed to delete backup, error: %s"), error.what());
                LogPrintf("%s\n", strBackupWarning);
                return false;
            }
        }
    }
    return true;
}

bool AutoBackupWallet(const CWallet& wallet, std::string& strBackupWarning, std::string& strBackupError)
{
    std::string strWalletFile = wallet.GetDBHandle().GetName();

    strBackupWarning = strBackupError = "";
    int nWalletBackups = std::max(0, std::min(10, (int)gArgs.GetArg("-createwalletbackups", DEFAULT_CREATEWALLETBACKUPS)));
    if (nWalletBackups == 0) {
        LogPrintf("Automatic wallet backups are disabled!\n");
        return false;
    }

    fs::path backupsDir = GetDataDir() / "backups";
    backupsDir.make_preferred();
    TryCreateDirectories(backupsDir);

    // Copy wallet file
    fs::path sourceFile = GetDataDir() / strWalletFile;
    fs::path backupFile = backupsDir / (strWalletFile + FormatISO8601DateTimeForBackup(GetTime()));
    sourceFile.make_preferred();
    backupFile.make_preferred();
    if (fs::exists(backupFile)) {
        LogPrintf("%s\n", _("Failed to create backup, file already exists! This could happen if you restarted wallet in less than 60 seconds. You can continue if you are ok with this."));
        return false;
    }

    if (!fs::exists(sourceFile)) {
        strBackupWarning = _("Failed to create backup, wallet file not found");
        LogPrintf("%s\n", strBackupWarning);
        return false;
    }

    // Try to backup
    if (!AttemptBackupWallet(&wallet, sourceFile, backupFile)) {
        return false; // error is logged internally
    }

    // Keep only 0 < nWalletBackups <= 10 backups, including the new one of course
    folder_set_t folder_set = buildBackupsMapSortedByLastWrite(strWalletFile, backupsDir);
    return cleanWalletBackups(folder_set, nWalletBackups, strBackupWarning);
}

void MultiBackup(const CWallet& wallet, fs::path pathCustom, fs::path pathWithFile, const fs::path& pathSrc)
{
    int nThreshold = gArgs.GetArg("-custombackupthreshold", DEFAULT_CUSTOMBACKUPTHRESHOLD);
    if (nThreshold > 0) {
        std::string strBackupWarning;
        pathCustom.make_preferred();
        folder_set_t folderSet = buildBackupsMapSortedByLastWrite(wallet.GetDBHandle().GetName(), pathCustom);
        if (!cleanWalletBackups(folderSet, nThreshold, strBackupWarning)) {
            NotifyBacked(wallet, false, strBackupWarning);
        }

        // TODO: add seconds to avoid naming conflicts
        for (const auto& entry : folderSet) {
            if(entry.second == pathWithFile) {
                pathWithFile += "(1)";
            }
        }
    }
    AttemptBackupWallet(&wallet, pathSrc.string(), pathWithFile.string());
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
                bool defaultPath = AttemptBackupWallet(&wallet, pathSrc.string(), pathDest.string());

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

bool AttemptBackupWallet(const CWallet* wallet, const fs::path& pathSrc, const fs::path& pathDest)
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
        strMessage = strprintf("copied %s to %s\n", pathSrc.string(), pathDest.string());
        LogPrintf("%s : %s\n", __func__, strMessage);
        retStatus = true;
    } catch (const fs::filesystem_error& e) {
        retStatus = false;
        strMessage = strprintf("%s\n", e.what());
        LogPrintf("%s : %s\n", __func__, strMessage);
    }
    if (wallet) NotifyBacked(*wallet, retStatus, strMessage);
    return retStatus;
}

//
// Try to (very carefully!) recover wallet file if there is a problem.
//
bool CWalletDB::Recover(const std::string& filename, void *callbackDataIn, bool (*recoverKVcallback)(void* callbackData, CDataStream ssKey, CDataStream ssValue), std::string& out_backup_filename)
{
    return CDB::Recover(filename, callbackDataIn, recoverKVcallback, out_backup_filename);
}

bool CWalletDB::Recover(const std::string& filename, std::string& out_backup_filename)
{
    // recover without a key filter callback
    // results in recovering all record types
    return CWalletDB::Recover(filename, NULL, NULL, out_backup_filename);
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
    if (!IsKeyType(strType) && strType != DBKeys::HDCHAIN)
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
    return WriteIC(std::make_pair(std::string(DBKeys::DESTDATA), std::make_pair(address, key)), value);
}

bool CWalletDB::EraseDestData(const std::string& address, const std::string& key)
{
    return EraseIC(std::make_pair(std::string(DBKeys::DESTDATA), std::make_pair(address, key)));
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
