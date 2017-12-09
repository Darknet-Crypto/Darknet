// Copyright (c) 2009-2013 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "crypter.h"

#include "script/script.h"
#include "script/standard.h"
#include "util.h"
#include "primitives/zerocoin.h"
#include "libzerocoin/bignum.h"
#include "init.h"

#include <boost/foreach.hpp>
#include <openssl/aes.h>
#include <openssl/evp.h>
#include <string>
#include <vector>

bool CCrypter::SetKeyFromPassphrase(const SecureString& strKeyData, const std::vector<unsigned char>& chSalt, const unsigned int nRounds, const unsigned int nDerivationMethod)
{
    if (nRounds < 1 || chSalt.size() != WALLET_CRYPTO_SALT_SIZE)
        return false;

    int i = 0;
    if (nDerivationMethod == 0)
        i = EVP_BytesToKey(EVP_aes_256_cbc(), EVP_sha512(), &chSalt[0],
            (unsigned char*)&strKeyData[0], strKeyData.size(), nRounds, chKey, chIV);

    if (i != (int)WALLET_CRYPTO_KEY_SIZE) {
        OPENSSL_cleanse(chKey, sizeof(chKey));
        OPENSSL_cleanse(chIV, sizeof(chIV));
        return false;
    }

    fKeySet = true;
    return true;
}

bool CCrypter::SetKey(const CKeyingMaterial& chNewKey, const std::vector<unsigned char>& chNewIV)
{
    if (chNewKey.size() != WALLET_CRYPTO_KEY_SIZE || chNewIV.size() != WALLET_CRYPTO_KEY_SIZE)
        return false;

    memcpy(&chKey[0], &chNewKey[0], sizeof chKey);
    memcpy(&chIV[0], &chNewIV[0], sizeof chIV);

    fKeySet = true;
    return true;
}

bool CCrypter::Encrypt(const CKeyingMaterial& vchPlaintext, std::vector<unsigned char>& vchCiphertext)
{
    if (!fKeySet)
        return false;

    // max ciphertext len for a n bytes of plaintext is
    // n + AES_BLOCK_SIZE - 1 bytes
    int nLen = vchPlaintext.size();
    int nCLen = nLen + AES_BLOCK_SIZE, nFLen = 0;
    vchCiphertext = std::vector<unsigned char>(nCLen);

    EVP_CIPHER_CTX ctx;

    bool fOk = true;

    EVP_CIPHER_CTX_init(&ctx);
    if (fOk) fOk = EVP_EncryptInit_ex(&ctx, EVP_aes_256_cbc(), NULL, chKey, chIV) != 0;
    if (fOk) fOk = EVP_EncryptUpdate(&ctx, &vchCiphertext[0], &nCLen, &vchPlaintext[0], nLen) != 0;
    if (fOk) fOk = EVP_EncryptFinal_ex(&ctx, (&vchCiphertext[0]) + nCLen, &nFLen) != 0;
    EVP_CIPHER_CTX_cleanup(&ctx);

    if (!fOk) return false;

    vchCiphertext.resize(nCLen + nFLen);
    return true;
}

bool CCrypter::Decrypt(const std::vector<unsigned char>& vchCiphertext, CKeyingMaterial& vchPlaintext)
{
    if (!fKeySet)
        return false;

    // plaintext will always be equal to or lesser than length of ciphertext
    int nLen = vchCiphertext.size();
    int nPLen = nLen, nFLen = 0;

    vchPlaintext = CKeyingMaterial(nPLen);

    EVP_CIPHER_CTX ctx;

    bool fOk = true;

    EVP_CIPHER_CTX_init(&ctx);
    if (fOk) fOk = EVP_DecryptInit_ex(&ctx, EVP_aes_256_cbc(), NULL, chKey, chIV) != 0;
    if (fOk) fOk = EVP_DecryptUpdate(&ctx, &vchPlaintext[0], &nPLen, &vchCiphertext[0], nLen) != 0;
    if (fOk) fOk = EVP_DecryptFinal_ex(&ctx, (&vchPlaintext[0]) + nPLen, &nFLen) != 0;
    EVP_CIPHER_CTX_cleanup(&ctx);

    if (!fOk) return false;

    vchPlaintext.resize(nPLen + nFLen);
    return true;
}


