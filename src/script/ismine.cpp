// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2016-2019 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ismine.h"

#include "keystore.h"
#include "script/script.h"
#include "script/sign.h"
#include "util/system.h"



typedef std::vector<unsigned char> valtype;

unsigned int HaveKeys(const std::vector<valtype>& pubkeys, const CKeyStore& keystore)
{
    unsigned int nResult = 0;
    for (const valtype& pubkey : pubkeys) {
        CKeyID keyID = CPubKey(pubkey).GetID();
        if(keystore.HaveKey(keyID))
            ++nResult;
    }
    return nResult;
}

isminetype IsMine(const CKeyStore& keystore, const CTxDestination& dest)
{
    CScript script = GetScriptForDestination(dest);
    return IsMine(keystore, script);
}

isminetype IsMine(const CKeyStore& keystore, const libzcash::SaplingPaymentAddress& pa)
{
    libzcash::SaplingIncomingViewingKey ivk;
    libzcash::SaplingExtendedFullViewingKey exfvk;
    if (keystore.GetSaplingIncomingViewingKey(pa, ivk) &&
        keystore.GetSaplingFullViewingKey(ivk, exfvk) &&
        keystore.HaveSaplingSpendingKey(exfvk)) {
        return ISMINE_SPENDABLE_SHIELDED;
    } else if (!ivk.IsNull()) {
        return ISMINE_WATCH_ONLY_SHIELDED;
    } else {
        return ISMINE_NO;
    }
}

namespace
{
    class CWDestinationVisitor : public boost::static_visitor<isminetype>
    {
    private:
        const CKeyStore& keystore;
    public:
        CWDestinationVisitor(const CKeyStore& _keystore) : keystore(_keystore) {}

        isminetype operator()(const CTxDestination& dest) const {
            return ::IsMine(keystore, dest);
        }

        isminetype operator()(const libzcash::SaplingPaymentAddress& pa) const {
            return ::IsMine(keystore, pa);
        }
    };
}

isminetype IsMine(const CKeyStore& keystore, const CWDestination& dest)
{
    return boost::apply_visitor(CWDestinationVisitor(keystore), dest);
}

isminetype IsMine(const CKeyStore& keystore, const CScript& scriptPubKey)
{
    std::vector<valtype> vSolutions;
    txnouttype whichType;
    if(!Solver(scriptPubKey, whichType, vSolutions)) {
        if(keystore.HaveWatchOnly(scriptPubKey)) {
            return ISMINE_WATCH_ONLY;
        }

        return ISMINE_NO;
    }

    CKeyID keyID;
    switch (whichType) {
    case TX_NONSTANDARD:
    case TX_NULL_DATA:
        break;
    case TX_PUBKEY:
        keyID = CPubKey(vSolutions[0]).GetID();
        if(keystore.HaveKey(keyID))
            return ISMINE_SPENDABLE;
        break;
    case TX_PUBKEYHASH:
        keyID = CKeyID(uint160(vSolutions[0]));
        if(keystore.HaveKey(keyID))
            return ISMINE_SPENDABLE;
        break;
    case TX_SCRIPTHASH: {
        CScriptID scriptID = CScriptID(uint160(vSolutions[0]));
        CScript subscript;
        if(keystore.GetCScript(scriptID, subscript)) {
            isminetype ret = IsMine(keystore, subscript);
            if(ret != ISMINE_NO)
                return ret;
        }
        break;
    }
    case TX_COLDSTAKE: {
        CKeyID stakeKeyID = CKeyID(uint160(vSolutions[0]));
        bool stakeKeyIsMine = keystore.HaveKey(stakeKeyID);
        CKeyID ownerKeyID = CKeyID(uint160(vSolutions[1]));
        bool spendKeyIsMine = keystore.HaveKey(ownerKeyID);

        if (spendKeyIsMine) {
            // If the wallet has both keys, ISMINE_SPENDABLE_DELEGATED
            // takes precedence over ISMINE_COLD
            return ISMINE_SPENDABLE_DELEGATED;
        } else if (stakeKeyIsMine) {
            return ISMINE_COLD;
        } else {
            // todo: Include watch only..
        }
        break;
    }
    case TX_MULTISIG: {
        // Only consider transactions "mine" if we own ALL the
        // keys involved. multi-signature transactions that are
        // partially owned (somebody else has a key that can spend
        // them) enable spend-out-from-under-you attacks, especially
        // in shared-wallet situations.
        std::vector<valtype> keys(vSolutions.begin() + 1, vSolutions.begin() + vSolutions.size() - 1);
        if(HaveKeys(keys, keystore) == keys.size())
            return ISMINE_SPENDABLE;
        break;
    }
    }

    if (keystore.HaveWatchOnly(scriptPubKey)) {
        return ISMINE_WATCH_ONLY;
    }

    return ISMINE_NO;
}
