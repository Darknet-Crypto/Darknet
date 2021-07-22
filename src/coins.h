// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2016-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_COINS_H
#define BITCOIN_COINS_H

#include "compressor.h"
#include "consensus/consensus.h" // can be removed once policy/ established
#include "memusage.h"
#include "sapling/incrementalmerkletree.h"
#include "script/standard.h"
#include "serialize.h"
#include "uint256.h"

#include <assert.h>
#include <stdint.h>

#include <unordered_map>

/**
 * A UTXO entry.
 *
 * Serialized format:
 * - VARINT((coinbase ? 2 : 0) | (coinstake ? 1 : 0) | (height << 2))
 * - the non-spent CTxOut (via CTxOutCompressor)
 */
class Coin
{
public:
    //! whether the containing transaction was a coinbase
    bool fCoinBase;

    //! whether the containing transaction was a coinstake
    bool fCoinStake;

    //! unspent transaction output
    CTxOut out;

    //! at which height the containing transaction was included in the active block chain
    uint32_t nHeight;

    //! construct a Coin from a CTxOut and height/coinbase properties.
    Coin(CTxOut&& outIn, int nHeightIn, bool fCoinBaseIn, bool fCoinStakeIn) : fCoinBase(fCoinBaseIn), fCoinStake(fCoinStakeIn), out(std::move(outIn)), nHeight(nHeightIn) {}
    Coin(const CTxOut& outIn, int nHeightIn, bool fCoinBaseIn, bool fCoinStakeIn) : fCoinBase(fCoinBaseIn), fCoinStake(fCoinStakeIn), out(outIn), nHeight(nHeightIn) {}

    void Clear() {
        out.SetNull();
        fCoinBase = false;
        fCoinStake = false;
        nHeight = 0;
    }

    //! empty constructor
    Coin() : fCoinBase(false), fCoinStake(false), nHeight(0) { }

    bool IsCoinBase() const {
        return fCoinBase;
    }

    bool IsCoinStake() const {
        return fCoinStake;
    }

    template<typename Stream>
    void Serialize(Stream &s) const {
        assert(!IsSpent());
        uint32_t code = nHeight * 4 + (fCoinBase ? 2 : 0) + (fCoinStake ? 1 : 0);
        ::Serialize(s, VARINT(code));
        ::Serialize(s, Using<TxOutCompression>(out));
    }

    template<typename Stream>
    void Unserialize(Stream &s) {
        uint32_t code = 0;
        ::Unserialize(s, VARINT(code));
        nHeight = code >> 2;
        fCoinBase = code & 2;
        fCoinStake = code & 1;
        ::Unserialize(s, Using<TxOutCompression>(out));
    }

    bool IsSpent() const {
        return out.IsNull();
    }

    size_t DynamicMemoryUsage() const {
        return memusage::DynamicUsage(out.scriptPubKey);
    }
};

class SaltedOutpointHasher
{
private:
    /** Salt */
    const uint64_t k0, k1;

public:
    SaltedOutpointHasher();

    /**
     * This *must* return size_t. With Boost 1.46 on 32-bit systems the
     * unordered_map will behave unpredictably if the custom hasher returns a
     * uint64_t, resulting in failures when syncing the chain (#4634).
     */
    size_t operator()(const COutPoint& id) const {
        return SipHashUint256Extra(k0, k1, id.hash, id.n);
    }
};

// Used on Sapling nullifiers, anchor maps and txmempool::mapTx: sorted by txid
class SaltedIdHasher
{
private:
    /** Salt */
    const uint64_t k0, k1;

public:
    SaltedIdHasher();

    /**
     * This *must* return size_t. With Boost 1.46 on 32-bit systems the
     * unordered_map will behave unpredictably if the custom hasher returns a
     * uint64_t, resulting in failures when syncing the chain (#4634).
     */
    size_t operator()(const uint256& txid) const {
        return SipHashUint256(k0, k1, txid);
    }
};

