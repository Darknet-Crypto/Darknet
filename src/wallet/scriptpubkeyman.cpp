// Copyright (c) 2019 The Bitcoin Core developers
// Copyright (c) 2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "wallet/scriptpubkeyman.h"
#include "crypter.h"
#include "script/standard.h"

bool ScriptPubKeyMan::SetupGeneration(bool newKeypool, bool force, bool memOnly)
{
    if (CanGenerateKeys() && !force) {
        return false;
    }

    SetHDSeed(GenerateNewSeed(), force, memOnly);
    if (newKeypool && !NewKeyPool()) {
        return false;
    }
    return true;
}

bool ScriptPubKeyMan::Upgrade(const int prev_version, std::string& error)
{
    LOCK(wallet->cs_KeyStore);
    error = "";
    bool hd_upgrade = false;
    if (!IsHDEnabled()) {
        LogPrintf("Upgrading wallet to use HD chain split\n");
        wallet->SetMinVersion(FEATURE_PRE_SPLIT_KEYPOOL);

        // generate a new master key
        CPubKey masterPubKey = GenerateNewSeed();
        SetHDSeed(masterPubKey);
        hd_upgrade = true;
    }

    // Regenerate the keypool if upgraded to HD
    if (hd_upgrade) {
        // Mark all keys currently in the keypool as pre-split
        MarkPreSplitKeys();

        // Fill keypool
        if (!TopUp()) {
            error = _("Unable to generate keys");
            return false;
        }
    }
    return true;
}

bool ScriptPubKeyMan::CanGenerateKeys()
{
    // A wallet can generate keys if it has an HD seed (IsHDEnabled) or it is a non-HD wallet (pre FEATURE_HD)
    LOCK(wallet->cs_wallet);
    return IsHDEnabled() || wallet->GetVersion() < FEATURE_PRE_SPLIT_KEYPOOL;
}

bool ScriptPubKeyMan::IsHDEnabled() const
{
    return !hdChain.IsNull();
}

bool ScriptPubKeyMan::CanGetAddresses(const uint8_t& type)
{
    LOCK(wallet->cs_wallet);
    // Check if the keypool has keys
    const bool isHDEnabled = IsHDEnabled();
    bool keypool_has_keys = false;
    if (isHDEnabled && type == HDChain::ChangeType::INTERNAL) {
        keypool_has_keys = setInternalKeyPool.size() > 0;
    } else if (isHDEnabled && type == HDChain::ChangeType::STAKING) {
        keypool_has_keys = setStakingKeyPool.size() > 0;
    } else {
        // either external key was requested or HD is not enabled
        keypool_has_keys = KeypoolCountExternalKeys() > 0;
    }
    // If the keypool doesn't have keys, check if we can generate them
    if (!keypool_has_keys) {
        return CanGenerateKeys();
    }
    return keypool_has_keys;
}

static int64_t GetOldestKeyTimeInPool(const std::set<int64_t>& setKeyPool, CWalletDB& batch) {
    if (setKeyPool.empty()) {
        return GetTime();
    }

    CKeyPool keypool;
    int64_t nIndex = *(setKeyPool.begin());
    if (!batch.ReadPool(nIndex, keypool)) {
        throw std::runtime_error(std::string(__func__) + ": read oldest key in keypool failed");
    }
    assert(keypool.vchPubKey.IsValid());
    return keypool.nTime;
}

int64_t ScriptPubKeyMan::GetOldestKeyPoolTime()
{
    LOCK(wallet->cs_KeyStore);

    CWalletDB batch(wallet->GetDBHandle());
    // load oldest key from keypool, get time and return
    int64_t oldestKey = GetOldestKeyTimeInPool(setExternalKeyPool, batch);
    if (IsHDEnabled()) {
        oldestKey = std::max(GetOldestKeyTimeInPool(setInternalKeyPool, batch), oldestKey);
        oldestKey = std::max(GetOldestKeyTimeInPool(setStakingKeyPool, batch), oldestKey);
        if (!set_pre_split_keypool.empty()) {
            oldestKey = std::max(GetOldestKeyTimeInPool(set_pre_split_keypool, batch), oldestKey);
        }
    }

    return oldestKey;
}

