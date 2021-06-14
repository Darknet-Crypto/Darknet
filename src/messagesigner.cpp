// Copyright (c) 2014-2018 The Dash Core developers
// Copyright (c) 2018-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "base58.h"
#include "bls/bls_wrapper.h"
#include "hash.h"
#include "messagesigner.h"
#include "tinyformat.h"
#include "util/system.h"
#include "utilstrencodings.h"

const std::string strMessageMagic = "DarkNet Signed Message:\n";

bool CMessageSigner::GetKeysFromSecret(const std::string& strSecret, CKey& keyRet, CPubKey& pubkeyRet)
{
    keyRet = DecodeSecret(strSecret);
    if (!keyRet.IsValid())
        return false;

    pubkeyRet = keyRet.GetPubKey();
    return pubkeyRet.IsValid();
}

bool CMessageSigner::GetKeysFromSecret(const std::string& strSecret, CKey& keyRet, CKeyID& keyIDRet)
{
    CPubKey pubkey;
    if (!GetKeysFromSecret(strSecret, keyRet, pubkey)) {
        return false;
    }
    keyIDRet = pubkey.GetID();
    return true;
}

uint256 CMessageSigner::GetMessageHash(const std::string& strMessage)
{
    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;
    return ss.GetHash();
}

bool CMessageSigner::SignMessage(const std::string& strMessage, std::vector<unsigned char>& vchSigRet, const CKey& key)
{
    return CHashSigner::SignHash(GetMessageHash(strMessage), key, vchSigRet);
}

bool CMessageSigner::VerifyMessage(const CPubKey& pubkey, const std::vector<unsigned char>& vchSig, const std::string& strMessage, std::string& strErrorRet)
{
    return VerifyMessage(pubkey.GetID(), vchSig, strMessage, strErrorRet);
}

bool CMessageSigner::VerifyMessage(const CKeyID& keyID, const std::vector<unsigned char>& vchSig, const std::string& strMessage, std::string& strErrorRet)
{
    return CHashSigner::VerifyHash(GetMessageHash(strMessage), keyID, vchSig, strErrorRet);
}

bool CHashSigner::SignHash(const uint256& hash, const CKey& key, std::vector<unsigned char>& vchSigRet)
{
    return key.SignCompact(hash, vchSigRet);
}

bool CHashSigner::VerifyHash(const uint256& hash, const CPubKey& pubkey, const std::vector<unsigned char>& vchSig, std::string& strErrorRet)
{
    return VerifyHash(hash, pubkey.GetID(), vchSig, strErrorRet);
}

bool CHashSigner::VerifyHash(const uint256& hash, const CKeyID& keyID, const std::vector<unsigned char>& vchSig, std::string& strErrorRet)
{
    CPubKey pubkeyFromSig;
    if(!pubkeyFromSig.RecoverCompact(hash, vchSig)) {
        strErrorRet = "Error recovering public key.";
        return false;
    }

    if(pubkeyFromSig.GetID() != keyID) {
        strErrorRet = strprintf("Keys don't match: pubkey=%s, pubkeyFromSig=%s, hash=%s, vchSig=%s",
                EncodeDestination(keyID), EncodeDestination(pubkeyFromSig.GetID()),
                hash.ToString(), EncodeBase64(&vchSig[0], vchSig.size()));
        return false;
    }

    return true;
}

/** CSignedMessage Class
 *  Functions inherited by network signed-messages
 */

bool CSignedMessage::Sign(const CKey& key, const CKeyID& keyID)
{
    nMessVersion = MessageVersion::MESS_VER_HASH;
    std::string strError = "";
    uint256 hash = GetSignatureHash();

    if(!CHashSigner::SignHash(hash, key, vchSig)) {
        return error("%s : SignHash() failed", __func__);
    }

    if (!CHashSigner::VerifyHash(hash, keyID, vchSig, strError)) {
        return error("%s : VerifyHash() failed, error: %s", __func__, strError);
    }

    return true;
}

bool CSignedMessage::Sign(const std::string strSignKey)
{
    CKey key;
    CPubKey pubkey;

    if (!CMessageSigner::GetKeysFromSecret(strSignKey, key, pubkey)) {
        return error("%s : Invalid strSignKey", __func__);
    }

    return Sign(key, pubkey.GetID());
}

bool CSignedMessage::CheckSignature(const CKeyID& keyID) const
{
    std::string strError = "";

    if (nMessVersion == MessageVersion::MESS_VER_HASH) {
        uint256 hash = GetSignatureHash();
        return CHashSigner::VerifyHash(hash, keyID, vchSig, strError);
    }

    std::string strMessage = GetStrMessage();
    return CMessageSigner::VerifyMessage(keyID, vchSig, strMessage, strError);
}

std::string CSignedMessage::GetSignatureBase64() const
{
    return EncodeBase64(&vchSig[0], vchSig.size());
}

bool CSignedMessage::Sign(const CBLSSecretKey& sk)
{
    if (!sk.IsValid()) {
        return false;
    }
    const uint256& mess = GetSignatureHash();
    const CBLSSignature& sig = sk.Sign(mess);
    vchSig = sig.ToByteVector();
    return true;
}

bool CSignedMessage::CheckSignature(const CBLSPublicKey& pk) const
{
    if (!pk.IsValid()) {
        return false;
    }
    const uint256& mess = GetSignatureHash();
    CBLSSignature sig(vchSig);
    return sig.IsValid() && sig.VerifyInsecure(pk, mess);
}