struct CCoinsCacheEntry {
    Coin coin; // The actual cached data.
    unsigned char flags;

    enum Flags {
        DIRTY = (1 << 0), // This cache entry is potentially different from the version in the parent view.
        FRESH = (1 << 1), // The parent view does not have this entry (or it is pruned).
    };

    CCoinsCacheEntry() : flags(0) {}
    explicit CCoinsCacheEntry(Coin&& coin_) : coin(std::move(coin_)), flags(0) {}
};

// Sapling

struct CAnchorsSaplingCacheEntry
{
    bool entered; // This will be false if the anchor is removed from the cache
    SaplingMerkleTree tree; // The tree itself
    unsigned char flags;

    enum Flags {
        DIRTY = (1 << 0), // This cache entry is potentially different from the version in the parent view.
    };

    CAnchorsSaplingCacheEntry() : entered(false), flags(0) {}
};

struct CNullifiersCacheEntry
{
    bool entered; // If the nullifier is spent or not
    unsigned char flags;

    enum Flags {
        DIRTY = (1 << 0), // This cache entry is potentially different from the version in the parent view.
    };

    CNullifiersCacheEntry() : entered(false), flags(0) {}
};

typedef std::unordered_map<uint256, CAnchorsSaplingCacheEntry, SaltedIdHasher> CAnchorsSaplingMap;
typedef std::unordered_map<uint256, CNullifiersCacheEntry, SaltedIdHasher> CNullifiersMap;

typedef std::unordered_map<COutPoint, CCoinsCacheEntry, SaltedOutpointHasher> CCoinsMap;

/** Cursor for iterating over CoinsView state */
class CCoinsViewCursor
{
public:
    CCoinsViewCursor(const uint256 &hashBlockIn): hashBlock(hashBlockIn) {}
    virtual ~CCoinsViewCursor() {}

    virtual bool GetKey(COutPoint& key) const = 0;
    virtual bool GetValue(Coin& coin) const = 0;
    /* Don't care about GetKeySize here */
    virtual unsigned int GetValueSize() const = 0;

    virtual bool Valid() const = 0;
    virtual void Next() = 0;

    //! Get best block at the time this cursor was created
    const uint256 &GetBestBlock() const { return hashBlock; }
private:
    uint256 hashBlock;
};

/** Abstract view on the open txout dataset. */
class CCoinsView
{
public:
    //! Retrieve the Coin (unspent transaction output) for a given outpoint.
    virtual bool GetCoin(const COutPoint& outpoint, Coin& coin) const;

    //! Just check whether we have data for a given outpoint.
    //! This may (but cannot always) return true for spent outputs.
    virtual bool HaveCoin(const COutPoint& outpoint) const;

    //! Retrieve the block hash whose state this CCoinsView currently represents
    virtual uint256 GetBestBlock() const;

    //! Retrieve the range of blocks that may have been only partially written.
    //! If the database is in a consistent state, the result is the empty vector.
    //! Otherwise, a two-element vector is returned consisting of the new and
    //! the old block hash, in that order.
    virtual std::vector<uint256> GetHeadBlocks() const;

    //! Do a bulk modification (multiple Coin changes + BestBlock change).
    //! The passed mapCoins can be modified.
    virtual bool BatchWrite(CCoinsMap& mapCoins,
                            const uint256& hashBlock,
                            const uint256& hashSaplingAnchor,
                            CAnchorsSaplingMap& mapSaplingAnchors,
                            CNullifiersMap& mapSaplingNullifiers);

    //! Get a cursor to iterate over the whole state
    virtual CCoinsViewCursor* Cursor() const;

    //! As we use CCoinsViews polymorphically, have a virtual destructor
    virtual ~CCoinsView() {}

    //! Estimate database size (0 if not implemented)
    virtual size_t EstimateSize() const { return 0; }

    // Sapling
    //! Retrieve the tree (Sapling) at a particular anchored root in the chain
    virtual bool GetSaplingAnchorAt(const uint256 &rt, SaplingMerkleTree &tree) const;

