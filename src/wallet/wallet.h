// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_WALLET_H
#define PIVX_WALLET_H

#include "addressbook.h"
#include "amount.h"
#include "consensus/tx_verify.h"
#include "consensus/validation.h"
#include "crypter.h"
#include "destination_io.h"
#include "kernel.h"
#include "key.h"
#include "key_io.h"
#include "keystore.h"
#include "pairresult.h"
#include "policy/feerate.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "sapling/address.h"
#include "guiinterface.h"
#include "util/system.h"
#include "util/memory.h"
#include "utilstrencodings.h"
#include "validationinterface.h"
#include "script/ismine.h"
#include "wallet/scriptpubkeyman.h"
#include "sapling/saplingscriptpubkeyman.h"
#include "validation.h"
#include "wallet/walletdb.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <stdexcept>
#include <stdint.h>
#include <string>
#include <utility>
#include <vector>

typedef CWallet* CWalletRef;
extern std::vector<CWalletRef> vpwallets;

/**
 * Settings
 */
extern CFeeRate payTxFee;
extern CAmount maxTxFee;
extern unsigned int nTxConfirmTarget;
extern bool bSpendZeroConfChange;
extern bool bdisableSystemnotifications;
extern bool fPayAtLeastCustomFee;

//! -paytxfee default
static const CAmount DEFAULT_TRANSACTION_FEE = 0;
//! -paytxfee will warn if called with a higher fee than this amount (in satoshis) per KB
static const CAmount nHighTransactionFeeWarning = 0.1 * COIN;
//! -maxtxfee default
static const CAmount DEFAULT_TRANSACTION_MAXFEE = 1 * COIN;
//! -maxtxfee will warn if called with a higher fee than this amount (in satoshis)
static const CAmount nHighTransactionMaxFeeWarning = 100 * nHighTransactionFeeWarning;
//! -minstakesplit default
static const CAmount DEFAULT_MIN_STAKE_SPLIT_THRESHOLD = 100 * COIN;
//! Default for -spendzeroconfchange
static const bool DEFAULT_SPEND_ZEROCONF_CHANGE = true;
//! Default for -staking
static const bool DEFAULT_STAKING = true;
//! Default for -coldstaking
static const bool DEFAULT_COLDSTAKING = true;
//! Defaults for -gen and -genproclimit
static const bool DEFAULT_GENERATE = false;
static const unsigned int DEFAULT_GENERATE_PROCLIMIT = 1;
//! Default for -createwalletbackups
static const unsigned int DEFAULT_CREATEWALLETBACKUPS = 10;
//! Default for -disablewallet
static const bool DEFAULT_DISABLE_WALLET = false;

static const int64_t TIMESTAMP_MIN = 0;

class CAddressBookIterator;
class CCoinControl;
class COutput;
class CStakeableOutput;
class CReserveKey;
class CScript;
class CScheduler;
class ScriptPubKeyMan;
class SaplingScriptPubKeyMan;
class SaplingNoteData;
struct SaplingNoteEntry;
class CDeterministicMNList;

/** (client) version numbers for particular wallet features */
enum WalletFeature {
    FEATURE_BASE = 10500, // the earliest version new wallets supports (only useful for getinfo's clientversion output)

    FEATURE_WALLETCRYPT = 40000, // wallet encryption
    FEATURE_COMPRPUBKEY = 60000, // compressed public keys

    FEATURE_PRE_PIVX = 61000, // inherited version..

    // The following features were implemented in BTC but not in our wallet, we can simply skip them.
    // FEATURE_HD = 130000,  Hierarchical key derivation after BIP32 (HD Wallet)
    // FEATURE_HD_SPLIT = 139900, // Wallet with HD chain split (change outputs will use m/0'/1'/k)

    FEATURE_PRE_SPLIT_KEYPOOL = 169900, // Upgraded to HD SPLIT and can have a pre-split keypool

    FEATURE_SAPLING = 170000, // Upgraded to Saplings key manager.

    FEATURE_LATEST = FEATURE_SAPLING
};

/** A key pool entry */
class CKeyPool
{
public:
    //! The time at which the key was generated. Set in AddKeypoolPubKeyWithDB
    int64_t nTime;
    //! The public key
    CPubKey vchPubKey;
    //! Whether this keypool entry is in the internal, external or staking keypool.
    uint8_t type;
    //! Whether this key was generated for a keypool before the wallet was upgraded to HD-split
    bool m_pre_split;

    CKeyPool();
    CKeyPool(const CPubKey& vchPubKeyIn, const uint8_t& type);

    bool IsInternal() const { return type == HDChain::ChangeType::INTERNAL; }
    bool IsExternal() const { return type == HDChain::ChangeType::EXTERNAL; }
    bool IsStaking() const { return type == HDChain::ChangeType::STAKING; }

    template<typename Stream>
    void Serialize(Stream& s) const
    {
        int nVersion = s.GetVersion();
        if (!(s.GetType() & SER_GETHASH)) {
            s << nVersion;
        }
        s << nTime << vchPubKey << Span<unsigned char>((unsigned char*)&type, 1) << m_pre_split;
    }

    template<typename Stream>
    void Unserialize(Stream& s)
    {
        int nVersion = s.GetVersion();
        if (!(s.GetType() & SER_GETHASH)) {
            s >> nVersion;
        }
        s >> nTime >> vchPubKey;
        try {
            s >> Span<unsigned char>((unsigned char*)&type, 1);
        } catch (std::ios_base::failure&) {
            /* flag as external address if we can't read the internal boolean
               (this will be the case for any wallet before the HD chain split version) */
            type = HDChain::ChangeType::EXTERNAL;
        }
        try {
            s >> m_pre_split;
        } catch (std::ios_base::failure&) {
            /* flag as pre-split address if we can't read the m_pre_split boolean
               (this will be the case for any wallet prior to the HD chain upgrade) */
            m_pre_split = true;
        }
    }
};

/** Record info about last stake attempt:
 *  - tipBlock       index of the block on top of which last stake attempt was made
 *  - nTime          time slot of last attempt
 *  - nTries         number of UTXOs hashed during last attempt
 *  - nCoins         number of stakeable utxos during last attempt
**/
class CStakerStatus
{
private:
    const CBlockIndex* tipBlock{nullptr};
    int64_t nTime{0};
    int nTries{0};
    int nCoins{0};

public:
    // Get
    const CBlockIndex* GetLastTip() const { return tipBlock; }
    uint256 GetLastHash() const { return (GetLastTip() == nullptr ? UINT256_ZERO : GetLastTip()->GetBlockHash()); }
    int GetLastHeight() const { return (GetLastTip() == nullptr ? 0 : GetLastTip()->nHeight); }
    int GetLastCoins() const { return nCoins; }
    int GetLastTries() const { return nTries; }
    int64_t GetLastTime() const { return nTime; }
    // Set
    void SetLastCoins(const int coins) { nCoins = coins; }
    void SetLastTries(const int tries) { nTries = tries; }
    void SetLastTip(const CBlockIndex* lastTip) { tipBlock = lastTip; }
    void SetLastTime(const uint64_t lastTime) { nTime = lastTime; }
    void SetNull()
    {
        SetLastCoins(0);
        SetLastTries(0);
        SetLastTip(nullptr);
        SetLastTime(0);
    }
    // Check whether staking status is active (last attempt earlier than 30 seconds ago)
    bool IsActive() const { return (nTime + 30) >= GetTime(); }
};

