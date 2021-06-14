// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PRIMITIVES_TRANSACTION_H
#define BITCOIN_PRIMITIVES_TRANSACTION_H

#include "amount.h"
#include "memusage.h"
#include "script/script.h"
#include "serialize.h"
#include "optional.h"
#include "uint256.h"

#include "sapling/sapling_transaction.h"

#include <atomic>
#include <list>

class CTransaction;

enum SigVersion
{
    SIGVERSION_BASE = 0,
    SIGVERSION_SAPLING = 1,
};

/** An outpoint - a combination of a transaction hash and an index n into its vout */
class BaseOutPoint
{
public:
    uint256 hash;
    uint32_t n;
    bool isTransparent{true};

    BaseOutPoint() { SetNull(); }
    BaseOutPoint(const uint256& hashIn, const uint32_t nIn, bool isTransparentIn = true) :
        hash(hashIn), n(nIn), isTransparent(isTransparentIn) { }

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(hash);
        READWRITE(n);
    }

    void SetNull() { hash.SetNull(); n = (uint32_t) -1; }
    bool IsNull() const { return (hash.IsNull() && n == (uint32_t) -1); }

    friend bool operator<(const BaseOutPoint& a, const BaseOutPoint& b)
    {
        return (a.hash < b.hash || (a.hash == b.hash && a.n < b.n));
    }

    friend bool operator==(const BaseOutPoint& a, const BaseOutPoint& b)
    {
        return (a.hash == b.hash && a.n == b.n);
    }

    friend bool operator!=(const BaseOutPoint& a, const BaseOutPoint& b)
    {
        return !(a == b);
    }

    std::string ToString() const;
    std::string ToStringShort() const;

    size_t DynamicMemoryUsage() const { return 0; }

};

/** An outpoint - a combination of a transaction hash and an index n into its vout */
class COutPoint : public BaseOutPoint
{
public:
    COutPoint() : BaseOutPoint() {};
    COutPoint(const uint256& hashIn, const uint32_t nIn) : BaseOutPoint(hashIn, nIn, true) {};
    std::string ToString() const;
};

/** An outpoint - a combination of a transaction hash and an index n into its sapling
 * output description (vShieldedOutput) */
class SaplingOutPoint : public BaseOutPoint
{
public:
    SaplingOutPoint() : BaseOutPoint() {};
    SaplingOutPoint(const uint256& hashIn, const uint32_t nIn) : BaseOutPoint(hashIn, nIn, false) {};
    std::string ToString() const;
};

/** An input of a transaction.  It contains the location of the previous
 * transaction's output that it claims and a signature that matches the
 * output's public key.
 */
class CTxIn
{
public:
    COutPoint prevout;
    CScript scriptSig;
    uint32_t nSequence;

    /* Setting nSequence to this value for every input in a transaction
     * disables nLockTime. */
    static const uint32_t SEQUENCE_FINAL = 0xffffffff;

    CTxIn() { nSequence = SEQUENCE_FINAL; }
    explicit CTxIn(COutPoint prevoutIn, CScript scriptSigIn=CScript(), uint32_t nSequenceIn=SEQUENCE_FINAL);
    CTxIn(uint256 hashPrevTx, uint32_t nOut, CScript scriptSigIn=CScript(), uint32_t nSequenceIn=SEQUENCE_FINAL);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(prevout);
        READWRITE(scriptSig);
        READWRITE(nSequence);
    }

    bool IsFinal() const { return nSequence == SEQUENCE_FINAL; }
    bool IsNull() const { return prevout.IsNull() && scriptSig.empty() && IsFinal(); }

    bool IsZerocoinSpend() const;
    bool IsZerocoinPublicSpend() const;

    friend bool operator==(const CTxIn& a, const CTxIn& b)
    {
        return (a.prevout   == b.prevout &&
                a.scriptSig == b.scriptSig &&
                a.nSequence == b.nSequence);
    }

    friend bool operator!=(const CTxIn& a, const CTxIn& b)
    {
        return !(a == b);
    }

    std::string ToString() const;

    size_t DynamicMemoryUsage() const { return scriptSig.DynamicMemoryUsage(); }
};

/** An output of a transaction.  It contains the public key that the next input
 * must be able to sign with to claim it.
 */
class CTxOut
{
public:
    CAmount nValue;
    CScript scriptPubKey;
    int nRounds;

    CTxOut()
    {
        SetNull();
    }