bool EncryptSecret(const CKeyingMaterial& vMasterKey, const CKeyingMaterial& vchPlaintext, const uint256& nIV, std::vector<unsigned char>& vchCiphertext)
{
    CCrypter cKeyCrypter;
    std::vector<unsigned char> chIV(WALLET_CRYPTO_KEY_SIZE);
    memcpy(&chIV[0], &nIV, WALLET_CRYPTO_KEY_SIZE);
    if (!cKeyCrypter.SetKey(vMasterKey, chIV))
        return false;
    return cKeyCrypter.Encrypt(*((const CKeyingMaterial*)&vchPlaintext), vchCiphertext);
}


bool CCrypter::CryptZerocoinMint(const CZerocoinMint &mintIn, CZerocoinMint& mintOut, CryptionMethod method)
{
    LogPrintf("inside zercoin crypt xyz123");
    if (!fKeySet)
        return false;

    //already encrypted
    if(mintIn.IsCrypted() && method == ENC) {
        mintOut = mintIn;
        return true;
    }

    //already decryted
    if(!mintIn.IsCrypted() && method == DEC) {
        mintOut = mintIn;
        return true;
    }

    vector< vector<unsigned char> > vchSecrets;

    vchSecrets[SERIAL] = mintIn.GetSerialNumber().getvch();
    vchSecrets[RANDOM] = mintIn.GetRandomness().getvch();

    for(auto& secret : vchSecrets) {
        int nSecretLength = (int) secret.size();
        int nCipherLength = nSecretLength + AES_BLOCK_SIZE, nFLen = 0;

        vector<unsigned char> secretCiphered;

        EVP_CIPHER_CTX ctx;

        bool fOk = true;
        switch(method) {
            case CryptionMethod::ENC:
                EVP_CIPHER_CTX_init(&ctx);
                if (fOk) fOk = EVP_EncryptInit_ex(&ctx, EVP_aes_256_cbc(), NULL, chKey, chIV) != 0;
                if (fOk) fOk = EVP_EncryptUpdate(&ctx, &secretCiphered[0], &nCipherLength, &secret[0], nSecretLength) != 0;
                if (fOk) fOk = EVP_EncryptFinal_ex(&ctx, (&secretCiphered[0]) + nSecretLength, &nFLen) != 0;
                EVP_CIPHER_CTX_cleanup(&ctx);
                break;
            case CryptionMethod::DEC:
                EVP_CIPHER_CTX_init(&ctx);
                if (fOk) fOk = EVP_DecryptInit_ex(&ctx, EVP_aes_256_cbc(), NULL, chKey, chIV) != 0;
                if (fOk) fOk = EVP_DecryptUpdate(&ctx, &secretCiphered[0], &nCipherLength, &secret[0], nSecretLength) != 0;
                if (fOk) fOk = EVP_DecryptFinal_ex(&ctx, (&secretCiphered[0]) + nCipherLength, &nFLen) != 0;
                EVP_CIPHER_CTX_cleanup(&ctx);
                break;
        }
        if (!fOk) return false;
    }

    mintOut = CZerocoinMint(mintIn);
    mintOut.SetSerialNumber(CBigNum(vchSecrets[SERIAL]));
    mintOut.SetRandomness(CBigNum(vchSecrets[RANDOM]));
    method == ENC ? mintOut.SetIsCrypted(true) : mintOut.SetIsCrypted(false);

    return true;
}