class CRecipientBase {
public:
    CAmount nAmount;
    bool fSubtractFeeFromAmount;
    CRecipientBase(const CAmount& _nAmount, bool _fSubtractFeeFromAmount) :
                   nAmount(_nAmount), fSubtractFeeFromAmount(_fSubtractFeeFromAmount) {}
    virtual bool isTransparent() const { return true; };
    virtual Optional<CScript> getScript() const { return nullopt; }
    virtual Optional<libzcash::SaplingPaymentAddress> getSapPaymentAddr() const { return nullopt; }
    virtual std::string getMemo() const { return ""; }
};

class CRecipient final : public CRecipientBase
{
public:
    CScript scriptPubKey;
    CRecipient(const CScript& _scriptPubKey, const CAmount& _nAmount, bool _fSubtractFeeFromAmount) :
            CRecipientBase(_nAmount, _fSubtractFeeFromAmount), scriptPubKey(_scriptPubKey) {}
    bool isTransparent() const override { return true; }
    Optional<CScript> getScript() const override { return {scriptPubKey}; }
};

class CAddressBookIterator
{
public:
    explicit CAddressBookIterator(std::map<CWDestination, AddressBook::CAddressBookData>& _map) : map(_map), it(_map.begin()), itEnd(_map.end()) {}
    const CWDestination* GetDestKey();
    const CTxDestination* GetCTxDestKey();
    const libzcash::SaplingPaymentAddress* GetShieldedDestKey();
    AddressBook::CAddressBookData GetValue() { return it->second; }

    bool IsValid() { return it != itEnd; }

    bool Next() {
        if (!IsValid()) return false;
        it++;
        return IsValid();
    }

    void SetFilter(CTxDestination& filter)
    {
        it = map.find(filter);
        if (it != itEnd) {
            itEnd = std::next(it);
        }
    }

private:
    std::map<CWDestination, AddressBook::CAddressBookData>& map;
    std::map<CWDestination, AddressBook::CAddressBookData>::iterator it;
    std::map<CWDestination, AddressBook::CAddressBookData>::iterator itEnd;
};

template <class T>
using TxSpendMap = std::multimap<T, uint256>;
typedef std::map<SaplingOutPoint, SaplingNoteData> mapSaplingNoteData_t;

typedef std::map<std::string, std::string> mapValue_t;

static inline void ReadOrderPos(int64_t& nOrderPos, mapValue_t& mapValue)
{
    if (!mapValue.count("n")) {
        nOrderPos = -1; // TODO: calculate elsewhere
        return;
    }
    nOrderPos = atoi64(mapValue["n"].c_str());
}


static inline void WriteOrderPos(const int64_t& nOrderPos, mapValue_t& mapValue)
{
    if (nOrderPos == -1)
        return;
    mapValue["n"] = i64tostr(nOrderPos);
}

struct COutputEntry {
    CTxDestination destination;
    CAmount amount;
    int vout;
};

/** Legacy class used for deserializing vtxPrev for backwards compatibility.
 * vtxPrev was removed in commit 93a18a3650292afbb441a47d1fa1b94aeb0164e3,
 * but old wallet.dat files may still contain vtxPrev vectors of CMerkleTxs.
 * These need to get deserialized for field alignment when deserializing
 * a CWalletTx, but the deserialized values are discarded.**/
class CMerkleTx
{
private:

public:
    template<typename Stream>
    void Unserialize(Stream& s)
    {
        CTransactionRef tx;
        uint256 hashBlock;
        std::vector<uint256> vMerkleBranch;
        int nIndex;

        s >> tx >> hashBlock >> vMerkleBranch >> nIndex;
    }
};

/**
 * A transaction with a bunch of additional info that only the owner cares about.
 * It includes any unrecorded transactions needed to link it back to the block chain.
 */
class CWalletTx
{
private:
    const CWallet* pwallet;

    /** Constant used in hashBlock to indicate tx has been abandoned, only used at
     * serialization/deserialization to avoid ambiguity with conflicted.
     */
    static const uint256 ABANDON_HASH;

public:
    mapValue_t mapValue;
    mapSaplingNoteData_t mapSaplingNoteData;
    std::vector<std::pair<std::string, std::string> > vOrderForm;
    unsigned int fTimeReceivedIsTxTime;
    unsigned int nTimeReceived; //! time received by this node
    /**
     * Stable timestamp representing the block time, for a transaction included in a block,
     * or else the time when the transaction was received if it isn't yet part of a block.
     */
    unsigned int nTimeSmart;
    char fFromMe;
    int64_t nOrderPos; //! position in ordered transaction list

    // memory only
    enum AmountType { DEBIT, CREDIT, IMMATURE_CREDIT, AVAILABLE_CREDIT, AMOUNTTYPE_ENUM_ELEMENTS };
    CAmount GetCachableAmount(AmountType type, const isminefilter& filter, bool recalculate = false) const;
    bool IsAmountCached(AmountType type, const isminefilter& filter) const; // Only used in unit tests
    mutable CachableAmount m_amounts[AMOUNTTYPE_ENUM_ELEMENTS];

    mutable bool fStakeDelegationVoided;
    mutable bool fChangeCached;
    mutable bool fInMempool;
    mutable CAmount nChangeCached;
    mutable bool fShieldedChangeCached;
    mutable CAmount nShieldedChangeCached;

    CWalletTx(const CWallet* pwalletIn, CTransactionRef arg);
    void Init(const CWallet* pwalletIn);

    CTransactionRef tx;

    /* New transactions start as UNCONFIRMED. At BlockConnected,
     * they will transition to CONFIRMED. In case of reorg, at BlockDisconnected,
     * they roll back to UNCONFIRMED. If we detect a conflicting transaction at
     * block connection, we update conflicted tx and its dependencies as CONFLICTED.
     * If tx isn't confirmed and outside of mempool, the user may switch it to ABANDONED
     * by using the abandontransaction call. This last status may be override by a CONFLICTED
     * or CONFIRMED transition.
     */
    enum Status {
        UNCONFIRMED,
        CONFIRMED,
        CONFLICTED,
        ABANDONED
    };

    /* Confirmation includes tx status and a triplet of {block height/block hash/tx index in block}
     * at which tx has been confirmed. All three are set to 0 if tx is unconfirmed or abandoned.
     * Meaning of these fields changes with CONFLICTED state where they instead point to block hash
     * and block height of the deepest conflicting tx.
     */
    struct Confirmation {
        Status status;
        int block_height;
        uint256 hashBlock;
        int nIndex;
        Confirmation(Status s = UNCONFIRMED, int b = 0, const uint256& h = UINT256_ZERO, int i = 0) : status(s), block_height(b), hashBlock(h), nIndex(i) {}
    };

    Confirmation m_confirm;

