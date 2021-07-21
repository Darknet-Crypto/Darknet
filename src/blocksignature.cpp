// Copyright (c) 2017-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "blocksignature.h"

#include "script/standard.h"
#include "zpivchain.h"

bool SignBlockWithKey(CBlock& block, const CKey& key)
{
    if (!key.Sign(block.GetHash(), block.vchBlockSig))
        return error("%s: failed to sign block hash with key", __func__);

    return true;
}

bool SignBlock(CBlock& block, const CKeyStore& keystore)
{
    CKeyID keyID;
    if (block.IsProofOfWork()) {
        bool fFoundID = false;
        for (const CTxOut& txout : block.vtx[0]->vout) {
            if (!txout.GetKeyIDFromUTXO(keyID))
                continue;
            fFoundID = true;
            break;
        }
        if (!fFoundID)
            return error("%s: failed to find key for PoW", __func__);
    } else {
        if (!block.vtx[1]->vout[1].GetKeyIDFromUTXO(keyID))
            return error("%s: failed to find key for PoS", __func__);
    }

    CKey key;
    if (!keystore.GetKey(keyID, key))
        return error("%s: failed to get key from keystore", __func__);

    return SignBlockWithKey(block, key);
}

bool CheckBlockSignature(const CBlock& block)
{
    if (block.IsProofOfWork())
        return block.vchBlockSig.empty();

    if (block.vchBlockSig.empty())
        return error("%s: vchBlockSig is empty!", __func__);

    /** Each block is signed by the private key of the input that is staked. This can be either zPIV or normal UTXO
     *  zPIV: Each zPIV has a keypair associated with it. The serial number is a hash of the public key.
     *  UTXO: The public key that signs must match the public key associated with the first utxo of the coinstake tx.
     */
    CPubKey pubkey;
    bool fzPIVStake = block.vtx[1]->vin[0].IsZerocoinSpend();
    if (fzPIVStake) {
        libzerocoin::CoinSpend spend = TxInToZerocoinSpend(block.vtx[1]->vin[0]);
        pubkey = spend.getPubKey();
    } else {
        txnouttype whichType;
        std::vector<valtype> vSolutions;
        const CTxOut& txout = block.vtx[1]->vout[1];
        if (!Solver(txout.scriptPubKey, whichType, vSolutions))
            return false;

        if (whichType == TX_PUBKEY) {
            valtype& vchPubKey = vSolutions[0];
            pubkey = CPubKey(vchPubKey);
        } else if (whichType == TX_PUBKEYHASH) {
            const CTxIn& txin = block.vtx[1]->vin[0];
            // Check if the scriptSig is for a p2pk or a p2pkh
            if (txin.scriptSig.size() == 73) { // Sig size + DER signature size.
                // If the input is for a p2pk and the output is a p2pkh.
                // We don't have the pubkey to verify the block sig anywhere in this block.
                // p2pk scriptsig only contains the signature and p2pkh scriptpubkey only contain the hash.
                return false;
            } else {
                int start = 1 + (int) *txin.scriptSig.begin(); // skip sig
                pubkey = CPubKey(txin.scriptSig.begin()+start+1, txin.scriptSig.end());
            }
        } else if (whichType == TX_COLDSTAKE) {
            // pick the public key from the P2CS input
            const CTxIn& txin = block.vtx[1]->vin[0];
            int start = 1 + (int) *txin.scriptSig.begin(); // skip sig
            start += 1 + (int) *(txin.scriptSig.begin()+start); // skip flag
            pubkey = CPubKey(txin.scriptSig.begin()+start+1, txin.scriptSig.end());
        }
    }

    if (!pubkey.IsValid())
        return error("%s: invalid pubkey %s", __func__, HexStr(pubkey.Raw()));

    return pubkey.Verify(block.GetHash(), block.vchBlockSig);
}