bool CCrypter::EncryptZerocoinMint(const CZerocoinMint& mintPlain, CZerocoinMint& mintCrypted)
{
    if (!fKeySet)
        return false;

    vector< vector<unsigned char> > vchSecrets;

    vchSecrets[ZerocoinSecrets::SERIAL] = mintPlain.GetSerialNumber().getvch();
    vchSecrets[ZerocoinSecrets::RANDOM] = mintPlain.GetRandomness().getvch();

    for(auto& plaintext : vchSecrets) {
        // max ciphertext len for a n bytes of plaintext is
        // n + AES_BLOCK_SIZE - 1 bytes
        int nLen = plaintext.size();
        int nCLen = nLen + AES_BLOCK_SIZE, nFLen = 0;
        std::vector<unsigned char> vchCiphertext(nCLen);

        EVP_CIPHER_CTX ctx;

        bool fOk = true;

        EVP_CIPHER_CTX_init(&ctx);
        if (fOk) fOk = EVP_EncryptInit_ex(&ctx, EVP_aes_256_cbc(), NULL, chKey, chIV) != 0;
        if (fOk) fOk = EVP_EncryptUpdate(&ctx, &vchCiphertext[0], &nCLen, &plaintext[0], nLen) != 0;
        if (fOk) fOk = EVP_EncryptFinal_ex(&ctx, (&vchCiphertext[0]) + nCLen, &nFLen) != 0;
        EVP_CIPHER_CTX_cleanup(&ctx);

        if (!fOk) return false;

        vchCiphertext.resize(nCLen + nFLen);
    }

    mintCrypted = CZerocoinMint(mintPlain);
    mintCrypted.SetSerialNumber(CBigNum(vchSecrets[ZerocoinSecrets::SERIAL]));
    mintCrypted.SetRandomness(CBigNum(vchSecrets[ZerocoinSecrets::RANDOM]));
    return true;
}

bool CCrypter::DecryptZerocoinMint(const CZerocoinMint &mintCrypted, CZerocoinMint &mintPlain)
{
    if (!fKeySet)
        return false;

    vector< vector<unsigned char> > vchSecrets;

    vchSecrets[ZerocoinSecrets::SERIAL] = mintCrypted.GetSerialNumber().getvch();
    vchSecrets[ZerocoinSecrets::RANDOM] = mintCrypted.GetRandomness().getvch();

    for(auto& crypted : vchSecrets) {
        // plaintext will always be equal to or lesser than length of ciphertext
        int nLen = crypted.size();
        int nPLen = nLen, nFLen = 0;

        std::vector<unsigned char> vchPlaintext(nPLen);

        EVP_CIPHER_CTX ctx;

        bool fOk = true;

        EVP_CIPHER_CTX_init(&ctx);
        if (fOk) fOk = EVP_DecryptInit_ex(&ctx, EVP_aes_256_cbc(), NULL, chKey, chIV) != 0;
        if (fOk) fOk = EVP_DecryptUpdate(&ctx, &vchPlaintext[0], &nPLen, &crypted[0], nLen) != 0;
        if (fOk) fOk = EVP_DecryptFinal_ex(&ctx, (&vchPlaintext[0]) + nPLen, &nFLen) != 0;
        EVP_CIPHER_CTX_cleanup(&ctx);

        if (!fOk) return false;

        vchPlaintext.resize(nPLen + nFLen);
    }

    mintPlain = CZerocoinMint(mintCrypted);
    mintPlain.SetSerialNumber(CBigNum(vchSecrets[ZerocoinSecrets::SERIAL]));
    mintPlain.SetRandomness(CBigNum(vchSecrets[ZerocoinSecrets::RANDOM]));

    return true;
}



// General secure AES 256 CBC encryption routine
bool EncryptAES256(const SecureString& sKey, const SecureString& sPlaintext, const std::string& sIV, std::string& sCiphertext)
{
    // max ciphertext len for a n bytes of plaintext is
    // n + AES_BLOCK_SIZE - 1 bytes
    int nLen = sPlaintext.size();
    int nCLen = nLen + AES_BLOCK_SIZE;
    int nFLen = 0;

    // Verify key sizes
    if (sKey.size() != 32 || sIV.size() != AES_BLOCK_SIZE) {
        LogPrintf("crypter EncryptAES256 - Invalid key or block size: Key: %d sIV:%d\n", sKey.size(), sIV.size());
        return false;
    }

    // Prepare output buffer
    sCiphertext.resize(nCLen);

    // Perform the encryption
    EVP_CIPHER_CTX ctx;

    bool fOk = true;

    EVP_CIPHER_CTX_init(&ctx);
    if (fOk) fOk = EVP_EncryptInit_ex(&ctx, EVP_aes_256_cbc(), NULL, (const unsigned char*)&sKey[0], (const unsigned char*)&sIV[0]);
    if (fOk) fOk = EVP_EncryptUpdate(&ctx, (unsigned char*)&sCiphertext[0], &nCLen, (const unsigned char*)&sPlaintext[0], nLen);
    if (fOk) fOk = EVP_EncryptFinal_ex(&ctx, (unsigned char*)(&sCiphertext[0]) + nCLen, &nFLen);
    EVP_CIPHER_CTX_cleanup(&ctx);

    if (!fOk) return false;

    sCiphertext.resize(nCLen + nFLen);
    return true;
}