    template<typename Stream>
    void Serialize(Stream& s) const
    {
        mapValue_t mapValueCopy = mapValue;

        mapValueCopy["fromaccount"] = "";
        WriteOrderPos(nOrderPos, mapValueCopy);
        if (nTimeSmart) {
            mapValueCopy["timesmart"] = strprintf("%u", nTimeSmart);
        }

        std::vector<char> dummy_vector1; //!< Used to be vMerkleBranch
        std::vector<char> dummy_vector2; //!< Used to be vtxPrev
        char dummy_char = false; //!< Used to be fSpent
        uint256 serializedHash = isAbandoned() ? ABANDON_HASH : m_confirm.hashBlock;
        int serializedIndex = isAbandoned() || isConflicted() ? -1 : m_confirm.nIndex;
        s << tx << serializedHash << dummy_vector1 << serializedIndex << dummy_vector2 << mapValueCopy << vOrderForm << fTimeReceivedIsTxTime << nTimeReceived << fFromMe << dummy_char;

        if (this->tx->isSaplingVersion()) {
            s << mapSaplingNoteData;
        }
    }

    template<typename Stream>
    void Unserialize(Stream& s)
    {
        Init(nullptr);

        std::vector<uint256> dummy_vector1; //!< Used to be vMerkleBranch
        std::vector<CMerkleTx> dummy_vector2; //!< Used to be vtxPrev
        char dummy_char; //! Used to be fSpent
        int serializedIndex;
        s >> tx >> m_confirm.hashBlock >> dummy_vector1 >> serializedIndex >> dummy_vector2 >> mapValue >> vOrderForm >> fTimeReceivedIsTxTime >> nTimeReceived >> fFromMe >> dummy_char;

        if (this->tx->isSaplingVersion()) {
            s >> mapSaplingNoteData;
        }

        /* At serialization/deserialization, an nIndex == -1 means that hashBlock refers to
         * the earliest block in the chain we know this or any in-wallet ancestor conflicts
         * with. If nIndex == -1 and hashBlock is ABANDON_HASH, it means transaction is abandoned.
         * In same context, an nIndex >= 0 refers to a confirmed transaction (if hashBlock set) or
         * unconfirmed one. Older clients interpret nIndex == -1 as unconfirmed for backward
         * compatibility (pre-commit 9ac63d6).
         */
        if (serializedIndex == -1 && m_confirm.hashBlock == ABANDON_HASH) {
            setAbandoned();
        } else if (serializedIndex == -1) {
            setConflicted();
        } else if (!m_confirm.hashBlock.IsNull()) {
            m_confirm.nIndex = serializedIndex;
            setConfirmed();
        }

        ReadOrderPos(nOrderPos, mapValue);
        nTimeSmart = mapValue.count("timesmart") ? (unsigned int)atoi64(mapValue["timesmart"]) : 0;

        mapValue.erase("fromaccount");
        mapValue.erase("version");
        mapValue.erase("spent");
        mapValue.erase("n");
        mapValue.erase("timesmart");
    }

    void SetTx(CTransactionRef arg) { tx = std::move(arg); }

    //! make sure balances are recalculated
    void MarkDirty();

    void BindWallet(CWallet* pwalletIn);

    void SetSaplingNoteData(mapSaplingNoteData_t& noteData);

    Optional<std::pair<
            libzcash::SaplingNotePlaintext,
            libzcash::SaplingPaymentAddress>> DecryptSaplingNote(const SaplingOutPoint& op) const;

    Optional<std::pair<
            libzcash::SaplingNotePlaintext,
            libzcash::SaplingPaymentAddress>> RecoverSaplingNote(const SaplingOutPoint& op, const std::set<uint256>& ovks) const;

    //! checks whether a tx has P2CS inputs or not
    bool HasP2CSInputs() const;

    //! Store a comment
    void SetComment(const std::string& comment) { mapValue["comment"] = comment; }
    std::string GetComment() const {
        const auto& it = mapValue.find("comment");
        return it != mapValue.end() ? it->second : "";
    }

    int GetDepthAndMempool(bool& fConflicted) const;

    //! filter decides which addresses will count towards the debit
    CAmount GetDebit(const isminefilter& filter) const;
    CAmount GetCredit(const isminefilter& filter, bool recalculate = false) const;
    CAmount GetImmatureCredit(bool fUseCache = true, const isminefilter& filter = ISMINE_SPENDABLE_ALL) const;
    CAmount GetAvailableCredit(bool fUseCache = true, const isminefilter& filter=ISMINE_SPENDABLE) const;
    // Return sum of locked coins
    CAmount GetLockedCredit() const;
    CAmount GetImmatureWatchOnlyCredit(const bool& fUseCache = true) const;
    CAmount GetAvailableWatchOnlyCredit(const bool& fUseCache = true) const;
    CAmount GetChange() const;

    // Shielded credit/debit/change
    CAmount GetShieldedChange() const;
    CAmount GetShieldedAvailableCredit(bool fUseCache = true) const;

    // Cold staking contracts credit/debit
    CAmount GetColdStakingCredit(bool fUseCache = true) const;
    CAmount GetColdStakingDebit(bool fUseCache = true) const;
    CAmount GetStakeDelegationCredit(bool fUseCache = true) const;
    CAmount GetStakeDelegationDebit(bool fUseCache = true) const;

    void GetAmounts(std::list<COutputEntry>& listReceived,
                    std::list<COutputEntry>& listSent,
                    CAmount& nFee,
                    const isminefilter& filter) const;

    bool IsFromMe(const isminefilter& filter) const;

    bool InMempool() const;

    // True if only scriptSigs are different
    bool IsEquivalentTo(const CWalletTx& tx) const;

    bool IsTrusted() const;
    bool IsTrusted(int& nDepth, bool& fConflicted) const;

    int64_t GetTxTime() const;
    void UpdateTimeSmart();
    void RelayWalletTransaction(CConnman* connman);
    std::set<uint256> GetConflicts() const;

    /**
     * Return depth of transaction in blockchain:
     * <0  : conflicts with a transaction this deep in the blockchain
     *  0  : in memory pool, waiting to be included in a block
     * >=1 : this many blocks deep in the main chain
     */
    // TODO: Remove "NO_THREAD_SAFETY_ANALYSIS" and replace it with the correct
    // annotation "EXCLUSIVE_LOCKS_REQUIRED(pwallet->cs_wallet)". The annotation
    // "NO_THREAD_SAFETY_ANALYSIS" was temporarily added to avoid having to
    // resolve the issue of member access into incomplete type CWallet. Note
    // that we still have the runtime check "AssertLockHeld(pwallet->cs_wallet)"
    // in place.
    int GetDepthInMainChain() const NO_THREAD_SAFETY_ANALYSIS;
    bool IsInMainChainImmature() const;
    int GetBlocksToMaturity() const;

