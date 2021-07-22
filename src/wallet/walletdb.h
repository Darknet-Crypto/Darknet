// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin developers
// Copyright (c) 2016-2021 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_WALLETDB_H
#define PIVX_WALLETDB_H

#include "amount.h"
#include "wallet/db.h"
#include "wallet/hdchain.h"
#include "key.h"
#include "keystore.h"
#include "script/keyorigin.h"

#include <list>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

/**
 * Overview of wallet database classes:
 *
 * - CDBEnv is an environment in which the database exists (has no analog in dbwrapper.h)
 * - CWalletDBWrapper represents a wallet database (similar to CDBWrapper in dbwrapper.h)
 * - CDB is a low-level database transaction (similar to CDBBatch in dbwrapper.h)
 * - CWalletDB is a modifier object for the wallet, and encapsulates a database
 *   transaction as well as methods to act on the database (no analog in
 *   dbwrapper.h)
 *
 * The latter two are named confusingly, in contrast to what the names CDB
 * and CWalletDB suggest they are transient transaction objects and don't
 * represent the database itself.
 */

static const bool DEFAULT_FLUSHWALLET = true;

struct CBlockLocator;
class CKeyPool;
class CMasterKey;
class CScript;
class CWallet;
class CWalletTx;
class uint160;
class uint256;

/** Error statuses for the wallet database */
enum DBErrors {
    DB_LOAD_OK,
    DB_CORRUPT,
    DB_NONCRITICAL_ERROR,
    DB_TOO_NEW,
    DB_LOAD_FAIL,
    DB_NEED_REWRITE
};

class CKeyMetadata
{
public:
    // Metadata versions
    static const int VERSION_BASIC = 1;
    static const int VERSION_WITH_KEY_ORIGIN = 12;
    // Active version
    static const int CURRENT_VERSION = VERSION_WITH_KEY_ORIGIN;

    int nVersion;
    int64_t nCreateTime; // 0 means unknown
    CKeyID hd_seed_id; //id of the HD seed used to derive this key
    KeyOriginInfo key_origin; // Key origin info with path and fingerprint

    CKeyMetadata()
    {
        SetNull();
    }
    CKeyMetadata(int64_t nCreateTime_)
    {
        SetNull();
        nCreateTime = nCreateTime_;
    }

    SERIALIZE_METHODS(CKeyMetadata, obj)
    {
        READWRITE(obj.nVersion, obj.nCreateTime);
        if (obj.HasKeyOrigin()) {
            READWRITE(obj.hd_seed_id, obj.key_origin);
        }
    }

    void SetNull()
    {
        nVersion = CKeyMetadata::CURRENT_VERSION;
        nCreateTime = 0;
        hd_seed_id.SetNull();
        key_origin.clear();
    }

    bool HasKeyOrigin() const
    {
        return this->nVersion >= VERSION_WITH_KEY_ORIGIN;
    }
};

/** Access to the wallet database.
 * This should really be named CWalletDBBatch, as it represents a single transaction at the
 * database. It will be committed when the object goes out of scope.
 * Optionally (on by default) it will flush to disk as well.
 */
class CWalletDB
{
private:
    template <typename K, typename T>
    bool WriteIC(const K& key, const T& value, bool fOverwrite = true)
    {
        if (!batch.Write(key, value, fOverwrite)) {
            return false;
        }
        m_dbw.IncrementUpdateCounter();
        return true;
    }

    template <typename K>
    bool EraseIC(const K& key)
    {
        if (!batch.Erase(key)) {
            return false;
        }
        m_dbw.IncrementUpdateCounter();
        return true;
    }

public:
    CWalletDB(CWalletDBWrapper& dbw, const char* pszMode = "r+", bool _fFlushOnClose = true) :
        batch(dbw, pszMode, _fFlushOnClose),
        m_dbw(dbw)
    {
    }
    CWalletDB(const CWalletDB&) = delete;
    CWalletDB& operator=(const CWalletDB&) = delete;

    bool WriteName(const std::string& strAddress, const std::string& strName);
    bool EraseName(const std::string& strAddress);

    bool WritePurpose(const std::string& strAddress, const std::string& purpose);
    bool ErasePurpose(const std::string& strAddress);

    bool WriteTx(const CWalletTx& wtx);
    bool EraseTx(uint256 hash);

    bool WriteKey(const CPubKey& vchPubKey, const CPrivKey& vchPrivKey, const CKeyMetadata& keyMeta);
    bool WriteCryptedKey(const CPubKey& vchPubKey, const std::vector<unsigned char>& vchCryptedSecret, const CKeyMetadata& keyMeta);
    bool WriteMasterKey(unsigned int nID, const CMasterKey& kMasterKey);

    bool WriteCScript(const uint160& hash, const CScript& redeemScript);