bool DecryptSecret(const CKeyingMaterial& vMasterKey, const std::vector<unsigned char>& vchCiphertext, const uint256& nIV, CKeyingMaterial& vchPlaintext)
{
    CCrypter cKeyCrypter;
    std::vector<unsigned char> chIV(WALLET_CRYPTO_KEY_SIZE);
    memcpy(&chIV[0], &nIV, WALLET_CRYPTO_KEY_SIZE);
    if (!cKeyCrypter.SetKey(vMasterKey, chIV))
        return false;
    return cKeyCrypter.Decrypt(vchCiphertext, *((CKeyingMaterial*)&vchPlaintext));
}

bool DecryptAES256(const SecureString& sKey, const std::string& sCiphertext, const std::string& sIV, SecureString& sPlaintext)
{
    // plaintext will always be equal to or lesser than length of ciphertext
    int nLen = sCiphertext.size();
    int nPLen = nLen, nFLen = 0;

    // Verify key sizes
    if (sKey.size() != 32 || sIV.size() != AES_BLOCK_SIZE) {
        LogPrintf("crypter DecryptAES256 - Invalid key or block size\n");
        return false;
    }

    sPlaintext.resize(nPLen);

    EVP_CIPHER_CTX ctx;

    bool fOk = true;

    EVP_CIPHER_CTX_init(&ctx);
    if (fOk) fOk = EVP_DecryptInit_ex(&ctx, EVP_aes_256_cbc(), NULL, (const unsigned char*)&sKey[0], (const unsigned char*)&sIV[0]);
    if (fOk) fOk = EVP_DecryptUpdate(&ctx, (unsigned char*)&sPlaintext[0], &nPLen, (const unsigned char*)&sCiphertext[0], nLen);
    if (fOk) fOk = EVP_DecryptFinal_ex(&ctx, (unsigned char*)(&sPlaintext[0]) + nPLen, &nFLen);
    EVP_CIPHER_CTX_cleanup(&ctx);

    if (!fOk) return false;

    sPlaintext.resize(nPLen + nFLen);
    return true;
}


bool CCryptoKeyStore::SetCrypted()
{
    LOCK(cs_KeyStore);
    if (fUseCrypto)
        return true;
    if (!mapKeys.empty())
        return false;
    fUseCrypto = true;
    return true;
}

bool CCryptoKeyStore::Lock()
{
    if (!SetCrypted())
        return false;

    {
        LOCK(cs_KeyStore);
        vMasterKey.clear();
    }

    NotifyStatusChanged(this);
    return true;
}

bool CCryptoKeyStore::Unlock(const CKeyingMaterial& vMasterKeyIn)
{
    {
        LOCK(cs_KeyStore);
        if (!SetCrypted())
            return false;

        bool keyPass = false;
        bool keyFail = false;
        CryptedKeyMap::const_iterator mi = mapCryptedKeys.begin();
        for (; mi != mapCryptedKeys.end(); ++mi) {
            const CPubKey& vchPubKey = (*mi).second.first;
            const std::vector<unsigned char>& vchCryptedSecret = (*mi).second.second;
            CKeyingMaterial vchSecret;
            if (!DecryptSecret(vMasterKeyIn, vchCryptedSecret, vchPubKey.GetHash(), vchSecret)) {
                keyFail = true;
                break;
            }
            if (vchSecret.size() != 32) {
                keyFail = true;
                break;
            }
            CKey key;
            key.Set(vchSecret.begin(), vchSecret.end(), vchPubKey.IsCompressed());
            if (key.GetPubKey() != vchPubKey) {
                keyFail = true;
                break;
            }
            keyPass = true;
            if (fDecryptionThoroughlyChecked)
                break;
        }
        if (keyPass && keyFail) {
            LogPrintf("The wallet is probably corrupted: Some keys decrypt but not all.");
            assert(false);
        }
        if (keyFail || !keyPass)
            return false;
        vMasterKey = vMasterKeyIn;
        fDecryptionThoroughlyChecked = true;
    }
    NotifyStatusChanged(this);
    return true;
}