    bool isAbandoned() const { return m_confirm.status == CWalletTx::ABANDONED; }
    void setAbandoned()
    {
        m_confirm.status = CWalletTx::ABANDONED;
        m_confirm.hashBlock = UINT256_ZERO;
        m_confirm.block_height = 0;
        m_confirm.nIndex = 0;
    }
    bool isConflicted() const { return m_confirm.status == CWalletTx::CONFLICTED; }
    void setConflicted() { m_confirm.status = CWalletTx::CONFLICTED; }
    bool isUnconfirmed() const { return m_confirm.status == CWalletTx::UNCONFIRMED; }
    void setUnconfirmed() { m_confirm.status = CWalletTx::UNCONFIRMED; }
    bool isConfirmed() const { return m_confirm.status == CWalletTx::CONFIRMED; }
    void setConfirmed() { m_confirm.status = CWalletTx::CONFIRMED; }

    const uint256& GetHash() const { return tx->GetHash(); }
    bool IsCoinBase() const { return tx->IsCoinBase(); }
    bool IsCoinStake() const { return tx->IsCoinStake(); }

    /** Pass this transaction to the mempool. Fails if absolute fee exceeds absurd fee. */
    bool AcceptToMemoryPool(CValidationState& state);
};


class WalletRescanReserver; //forward declarations for ScanForWalletTransactions/RescanFromTime

/**
 * A CWallet is an extension of a keystore, which also maintains a set of transactions and balances,
 * and provides the ability to create new transactions.
 */
class CWallet : public CCryptoKeyStore, public CValidationInterface
{
private:
    static std::atomic<bool> fFlushScheduled;
    std::atomic<bool> fAbortRescan;
    std::atomic<bool> fScanningWallet; //controlled by WalletRescanReserver
    std::mutex mutexScanning;
    friend class WalletRescanReserver;


    //! keeps track of whether Unlock has run a thorough check before
    bool fDecryptionThoroughlyChecked{false};

    //! Key manager //
    std::unique_ptr<ScriptPubKeyMan> m_spk_man = MakeUnique<ScriptPubKeyMan>(this);
    std::unique_ptr<SaplingScriptPubKeyMan> m_sspk_man = MakeUnique<SaplingScriptPubKeyMan>(this);

    //! the current wallet version: clients below this version are not able to load the wallet
    int nWalletVersion;

    //! the maximum wallet format version: memory-only variable that specifies to what version this wallet may be upgraded
    int nWalletMaxVersion;

    /**
     * Wallet filename from wallet=<path> command line or config option.
     * Used in debug logs and to send RPCs to the right wallet instance when
     * more than one wallet is loaded.
     */
    std::string m_name;

    /** Internal database handle. */
    std::unique_ptr<CWalletDBWrapper> dbw;

    /**
     * The following is used to keep track of how far behind the wallet is
     * from the chain sync, and to allow clients to block on us being caught up.
     *
     * Note that this is *not* how far we've processed, we may need some rescan
     * to have seen all transactions in the chain, but is only used to track
     * live BlockConnected callbacks.
     *
     * Protected by cs_main (see BlockUntilSyncedToCurrentChain)
     */
    uint256 m_last_block_processed GUARDED_BY(cs_wallet) = UINT256_ZERO;

    /* Height of last block processed is used by wallet to know depth of transactions
    * without relying on Chain interface beyond asynchronous updates. For safety, we
    * initialize it to -1. Height is a pointer on node's tip and doesn't imply
    * that the wallet has scanned sequentially all blocks up to this one.
    */
    int m_last_block_processed_height GUARDED_BY(cs_wallet) = -1;
    int64_t m_last_block_processed_time GUARDED_BY(cs_wallet) = 0;

    int64_t nNextResend;
    int64_t nLastResend;

    /**
     * Used to keep track of spent outpoints, and
     * detect and report conflicts (double-spends or
     * mutated transactions where the mutant gets mined).
     */
    typedef TxSpendMap<COutPoint> TxSpends;
    TxSpends mapTxSpends;
    void AddToSpends(const COutPoint& outpoint, const uint256& wtxid);
    void AddToSpends(const uint256& wtxid);

    /* Mark a transaction (and its in-wallet descendants) as conflicting with a particular block. */
    void MarkConflicted(const uint256& hashBlock, int conflicting_height, const uint256& hashTx);

    template <class T>
    void SyncMetaData(std::pair<typename TxSpendMap<T>::iterator, typename TxSpendMap<T>::iterator> range);
    void ChainTipAdded(const CBlockIndex *pindex, const CBlock *pblock, SaplingMerkleTree saplingTree);

    /* Used by TransactionAddedToMemorypool/BlockConnected/Disconnected */
    void SyncTransaction(const CTransactionRef& tx, const CWalletTx::Confirmation& confirm);

    bool IsKeyUsed(const CPubKey& vchPubKey);

    struct OutputAvailabilityResult
    {
        bool available{false};
        bool solvable{false};
        bool spendable{false};
    };

    OutputAvailabilityResult CheckOutputAvailability(const CTxOut& output,
                                                     const unsigned int outIndex,
                                                     const uint256& wtxid,
                                                     const CCoinControl* coinControl,
                                                     const bool fCoinsSelected,
                                                     const bool fIncludeColdStaking,
                                                     const bool fIncludeDelegated,
                                                     const bool fIncludeLocked) const;

    //! Destination --> label/purpose mapping.
    std::map<CWDestination, AddressBook::CAddressBookData> mapAddressBook;

public:

    static const CAmount DEFAULT_STAKE_SPLIT_THRESHOLD = 500 * COIN;

    //! Generates hd wallet //
    bool SetupSPKM(bool newKeypool = true, bool memOnly = false);
    //! Whether the wallet is hd or not //
    bool IsHDEnabled() const;
    //! Whether the wallet supports Sapling or not //
    bool IsSaplingUpgradeEnabled() const;

    /** Get last block processed height */
    int GetLastBlockHeight() const EXCLUSIVE_LOCKS_REQUIRED(cs_wallet)
    {
        AssertLockHeld(cs_wallet);
        assert(m_last_block_processed_height >= 0);
        return m_last_block_processed_height;
    };
    /** Set last block processed height, currently only use in unit test */
    void SetLastBlockProcessed(const CBlockIndex* pindex) EXCLUSIVE_LOCKS_REQUIRED(cs_wallet)
    {
        AssertLockHeld(cs_wallet);
        m_last_block_processed_height = pindex->nHeight;
        m_last_block_processed = pindex->GetBlockHash();
        m_last_block_processed_time = pindex->GetBlockTime();
    };

    /* SPKM Helpers */
    const CKeyingMaterial& GetEncryptionKey() const;
    bool HasEncryptionKeys() const;

    //! Get spkm
    ScriptPubKeyMan* GetScriptPubKeyMan() const;
    SaplingScriptPubKeyMan* GetSaplingScriptPubKeyMan() const { return m_sspk_man.get(); }

    bool HasSaplingSPKM() const;

    /*
     * Main wallet lock.
     * This lock protects all the fields added by CWallet.
     */
    mutable RecursiveMutex cs_wallet;

    bool fWalletUnlockStaking;

    CWalletDB* pwalletdbEncryption;

    std::map<CKeyID, CKeyMetadata> mapKeyMetadata;

    typedef std::map<unsigned int, CMasterKey> MasterKeyMap;
    MasterKeyMap mapMasterKeys;
    unsigned int nMasterKeyMaxID;