    //! Determine whether a nullifier is spent or not
    virtual bool GetNullifier(const uint256 &nullifier) const;

    //! Get the current "tip" or the latest anchored tree root in the chain
    virtual uint256 GetBestAnchor() const;
};


/** CCoinsView backed by another CCoinsView */
class CCoinsViewBacked : public CCoinsView
{
protected:
    CCoinsView* base;

public:
    CCoinsViewBacked(CCoinsView* viewIn);
    bool GetCoin(const COutPoint& outpoint, Coin& coin) const override;
    bool HaveCoin(const COutPoint& outpoint) const override;
    uint256 GetBestBlock() const override;
    std::vector<uint256> GetHeadBlocks() const override;
    void SetBackend(CCoinsView& viewIn);
    CCoinsViewCursor* Cursor() const override;
    size_t EstimateSize() const override;

    bool BatchWrite(CCoinsMap& mapCoins,
                    const uint256& hashBlock,
                    const uint256& hashSaplingAnchor,
                    CAnchorsSaplingMap& mapSaplingAnchors,
                    CNullifiersMap& mapSaplingNullifiers) override;

    // Sapling
    bool GetSaplingAnchorAt(const uint256 &rt, SaplingMerkleTree &tree) const override;
    bool GetNullifier(const uint256 &nullifier) const override;
    uint256 GetBestAnchor() const override;
};


/** Used as the flags parameter to sequence and nLocktime checks in non-consensus code. */
static const unsigned int STANDARD_LOCKTIME_VERIFY_FLAGS = LOCKTIME_VERIFY_SEQUENCE |
                                                           LOCKTIME_MEDIAN_TIME_PAST;

/** CCoinsView that adds a memory cache for transactions to another CCoinsView */
class CCoinsViewCache : public CCoinsViewBacked
{
protected:
    /**
     * Make mutable so that we can "fill the cache" even from Get-methods
     * declared as "const".
     */
    mutable uint256 hashBlock;
    mutable CCoinsMap cacheCoins;

    // Sapling
    mutable uint256 hashSaplingAnchor;
    mutable CAnchorsSaplingMap cacheSaplingAnchors;
    mutable CNullifiersMap cacheSaplingNullifiers;

    /* Cached dynamic memory usage for the inner Coin objects. */
    mutable size_t cachedCoinsUsage;

public:
    CCoinsViewCache(CCoinsView *baseIn);

    /**
     * By deleting the copy constructor, we prevent accidentally using it when one intends to create a cache on top of a base cache.
     */
    CCoinsViewCache(const CCoinsViewCache &) = delete;

    // Sapling methods
    bool GetSaplingAnchorAt(const uint256 &rt, SaplingMerkleTree &tree) const override;
    bool GetNullifier(const uint256 &nullifier) const override;
    uint256 GetBestAnchor() const override;

    // Adds the tree to mapSaplingAnchors
    // and sets the current commitment root to this root.
    template<typename Tree> void PushAnchor(const Tree &tree);

    // Removes the current commitment root from mapAnchors and sets
    // the new current root.
    void PopAnchor(const uint256 &rt);

    // Marks nullifiers for a given transaction as spent or not.
    void SetNullifiers(const CTransaction& tx, bool spent);

    //! Check whether all sapling spend requirements (anchors/nullifiers) are satisfied
    bool HaveShieldedRequirements(const CTransaction& tx) const;

    // Standard CCoinsView methods
    bool GetCoin(const COutPoint& outpoint, Coin& coin) const override;
    bool HaveCoin(const COutPoint& outpoint) const override;
    uint256 GetBestBlock() const override;
    void SetBestBlock(const uint256& hashBlock);

    bool BatchWrite(CCoinsMap& mapCoins,
                    const uint256& hashBlock,
                    const uint256& hashSaplingAnchor,
                    CAnchorsSaplingMap& mapSaplingAnchors,
                    CNullifiersMap& mapSaplingNullifiers) override;