bool CCryptoKeyStore::AddKeyPubKey(const CKey& key, const CPubKey& pubkey)
{
    {
        LOCK(cs_KeyStore);
        if (!IsCrypted())
            return CBasicKeyStore::AddKeyPubKey(key, pubkey);

        if (IsLocked())
            return false;

        std::vector<unsigned char> vchCryptedSecret;
        CKeyingMaterial vchSecret(key.begin(), key.end());
        if (!EncryptSecret(vMasterKey, vchSecret, pubkey.GetHash(), vchCryptedSecret))
            return false;

        if (!AddCryptedKey(pubkey, vchCryptedSecret))
            return false;
    }
    return true;
}


bool CCryptoKeyStore::AddCryptedKey(const CPubKey& vchPubKey, const std::vector<unsigned char>& vchCryptedSecret)
{
    {
        LOCK(cs_KeyStore);
        if (!SetCrypted())
            return false;

        mapCryptedKeys[vchPubKey.GetID()] = make_pair(vchPubKey, vchCryptedSecret);
    }
    return true;
}

bool CCryptoKeyStore::GetKey(const CKeyID& address, CKey& keyOut) const
{
    {
        LOCK(cs_KeyStore);
        if (!IsCrypted())
            return CBasicKeyStore::GetKey(address, keyOut);

        CryptedKeyMap::const_iterator mi = mapCryptedKeys.find(address);
        if (mi != mapCryptedKeys.end()) {
            const CPubKey& vchPubKey = (*mi).second.first;
            const std::vector<unsigned char>& vchCryptedSecret = (*mi).second.second;
            CKeyingMaterial vchSecret;
            if (!DecryptSecret(vMasterKey, vchCryptedSecret, vchPubKey.GetHash(), vchSecret))
                return false;
            if (vchSecret.size() != 32)
                return false;
            keyOut.Set(vchSecret.begin(), vchSecret.end(), vchPubKey.IsCompressed());
            return true;
        }
    }
    return false;
}

bool CCryptoKeyStore::GetPubKey(const CKeyID& address, CPubKey& vchPubKeyOut) const
{
    {
        LOCK(cs_KeyStore);
        if (!IsCrypted())
            return CKeyStore::GetPubKey(address, vchPubKeyOut);

        CryptedKeyMap::const_iterator mi = mapCryptedKeys.find(address);
        if (mi != mapCryptedKeys.end()) {
            vchPubKeyOut = (*mi).second.first;
            return true;
        }
    }
    return false;
}

bool CCryptoKeyStore::EncryptKeys(CKeyingMaterial& vMasterKeyIn)
{
    {
        LOCK(cs_KeyStore);
        if (!mapCryptedKeys.empty() || IsCrypted())
            return false;

        fUseCrypto = true;
        BOOST_FOREACH (KeyMap::value_type& mKey, mapKeys) {
            const CKey& key = mKey.second;
            CPubKey vchPubKey = key.GetPubKey();
            CKeyingMaterial vchSecret(key.begin(), key.end());
            std::vector<unsigned char> vchCryptedSecret;
            if (!EncryptSecret(vMasterKeyIn, vchSecret, vchPubKey.GetHash(), vchCryptedSecret))
                return false;
            if (!AddCryptedKey(vchPubKey, vchCryptedSecret))
                return false;
        }
        mapKeys.clear();
    }
    return true;
}