    // Stake split threshold
    CAmount nStakeSplitThreshold;
    // minimum value allowed for nStakeSplitThreshold (customizable with -minstakesplit flag)
    static CAmount minStakeSplitThreshold;
    // Staker status (last hashed block and time)
    CStakerStatus* pStakerStatus = nullptr;

    // User-defined fee PIV/kb
    bool fUseCustomFee;
    CAmount nCustomFee;

    //Auto Combine Inputs
    bool fCombineDust;
    CAmount nAutoCombineThreshold;

    /** Get database handle used by this wallet. Ideally this function would
     * not be necessary.
     */
    CWalletDBWrapper* GetDBHandlePtr() const { return dbw.get(); }
    CWalletDBWrapper& GetDBHandle() const { return *dbw; }

    /** Get a name for this wallet for logging/debugging purposes.
     */
    const std::string& GetName() const { return m_name; }

    /** Get the path to the wallet's db file */
    fs::path GetPathToDBFile() { return dbw->GetPathToFile(); }

    /** Construct wallet with specified name and database implementation. */
    CWallet(std::string name, std::unique_ptr<CWalletDBWrapper> dbw_in);
    ~CWallet();
    void SetNull();

    std::map<uint256, CWalletTx> mapWallet;

    typedef std::multimap<int64_t, CWalletTx*> TxItems;
    TxItems wtxOrdered;

    int64_t nOrderPosNext;

    std::set<COutPoint> setLockedCoins;

    int64_t nTimeFirstKey;

    // Public SyncMetadata interface used for the sapling spent nullifier map.
    void SyncMetaDataN(std::pair<TxSpendMap<uint256>::iterator, TxSpendMap<uint256>::iterator> range);

    const CWalletTx* GetWalletTx(const uint256& hash) const;

    std::vector<CWalletTx> getWalletTxs();
    std::string GetUniqueWalletBackupName() const;

    //! check whether we are allowed to upgrade (or already support) to the named feature
    bool CanSupportFeature(enum WalletFeature wf);

    struct AvailableCoinsFilter {
        public:
        AvailableCoinsFilter() {}
        AvailableCoinsFilter(bool _fIncludeDelegated,
                             bool _fIncludeColdStaking,
                             bool _fOnlySafe,
                             bool _fOnlySpendable,
                             std::set<CTxDestination>* _onlyFilteredDest,
                             int _minDepth,
                             bool _fIncludeLocked = false,
                             CAmount _nMaxOutValue = 0) :
                fIncludeDelegated(_fIncludeDelegated),
                fIncludeColdStaking(_fIncludeColdStaking),
                fOnlySafe(_fOnlySafe),
                fOnlySpendable(_fOnlySpendable),
                onlyFilteredDest(_onlyFilteredDest),
                minDepth(_minDepth),
                fIncludeLocked(_fIncludeLocked),
                nMaxOutValue(_nMaxOutValue) {}

        bool fIncludeDelegated{true};
        bool fIncludeColdStaking{false};
        bool fOnlySafe{true};
        bool fOnlySpendable{false};
        std::set<CTxDestination>* onlyFilteredDest{nullptr};
        int minDepth{0};
        bool fIncludeLocked{false};
        // Select outputs with value <= nMaxOutValue
        CAmount nMaxOutValue{0}; // 0 means not active
        CAmount nMinOutValue{0}; // 0 means not active
        CAmount nMinimumSumAmount{0}; // 0 means not active
        unsigned int nMaximumCount{0}; // 0 means not active
    };

    //! >> Available coins (generic)
    bool AvailableCoins(std::vector<COutput>* pCoins,   // --> populates when != nullptr
                        const CCoinControl* coinControl = nullptr,
                        AvailableCoinsFilter coinsFilter = AvailableCoinsFilter()
                        ) const;
    //! >> Available coins (spending)
    bool SelectCoinsToSpend(const std::vector<COutput>& vAvailableCoins, const CAmount& nTargetValue, std::set<std::pair<const CWalletTx*, unsigned int> >& setCoinsRet, CAmount& nValueRet, const CCoinControl* coinControl = nullptr) const;
    bool SelectCoinsMinConf(const CAmount& nTargetValue, int nConfMine, int nConfTheirs, std::vector<COutput> vCoins, std::set<std::pair<const CWalletTx*, unsigned int> >& setCoinsRet, CAmount& nValueRet) const;
    //! >> Available coins (staking)
    bool StakeableCoins(std::vector<CStakeableOutput>* pCoins = nullptr);
    //! >> Available coins (P2CS)
    void GetAvailableP2CSCoins(std::vector<COutput>& vCoins) const;

    std::map<CTxDestination, std::vector<COutput> > AvailableCoinsByAddress(bool fConfirmed, CAmount maxCoinValue, bool fIncludeColdStaking);

    /**
     * Return list of available coins and locked coins grouped by non-change output address.
     * PIVX: group coins by pair <CTxDestination, Optional<CTxDestination>>. The optional destination
     * is reserved for the staker address in case of P2CS.
     */
    std::map<std::pair<CTxDestination, Optional<CTxDestination>>, std::vector<COutput>> ListCoins() const;
    /**
     * Return list of available shield notes grouped by sapling address.
     */
    std::map<libzcash::SaplingPaymentAddress, std::vector<SaplingNoteEntry>> ListNotes() const;

    /// Get 10000 PIV output and keys which can be used for the Masternode
    bool GetMasternodeVinAndKeys(CTxIn& txinRet, CPubKey& pubKeyRet,
            CKey& keyRet, std::string strTxHash, std::string strOutputIndex, std::string& strError);
    /// Extract txin information and keys from output
    bool GetVinAndKeysFromOutput(COutput out, CTxIn& txinRet, CPubKey& pubKeyRet, CKey& keyRet, bool fColdStake = false);

    bool IsSpent(const COutPoint& outpoint) const;
    bool IsSpent(const uint256& hash, unsigned int n) const;

    bool IsLockedCoin(const uint256& hash, unsigned int n) const;
    void LockCoin(const COutPoint& output);
    void UnlockCoin(const COutPoint& output);
    void UnlockAllCoins();
    std::set<COutPoint> ListLockedCoins();

    /*
     * Rescan abort properties
     */
    void AbortRescan() { fAbortRescan = true; }
    bool IsAbortingRescan() { return fAbortRescan; }
    bool IsScanning() { return fScanningWallet; }

    /*
     * Stake Split threshold
     */
    bool SetStakeSplitThreshold(const CAmount sst);
    CAmount GetStakeSplitThreshold() const { LOCK(cs_wallet); return nStakeSplitThreshold; }

    /*
     *  Requires cs_wallet lock.
     *  Lock for spending the coin c, if it's owned by the wallet, it's unspent, and:
     *  -- If ptx is not null, c is one of the outputs of *ptx
     *  -- If ptx is null, c is the output of a transaction in mapWallet
     */
    void LockOutpointIfMine(const CTransactionRef& ptx, const COutPoint& c);

    /*
     *  Locks cs_wallet
     *  Called during Init. If a DMN collateral is found in the wallet,
     *  lock the corresponding coin, to prevent accidental spending.
     */
    void ScanMasternodeCollateralsAndLock(const CDeterministicMNList& mnList);

