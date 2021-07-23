// Copyright (c) 2018-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "zpivchain.h"

#include "guiinterface.h"
#include "invalid.h"
#include "txdb.h"
#include "wallet/wallet.h"
#include "zpiv/zpivmodule.h"

// 6 comes from OPCODE (1) + vch.size() (1) + BIGNUM size (4)
#define SCRIPT_OFFSET 6

static bool IsTransactionInChain(const uint256& txId, int& nHeightTx, CTransactionRef& tx)
{
    uint256 hashBlock;
    if (!GetTransaction(txId, tx, hashBlock, true))
        return false;

    if (hashBlock.IsNull() || !mapBlockIndex.count(hashBlock)) {
        return false;
    }

    CBlockIndex* pindex = mapBlockIndex[hashBlock];
    if (!chainActive.Contains(pindex)) {
        return false;
    }

    nHeightTx = pindex->nHeight;
    return true;
}

static bool IsTransactionInChain(const uint256& txId, int& nHeightTx)
{
    CTransactionRef tx;
    return IsTransactionInChain(txId, nHeightTx, tx);
}

bool IsSerialInBlockchain(const CBigNum& bnSerial, int& nHeightTx)
{
    uint256 txHash;
    // if not in zerocoinDB then its not in the blockchain
    if (!zerocoinDB->ReadCoinSpend(bnSerial, txHash))
        return false;

    return IsTransactionInChain(txHash, nHeightTx);
}

libzerocoin::CoinSpend TxInToZerocoinSpend(const CTxIn& txin)
{
    CDataStream serializedCoinSpend = ZPIVModule::ScriptSigToSerializedSpend(txin.scriptSig);
    return libzerocoin::CoinSpend(serializedCoinSpend);
}

bool TxOutToPublicCoin(const CTxOut& txout, libzerocoin::PublicCoin& pubCoin, CValidationState& state)
{
    CBigNum publicZerocoin;
    std::vector<unsigned char> vchZeroMint;
    vchZeroMint.insert(vchZeroMint.end(), txout.scriptPubKey.begin() + SCRIPT_OFFSET,
                       txout.scriptPubKey.begin() + txout.scriptPubKey.size());
    publicZerocoin.setvch(vchZeroMint);

    libzerocoin::CoinDenomination denomination = libzerocoin::AmountToZerocoinDenomination(txout.nValue);
    LogPrint(BCLog::LEGACYZC, "%s : denomination %d for pubcoin %s\n", __func__, denomination, publicZerocoin.GetHex());
    if (denomination == libzerocoin::ZQ_ERROR)
        return state.DoS(100, error("TxOutToPublicCoin : txout.nValue is not correct"));

    libzerocoin::PublicCoin checkPubCoin(Params().GetConsensus().Zerocoin_Params(false), publicZerocoin, denomination);
    pubCoin = checkPubCoin;

    return true;
}