size_t ScriptPubKeyMan::KeypoolCountExternalKeys()
{
    AssertLockHeld(wallet->cs_wallet);
    return setExternalKeyPool.size() + set_pre_split_keypool.size();
}

unsigned int ScriptPubKeyMan::GetKeyPoolSize() const
{
    AssertLockHeld(wallet->cs_wallet);
    return setInternalKeyPool.size() + setExternalKeyPool.size() + set_pre_split_keypool.size();
}

unsigned int ScriptPubKeyMan::GetStakingKeyPoolSize() const
{
    AssertLockHeld(wallet->cs_wallet);
    return setStakingKeyPool.size();
}

bool ScriptPubKeyMan::GetKeyFromPool(CPubKey& result, const uint8_t& changeType)
{
    if (!CanGetAddresses(changeType)) {
        LogPrintf("%s: Cannot get address, type: %d\n", __func__ ,changeType);
        return false;
    }

    CKeyPool keypool;
    {
        LOCK(wallet->cs_wallet);
        int64_t nIndex;
        if (!ReserveKeyFromKeyPool(nIndex, keypool, changeType)) {
            if (wallet->IsLocked()) {
                LogPrintf("%s: Wallet locked, cannot get address\n", __func__);
                return false;
            }
            CWalletDB batch(wallet->GetDBHandle());
            result = GenerateNewKey(batch, changeType);
            return true;
        }
        KeepDestination(nIndex);
        result = keypool.vchPubKey;
    }
    return true;
}

bool ScriptPubKeyMan::GetReservedKey(const uint8_t& changeType, int64_t& index, CKeyPool& keypool)
{
    if (!CanGetAddresses(changeType)) {
        return error("%s : Cannot get address for change type %d", __func__, changeType);
    }
    if (!ReserveKeyFromKeyPool(index, keypool, changeType)) {
        return error("%s : Cannot reserve key from pool for change type %d", __func__, changeType);
    }
    return true;
}

bool ScriptPubKeyMan::ReserveKeyFromKeyPool(int64_t& nIndex, CKeyPool& keypool, const uint8_t& type)
{
    nIndex = -1;
    keypool.vchPubKey = CPubKey();
    {
        LOCK(wallet->cs_wallet);

        bool isHDEnabled = IsHDEnabled();
        bool fReturningInternal = type == HDChain::ChangeType::INTERNAL && isHDEnabled;
        bool fReturningStaking = type == HDChain::ChangeType::STAKING && isHDEnabled;
        bool use_split_keypool = set_pre_split_keypool.empty();
        std::set<int64_t>& setKeyPool = use_split_keypool ?
                ( fReturningInternal ? setInternalKeyPool : (fReturningStaking ? setStakingKeyPool : setExternalKeyPool) ) : set_pre_split_keypool;

        // Get the oldest key
        if (setKeyPool.empty()) {
            return false;
        }

        CWalletDB batch(wallet->GetDBHandle());

        auto it = setKeyPool.begin();
        nIndex = *it;
        setKeyPool.erase(it);
        if (!batch.ReadPool(nIndex, keypool)) {
            throw std::runtime_error(std::string(__func__) + ": read failed");
        }
        CPubKey pk;
        if (!wallet->GetPubKey(keypool.vchPubKey.GetID(), pk)) {
            throw std::runtime_error(std::string(__func__) + ": unknown key in key pool");
        }
        // If the key was pre-split keypool, we don't care about what type it is
        if (use_split_keypool && keypool.IsInternal() != fReturningInternal) {
            throw std::runtime_error(std::string(__func__) + ": keypool internal entry misclassified");
        }

        if (use_split_keypool && keypool.IsStaking() != fReturningStaking) {
            throw std::runtime_error(std::string(__func__) + ": keypool staking entry misclassified");
        }

        if (!keypool.vchPubKey.IsValid()) {
            throw std::runtime_error(std::string(__func__) + ": keypool entry invalid");
        }

        assert(m_index_to_reserved_key.count(nIndex) == 0);
        m_index_to_reserved_key[nIndex] = keypool.vchPubKey.GetID();
        m_pool_key_to_index.erase(keypool.vchPubKey.GetID());
        LogPrintf("%s: keypool reserve %d\n", __func__, nIndex);
    }
    //NotifyCanGetAddressesChanged();
    return true;
}