    /*
     *  Requires cs_wallet lock.
     *  Called from AddToWalletIfInvolvingMe. If ptx is a ProRegTx, and the
     *  collateral (either referenced or created) is owned by this wallet,
     *  lock the corresponding coin, to prevent accidental spending.
     */
    void LockIfMyCollateral(const CTransactionRef& ptx);

    //  keystore implementation
    PairResult getNewAddress(CTxDestination& ret, const std::string addressLabel, const std::string purpose,
                                           const CChainParams::Base58Type addrType = CChainParams::PUBKEY_ADDRESS);
    PairResult getNewAddress(CTxDestination& ret, std::string label);
    PairResult getNewStakingAddress(CTxDestination& ret, std::string label);
    int64_t GetKeyCreationTime(const CWDestination& dest);
    int64_t GetKeyCreationTime(CPubKey pubkey);
    int64_t GetKeyCreationTime(const CTxDestination& address);
    int64_t GetKeyCreationTime(const libzcash::SaplingPaymentAddress& address);

    //////////// Sapling //////////////////

    // Search for notes and addresses from this wallet in the tx, and add the addresses --> IVK mapping to the keystore if missing.
    bool FindNotesDataAndAddMissingIVKToKeystore(const CTransaction& tx, Optional<mapSaplingNoteData_t>& saplingNoteData);
    // Decrypt sapling output notes with the inputs ovk and updates saplingNoteDataMap
    void AddExternalNotesDataToTx(CWalletTx& wtx) const;

    //! Generates new Sapling key
    libzcash::SaplingPaymentAddress GenerateNewSaplingZKey(std::string label = "");

    //! pindex is the new tip being connected.
    void IncrementNoteWitnesses(const CBlockIndex* pindex,
                                const CBlock* pblock,
                                SaplingMerkleTree& saplingTree);

    //! pindex is the old tip being disconnected.
    void DecrementNoteWitnesses(const CBlockIndex* pindex);


    //! Adds Sapling spending key to the store, and saves it to disk
    bool AddSaplingZKey(const libzcash::SaplingExtendedSpendingKey &key);
    bool AddSaplingIncomingViewingKeyW(
            const libzcash::SaplingIncomingViewingKey &ivk,
            const libzcash::SaplingPaymentAddress &addr);
    bool AddCryptedSaplingSpendingKeyW(
            const libzcash::SaplingExtendedFullViewingKey &extfvk,
            const std::vector<unsigned char> &vchCryptedSecret);
    //! Returns true if the wallet contains the spending key
    bool HaveSpendingKeyForPaymentAddress(const libzcash::SaplingPaymentAddress &zaddr) const;


    //! Adds spending key to the store, without saving it to disk (used by LoadWallet)
    bool LoadSaplingZKey(const libzcash::SaplingExtendedSpendingKey &key);
    //! Load spending key metadata (used by LoadWallet)
    bool LoadSaplingZKeyMetadata(const libzcash::SaplingIncomingViewingKey &ivk, const CKeyMetadata &meta);
    //! Adds a Sapling payment address -> incoming viewing key map entry,
    //! without saving it to disk (used by LoadWallet)
    bool LoadSaplingPaymentAddress(
            const libzcash::SaplingPaymentAddress &addr,
            const libzcash::SaplingIncomingViewingKey &ivk);
    //! Adds an encrypted spending key to the store, without saving it to disk (used by LoadWallet)
    bool LoadCryptedSaplingZKey(const libzcash::SaplingExtendedFullViewingKey &extfvk,
                                const std::vector<unsigned char> &vchCryptedSecret);

    //////////// End Sapling //////////////

    //! Adds a key to the store, and saves it to disk.
    bool AddKeyPubKey(const CKey& key, const CPubKey& pubkey) override;
    //! Adds a key to the store, without saving it to disk (used by LoadWallet)
    bool LoadKey(const CKey& key, const CPubKey& pubkey) { return CCryptoKeyStore::AddKeyPubKey(key, pubkey); }
    //! Load metadata (used by LoadWallet)
    bool LoadKeyMetadata(const CPubKey& pubkey, const CKeyMetadata& metadata);

    bool LoadMinVersion(int nVersion);
    void UpdateTimeFirstKey(int64_t nCreateTime);

    //! Adds an encrypted key to the store, and saves it to disk.
    bool AddCryptedKey(const CPubKey& vchPubKey, const std::vector<unsigned char>& vchCryptedSecret) override;
    //! Adds an encrypted key to the store, without saving it to disk (used by LoadWallet)
    bool LoadCryptedKey(const CPubKey& vchPubKey, const std::vector<unsigned char>& vchCryptedSecret);
    bool AddCScript(const CScript& redeemScript) override;
    bool LoadCScript(const CScript& redeemScript);

    //! Adds a destination data tuple to the store, and saves it to disk
    bool AddDestData(const CTxDestination& dest, const std::string& key, const std::string& value);
    //! Erases a destination data tuple in the store and on disk
    bool EraseDestData(const CTxDestination& dest, const std::string& key);
    //! Adds a destination data tuple to the store, without saving it to disk
    bool LoadDestData(const CTxDestination& dest, const std::string& key, const std::string& value);
    //! Get all destination values matching a prefix.
    std::vector<std::string> GetDestValues(const std::string& prefix) const;

    //! Adds a watch-only address to the store, and saves it to disk.
    bool AddWatchOnly(const CScript& dest) override;
    bool RemoveWatchOnly(const CScript& dest) override;
    //! Adds a watch-only address to the store, without saving it to disk (used by LoadWallet)
    bool LoadWatchOnly(const CScript& dest);

    //! Lock Wallet
    //! Holds a timestamp at which point the wallet is scheduled (externally) to be relocked. Caller must arrange for actual relocking to occur via Lock().
    int64_t nRelockTime;
    bool Lock();
    bool Unlock(const SecureString& strWalletPassphrase, bool anonimizeOnly = false);
    bool Unlock(const CKeyingMaterial& vMasterKeyIn);

    bool ChangeWalletPassphrase(const SecureString& strOldWalletPassphrase, const SecureString& strNewWalletPassphrase);
    bool EncryptWallet(const SecureString& strWalletPassphrase);

    std::vector<CKeyID> GetAffectedKeys(const CScript& spk);
    void GetKeyBirthTimes(std::map<CKeyID, int64_t>& mapKeyBirth) const;

    /**
     * Increment the next transaction order id
     * @return next transaction order id
     */
    int64_t IncOrderPosNext(CWalletDB* pwalletdb = NULL);

    void MarkDirty();
    bool AddToWallet(const CWalletTx& wtxIn, bool fFlushOnClose = true);
    bool LoadToWallet(CWalletTx& wtxIn);
    void TransactionAddedToMempool(const CTransactionRef& tx) override;
    void BlockConnected(const std::shared_ptr<const CBlock>& pblock, const CBlockIndex *pindex) override;
    void BlockDisconnected(const std::shared_ptr<const CBlock>& pblock, const uint256& blockHash, int nBlockHeight, int64_t blockTime) override;
    bool AddToWalletIfInvolvingMe(const CTransactionRef& tx, const CWalletTx::Confirmation& confirm, bool fUpdate);
    void EraseFromWallet(const uint256& hash);

