// Copyright (c) 2016-2020 The ZCash developers
// Copyright (c) 2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "sapling/noteencryption.h"

#include "sapling/prf.h"
#include "sapling/sapling_util.h"

#include <librustzcash.h>
#include <sodium.h>

#include <stdexcept>

#define NOTEENCRYPTION_CIPHER_KEYSIZE 32

void clamp_curve25519(unsigned char key[crypto_scalarmult_SCALARBYTES])
{
    key[0] &= 248;
    key[31] &= 127;
    key[31] |= 64;
}

void PRF_ock(
    unsigned char K[NOTEENCRYPTION_CIPHER_KEYSIZE],
    const uint256 &ovk,
    const uint256 &cv,
    const uint256 &cm,
    const uint256 &epk
)
{
    unsigned char block[128] = {};
    memcpy(block+0, ovk.begin(), 32);
    memcpy(block+32, cv.begin(), 32);
    memcpy(block+64, cm.begin(), 32);
    memcpy(block+96, epk.begin(), 32);

    unsigned char personalization[crypto_generichash_blake2b_PERSONALBYTES] = {};
    memcpy(personalization, "Zcash_Derive_ock", 16);

    if (crypto_generichash_blake2b_salt_personal(K, NOTEENCRYPTION_CIPHER_KEYSIZE,
                                                 block, 128,
                                                 nullptr, 0, // No key.
                                                 nullptr,    // No salt.
                                                 personalization
                                                ) != 0)
    {
        throw std::logic_error("hash function failure");
    }
}

void KDF_Sapling(
    unsigned char K[NOTEENCRYPTION_CIPHER_KEYSIZE],
    const uint256 &dhsecret,
    const uint256 &epk
)
{
    unsigned char block[64] = {};
    memcpy(block+0, dhsecret.begin(), 32);
    memcpy(block+32, epk.begin(), 32);

    unsigned char personalization[crypto_generichash_blake2b_PERSONALBYTES] = {};
    memcpy(personalization, "Zcash_SaplingKDF", 16);

    if (crypto_generichash_blake2b_salt_personal(K, NOTEENCRYPTION_CIPHER_KEYSIZE,
                                                 block, 64,
                                                 nullptr, 0, // No key.
                                                 nullptr,    // No salt.
                                                 personalization
                                                ) != 0)
    {
        throw std::logic_error("hash function failure");
    }
}

void KDF(unsigned char K[NOTEENCRYPTION_CIPHER_KEYSIZE],
    const uint256 &dhsecret,
    const uint256 &epk,
    const uint256 &pk_enc,
    const uint256 &hSig,
    unsigned char nonce
   )
{
    if (nonce == 0xff) {
        throw std::logic_error("no additional nonce space for KDF");
    }

    unsigned char block[128] = {};
    memcpy(block+0, hSig.begin(), 32);
    memcpy(block+32, dhsecret.begin(), 32);
    memcpy(block+64, epk.begin(), 32);
    memcpy(block+96, pk_enc.begin(), 32);

    unsigned char personalization[crypto_generichash_blake2b_PERSONALBYTES] = {};
    memcpy(personalization, "ZcashKDF", 8);
    memcpy(personalization+8, &nonce, 1);

    if (crypto_generichash_blake2b_salt_personal(K, NOTEENCRYPTION_CIPHER_KEYSIZE,
                                                 block, 128,
                                                 nullptr, 0, // No key.
                                                 nullptr,    // No salt.
                                                 personalization
                                                ) != 0)
    {
        throw std::logic_error("hash function failure");
    }
}