void ScriptPubKeyMan::KeepDestination(int64_t nIndex)
{
    // Remove from key pool
    CWalletDB batch(wallet->GetDBHandle());
    batch.ErasePool(nIndex);
    CPubKey pubkey;
    bool have_pk = wallet->GetPubKey(m_index_to_reserved_key.at(nIndex), pubkey);
    assert(have_pk);
    m_index_to_reserved_key.erase(nIndex);
    LogPrintf("keypool keep %d\n", nIndex);
}

void ScriptPubKeyMan::ReturnDestination(int64_t nIndex, const uint8_t& type, const CTxDestination&)
{
    // Return to key pool
    {
        LOCK(wallet->cs_wallet);
        const bool isHDEnabled = IsHDEnabled();
        if (isHDEnabled && type == HDChain::ChangeType::INTERNAL) {
            setInternalKeyPool.insert(nIndex);
        } else if (isHDEnabled && type == HDChain::ChangeType::STAKING) {
            setStakingKeyPool.insert(nIndex);
        } else if (isHDEnabled && !set_pre_split_keypool.empty()) {
            set_pre_split_keypool.insert(nIndex);
        } else {
            setExternalKeyPool.insert(nIndex);
        }
        CKeyID& pubkey_id = m_index_to_reserved_key.at(nIndex);
        m_pool_key_to_index[pubkey_id] = nIndex;
        m_index_to_reserved_key.erase(nIndex);
        //NotifyCanGetAddressesChanged();
    }
    LogPrintf("%s: keypool return %d\n", __func__, nIndex);
}

void ScriptPubKeyMan::MarkReserveKeysAsUsed(int64_t keypool_id)
{
    AssertLockHeld(wallet->cs_wallet);
    bool internal = setInternalKeyPool.count(keypool_id);
    bool staking = setStakingKeyPool.count(keypool_id);
    if (!internal) assert(setExternalKeyPool.count(keypool_id) || set_pre_split_keypool.count(keypool_id) || staking);
    std::set<int64_t> *setKeyPool = internal ? &setInternalKeyPool : (set_pre_split_keypool.empty() ?
            (staking ? &setStakingKeyPool : &setExternalKeyPool) : &set_pre_split_keypool);
    auto it = setKeyPool->begin();

    CWalletDB batch(wallet->GetDBHandle());
    while (it != std::end(*setKeyPool)) {
        const int64_t& index = *(it);
        if (index > keypool_id) break; // set*KeyPool is ordered

        CKeyPool keypool;
        if (batch.ReadPool(index, keypool)) {
            const CKeyID& keyid = keypool.vchPubKey.GetID();
            m_pool_key_to_index.erase(keyid);
            // add missing receive addresses to the AddressBook
            if (!internal && !wallet->HasAddressBook(keyid)) {
                wallet->SetAddressBook(keyid, "", staking ? AddressBook::AddressBookPurpose::COLD_STAKING
                                                          : AddressBook::AddressBookPurpose::RECEIVE);
            }
        }
        batch.ErasePool(index);
        LogPrintf("keypool index %d removed\n", index);
        it = setKeyPool->erase(it);
    }
}

void ScriptPubKeyMan::MarkUnusedAddresses(const CScript& script)
{
    AssertLockHeld(wallet->cs_wallet);
    // extract addresses and check if they match with an unused keypool key
    for (const auto& keyid : wallet->GetAffectedKeys(script)) {
        std::map<CKeyID, int64_t>::const_iterator mi = m_pool_key_to_index.find(keyid);
        if (mi != m_pool_key_to_index.end()) {
            LogPrintf("%s: Detected a used keypool key, mark all keypool key up to this key as used\n", __func__);
            MarkReserveKeysAsUsed(mi->second);

            if (!TopUp()) {
                LogPrintf("%s: Topping up keypool failed (locked wallet)\n", __func__);
            }
        }
    }
}

