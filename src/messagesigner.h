// Copyright (c) 2014-2018 The Dash Core developers
// Copyright (c) 2018-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MESSAGESIGNER_H
#define MESSAGESIGNER_H

#include "key.h"
#include "primitives/transaction.h" // for CTxIn

extern const std::string strMessageMagic;

enum MessageVersion {
        MESS_VER_STRMESS    = 0,
        MESS_VER_HASH       = 1,
};

/** Helper class for signing messages and checking their signatures
 */
class CMessageSigner
{
public:
    /// Set the private/public key values, returns true if successful
    static bool GetKeysFromSecret(const std::string& strSecret, CKey& keyRet, CPubKey& pubkeyRet);
    static bool GetKeysFromSecret(const std::string& strSecret, CKey& keyRet, CKeyID& keyIDRet);
    /// Get the hash based on the input message
    static uint256 GetMessageHash(const std::string& strMessage);
    /// Sign the message, returns true if successful
    static bool SignMessage(const std::string& strMessage, std::vector<unsigned char>& vchSigRet, const CKey& key);
    /// Verify the message signature, returns true if successful
    static bool VerifyMessage(const CPubKey& pubkey, const std::vector<unsigned char>& vchSig, const std::string& strMessage, std::string& strErrorRet);
    /// Verify the message signature, returns true if successful
    static bool VerifyMessage(const CKeyID& keyID, const std::vector<unsigned char>& vchSig, const std::string& strMessage, std::string& strErrorRet);
};

/** Helper class for signing hashes and checking their signatures
 */
class CHashSigner
{
public:
    /// Sign the hash, returns true if successful
    static bool SignHash(const uint256& hash, const CKey& key, std::vector<unsigned char>& vchSigRet);
    /// Verify the hash signature, returns true if successful
    static bool VerifyHash(const uint256& hash, const CPubKey& pubkey, const std::vector<unsigned char>& vchSig, std::string& strErrorRet);
    /// Verify the hash signature, returns true if successful
    static bool VerifyHash(const uint256& hash, const CKeyID& keyID, const std::vector<unsigned char>& vchSig, std::string& strErrorRet);
};

/** Base Class for all signed messages on the network
 */

class CBLSPublicKey;
class CBLSSecretKey;

class CSignedMessage
{
protected:
    std::vector<unsigned char> vchSig;

public:
    int nMessVersion;

    CSignedMessage() :
        vchSig(),
        nMessVersion(MessageVersion::MESS_VER_HASH)
    {}
    virtual ~CSignedMessage() {};

    // Sign-Verify message
    bool Sign(const CKey& key, const CKeyID& keyID);
    bool Sign(const std::string strSignKey);
    bool CheckSignature(const CKeyID& keyID) const;

    // Pure virtual functions (used in Sign-Verify functions)
    // Must be implemented in child classes
    virtual uint256 GetSignatureHash() const = 0;
    virtual std::string GetStrMessage() const = 0;

    // Setters and getters
    void SetVchSig(const std::vector<unsigned char>& vchSigIn) { vchSig = vchSigIn; }
    std::vector<unsigned char> GetVchSig() const { return vchSig; }
    std::string GetSignatureBase64() const;

    // Sign-Verify with BLS
    bool Sign(const CBLSSecretKey& sk);
    bool CheckSignature(const CBLSPublicKey& pk) const;
};

#endif