    bool WriteWatchOnly(const CScript& script);
    bool EraseWatchOnly(const CScript& script);

    bool WriteBestBlock(const CBlockLocator& locator);
    bool ReadBestBlock(CBlockLocator& locator);

    bool WriteOrderPosNext(int64_t nOrderPosNext);

    bool WriteStakeSplitThreshold(const CAmount& nStakeSplitThreshold);
    bool WriteUseCustomFee(bool fUse);
    bool WriteCustomFeeValue(const CAmount& nCustomFee);
    bool WriteAutoCombineSettings(bool fEnable, CAmount nCombineThreshold);

    bool ReadPool(int64_t nPool, CKeyPool& keypool);
    bool WritePool(int64_t nPool, const CKeyPool& keypool);
    bool ErasePool(int64_t nPool);

    bool WriteMinVersion(int nVersion);

    //! write the hdchain model (external/internal chain child index counter)
    bool WriteHDChain(const CHDChain& chain);

    /// Write extended spending key to wallet database, where the key is the incoming viewing key
    bool WriteSaplingZKey(const libzcash::SaplingIncomingViewingKey &ivk,
                          const libzcash::SaplingExtendedSpendingKey &key,
                          const CKeyMetadata  &keyMeta);

    bool WriteSaplingPaymentAddress(const libzcash::SaplingPaymentAddress &addr,
                                    const libzcash::SaplingIncomingViewingKey &ivk);

    bool WriteCryptedSaplingZKey(const libzcash::SaplingExtendedFullViewingKey &extfvk,
                                 const std::vector<unsigned char>& vchCryptedSecret,
                                 const CKeyMetadata &keyMeta);

    /// Common output viewing key, used when shielding transparent funds
    bool WriteSaplingCommonOVK(const uint256& ovk);
    bool ReadSaplingCommonOVK(uint256& ovkRet);

    bool WriteWitnessCacheSize(int64_t nWitnessCacheSize);

    /// Write destination data key,value tuple to database
    bool WriteDestData(const std::string& address, const std::string& key, const std::string& value);
    /// Erase destination data tuple from wallet database
    bool EraseDestData(const std::string& address, const std::string& key);

    DBErrors ReorderTransactions(CWallet* pwallet);
    DBErrors LoadWallet(CWallet* pwallet);
    DBErrors FindWalletTx(CWallet* pwallet, std::vector<uint256>& vTxHash, std::vector<CWalletTx>& vWtx);
    DBErrors ZapWalletTx(CWallet* pwallet, std::vector<CWalletTx>& vWtx);
    /* Try to (very carefully!) recover wallet database (with a possible key type filter) */
    static bool Recover(const std::string& filename, void *callbackDataIn, bool (*recoverKVcallback)(void* callbackData, CDataStream ssKey, CDataStream ssValue), std::string& out_backup_filename);
    /* Recover convenience-function to bypass the key filter callback, called when verify fails, recovers everything */
    static bool Recover(const std::string& filename, std::string& out_backup_filename);
    /* Recover filter (used as callback), will only let keys (cryptographical keys) as KV/key-type pass through */
    static bool RecoverKeysOnlyFilter(void *callbackData, CDataStream ssKey, CDataStream ssValue);
    /* Function to determin if a certain KV/key-type is a key (cryptographical key) type */
    static bool IsKeyType(const std::string& strType);
    /* verifies the database environment */
    static bool VerifyEnvironment(const std::string& walletFile, const fs::path& dataDir, std::string& errorStr);
    /* verifies the database file */
    static bool VerifyDatabaseFile(const std::string& walletFile, const fs::path& dataDir, std::string& warningStr, std::string& errorStr);

    //! Begin a new transaction
    bool TxnBegin();
    //! Commit current transaction
    bool TxnCommit();
    //! Abort current transaction
    bool TxnAbort();
    //! Read wallet version
    bool ReadVersion(int& nVersion);
    //! Write wallet version
    bool WriteVersion(int nVersion);
private:
    CDB batch;
    CWalletDBWrapper& m_dbw;
};

void NotifyBacked(const CWallet& wallet, bool fSuccess, std::string strMessage);
bool BackupWallet(const CWallet& wallet, const fs::path& strDest);
// If wallet is null, the NotifyBacked signal will not be broadcasted.
// todo: move NotifyBacked() signal to the caller side and/or decouple it from here in another function
bool AttemptBackupWallet(const CWallet* wallet, const fs::path& pathSrc, const fs::path& pathDest);

//! Called during init: Automatic backups of wallet not running (just copying and renaming dat file)
bool AutoBackupWallet(const CWallet& wallet, std::string& strBackupWarning, std::string& strBackupError);

//! Compacts BDB state so that wallet.dat is self-contained (if there are changes)
void MaybeCompactWalletDB();

#endif // PIVX_WALLETDB_H