void ScriptPubKeyMan::MarkPreSplitKeys()
{
    CWalletDB batch(wallet->GetDBHandle());
    for (auto it = setExternalKeyPool.begin(); it != setExternalKeyPool.end();) {
        int64_t index = *it;
        CKeyPool keypool;
        if (!batch.ReadPool(index, keypool)) {
            throw std::runtime_error(std::string(__func__) + ": read keypool entry failed");
        }
        keypool.m_pre_split = true;
        if (!batch.WritePool(index, keypool)) {
            throw std::runtime_error(std::string(__func__) + ": writing modified keypool entry failed");
        }
        set_pre_split_keypool.insert(index);
        it = setExternalKeyPool.erase(it);
    }
}

/**
 * Mark old keypool keys as used,
 * and generate all new keys
 */
bool ScriptPubKeyMan::NewKeyPool()
{
    {
        LOCK(wallet->cs_wallet);

        CWalletDB walletdb(wallet->GetDBHandle());
        // Internal
        for (const int64_t nIndex : setInternalKeyPool) {
            walletdb.ErasePool(nIndex);
        }
        setInternalKeyPool.clear();

        // External
        for (const int64_t nIndex : setExternalKeyPool) {
            walletdb.ErasePool(nIndex);
        }
        setExternalKeyPool.clear();

        // Staking
        for (const int64_t nIndex : setStakingKeyPool) {
            walletdb.ErasePool(nIndex);
        }
        setStakingKeyPool.clear();

        // key -> index.
        m_pool_key_to_index.clear();

        if (!TopUp()) {
            return false;
        }
        LogPrintf("ScriptPubKeyMan::NewKeyPool rewrote keypool\n");
    }
    return true;
}

/**
 * Fill the key pool
 */
bool ScriptPubKeyMan::TopUp(unsigned int kpSize)
{
    if (!CanGenerateKeys()) {
        return false;
    }
    {
        LOCK(wallet->cs_wallet);
        if (wallet->IsLocked()) return false;

        // Top up key pool
        unsigned int nTargetSize;
        if (kpSize > 0)
            nTargetSize = kpSize;
        else
            nTargetSize = std::max(gArgs.GetArg("-keypool", DEFAULT_KEYPOOL_SIZE), (int64_t) 0);

        // Count amount of available keys (internal, external)
        // make sure the keypool of external and internal keys fits the user selected target (-keypool)
        int64_t missingExternal = std::max(std::max((int64_t) nTargetSize, (int64_t) 1) - (int64_t)setExternalKeyPool.size(), (int64_t) 0);
        int64_t missingInternal = std::max(std::max((int64_t) nTargetSize, (int64_t) 1) - (int64_t)setInternalKeyPool.size(), (int64_t) 0);
        int64_t missingStaking = std::max(std::max((int64_t) nTargetSize, (int64_t) 1) - (int64_t)setStakingKeyPool.size(), (int64_t) 0);

        if (!IsHDEnabled()) {
            // don't create extra internal or staking keys
            missingInternal = 0;
            missingStaking = 0;
        }

        CWalletDB batch(wallet->GetDBHandle());
        GeneratePool(batch, missingExternal, HDChain::ChangeType::EXTERNAL);
        GeneratePool(batch, missingInternal, HDChain::ChangeType::INTERNAL);
        GeneratePool(batch, missingStaking, HDChain::ChangeType::STAKING);

        if (missingInternal + missingExternal > 0) {
            LogPrintf("keypool added %d keys (%d internal), size=%u (%u internal), \n", missingInternal + missingExternal, missingInternal, setInternalKeyPool.size() + setExternalKeyPool.size() + set_pre_split_keypool.size(), setInternalKeyPool.size());
        }
        if (missingStaking > 0) {
            LogPrintf("keypool added %d staking keys\n", setStakingKeyPool.size());
        }
    }
    // TODO: Implement this.
    //NotifyCanGetAddressesChanged();
    return true;
}