    /**
     * Upgrade wallet to HD and Sapling if needed. Does nothing if not.
     */
    bool Upgrade(std::string& error, const int& prevVersion);
    bool ActivateSaplingWallet(bool memOnly = false);

    int64_t RescanFromTime(int64_t startTime, const WalletRescanReserver& reserver, bool update);
    CBlockIndex* ScanForWalletTransactions(CBlockIndex* pindexStart, CBlockIndex* pindexStop, const WalletRescanReserver& reserver, bool fUpdate = false, bool fromStartup = false);
    void TransactionRemovedFromMempool(const CTransactionRef &ptx, MemPoolRemovalReason reason) override;
    void ReacceptWalletTransactions(bool fFirstLoad = false);
    void ResendWalletTransactions(CConnman* connman) override;

    struct Balance {
        CAmount m_mine_trusted{0};               //!< Trusted, at depth=GetBalance.min_depth or more
        CAmount m_mine_untrusted_pending{0};     //!< Untrusted, but in mempool (pending)
        CAmount m_mine_immature{0};              //!< Immature coinbases/coinstakes in the main chain
        CAmount m_mine_trusted_shield{0};        //!< Trusted shield, at depth=GetBalance.min_depth or more
        CAmount m_mine_untrusted_shielded_balance{0}; //!< Untrusted shield, but in mempool (pending)
        CAmount m_mine_cs_delegated_trusted{0};  //!< Trusted, at depth=GetBalance.min_depth or more. Part of m_mine_trusted as well
    };
    Balance GetBalance(int min_depth = 0) const;

    CAmount loopTxsBalance(const std::function<void(const uint256&, const CWalletTx&, CAmount&)>&method) const;
    CAmount GetAvailableBalance(bool fIncludeDelegated = true, bool fIncludeShielded = true) const;
    CAmount GetAvailableBalance(isminefilter& filter, bool useCache = false, int minDepth = 1) const;
    CAmount GetColdStakingBalance() const;  // delegated coins for which we have the staking key
    CAmount GetImmatureColdStakingBalance() const;
    CAmount GetStakingBalance(const bool fIncludeColdStaking = true) const;
    CAmount GetDelegatedBalance() const;    // delegated coins for which we have the spending key
    CAmount GetImmatureDelegatedBalance() const;
    CAmount GetLockedCoins() const;
    CAmount GetUnconfirmedBalance(isminetype filter = ISMINE_SPENDABLE_ALL) const;
    CAmount GetImmatureBalance() const;
    CAmount GetWatchOnlyBalance() const;
    CAmount GetUnconfirmedWatchOnlyBalance() const;
    CAmount GetImmatureWatchOnlyBalance() const;
    CAmount GetLegacyBalance(const isminefilter& filter, int minDepth) const;
    bool FundTransaction(CMutableTransaction& tx, CAmount &nFeeRet, bool overrideEstimatedFeeRate, const CFeeRate& specificFeeRate, int& nChangePosInOut, std::string& strFailReason, bool includeWatching, bool lockUnspents, const std::set<int>& setSubtractFeeFromOutputs, const CTxDestination& destChange = CNoDestination());
    /**
     * Create a new transaction paying the recipients with a set of coins
     * selected by SelectCoins(); Also create the change output, when needed
     * @note passing nChangePosInOut as -1 will result in setting a random position
     */
    bool CreateTransaction(const std::vector<CRecipient>& vecSend,
        CTransactionRef& txRet,
        CReserveKey& reservekey,
        CAmount& nFeeRet,
        int& nChangePosInOut,
        std::string& strFailReason,
        const CCoinControl* coinControl = NULL,
        bool sign = true,
        CAmount nFeePay = 0,
        bool fIncludeDelegated = false,
        bool* fStakeDelegationVoided = nullptr,
        int nExtraSize = 0);

    bool CreateTransaction(CScript scriptPubKey, const CAmount& nValue, CTransactionRef& tx, CReserveKey& reservekey, CAmount& nFeeRet, std::string& strFailReason, const CCoinControl* coinControl = NULL, CAmount nFeePay = 0, bool fIncludeDelegated = false);

    // enumeration for CommitResult (return status of CommitTransaction)
    enum CommitStatus
    {
        OK,
        Abandoned,              // Failed to accept to memory pool. Successfully removed from the wallet.
        NotAccepted,            // Failed to accept to memory pool. Unable to abandon.
    };
    struct CommitResult
    {
        CommitResult(): status(CommitStatus::NotAccepted) {}
        CWallet::CommitStatus status;
        CValidationState state;
        uint256 hashTx = UINT256_ZERO;
        // converts CommitResult in human-readable format
        std::string ToString() const;
    };
    CWallet::CommitResult CommitTransaction(CTransactionRef tx, CReserveKey& opReservekey, CConnman* connman);
    CWallet::CommitResult CommitTransaction(CTransactionRef tx, CReserveKey* reservekey, CConnman* connman, mapValue_t* extraValues=nullptr);
    bool CreateCoinStake(const CBlockIndex* pindexPrev,
                         unsigned int nBits,
                         CMutableTransaction& txNew,
                         int64_t& nTxNewTime,
                         std::vector<CStakeableOutput>* availableCoins) const;
    bool SignCoinStake(CMutableTransaction& txNew) const;
    void AutoCombineDust(CConnman* connman);

    // Shielded balances
    CAmount GetAvailableShieldedBalance(bool fUseCache = true) const;
    CAmount GetUnconfirmedShieldedBalance() const;

    static CFeeRate minTxFee;

    size_t KeypoolCountExternalKeys();
    bool TopUpKeyPool(unsigned int kpSize = 0);
    void KeepKey(int64_t nIndex);
    void ReturnKey(int64_t nIndex, const bool& internal = false, const bool& staking = false);
    bool GetKeyFromPool(CPubKey& key, const uint8_t& type = HDChain::ChangeType::EXTERNAL);
    int64_t GetOldestKeyPoolTime();

    std::set<std::set<CTxDestination> > GetAddressGroupings();
    std::map<CTxDestination, CAmount> GetAddressBalances();

    std::set<CTxDestination> GetLabelAddresses(const std::string& label) const;

    bool CreateBudgetFeeTX(CTransactionRef& tx, const uint256& hash, CReserveKey& keyChange, bool fFinalization);

    bool IsUsed(const CTxDestination address) const;

    isminetype IsMine(const CTxIn& txin) const;
    CAmount GetDebit(const CTxIn& txin, const isminefilter& filter) const;
    isminetype IsMine(const CTxOut& txout) const;
    CAmount GetCredit(const CTxOut& txout, const isminefilter& filter) const;
    bool IsChange(const CTxOut& txout) const;
    bool IsChange(const CTxDestination& address) const;
    CAmount GetChange(const CTxOut& txout) const;
    bool IsMine(const CTransactionRef& tx) const;
    /** should probably be renamed to IsRelevantToMe */
    bool IsFromMe(const CTransactionRef& tx) const;
    CAmount GetDebit(const CTransactionRef& tx, const isminefilter& filter) const;
    CAmount GetCredit(const CWalletTx& tx, const isminefilter& filter) const;
    CAmount GetChange(const CTransactionRef& tx) const;