namespace libzcash {

boost::optional<SaplingNoteEncryption> SaplingNoteEncryption::FromDiversifier(diversifier_t d) {
    uint256 epk;
    uint256 esk;

    // Pick random esk
    librustzcash_sapling_generate_r(esk.begin());

    // Compute epk given the diversifier
    if (!librustzcash_sapling_ka_derivepublic(d.begin(), esk.begin(), epk.begin())) {
        return boost::none;
    }

    return SaplingNoteEncryption(epk, esk);
}

boost::optional<SaplingEncCiphertext> SaplingNoteEncryption::encrypt_to_recipient(
    const uint256 &pk_d,
    const SaplingEncPlaintext &message
)
{
    if (already_encrypted_enc) {
        throw std::logic_error("already encrypted to the recipient using this key");
    }

    uint256 dhsecret;

    if (!librustzcash_sapling_ka_agree(pk_d.begin(), esk.begin(), dhsecret.begin())) {
        return boost::none;
    }

    // Construct the symmetric key
    unsigned char K[NOTEENCRYPTION_CIPHER_KEYSIZE];
    KDF_Sapling(K, dhsecret, epk);

    // The nonce is zero because we never reuse keys
    unsigned char cipher_nonce[crypto_aead_chacha20poly1305_IETF_NPUBBYTES] = {};

    SaplingEncCiphertext ciphertext;

    crypto_aead_chacha20poly1305_ietf_encrypt(
        ciphertext.begin(), nullptr,
        message.begin(), ZC_SAPLING_ENCPLAINTEXT_SIZE,
        nullptr, 0, // no "additional data"
        nullptr, cipher_nonce, K
    );

    already_encrypted_enc = true;

    return ciphertext;
}

boost::optional<SaplingEncPlaintext> AttemptSaplingEncDecryption(
    const SaplingEncCiphertext &ciphertext,
    const uint256 &ivk,
    const uint256 &epk
)
{
    uint256 dhsecret;

    if (!librustzcash_sapling_ka_agree(epk.begin(), ivk.begin(), dhsecret.begin())) {
        return boost::none;
    }

    // Construct the symmetric key
    unsigned char K[NOTEENCRYPTION_CIPHER_KEYSIZE];
    KDF_Sapling(K, dhsecret, epk);

    // The nonce is zero because we never reuse keys
    unsigned char cipher_nonce[crypto_aead_chacha20poly1305_IETF_NPUBBYTES] = {};

    SaplingEncPlaintext plaintext;

    if (crypto_aead_chacha20poly1305_ietf_decrypt(
        plaintext.begin(), nullptr,
        nullptr,
        ciphertext.begin(), ZC_SAPLING_ENCCIPHERTEXT_SIZE,
        nullptr,
        0,
        cipher_nonce, K) != 0)
    {
        return boost::none;
    }

    return plaintext;
}

boost::optional<SaplingEncPlaintext> AttemptSaplingEncDecryption (
    const SaplingEncCiphertext &ciphertext,
    const uint256 &epk,
    const uint256 &esk,
    const uint256 &pk_d
)
{
    uint256 dhsecret;

    if (!librustzcash_sapling_ka_agree(pk_d.begin(), esk.begin(), dhsecret.begin())) {
        return boost::none;
    }

    // Construct the symmetric key
    unsigned char K[NOTEENCRYPTION_CIPHER_KEYSIZE];
    KDF_Sapling(K, dhsecret, epk);

    // The nonce is zero because we never reuse keys
    unsigned char cipher_nonce[crypto_aead_chacha20poly1305_IETF_NPUBBYTES] = {};

    SaplingEncPlaintext plaintext;

    if (crypto_aead_chacha20poly1305_ietf_decrypt(
        plaintext.begin(), nullptr,
        nullptr,
        ciphertext.begin(), ZC_SAPLING_ENCCIPHERTEXT_SIZE,
        nullptr,
        0,
        cipher_nonce, K) != 0)
    {
        return boost::none;
    }

    return plaintext;
}


SaplingOutCiphertext SaplingNoteEncryption::encrypt_to_ourselves(
    const uint256 &ovk,
    const uint256 &cv,
    const uint256 &cm,
    const SaplingOutPlaintext &message
)
{
    if (already_encrypted_out) {
        throw std::logic_error("already encrypted to the recipient using this key");
    }

    // Construct the symmetric key
    unsigned char K[NOTEENCRYPTION_CIPHER_KEYSIZE];
    PRF_ock(K, ovk, cv, cm, epk);

    // The nonce is zero because we never reuse keys
    unsigned char cipher_nonce[crypto_aead_chacha20poly1305_IETF_NPUBBYTES] = {};

    SaplingOutCiphertext ciphertext;

    crypto_aead_chacha20poly1305_ietf_encrypt(
        ciphertext.begin(), nullptr,
        message.begin(), ZC_SAPLING_OUTPLAINTEXT_SIZE,
        nullptr, 0, // no "additional data"
        nullptr, cipher_nonce, K
    );

    already_encrypted_out = true;

    return ciphertext;
}

boost::optional<SaplingOutPlaintext> AttemptSaplingOutDecryption(
    const SaplingOutCiphertext &ciphertext,
    const uint256 &ovk,
    const uint256 &cv,
    const uint256 &cm,
    const uint256 &epk
)
{
    // Construct the symmetric key
    unsigned char K[NOTEENCRYPTION_CIPHER_KEYSIZE];
    PRF_ock(K, ovk, cv, cm, epk);

    // The nonce is zero because we never reuse keys
    unsigned char cipher_nonce[crypto_aead_chacha20poly1305_IETF_NPUBBYTES] = {};

    SaplingOutPlaintext plaintext;

    if (crypto_aead_chacha20poly1305_ietf_decrypt(
        plaintext.begin(), nullptr,
        nullptr,
        ciphertext.begin(), ZC_SAPLING_OUTCIPHERTEXT_SIZE,
        nullptr,
        0,
        cipher_nonce, K) != 0)
    {
        return boost::none;
    }

    return plaintext;
}

}