void ScriptPubKeyMan::GeneratePool(CWalletDB& batch, int64_t targetSize, const uint8_t& type)
{
    for (int64_t i = targetSize; i--;) {
        CPubKey pubkey(GenerateNewKey(batch, type));
        AddKeypoolPubkeyWithDB(pubkey, type, batch);
    }
}

void ScriptPubKeyMan::AddKeypoolPubkeyWithDB(const CPubKey& pubkey, const uint8_t& type, CWalletDB &batch)
{
    LOCK(wallet->cs_wallet);
    assert(m_max_keypool_index < std::numeric_limits<int64_t>::max()); // How in the hell did you use so many keys?
    int64_t index = ++m_max_keypool_index;
    if (!batch.WritePool(index, CKeyPool(pubkey, type))) {
        throw std::runtime_error(std::string(__func__) + ": writing imported pubkey failed");
    }

    const bool isHDEnabled = IsHDEnabled();
    if (isHDEnabled && type == HDChain::ChangeType::INTERNAL) {
        setInternalKeyPool.insert(index);
    } else if (isHDEnabled && type == HDChain::ChangeType::STAKING) {
        setStakingKeyPool.insert(index);
    } else {
        setExternalKeyPool.insert(index);
    }

    m_pool_key_to_index[pubkey.GetID()] = index;
}

/**
 * Generate a new key and stores it in db.
 */
CPubKey ScriptPubKeyMan::GenerateNewKey(CWalletDB &batch, const uint8_t& type)
{
    AssertLockHeld(wallet->cs_wallet);
    bool fCompressed = wallet->CanSupportFeature(FEATURE_COMPRPUBKEY); // default to compressed public keys if we want 0.6.0 wallets

    CKey secret;

    // Create new metadata
    int64_t nCreationTime = GetTime();
    CKeyMetadata metadata(nCreationTime);

    // use HD key derivation if HD was enabled during wallet creation and a seed is present
    if (IsHDEnabled()) {
        DeriveNewChildKey(batch, metadata, secret, type);
    } else {
        secret.MakeNewKey(fCompressed);
    }

    // Compressed public keys were introduced in version 0.6.0
    if (fCompressed) {
        wallet->SetMinVersion(FEATURE_COMPRPUBKEY);
    }

    CPubKey pubkey = secret.GetPubKey();
    assert(secret.VerifyPubKey(pubkey));

    wallet->mapKeyMetadata[pubkey.GetID()] = metadata;
    UpdateTimeFirstKey(nCreationTime);

    if (!AddKeyPubKeyWithDB(batch, secret, pubkey)) {
        throw std::runtime_error(std::string(__func__) + ": AddKey failed");
    }
    return pubkey;
}

