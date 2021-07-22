// Copyright (c) 2019 The Bitcoin Core developers
// Copyright (c) 2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_SCRIPTPUBKEYMAN_H
#define PIVX_SCRIPTPUBKEYMAN_H

#include "wallet/hdchain.h"
#include "wallet/wallet.h"
#include "wallet/walletdb.h"

//! Default for -keypool
static const unsigned int DEFAULT_KEYPOOL_SIZE = 100;
static const uint32_t BIP32_HARDENED_KEY_LIMIT = 0x80000000;

/*
 * A class implementing ScriptPubKeyMan manages some (or all) scriptPubKeys used in a wallet.
 * It contains the scripts and keys related to the scriptPubKeys it manages.
 * A ScriptPubKeyMan will be able to give out scriptPubKeys to be used, as well as marking
 * when a scriptPubKey has been used. It also handles when and how to store a scriptPubKey
 * and its related scripts and keys, including encryption.
 */
class ScriptPubKeyMan {

public:
    ScriptPubKeyMan(CWallet* parent) : wallet(parent) {}
    ~ScriptPubKeyMan() {};

    /* Set the HD chain model (chain child index counters) */
    void SetHDChain(CHDChain& chain, bool memonly);
    const CHDChain& GetHDChain() const { return hdChain; }

    /** Sets up the key generation stuff, i.e. generates new HD seeds and sets them as active.
      * Returns false if already setup or setup fails, true if setup is successful
      * Set force=true to make it re-setup if already setup, used for upgrades
      */
    bool SetupGeneration(bool newKeypool = true, bool force = false, bool memOnly = false);

    /** Upgrades the wallet to the specified version */
    bool Upgrade(const int prev_version, std::string& error);

    /* Returns true if the wallet can generate new keys */
    bool CanGenerateKeys();

    /* True is HD wallet is enabled */
    bool IsHDEnabled() const;

    /* Return the oldest key time */
    int64_t GetOldestKeyPoolTime();

    /* Count external keypool available keys */
    size_t KeypoolCountExternalKeys();

    /* Key pool size */
    unsigned int GetKeyPoolSize() const;

    /* Staking key pool size */
    unsigned int GetStakingKeyPoolSize() const;

    /* Whether the wallet has or not keys in the pool */
    bool CanGetAddresses(const uint8_t& type = HDChain::ChangeType::EXTERNAL);

    /* Generates a new HD seed (will not be activated) */
    CPubKey GenerateNewSeed();

    /* Derives a new HD seed (will not be activated) */
    CPubKey DeriveNewSeed(const CKey& key);

    /* Set the current HD seed (will reset the chain child index counters)
      Sets the seed's version based on the current wallet version (so the
      caller must ensure the current wallet version is correct before calling
      this function). */
    void SetHDSeed(const CPubKey& key, bool force = false, bool memOnly = false);

    //! Load a keypool entry
    void LoadKeyPool(int64_t nIndex, const CKeyPool &keypool);
    //! Key pool
    bool NewKeyPool();
    //! Update pre HD keys in db with the pre-split flag enabled.
    void MarkPreSplitKeys();

    /** Fills internal address pool. Use within ScriptPubKeyMan implementations should be used sparingly and only
      * when something from the address pool is removed, excluding GetNewDestination and GetReservedDestination.
      * External wallet code is primarily responsible for topping up prior to fetching new addresses
      */
    bool TopUp(unsigned int size = 0);

    //! Mark unused addresses as being used
    void MarkUnusedAddresses(const CScript& script);

    //! First wallet key time
    void UpdateTimeFirstKey(int64_t nCreateTime);
    //! Generate a new key
    CPubKey GenerateNewKey(CWalletDB& batch, const uint8_t& type = HDChain::ChangeType::EXTERNAL);


    //! Fetches a key from the keypool
    bool GetKeyFromPool(CPubKey &key, const uint8_t& changeType = HDChain::ChangeType::EXTERNAL);
    //! Reserve + fetch a key from the keypool
    bool GetReservedKey(const uint8_t& changeType, int64_t& index, CKeyPool& keypool);

    const std::map<CKeyID, int64_t>& GetAllReserveKeys() const { return m_pool_key_to_index; }
    /**
     * Reserves a key from the keypool and sets nIndex to its index
     *
     * @param[out] nIndex the index of the key in keypool
     * @param[out] keypool the keypool the key was drawn from, which could be the
     *     the pre-split pool if present, or the internal or external pool
     * @param fRequestedInternal true if the caller would like the key drawn
     *     from the internal keypool, false if external is preferred
     *
     * @return true if succeeded, false if failed due to empty keypool
     * @throws std::runtime_error if keypool read failed, key was invalid,
     *     was not found in the wallet, or was misclassified in the internal
     *     or external keypool
     */
    bool ReserveKeyFromKeyPool(int64_t& nIndex, CKeyPool& keypool, const uint8_t& type = HDChain::ChangeType::EXTERNAL);

    void KeepDestination(int64_t index);
    void ReturnDestination(int64_t index, const uint8_t& type, const CTxDestination&);

    // TODO: This is public for now but shouldn't be here.
    std::set<int64_t> set_pre_split_keypool;

private:
    /* Parent wallet */
    CWallet* wallet{nullptr};
    /* the HD chain data model (external/internal chain counters) */
    CHDChain hdChain;

    /* TODO: This has not been implemented yet.. */
    CWalletDB *encrypted_batch = nullptr;

    // Key pool maps
    std::set<int64_t> setInternalKeyPool;
    std::set<int64_t> setExternalKeyPool;
    std::set<int64_t> setStakingKeyPool;
    int64_t m_max_keypool_index = 0;
    std::map<CKeyID, int64_t> m_pool_key_to_index;
    // Tracks keypool indexes to CKeyIDs of keys that have been taken out of the keypool but may be returned to it
    std::map<int64_t, CKeyID> m_index_to_reserved_key;

    /* */
    bool AddKeyPubKeyInner(const CKey& key, const CPubKey &pubkey);

    //! Adds a key to the store, and saves it to disk.
    bool AddKeyPubKeyWithDB(CWalletDB &batch,const CKey& key, const CPubKey &pubkey);
    /* Complete me */
    void AddKeypoolPubkeyWithDB(const CPubKey& pubkey, const uint8_t& type, CWalletDB& batch);
    void GeneratePool(CWalletDB& batch, int64_t targetSize, const uint8_t& type);

    /* HD derive new child key (on internal or external chain) */
    void DeriveNewChildKey(CWalletDB &batch, CKeyMetadata& metadata, CKey& secret, const uint8_t& type = HDChain::ChangeType::EXTERNAL);

    /**
     * Marks all keys in the keypool up to and including reserve_key as used.
     */
    void MarkReserveKeysAsUsed(int64_t keypool_id);
};


#endif //PIVX_SCRIPTPUBKEYMAN_H