    CTxOut(const CAmount& nValueIn, CScript scriptPubKeyIn);

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action) {
        READWRITE(nValue);
        READWRITE(scriptPubKey);
    }

    void SetNull()
    {
        nValue = -1;
        scriptPubKey.clear();
        nRounds = -10; // an initial value, should be no way to get this by calculations
    }

    bool IsNull() const
    {
        return (nValue == -1);
    }

    void SetEmpty()
    {
        nValue = 0;
        scriptPubKey.clear();
    }

    bool IsEmpty() const
    {
        return (nValue == 0 && scriptPubKey.empty());
    }

    uint256 GetHash() const;
    bool GetKeyIDFromUTXO(CKeyID& keyIDRet) const;

    bool IsZerocoinMint() const;

    friend bool operator==(const CTxOut& a, const CTxOut& b)
    {
        return (a.nValue       == b.nValue &&
                a.scriptPubKey == b.scriptPubKey &&
                a.nRounds      == b.nRounds);
    }

    friend bool operator!=(const CTxOut& a, const CTxOut& b)
    {
        return !(a == b);
    }

    std::string ToString() const;

    size_t DynamicMemoryUsage() const { return scriptPubKey.DynamicMemoryUsage(); }
};

struct CMutableTransaction;

/**
 * Transaction serialization format:
 * - int32_t nVersion
 * - std::vector<CTxIn> vin
 * - std::vector<CTxOut> vout
 * - uint32_t nLockTime
 * - Optional<SaplingTxData> sapData
 * - Optional<std::vector<uint8_t>> extraPayload
 */
template<typename Stream, typename TxType>
inline void UnserializeTransaction(TxType& tx, Stream& s) {
    tx.vin.clear();
    tx.vout.clear();

    s >> tx.nVersion;
    s >> tx.nType;
    s >> tx.vin;
    s >> tx.vout;
    s >> tx.nLockTime;
    if (tx.isSaplingVersion()) {
        s >> tx.sapData;
        if (!tx.IsNormalType()) {
            s >> tx.extraPayload;
        }
    }
}

template<typename Stream, typename TxType>
inline void SerializeTransaction(const TxType& tx, Stream& s) {
    s << tx.nVersion;
    s << tx.nType;
    s << tx.vin;
    s << tx.vout;
    s << tx.nLockTime;
    if (tx.isSaplingVersion()) {
        s << tx.sapData;
        if (!tx.IsNormalType()) {
            s << tx.extraPayload;
        }
    }
}

/** The basic transaction that is broadcasted on the network and contained in
 * blocks. A transaction can contain multiple inputs and outputs.
 */
class CTransaction
{
public:
    /** Transaction Versions */
    enum TxVersion: int16_t {
        LEGACY      = 1,
        SAPLING     = 3,
        TOOHIGH
    };

    /** Transaction types */
    enum TxType: int16_t {
        NORMAL = 0,
        PROREG = 1,
        PROUPSERV = 2,
        PROUPREG = 3,
        PROUPREV = 4,
    };

    static const int16_t CURRENT_VERSION = TxVersion::LEGACY;

    // The local variables are made const to prevent unintended modification
    // without updating the cached hash value. However, CTransaction is not
    // actually immutable; deserialization and assignment are implemented,
    // and bypass the constness. This is safe, as they update the entire
    // structure, including the hash.
    const int16_t nVersion;
    const int16_t nType;
    std::vector<CTxIn> vin;
    std::vector<CTxOut> vout;
    const uint32_t nLockTime;
    Optional<SaplingTxData> sapData{SaplingTxData()}; // Future: Don't initialize it by default
    Optional<std::vector<uint8_t>> extraPayload{nullopt};     // only available for special transaction types

    /** Construct a CTransaction that qualifies as IsNull() */
    CTransaction();

    /** Convert a CMutableTransaction into a CTransaction. */
    CTransaction(const CMutableTransaction &tx);
    CTransaction(CMutableTransaction &&tx);

    CTransaction(const CTransaction& tx) = default;

    template <typename Stream>
    inline void Serialize(Stream& s) const {
        SerializeTransaction(*this, s);
    }

    /** This deserializing constructor is provided instead of an Unserialize method.
      *  Unserialize is not possible, since it would require overwriting const fields. */
    template <typename Stream>
    CTransaction(deserialize_type, Stream& s) : CTransaction(CMutableTransaction(deserialize, s)) {}

    bool IsNull() const {
        return vin.empty() && vout.empty();
    }

    const uint256& GetHash() const {
        return hash;
    }

    bool hasSaplingData() const
    {
        return sapData != nullopt &&
            (!sapData->vShieldedOutput.empty() ||
            !sapData->vShieldedSpend.empty() ||
            sapData->valueBalance != 0 ||
            sapData->hasBindingSig());
    };

    bool isSaplingVersion() const
    {
        return nVersion >= TxVersion::SAPLING;
    }

    bool IsShieldedTx() const
    {
        return isSaplingVersion() && hasSaplingData();
    }