void ScriptPubKeyMan::DeriveNewChildKey(CWalletDB &batch, CKeyMetadata& metadata, CKey& secret, const uint8_t& changeType)
{
    AssertLockHeld(wallet->cs_wallet);
    // Use BIP44 keypath scheme i.e. m / purpose' / coin_type' / account' / change / address_index
    CKey seed;                     //seed (256bit)
    CExtKey masterKey;             //hd master key
    CExtKey purposeKey;            //key at m/purpose' --> key at m/44'
    CExtKey cointypeKey;           //key at m/purpose'/coin_type'  --> key at m/44'/119'
    CExtKey accountKey;            //key at m/purpose'/coin_type'/account' ---> key at m/44'/119'/account_num'
    CExtKey changeKey;             //key at m/purpose'/coin_type'/account'/change ---> key at m/44'/119'/account_num'/change', external = 0' or internal = 1'.
    CExtKey childKey;              //key at m/purpose'/coin_type'/account'/change/address_index ---> key at m/44'/119'/account_num'/change'/<n>'

    // For now only one account.
    int nAccountNumber = 0;

    // try to get the seed
    if (!wallet->GetKey(hdChain.GetID(), seed))
        throw std::runtime_error(std::string(__func__) + ": seed not found");

    masterKey.SetSeed(seed.begin(), seed.size());

    // derive m/0'
    // use hardened derivation (child keys >= 0x80000000 are hardened after bip32)
    masterKey.Derive(purposeKey, 44 | BIP32_HARDENED_KEY_LIMIT);
    // derive m/purpose'/coin_type'
    purposeKey.Derive(cointypeKey, 119 | BIP32_HARDENED_KEY_LIMIT);
    // derive m/purpose'/coin_type'/account' // Hardcoded to account 0 for now.
    cointypeKey.Derive(accountKey, nAccountNumber | BIP32_HARDENED_KEY_LIMIT);
    // derive m/purpose'/coin_type'/account'/change'
    accountKey.Derive(changeKey,  changeType | BIP32_HARDENED_KEY_LIMIT);

    // derive child key at next index, skip keys already known to the wallet
    do {
        // always derive hardened keys
        // childIndex | BIP32_HARDENED_KEY_LIMIT = derive childIndex in hardened child-index-range
        // example: 1 | BIP32_HARDENED_KEY_LIMIT == 0x80000001 == 2147483649

        // m/44'/119'/account_num/change'/<n>'
        metadata.key_origin.path.push_back(44 | BIP32_HARDENED_KEY_LIMIT);
        metadata.key_origin.path.push_back(119 | BIP32_HARDENED_KEY_LIMIT);
        metadata.key_origin.path.push_back(nAccountNumber | BIP32_HARDENED_KEY_LIMIT);
        // Child chain counter
        uint32_t& chainCounter = hdChain.GetChainCounter(changeType);
        changeKey.Derive(childKey, chainCounter | BIP32_HARDENED_KEY_LIMIT);
        metadata.key_origin.path.push_back( changeType | BIP32_HARDENED_KEY_LIMIT);
        metadata.key_origin.path.push_back(chainCounter | BIP32_HARDENED_KEY_LIMIT);
        chainCounter++;

    } while (wallet->HaveKey(childKey.key.GetPubKey().GetID()));

    secret = childKey.key;
    metadata.hd_seed_id = hdChain.GetID();
    CKeyID master_id = masterKey.key.GetPubKey().GetID();
    std::copy(master_id.begin(), master_id.begin() + 4, metadata.key_origin.fingerprint);
    // update the chain model in the database
    if (!batch.WriteHDChain(hdChain))
        throw std::runtime_error(std::string(__func__) + ": Writing HD chain model failed");
}

void ScriptPubKeyMan::LoadKeyPool(int64_t nIndex, const CKeyPool &keypool)
{
    AssertLockHeld(wallet->cs_wallet);
    if (keypool.m_pre_split) {
        set_pre_split_keypool.insert(nIndex);
    } else if (keypool.IsInternal()) {
        setInternalKeyPool.insert(nIndex);
    } else if (keypool.IsExternal()){
        setExternalKeyPool.insert(nIndex);
    } else if (keypool.IsStaking()){
        setStakingKeyPool.insert(nIndex);
    } else {
        throw std::runtime_error(std::string(__func__) + ": invalid CKeypool type");
    }
    m_max_keypool_index = std::max(m_max_keypool_index, nIndex);
    m_pool_key_to_index[keypool.vchPubKey.GetID()] = nIndex;

    // If no metadata exists yet, create a default with the pool key's
    // creation time. Note that this may be overwritten by actually
    // stored metadata for that key later, which is fine.
    CKeyID keyid = keypool.vchPubKey.GetID();
    if (wallet->mapKeyMetadata.count(keyid) == 0)
        wallet->mapKeyMetadata[keyid] = CKeyMetadata(keypool.nTime);
}

/**
 * Update wallet first key creation time. This should be called whenever keys
 * are added to the wallet, with the oldest key creation time.
 */
void ScriptPubKeyMan::UpdateTimeFirstKey(int64_t nCreateTime)
{
    AssertLockHeld(wallet->cs_wallet);
    if (nCreateTime <= 1) {
        // Cannot determine birthday information, so set the wallet birthday to
        // the beginning of time.
        wallet->nTimeFirstKey = 1;
    } else if (!wallet->nTimeFirstKey || nCreateTime < wallet->nTimeFirstKey) {
        wallet->nTimeFirstKey = nCreateTime;
    }
}

