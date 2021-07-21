// Copyright (c) 2015-2020 The Zcash developers
// Copyright (c) 2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "sapling/sapling_core_write.h"
#include "utilstrencodings.h"
#include "utilmoneystr.h"

static UniValue TxShieldedSpendsToJSON(const CTransaction& tx) {
    UniValue vdesc(UniValue::VARR);
    if (tx.sapData) {
        for (const SpendDescription& spendDesc : tx.sapData->vShieldedSpend) {
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("cv", spendDesc.cv.GetHex());
            obj.pushKV("anchor", spendDesc.anchor.GetHex());
            obj.pushKV("nullifier", spendDesc.nullifier.GetHex());
            obj.pushKV("rk", spendDesc.rk.GetHex());
            obj.pushKV("proof", HexStr(spendDesc.zkproof));
            obj.pushKV("spendAuthSig", HexStr(spendDesc.spendAuthSig));
            vdesc.push_back(obj);
        }
    }
    return vdesc;
}

static UniValue TxShieldedOutputsToJSON(const CTransaction& tx) {
    UniValue vdesc(UniValue::VARR);
    if (tx.sapData) {
        for (const OutputDescription& outputDesc : tx.sapData->vShieldedOutput) {
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("cv", outputDesc.cv.GetHex());
            obj.pushKV("cmu", outputDesc.cmu.GetHex());
            obj.pushKV("ephemeralKey", outputDesc.ephemeralKey.GetHex());
            obj.pushKV("encCiphertext", HexStr(outputDesc.encCiphertext));
            obj.pushKV("outCiphertext", HexStr(outputDesc.outCiphertext));
            obj.pushKV("proof", HexStr(outputDesc.zkproof));
            vdesc.push_back(obj);
        }
    }
    return vdesc;
}

void TxSaplingToJSON(const CTransaction& tx, UniValue& entry) {
    if (tx.IsShieldedTx()) {
        entry.pushKV("valueBalance", FormatMoney(tx.sapData->valueBalance));
        entry.pushKV("valueBalanceSat", tx.sapData->valueBalance);
        UniValue vspenddesc = TxShieldedSpendsToJSON(tx);
        entry.pushKV("vShieldSpend", vspenddesc);
        UniValue voutputdesc = TxShieldedOutputsToJSON(tx);
        entry.pushKV("vShieldOutput", voutputdesc);
        if (tx.sapData->hasBindingSig()) {
            entry.pushKV("bindingSig", HexStr(tx.sapData->bindingSig));
        }
    }
}