    bool hasExtraPayload() const
    {
        return extraPayload != nullopt && !extraPayload->empty();
    }

    bool IsSpecialTx() const
    {
        return isSaplingVersion() && nType != TxType::NORMAL && hasExtraPayload();
    }

    bool IsNormalType() const { return nType == TxType::NORMAL; }

    bool IsProRegTx() const
    {
        return IsSpecialTx() && nType == TxType::PROREG;
    }

    // Ensure that special and sapling fields are signed
    SigVersion GetRequiredSigVersion() const
    {
        return isSaplingVersion() ? SIGVERSION_SAPLING : SIGVERSION_BASE;
    }

    /*
     * Context for the two methods below:
     * We can think of the Sapling shielded part of the transaction as an input
     * or output according to whether valueBalance - the sum of shielded input
     * values minus the sum of shielded output values - is positive or negative.
     */

    // Return sum of txouts.
    CAmount GetValueOut() const;
    // GetValueIn() is a method on CCoinsViewCache, because
    // inputs must be known to compute value in.

    // Return sum of (positive valueBalance or zero) and JoinSplit vpub_new
    CAmount GetShieldedValueIn() const;

    // Compute priority, given priority of inputs and (optionally) tx size
    double ComputePriority(double dPriorityInputs, unsigned int nTxSize=0) const;

    // Compute modified tx size for priority calculation (optionally given tx size)
    unsigned int CalculateModifiedSize(unsigned int nTxSize=0) const;

    bool HasZerocoinSpendInputs() const;
    bool HasZerocoinPublicSpendInputs() const;

    bool HasZerocoinMintOutputs() const;

    bool ContainsZerocoins() const
    {
        return HasZerocoinSpendInputs() || HasZerocoinMintOutputs();
    }

    CAmount GetZerocoinSpent() const;

    bool IsCoinBase() const
    {
        return (vin.size() == 1 && vin[0].prevout.IsNull() && !vin[0].scriptSig.IsZerocoinSpend());
    }

    bool IsCoinStake() const;
    bool HasP2CSOutputs() const;

    friend bool operator==(const CTransaction& a, const CTransaction& b)
    {
        return a.hash == b.hash;
    }

    friend bool operator!=(const CTransaction& a, const CTransaction& b)
    {
        return a.hash != b.hash;
    }

    unsigned int GetTotalSize() const;

    std::string ToString() const;

    size_t DynamicMemoryUsage() const;

private:
    /** Memory only. */
    const uint256 hash;
    uint256 ComputeHash() const;
};

/** A mutable version of CTransaction. */
struct CMutableTransaction
{
    int16_t nVersion;
    int16_t nType;
    std::vector<CTxIn> vin;
    std::vector<CTxOut> vout;
    uint32_t nLockTime;
    Optional<SaplingTxData> sapData{SaplingTxData()}; // Future: Don't initialize it by default
    Optional<std::vector<uint8_t>> extraPayload{nullopt};

    CMutableTransaction();
    CMutableTransaction(const CTransaction& tx);

    template <typename Stream>
    inline void Serialize(Stream& s) const {
        SerializeTransaction(*this, s);
    }

    template <typename Stream>
    inline void Unserialize(Stream& s) {
        UnserializeTransaction(*this, s);
    }

    template <typename Stream>
    CMutableTransaction(deserialize_type, Stream& s) {
        Unserialize(s);
    }

    bool isSaplingVersion() const { return nVersion >= CTransaction::TxVersion::SAPLING; }
    bool IsNormalType() const { return nType == CTransaction::TxType::NORMAL; }

    /** Compute the hash of this CMutableTransaction. This is computed on the
     * fly, as opposed to GetHash() in CTransaction, which uses a cached result.
     */
    uint256 GetHash() const;

    bool hasExtraPayload() const
    {
        return extraPayload != nullopt && !extraPayload->empty();
    }

    // Ensure that special and sapling fields are signed
    SigVersion GetRequiredSigVersion() const
    {
        return isSaplingVersion() ? SIGVERSION_SAPLING : SIGVERSION_BASE;
    }
};

typedef std::shared_ptr<const CTransaction> CTransactionRef;
static inline CTransactionRef MakeTransactionRef() { return std::make_shared<const CTransaction>(); }
template <typename Tx> static inline CTransactionRef MakeTransactionRef(Tx&& txIn) { return std::make_shared<const CTransaction>(std::forward<Tx>(txIn)); }
static inline CTransactionRef MakeTransactionRef(const CTransactionRef& txIn) { return txIn; }
static inline CTransactionRef MakeTransactionRef(CTransactionRef&& txIn) { return std::move(txIn); }

#endif // BITCOIN_PRIMITIVES_TRANSACTION_H