bool ScriptPubKeyMan::AddKeyPubKeyWithDB(CWalletDB& batch, const CKey& secret, const CPubKey& pubkey)
{
    AssertLockHeld(wallet->cs_wallet);

    // Make sure we aren't adding private keys to private key disabled wallets
    //assert(!m_storage.IsWalletFlagSet(WALLET_FLAG_DISABLE_PRIVATE_KEYS));

    // FillableSigningProvider has no concept of wallet databases, but calls AddCryptedKey
    // which is overridden below.  To avoid flushes, the database handle is
    // tunneled through to it.
    bool needsDB = !encrypted_batch;
    if (needsDB) {
        encrypted_batch = &batch;
    }
    if (!AddKeyPubKeyInner(secret, pubkey)) {
        if (needsDB) encrypted_batch = nullptr;
        return false;
    }
    if (needsDB) encrypted_batch = nullptr;
    return true;
}

bool ScriptPubKeyMan::AddKeyPubKeyInner(const CKey& key, const CPubKey &pubkey)
{
    LOCK(wallet->cs_KeyStore);
    if (!wallet->HasEncryptionKeys()) {
        return wallet->AddKeyPubKey(key, pubkey);
    }

    if (wallet->IsLocked()) {
        return false;
    }

    std::vector<unsigned char> vchCryptedSecret;
    CKeyingMaterial vchSecret(key.begin(), key.end());
    if (!EncryptSecret(wallet->GetEncryptionKey(), vchSecret, pubkey.GetHash(), vchCryptedSecret)) {
        return false;
    }

    if (!wallet->AddCryptedKey(pubkey, vchCryptedSecret)) {
        return false;
    }
    return true;
}

////////////////////// Seed Generation ///////////////////////////////////

CPubKey ScriptPubKeyMan::GenerateNewSeed()
{
    CKey secret;
    secret.MakeNewKey(true);
    return DeriveNewSeed(secret);
}

CPubKey ScriptPubKeyMan::DeriveNewSeed(const CKey& key)
{
    int64_t nCreationTime = GetTime();
    CKeyMetadata metadata(nCreationTime);

    // Calculate the seed
    CPubKey seed = key.GetPubKey();
    assert(key.VerifyPubKey(seed));

    // Set seed id
    metadata.hd_seed_id = seed.GetID();

    {
        LOCK(wallet->cs_wallet);

        // mem store the metadata
        wallet->mapKeyMetadata[seed.GetID()] = metadata;

        // write the key&metadata to the database
        if (!wallet->AddKeyPubKey(key, seed))
            throw std::runtime_error(std::string(__func__) + ": AddKeyPubKey failed");
    }

    return seed;
}


//////////////////////////////////////////////////////////////////////

void ScriptPubKeyMan::SetHDSeed(const CPubKey& seed, bool force, bool memOnly)
{
    if (!hdChain.IsNull() && !force)
        throw std::runtime_error(std::string(__func__) + ": trying to set a hd seed on an already created chain");

    LOCK(wallet->cs_wallet);
    // store the keyid (hash160) together with
    // the child index counter in the database
    // as a hdChain object
    CHDChain newHdChain;
    if (!newHdChain.SetSeed(seed.GetID()) ) {
        throw std::runtime_error(std::string(__func__) + ": set hd seed failed");
    }

    SetHDChain(newHdChain, memOnly);
    // TODO: Connect this if is needed.
    //NotifyCanGetAddressesChanged();
}

void ScriptPubKeyMan::SetHDChain(CHDChain& chain, bool memonly)
{
    LOCK(wallet->cs_wallet);
    if (!memonly && !CWalletDB(wallet->GetDBHandle()).WriteHDChain(chain))
        throw std::runtime_error(std::string(__func__) + ": writing chain failed");

    hdChain = chain;

    // Sanity check
    if (!wallet->HaveKey(hdChain.GetID()))
        throw std::runtime_error(std::string(__func__) + ": Not found seed in wallet");
}