    /**
     * Check if we have the given utxo already loaded in this cache.
     * The semantics are the same as HaveCoin(), but no calls to
     * the backing CCoinsView are made.
     */
    bool HaveCoinInCache(const COutPoint& outpoint) const;

    /**
     * Return a reference to a Coin in the cache, or a pruned one if not found. This is
     * more efficient than GetCoin. Modifications to other cache entries are
     * allowed while accessing the returned pointer.
     */
    const Coin& AccessCoin(const COutPoint& output) const;

    /**
     * Add a coin. Set potential_overwrite to true if a non-pruned version may
     * already exist.
     */
    void AddCoin(const COutPoint& outpoint, Coin&& coin, bool potential_overwrite);

    /**
     * Spend a coin. Pass moveto in order to get the deleted data.
     * If no unspent output exists for the passed outpoint, this call
     * has no effect.
     */
    void SpendCoin(const COutPoint &outpoint, Coin* moveto = nullptr);

    /**
     * Push the modifications applied to this cache to its base.
     * Failure to call this method before destruction will cause the changes to be forgotten.
     * If false is returned, the state of this cache (and its backing view) will be undefined.
     */
    bool Flush();

    /**
     * Removes the UTXO with the given outpoint from the cache, if it is not modified.
     */
    void Uncache(const COutPoint &outpoint);

    //! Calculate the size of the cache (in number of transaction outputs)
    unsigned int GetCacheSize() const;

    //! Calculate the size of the cache (in bytes)
    size_t DynamicMemoryUsage() const;

    /**
     * Amount of pivx coming in to a transaction
     * Note that lightweight clients may not know anything besides the hash of previous transactions,
     * so may not be able to calculate this.
     *
     * @param[in] tx	transaction for which we are checking input total
     * @return	Sum of value of all inputs (scriptSigs)
     */
    CAmount GetValueIn(const CTransaction& tx) const;

    //! Check whether all prevouts of the transaction are present in the UTXO set represented by this view
    bool HaveInputs(const CTransaction& tx) const;

    /*
     * Return the depth of a coin at height nHeight, or -1 if not found
     */
    int GetCoinDepthAtHeight(const COutPoint& output, int nHeight) const;

    /*
     * Return the sum of the value of all transaction outputs
     */
    CAmount GetTotalAmount() const;

    /*
     * Prune zerocoin mints and frozen outputs - do it once, after initialization
     */
    bool PruneInvalidEntries();


private:
    CCoinsMap::iterator FetchCoin(const COutPoint& outpoint) const;

    //! Generalized interface for popping anchors
    template<typename Tree, typename Cache, typename CacheEntry>
    void AbstractPopAnchor(
            const uint256 &newrt,
            Cache &cacheAnchors,
            uint256 &hash
    );

    //! Generalized interface for pushing anchors
    template<typename Tree, typename Cache, typename CacheIterator, typename CacheEntry>
    void AbstractPushAnchor(
            const Tree &tree,
            Cache &cacheAnchors,
            uint256 &hash
    );

    //! Interface for bringing an anchor into the cache.
    template<typename Tree>
    void BringBestAnchorIntoCache(
            const uint256 &currentRoot,
            Tree &tree
    );

};

//! Utility function to add all of a transaction's outputs to a cache.
// PIVX: When check is false, this assumes that overwrites are never possible due to BIP34 always in effect
// When check is true, the underlying view may be queried to determine whether an addition is
// an overwrite.
// When fSkipInvalid is true, the invalid_out list is checked before adding the coin.
void AddCoins(CCoinsViewCache& cache, const CTransaction& tx, int nHeight, bool check = false, bool fSkipInvalid = false);

//! Utility function to find any unspent output with a given txid.
const Coin& AccessByTxid(const CCoinsViewCache& cache, const uint256& txid);

#endif // BITCOIN_COINS_H