    void SetBestChain(const CBlockLocator& loc) override;
    void SetBestChainInternal(CWalletDB& walletdb, const CBlockLocator& loc); // only public for testing purposes, must never be called directly in any other situation
    // Force balance recomputation if any transaction got conflicted
    void MarkAffectedTransactionsDirty(const CTransaction& tx); // only public for testing purposes, must never be called directly in any other situation

    DBErrors LoadWallet(bool& fFirstRunRet);
    DBErrors ZapWalletTx(std::vector<CWalletTx>& vWtx);

    static std::string ParseIntoAddress(const CWDestination& dest, const std::string& purpose);

    bool SetAddressBook(const CWDestination& address, const std::string& strName, const std::string& purpose);
    bool DelAddressBook(const CWDestination& address, const CChainParams::Base58Type addrType = CChainParams::PUBKEY_ADDRESS);
    bool HasAddressBook(const CWDestination& address) const;
    bool HasDelegator(const CTxOut& out) const;
    int GetAddressBookSize() const { return mapAddressBook.size(); };

    CAddressBookIterator NewAddressBookIterator() { return CAddressBookIterator(mapAddressBook); }
    std::string GetPurposeForAddressBookEntry(const CWDestination& address) const;
    std::string GetNameForAddressBookEntry(const CWDestination& address) const;
    Optional<AddressBook::CAddressBookData> GetAddressBookEntry(const CWDestination& address) const;

    void LoadAddressBookName(const CWDestination& dest, const std::string& strName);
    void LoadAddressBookPurpose(const CWDestination& dest, const std::string& strPurpose);

    unsigned int GetKeyPoolSize();
    unsigned int GetStakingKeyPoolSize();

    //! signify that a particular wallet feature is now used. this may change nWalletVersion and nWalletMaxVersion if those are lower
    bool SetMinVersion(enum WalletFeature, CWalletDB* pwalletdbIn = NULL, bool fExplicit = false);

    //! change which version we're allowed to upgrade to (note that this does not immediately imply upgrading to that format)
    bool SetMaxVersion(int nVersion);

    //! get the current wallet format (the oldest client version guaranteed to understand this wallet)
    int GetVersion();

    //! Get wallet transactions that conflict with given transaction (spend same outputs)
    std::set<uint256> GetConflicts(const uint256& txid) const;

    //! Flush wallet (bitdb flush)
    void Flush(bool shutdown=false);

    /* Mark a transaction (and it in-wallet descendants) as abandoned so its inputs may be respent. */
    bool AbandonTransaction(const uint256& hashTx);

    /* Initializes the wallet, returns a new CWallet instance or a null pointer in case of an error */
    static CWallet* CreateWalletFromFile(const std::string& name, const fs::path& path);

    /**
     * Wallet post-init setup
     * Gives the wallet a chance to register repetitive tasks and complete post-init tasks
     */
    void postInitProcess(CScheduler& scheduler);

    /**
     * Creates a wallet backup in strDest path
     */
    bool BackupWallet(const std::string& strDest);

    /**
     * Blocks until the wallet state is up-to-date to /at least/ the current
     * chain at the time this function is entered
     * Obviously holding cs_main/cs_wallet when going into this call may cause
     * deadlock
     */
    void BlockUntilSyncedToCurrentChain();

    /**
     * Address book entry changed.
     * @note called with lock cs_wallet held.
     */
    boost::signals2::signal<void(CWallet* wallet, const CWDestination& address, const std::string& label, bool isMine, const std::string& purpose, ChangeType status)> NotifyAddressBookChanged;

    /**
     * Wallet transaction added, removed or updated.
     * @note called with lock cs_wallet held.
     */
    boost::signals2::signal<void(CWallet* wallet, const uint256& hashTx, ChangeType status)> NotifyTransactionChanged;

    /** Show progress e.g. for rescan */
    boost::signals2::signal<void(const std::string& title, int nProgress)> ShowProgress;

    /** Watch-only address added */
    boost::signals2::signal<void(bool fHaveWatchOnly)> NotifyWatchonlyChanged;

    /** notify wallet file backed up */
    boost::signals2::signal<void (const bool& fSuccess, const std::string& filename)> NotifyWalletBacked;

    /** notify stake-split threshold changed */
    boost::signals2::signal<void (const CAmount stakeSplitThreshold)> NotifySSTChanged;
};

/** A key allocated from the key pool. */
class CReserveKey
{
protected:
    CWallet* pwallet;
    int64_t nIndex;
    bool internal{false};
    CPubKey vchPubKey;

public:
    CReserveKey(CWallet* pwalletIn)
    {
        nIndex = -1;
        pwallet = pwalletIn;
    }

    CReserveKey() = default;
    CReserveKey(const CReserveKey&) = delete;
    CReserveKey& operator=(const CReserveKey&) = delete;

    ~CReserveKey()
    {
        ReturnKey();
    }

    void ReturnKey();
    bool GetReservedKey(CPubKey& pubkey, bool internal = false);
    void KeepKey();
};

class COutput
{
public:
    const CWalletTx* tx;
    int i;
    int nDepth;

    /** Whether we have the private keys to spend this output */
    bool fSpendable;

    /** Whether we know how to spend this output, ignoring the lack of keys */
    bool fSolvable;

    /**
     * Whether this output is considered safe to spend. Unconfirmed transactions
     * from outside keys and unconfirmed replacement transactions are considered
     * unsafe and will not be used to fund new spending transactions.
     */
    bool fSafe;

    COutput(const CWalletTx *txIn, int iIn, int nDepthIn, bool fSpendableIn, bool fSolvableIn, bool fSafeIn) :
        tx(txIn), i(iIn), nDepth(nDepthIn), fSpendable(fSpendableIn), fSolvable(fSolvableIn), fSafe(fSafeIn)
    {}

    CAmount Value() const { return tx->tx->vout[i].nValue; }
    std::string ToString() const;
};

class CStakeableOutput : public COutput
{
public:
    const CBlockIndex* pindex{nullptr};

    CStakeableOutput(const CWalletTx* txIn, int iIn, int nDepthIn,
                     const CBlockIndex*& pindex);

};

/** RAII object to check and reserve a wallet rescan */
class WalletRescanReserver
{
private:
    CWalletRef m_wallet;
    bool m_could_reserve;
public:
    explicit WalletRescanReserver(CWalletRef w) : m_wallet(w), m_could_reserve(false) {}

    bool reserve()
    {
        assert(!m_could_reserve);
        std::lock_guard<std::mutex> lock(m_wallet->mutexScanning);
        if (m_wallet->fScanningWallet) {
            return false;
        }
        m_wallet->fScanningWallet = true;
        m_could_reserve = true;
        return true;
    }

    bool isReserved() const
    {
        return (m_could_reserve && m_wallet->fScanningWallet);
    }

    ~WalletRescanReserver()
    {
        std::lock_guard<std::mutex> lock(m_wallet->mutexScanning);
        if (m_could_reserve) {
            m_wallet->fScanningWallet = false;
        }
    }
};

#endif // PIVX_WALLET_H
