// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "budget/budgetmanager.h"
#include "checkpoints.h"
#include "clientversion.h"
#include "core_io.h"
#include "consensus/upgrades.h"
#include "kernel.h"
#include "key_io.h"
#include "masternodeman.h"
#include "policy/feerate.h"
#include "policy/policy.h"
#include "rpc/server.h"
#include "sync.h"
#include "txdb.h"
#include "util/system.h"
#include "utilmoneystr.h"
#include "utilstrencodings.h"
#include "hash.h"
#include "validationinterface.h"
#include "wallet/wallet.h"
#include "warnings.h"

#include <stdint.h>
#include <univalue.h>
#include <numeric>
#include <condition_variable>

#include <boost/thread/thread.hpp> // boost::thread::interrupt


struct CUpdatedBlock
{
    uint256 hash;
    int height;
};
static std::mutex cs_blockchange;
static std::condition_variable cond_blockchange;
static CUpdatedBlock latestblock;

extern void TxToJSON(CWallet* const pwallet, const CTransaction& tx, const uint256 hashBlock, UniValue& entry);

UniValue syncwithvalidationinterfacequeue(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 0) {
        throw std::runtime_error(
                "syncwithvalidationinterfacequeue\n"
                "\nWaits for the validation interface queue to catch up on everything that was there when we entered this function.\n"
                "\nExamples:\n"
                + HelpExampleCli("syncwithvalidationinterfacequeue","")
                + HelpExampleRpc("syncwithvalidationinterfacequeue","")
        );
    }
    SyncWithValidationInterfaceQueue();
    return NullUniValue;
}

double GetDifficulty(const CBlockIndex* blockindex)
{
    // Floating point number that is a multiple of the minimum difficulty,
    // minimum difficulty = 1.0.
    if (blockindex == NULL) {
        const CBlockIndex* pChainTip = GetChainTip();
        if (!pChainTip)
            return 1.0;
        else
            blockindex = pChainTip;
    }

    int nShift = (blockindex->nBits >> 24) & 0xff;

    double dDiff =
        (double)0x0000ffff / (double)(blockindex->nBits & 0x00ffffff);

    while (nShift < 29) {
        dDiff *= 256.0;
        nShift++;
    }
    while (nShift > 29) {
        dDiff /= 256.0;
        nShift--;
    }

    return dDiff;
}

static UniValue ValuePoolDesc(
        const Optional<CAmount> chainValue,
        const Optional<CAmount> valueDelta)
{
    UniValue rv(UniValue::VOBJ);
    rv.pushKV("chainValue",  ValueFromAmount(chainValue ? *chainValue : 0));
    rv.pushKV("valueDelta",  ValueFromAmount(valueDelta ? *valueDelta : 0));
    return rv;
}

UniValue blockheaderToJSON(const CBlockIndex* blockindex)
{
    UniValue result(UniValue::VOBJ);
    result.pushKV("hash", blockindex->GetBlockHash().GetHex());
    int confirmations = -1;
    // Only report confirmations if the block is on the main chain
    if (chainActive.Contains(blockindex))
        confirmations = chainActive.Height() - blockindex->nHeight + 1;
    result.pushKV("confirmations", confirmations);
    result.pushKV("height", blockindex->nHeight);
    result.pushKV("version", blockindex->nVersion);
    result.pushKV("merkleroot", blockindex->hashMerkleRoot.GetHex());
    result.pushKV("time", (int64_t)blockindex->nTime);
    result.pushKV("mediantime", (int64_t)blockindex->GetMedianTimePast());
    result.pushKV("nonce", (uint64_t)blockindex->nNonce);
    result.pushKV("bits", strprintf("%08x", blockindex->nBits));
    result.pushKV("difficulty", GetDifficulty(blockindex));
    result.pushKV("chainwork", blockindex->nChainWork.GetHex());
    result.pushKV("acc_checkpoint", blockindex->nAccumulatorCheckpoint.GetHex());
    // Sapling shield pool value
    result.pushKV("shield_pool_value", ValuePoolDesc(blockindex->nChainSaplingValue, blockindex->nSaplingValue));
    if (blockindex->pprev)
        result.pushKV("previousblockhash", blockindex->pprev->GetBlockHash().GetHex());
    CBlockIndex *pnext = chainActive.Next(blockindex);
    if (pnext)
        result.pushKV("nextblockhash", pnext->GetBlockHash().GetHex());
    return result;
}

UniValue blockToJSON(const CBlock& block, const CBlockIndex* blockindex, bool txDetails = false)
{
    UniValue result(UniValue::VOBJ);
    result.pushKV("hash", block.GetHash().GetHex());
    int confirmations = -1;
    // Only report confirmations if the block is on the main chain
    if (chainActive.Contains(blockindex))
        confirmations = chainActive.Height() - blockindex->nHeight + 1;
    result.pushKV("confirmations", confirmations);
    result.pushKV("size", (int)::GetSerializeSize(block, PROTOCOL_VERSION));
    result.pushKV("height", blockindex->nHeight);
    result.pushKV("version", block.nVersion);
    result.pushKV("merkleroot", block.hashMerkleRoot.GetHex());
    result.pushKV("acc_checkpoint", block.nAccumulatorCheckpoint.GetHex());
    result.pushKV("finalsaplingroot", block.hashFinalSaplingRoot.GetHex());
    UniValue txs(UniValue::VARR);
    for (const auto& txIn : block.vtx) {
        const CTransaction& tx = *txIn;
        if (txDetails) {
            UniValue objTx(UniValue::VOBJ);
            TxToJSON(nullptr, tx, UINT256_ZERO, objTx);
            txs.push_back(objTx);
        } else
            txs.push_back(tx.GetHash().GetHex());
    }
    result.pushKV("tx", txs);
    result.pushKV("time", block.GetBlockTime());
    result.pushKV("mediantime", (int64_t)blockindex->GetMedianTimePast());
    result.pushKV("nonce", (uint64_t)block.nNonce);
    result.pushKV("bits", strprintf("%08x", block.nBits));
    result.pushKV("difficulty", GetDifficulty(blockindex));
    result.pushKV("chainwork", blockindex->nChainWork.GetHex());

    if (blockindex->pprev)
        result.pushKV("previousblockhash", blockindex->pprev->GetBlockHash().GetHex());
    CBlockIndex* pnext = chainActive.Next(blockindex);
    if (pnext)
        result.pushKV("nextblockhash", pnext->GetBlockHash().GetHex());

    //////////
    ////////// Coin stake data ////////////////
    /////////
    if (block.IsProofOfStake()) {
        uint256 hashProofOfStakeRet{UINT256_ZERO};
        if (blockindex->pprev && !GetStakeKernelHash(hashProofOfStakeRet, block, blockindex->pprev))
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Cannot get proof of stake hash");

        std::string stakeModifier = (Params().GetConsensus().NetworkUpgradeActive(blockindex->nHeight, Consensus::UPGRADE_V3_4) ?
                                     blockindex->GetStakeModifierV2().GetHex() :
                                     strprintf("%016x", blockindex->GetStakeModifierV1()));
        result.pushKV("stakeModifier", stakeModifier);
        result.pushKV("hashProofOfStake", hashProofOfStakeRet.GetHex());
    }

    return result;
}

UniValue getblockcount(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getblockcount\n"
            "\nReturns the number of blocks in the longest block chain.\n"

            "\nResult:\n"
            "n    (numeric) The current block count\n"

            "\nExamples:\n" +
            HelpExampleCli("getblockcount", "") + HelpExampleRpc("getblockcount", ""));

    LOCK(cs_main);
    return chainActive.Height();
}

UniValue getbestblockhash(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getbestblockhash\n"
            "\nReturns the hash of the best (tip) block in the longest block chain.\n"

            "\nResult\n"
            "\"hex\"      (string) the block hash hex encoded\n"

            "\nExamples\n" +
            HelpExampleCli("getbestblockhash", "") + HelpExampleRpc("getbestblockhash", ""));

    LOCK(cs_main);
    return chainActive.Tip()->GetBlockHash().GetHex();
}

UniValue getbestsaplinganchor(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getbestsaplinganchor\n"
            "\nReturns the most recent SaplingMerkleTree root.\n"

            "\nResult\n"
            "\"hex\"      (string) the sapling anchor hex encoded\n"

            "\nExamples\n" +
            HelpExampleCli("getbestsaplinganchor", "") + HelpExampleRpc("getbestsaplinganchor", ""));

    return pcoinsTip->GetBestAnchor().ToString();
}

void RPCNotifyBlockChange(bool fInitialDownload, const CBlockIndex* pindex)
{
    if(pindex) {
        std::lock_guard<std::mutex> lock(cs_blockchange);
        latestblock.hash = pindex->GetBlockHash();
        latestblock.height = pindex->nHeight;
    }
    cond_blockchange.notify_all();
}

UniValue waitfornewblock(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "waitfornewblock ( timeout )\n"
            "\nWaits for a specific new block and returns useful info about it.\n"
            "\nReturns the current block on timeout or exit.\n"

            "\nArguments:\n"
            "1. timeout       (numeric, optional, default=0) Time in milliseconds to wait for a response. 0 indicates no timeout.\n"

            "\nResult:\n"
            "{                       (json object)\n"
            "  \"hash\": \"hash\",   (string) The blockhash\n"
            "  \"height\": n         (numeric) Block height\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("waitfornewblock", "1000")
            + HelpExampleRpc("waitfornewblock", "1000")
        );
    int timeout = 0;
    if (request.params.size() > 0)
        timeout = request.params[0].get_int();

    CUpdatedBlock block;
    {
        std::unique_lock<std::mutex> lock(cs_blockchange);
        block = latestblock;
        if(timeout)
            cond_blockchange.wait_for(lock, std::chrono::milliseconds(timeout), [&block]{return latestblock.height != block.height || latestblock.hash != block.hash || !IsRPCRunning(); });
        else
            cond_blockchange.wait(lock, [&block]{return latestblock.height != block.height || latestblock.hash != block.hash || !IsRPCRunning(); });
        block = latestblock;
    }
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("hash", block.hash.GetHex());
    ret.pushKV("height", block.height);
    return ret;
}

UniValue waitforblock(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "waitforblock blockhash ( timeout )\n"
            "\nWaits for a specific new block and returns useful info about it.\n"
            "\nReturns the current block on timeout or exit.\n"

            "\nArguments:\n"
            "1. \"blockhash\"    (string, required) Block hash to wait for.\n"
            "2. timeout          (numeric, optional, default=0) Time in milliseconds to wait for a response. 0 indicates no timeout.\n"

            "\nResult:\n"
            "{                       (json object)\n"
            "  \"hash\": \"hash\",   (string) The blockhash\n"
            "  \"height\": n         (numeric) Block height\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("waitforblock", "\"0000000000079f8ef3d2c688c244eb7a4570b24c9ed7b4a8c619eb02596f8862\", 1000")
            + HelpExampleRpc("waitforblock", "\"0000000000079f8ef3d2c688c244eb7a4570b24c9ed7b4a8c619eb02596f8862\", 1000")
        );
    int timeout = 0;

    uint256 hash = uint256S(request.params[0].get_str());

    if (request.params.size() > 1)
        timeout = request.params[1].get_int();

    CUpdatedBlock block;
    {
        std::unique_lock<std::mutex> lock(cs_blockchange);
        if(timeout)
            cond_blockchange.wait_for(lock, std::chrono::milliseconds(timeout), [&hash]{return latestblock.hash == hash || !IsRPCRunning();});
        else
            cond_blockchange.wait(lock, [&hash]{return latestblock.hash == hash || !IsRPCRunning(); });
        block = latestblock;
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("hash", block.hash.GetHex());
    ret.pushKV("height", block.height);
    return ret;
}

UniValue waitforblockheight(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "waitforblockheight height ( timeout )\n"
            "\nWaits for (at least) block height and returns the height and hash\n"
            "of the current tip.\n"
            "\nReturns the current block on timeout or exit.\n"

            "\nArguments:\n"
            "1. height           (numeric, required) Block height to wait for\n"
            "2. timeout          (numeric, optional, default=0) Time in milliseconds to wait for a response. 0 indicates no timeout.\n"

            "\nResult:\n"
            "{                       (json object)\n"
            "  \"hash\": \"hash\",   (string) The blockhash\n"
            "  \"height\": n         (int) Block height\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("waitforblockheight", "\"100\", 1000")
            + HelpExampleRpc("waitforblockheight", "\"100\", 1000")
        );
    int timeout = 0;

    int height = request.params[0].get_int();

    if (request.params.size() > 1)
        timeout = request.params[1].get_int();

    CUpdatedBlock block;
    {
        std::unique_lock<std::mutex> lock(cs_blockchange);
        if(timeout)
            cond_blockchange.wait_for(lock, std::chrono::milliseconds(timeout), [&height]{return latestblock.height >= height || !IsRPCRunning();});
        else
            cond_blockchange.wait(lock, [&height]{return latestblock.height >= height || !IsRPCRunning(); });
        block = latestblock;
    }
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("hash", block.hash.GetHex());
    ret.pushKV("height", block.height);
    return ret;
}

UniValue getdifficulty(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getdifficulty\n"
            "\nReturns the proof-of-work difficulty as a multiple of the minimum difficulty.\n"

            "\nResult:\n"
            "n.nnn       (numeric) the proof-of-work difficulty as a multiple of the minimum difficulty.\n"

            "\nExamples:\n" +
            HelpExampleCli("getdifficulty", "") + HelpExampleRpc("getdifficulty", ""));

    LOCK(cs_main);
    return GetDifficulty();
}

static std::string EntryDescriptionString()
{
    return "    \"size\" : n,             (numeric) transaction size in bytes\n"
           "    \"fee\" : n,              (numeric) transaction fee in " + CURRENCY_UNIT + "\n"
           "    \"modifiedfee\" : n,      (numeric) transaction fee with fee deltas used for mining priority\n"
           "    \"time\" : n,             (numeric) local time transaction entered pool in seconds since 1 Jan 1970 GMT\n"
           "    \"height\" : n,           (numeric) block height when transaction entered pool\n"
           "    \"descendantcount\" : n,  (numeric) number of in-mempool descendant transactions (including this one)\n"
           "    \"descendantsize\" : n,   (numeric) size of in-mempool descendants (including this one)\n"
           "    \"descendantfees\" : n,   (numeric) modified fees (see above) of in-mempool descendants (including this one)\n"
           "    \"depends\" : [           (array) unconfirmed transactions used as inputs for this transaction\n"
           "        \"transactionid\",    (string) parent transaction id\n"
           "       ... ]\n";
}

static void entryToJSON(UniValue &info, const CTxMemPoolEntry &e)
{
    AssertLockHeld(mempool.cs);

    info.pushKV("size", (int)e.GetTxSize());
    info.pushKV("fee", ValueFromAmount(e.GetFee()));
    info.pushKV("modifiedfee", ValueFromAmount(e.GetModifiedFee()));
    info.pushKV("time", e.GetTime());
    info.pushKV("height", (int)e.GetHeight());
    info.pushKV("descendantcount", e.GetCountWithDescendants());
    info.pushKV("descendantsize", e.GetSizeWithDescendants());
    info.pushKV("descendantfees", e.GetModFeesWithDescendants());
    const CTransaction& tx = e.GetTx();
    std::set<std::string> setDepends;
    for (const CTxIn& txin : tx.vin) {
        if (mempool.exists(txin.prevout.hash))
            setDepends.insert(txin.prevout.hash.ToString());
    }

    UniValue depends(UniValue::VARR);
    for (const std::string& dep : setDepends) {
        depends.push_back(dep);
    }

    info.pushKV("depends", depends);
}

UniValue mempoolToJSON(bool fVerbose = false)
{
    if (fVerbose) {
        LOCK(mempool.cs);
        UniValue o(UniValue::VOBJ);
        for (const CTxMemPoolEntry& e : mempool.mapTx) {
            const uint256& hash = e.GetTx().GetHash();
            UniValue info(UniValue::VOBJ);
            entryToJSON(info, e);
            o.pushKV(hash.ToString(), info);
        }
        return o;
    } else {
        std::vector<uint256> vtxid;
        mempool.queryHashes(vtxid);

        UniValue a(UniValue::VARR);
        for (const uint256& hash : vtxid)
            a.push_back(hash.ToString());

        return a;
    }
}

UniValue getrawmempool(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "getrawmempool ( verbose )\n"
            "\nReturns all transaction ids in memory pool as a json array of string transaction ids.\n"

            "\nArguments:\n"
            "1. verbose           (boolean, optional, default=false) True for a json object, false for array of transaction ids\n"

            "\nResult: (for verbose = false):\n"
            "[                     (json array of string)\n"
            "  \"transactionid\"     (string) The transaction id\n"
            "  ,...\n"
            "]\n"

            "\nResult: (for verbose = true):\n"
            "{                           (json object)\n"
            "  \"transactionid\" : {       (json object)\n"
            + EntryDescriptionString()
            + "  }, ...\n"
            "}\n"
            "\nExamples\n" +
            HelpExampleCli("getrawmempool", "true") + HelpExampleRpc("getrawmempool", "true"));

    LOCK(cs_main);

    bool fVerbose = false;
    if (request.params.size() > 0)
        fVerbose = request.params[0].get_bool();

    return mempoolToJSON(fVerbose);
}

UniValue getblockhash(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "getblockhash height\n"
            "\nReturns hash of block in best-block-chain at height provided.\n"

            "\nArguments:\n"
            "1. height        (numeric, required) The height index\n"

            "\nResult:\n"
            "\"hash\"         (string) The block hash\n"

            "\nExamples:\n" +
            HelpExampleCli("getblockhash", "1000") + HelpExampleRpc("getblockhash", "1000"));

    LOCK(cs_main);

    int nHeight = request.params[0].get_int();
    if (nHeight < 0 || nHeight > chainActive.Height())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Block height out of range");

    CBlockIndex* pblockindex = chainActive[nHeight];
    return pblockindex->GetBlockHash().GetHex();
}

UniValue getblock(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "getblock \"blockhash\" ( verbose )\n"
            "\nIf verbose is false, returns a string that is serialized, hex-encoded data for block 'hash'.\n"
            "If verbose is true, returns an Object with information about block <hash>.\n"

            "\nArguments:\n"
            "1. \"blockhash\"     (string, required) The block hash\n"
            "2. verbose           (boolean, optional, default=true) True for a json object, false for the hex encoded data\n"

            "\nResult (for verbose = true):\n"
            "{\n"
            "  \"hash\" : \"hash\",     (string) the block hash (same as provided)\n"
            "  \"confirmations\" : n,   (numeric) The number of confirmations, or -1 if the block is not on the main chain\n"
            "  \"size\" : n,            (numeric) The block size\n"
            "  \"height\" : n,          (numeric) The block height or index\n"
            "  \"version\" : n,         (numeric) The block version\n"
            "  \"merkleroot\" : \"xxxx\", (string) The merkle root\n"
            "  \"finalsaplingroot\" : \"xxxx\", (string) The root of the Sapling commitment tree after applying this block\n"
            "  \"tx\" : [               (array of string) The transaction ids\n"
            "     \"transactionid\"     (string) The transaction id\n"
            "     ,...\n"
            "  ],\n"
            "  \"time\" : ttt,          (numeric) The block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"mediantime\" : ttt,    (numeric) The median block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"nonce\" : n,           (numeric) The nonce\n"
            "  \"bits\" : \"1d00ffff\", (string) The bits\n"
            "  \"difficulty\" : x.xxx,  (numeric) The difficulty\n"
            "  \"previousblockhash\" : \"hash\",  (string) The hash of the previous block\n"
            "  \"nextblockhash\" : \"hash\"       (string) The hash of the next block\n"
            "  \"stakeModifier\" : \"xxx\",       (string) Proof of Stake modifier\n"
            "  \"hashProofOfStake\" : \"hash\",   (string) Proof of Stake hash\n"
            "  }\n"
            "}\n"

            "\nResult (for verbose=false):\n"
            "\"data\"             (string) A string that is serialized, hex-encoded data for block 'hash'.\n"

            "\nExamples:\n" +
            HelpExampleCli("getblock", "\"00000000000fd08c2fb661d2fcb0d49abb3a91e5f27082ce64feed3b4dede2e2\"") +
            HelpExampleRpc("getblock", "\"00000000000fd08c2fb661d2fcb0d49abb3a91e5f27082ce64feed3b4dede2e2\""));

    LOCK(cs_main);

    std::string strHash = request.params[0].get_str();
    uint256 hash(uint256S(strHash));

    bool fVerbose = true;
    if (request.params.size() > 1)
        fVerbose = request.params[1].get_bool();

    if (mapBlockIndex.count(hash) == 0)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");

    CBlock block;
    CBlockIndex* pblockindex = mapBlockIndex[hash];

    if (!ReadBlockFromDisk(block, pblockindex))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Can't read block from disk");

    if (!fVerbose) {
        CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION);
        ssBlock << block;
        std::string strHex = HexStr(ssBlock.begin(), ssBlock.end());
        return strHex;
    }

    return blockToJSON(block, pblockindex);
}

UniValue getblockheader(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "getblockheader \"blockhash\" ( verbose )\n"
            "\nIf verbose is false, returns a string that is serialized, hex-encoded data for block 'hash' header.\n"
            "If verbose is true, returns an Object with information about block <hash> header.\n"

            "\nArguments:\n"
            "1. \"blockhash\"     (string, required) The block hash\n"
            "2. verbose           (boolean, optional, default=true) True for a json object, false for the hex encoded data\n"

            "\nResult (for verbose = true):\n"
            "{\n"
            "  \"version\" : n,         (numeric) The block version\n"
            "  \"previousblockhash\" : \"hash\",  (string) The hash of the previous block\n"
            "  \"merkleroot\" : \"xxxx\", (string) The merkle root\n"
            "  \"time\" : ttt,          (numeric) The block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"mediantime\" : ttt,    (numeric) The median block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"nonce\" : n,           (numeric) The nonce\n"
            "  \"bits\" : \"1d00ffff\", (string) The bits\n"
            "  \"shield_pool_value\":   (object) Block shield pool value\n"
            "  {\n"
            "     \"chainValue\":        (numeric) Total value held by the Sapling circuit up to and including this block\n"
            "     \"valueDelta\":        (numeric) Change in value held by the Sapling circuit over this block\n"
            "  }\n"
            "}"
            "}\n"

            "\nResult (for verbose=false):\n"
            "\"data\"             (string) A string that is serialized, hex-encoded data for block 'hash' header.\n"

            "\nExamples:\n" +
            HelpExampleCli("getblockheader", "\"00000000000fd08c2fb661d2fcb0d49abb3a91e5f27082ce64feed3b4dede2e2\"") +
            HelpExampleRpc("getblockheader", "\"00000000000fd08c2fb661d2fcb0d49abb3a91e5f27082ce64feed3b4dede2e2\""));

    std::string strHash = request.params[0].get_str();
    uint256 hash(uint256S(strHash));

    bool fVerbose = true;
    if (request.params.size() > 1)
        fVerbose = request.params[1].get_bool();

    if (mapBlockIndex.count(hash) == 0)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");

    CBlockIndex* pblockindex = mapBlockIndex[hash];

    if (!fVerbose) {
        CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION);
        ssBlock << pblockindex->GetBlockHeader();
        std::string strHex = HexStr(ssBlock.begin(), ssBlock.end());
        return strHex;
    }

    return blockheaderToJSON(pblockindex);
}

UniValue getsupplyinfo(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "getsupplyinfo ( force_update )\n"
            "\nIf force_update=false (default if no argument is given): return the last cached money supply"
            "\n(sum of spendable transaction outputs) and the height of the chain when it was last updated"
            "\n(it is updated periodically, whenever the chainstate is flushed)."
            "\n"
            "\nIf force_update=true: Flush the chainstate to disk and return the money supply updated to"
            "\nthe current chain height.\n"

            "\nArguments:\n"
            "1. force_update       (boolean, optional, default=false) flush chainstate to disk and update cache\n"

            "\nResult:\n"
            "{\n"
            "  \"updateheight\" : n,       (numeric) The chain height when the transparent supply was updated\n"
            "  \"transparentsupply\" : n   (numeric) The sum of all spendable transaction outputs at height updateheight\n"
            "  \"shieldsupply\": n         (numeric) Chain tip shield pool value\n"
            "  \"totalsupply\": n          (numeric) The sum of transparentsupply and shieldsupply\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("getsupplyinfo", "") + HelpExampleCli("getsupplyinfo", "true") +
            HelpExampleRpc("getsupplyinfo", ""));

    const bool fForceUpdate = request.params.size() > 0 ? request.params[0].get_bool() : false;

    if (fForceUpdate) {
        // Flush state to disk (which updates the cached supply)
        FlushStateToDisk();
    }

    UniValue ret(UniValue::VOBJ);
    const CAmount tSupply = MoneySupply.Get();
    ret.pushKV("updateheight", MoneySupply.GetCacheHeight());
    ret.pushKV("transparentsupply", ValueFromAmount(tSupply));
    Optional<CAmount> shieldedPoolValue = WITH_LOCK(cs_main, return (chainActive.Tip() ? chainActive.Tip()->nChainSaplingValue : nullopt); );
    ret.pushKV("shieldsupply", ValuePoolDesc(shieldedPoolValue, nullopt)["chainValue"]);
    const CAmount totalSupply = tSupply + (shieldedPoolValue ? *shieldedPoolValue : 0);
    ret.pushKV("totalsupply", ValueFromAmount(totalSupply));

    return ret;
}

struct CCoinsStats
{
    int nHeight{0};
    uint256 hashBlock{UINT256_ZERO};
    uint64_t nTransactions{0};
    uint64_t nTransactionOutputs{0};
    uint256 hashSerialized{UINT256_ZERO};
    uint64_t nDiskSize{0};
    CAmount nTotalAmount{0};
};

static void ApplyStats(CCoinsStats &stats, CHashWriter& ss, const uint256& hash, const std::map<uint32_t, Coin>& outputs)
{
    assert(!outputs.empty());
    ss << hash;
    const Coin& coin = outputs.begin()->second;
    ss << VARINT(coin.nHeight * 4 + (coin.fCoinBase ? 2u : 0u) + (coin.fCoinStake ? 1u : 0u));
    stats.nTransactions++;
    for (const auto& output : outputs) {
        ss << VARINT(output.first + 1);
        ss << output.second.out.scriptPubKey;
        ss << VARINT_MODE(output.second.out.nValue, VarIntMode::NONNEGATIVE_SIGNED);
        stats.nTransactionOutputs++;
        stats.nTotalAmount += output.second.out.nValue;
    }
    ss << VARINT(0u);
}

//! Calculate statistics about the unspent transaction output set
static bool GetUTXOStats(CCoinsView *view, CCoinsStats &stats)
{
    std::unique_ptr<CCoinsViewCursor> pcursor(view->Cursor());
    assert(pcursor);

    CHashWriter ss(SER_GETHASH, PROTOCOL_VERSION);
    stats.hashBlock = pcursor->GetBestBlock();
    {
        LOCK(cs_main);
        stats.nHeight = mapBlockIndex.find(stats.hashBlock)->second->nHeight;
    }
    ss << stats.hashBlock;
    uint256 prevkey;
    std::map<uint32_t, Coin> outputs;
    while (pcursor->Valid()) {
        boost::this_thread::interruption_point();
        COutPoint key;
        Coin coin;
        if (pcursor->GetKey(key) && pcursor->GetValue(coin)) {
            if (!outputs.empty() && key.hash != prevkey) {
                ApplyStats(stats, ss, prevkey, outputs);
                outputs.clear();
            }
            prevkey = key.hash;
            outputs[key.n] = std::move(coin);
        } else {
            return error("%s: unable to read value", __func__);
        }
        pcursor->Next();
    }
    if (!outputs.empty()) {
        ApplyStats(stats, ss, prevkey, outputs);
    }
    stats.hashSerialized = ss.GetHash();
    stats.nDiskSize = view->EstimateSize();
    return true;
}

UniValue gettxoutsetinfo(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "gettxoutsetinfo\n"
            "\nReturns statistics about the unspent transaction output set.\n"
            "Note this call may take some time.\n"

            "\nResult:\n"
            "{\n"
            "  \"height\":n,     (numeric) The current block height (index)\n"
            "  \"bestblock\": \"hex\",   (string) the best block hash hex\n"
            "  \"transactions\": n,      (numeric) The number of transactions\n"
            "  \"txouts\": n,            (numeric) The number of output transactions\n"
            "  \"hash_serialized_2\": \"hash\",   (string) The serialized hash\n"
            "  \"disk_size\": n,         (numeric) The estimated size of the chainstate on disk\n"
            "  \"total_amount\": x.xxx          (numeric) The total amount\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("gettxoutsetinfo", "") + HelpExampleRpc("gettxoutsetinfo", ""));

    UniValue ret(UniValue::VOBJ);

    CCoinsStats stats;
    FlushStateToDisk();
    if (GetUTXOStats(pcoinsTip.get(), stats)) {
        ret.pushKV("height", (int64_t)stats.nHeight);
        ret.pushKV("bestblock", stats.hashBlock.GetHex());
        ret.pushKV("transactions", (int64_t)stats.nTransactions);
        ret.pushKV("txouts", (int64_t)stats.nTransactionOutputs);
        ret.pushKV("hash_serialized_2", stats.hashSerialized.GetHex());
        ret.pushKV("total_amount", ValueFromAmount(stats.nTotalAmount));
        ret.pushKV("disk_size", stats.nDiskSize);
    }
    return ret;
}

UniValue gettxout(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 2 || request.params.size() > 3)
        throw std::runtime_error(
            "gettxout \"txid\" n ( include_mempool )\n"
            "\nReturns details about an unspent transaction output.\n"

            "\nArguments:\n"
            "1. \"txid\"       (string, required) The transaction id\n"
            "2. n              (numeric, required) vout value\n"
            "3. include_mempool  (boolean, optional) Whether to included the mem pool\n"

            "\nResult:\n"
            "{\n"
            "  \"bestblock\" : \"hash\",    (string) the block hash\n"
            "  \"confirmations\" : n,       (numeric) The number of confirmations\n"
            "  \"value\" : x.xxx,           (numeric) The transaction value in PIV\n"
            "  \"scriptPubKey\" : {         (json object)\n"
            "     \"asm\" : \"code\",       (string) \n"
            "     \"hex\" : \"hex\",        (string) \n"
            "     \"reqSigs\" : n,          (numeric) Number of required signatures\n"
            "     \"type\" : \"pubkeyhash\", (string) The type, eg pubkeyhash\n"
            "     \"addresses\" : [          (array of string) array of pivx addresses\n"
            "     \"pivxaddress\"            (string) pivx address\n"
            "        ,...\n"
            "     ]\n"
            "  },\n"
            "  \"coinbase\" : true|false   (boolean) Coinbase or not\n"
            "}\n"

            "\nExamples:\n"
            "\nGet unspent transactions\n" +
            HelpExampleCli("listunspent", "") +
            "\nView the details\n" +
            HelpExampleCli("gettxout", "\"txid\" 1") +
            "\nAs a json rpc call\n" +
            HelpExampleRpc("gettxout", "\"txid\", 1"));

    LOCK(cs_main);

    UniValue ret(UniValue::VOBJ);

    std::string strHash = request.params[0].get_str();
    uint256 hash(uint256S(strHash));
    int n = request.params[1].get_int();
    COutPoint out(hash, n);
    bool fMempool = true;
    if (request.params.size() > 2)
        fMempool = request.params[2].get_bool();

    Coin coin;
    if (fMempool) {
        LOCK(mempool.cs);
        CCoinsViewMemPool view(pcoinsTip.get(), mempool);
        if (!view.GetCoin(out, coin) || mempool.isSpent(out)) {// TODO: filtering spent coins should be done by the CCoinsViewMemPool
            return NullUniValue;
        }
    } else {
        if (!pcoinsTip->GetCoin(out, coin)) {
            return NullUniValue;
        }
    }

    BlockMap::iterator it = mapBlockIndex.find(pcoinsTip->GetBestBlock());
    CBlockIndex* pindex = it->second;
    assert(pindex != nullptr);
    ret.pushKV("bestblock", pindex->GetBlockHash().GetHex());
    if (coin.nHeight == MEMPOOL_HEIGHT) {
        ret.pushKV("confirmations", 0);
    } else {
        ret.pushKV("confirmations", (int64_t)(pindex->nHeight - coin.nHeight + 1));
    }
    ret.pushKV("value", ValueFromAmount(coin.out.nValue));
    UniValue o(UniValue::VOBJ);
    ScriptPubKeyToUniv(coin.out.scriptPubKey, o, true);
    ret.pushKV("scriptPubKey", o);
    ret.pushKV("coinbase", (bool)coin.fCoinBase);

    return ret;
}

UniValue verifychain(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "verifychain ( nblocks )\n"
            "\nVerifies blockchain database.\n"

            "\nArguments:\n"
            "1. nblocks    (numeric, optional, default=288, 0=all) The number of blocks to check.\n"

            "\nResult:\n"
            "true|false       (boolean) Verified or not\n"

            "\nExamples:\n" +
            HelpExampleCli("verifychain", "") + HelpExampleRpc("verifychain", ""));

    LOCK(cs_main);

    int nCheckLevel = gArgs.GetArg("-checklevel", DEFAULT_CHECKLEVEL);
    int nCheckDepth = gArgs.GetArg("-checkblocks", DEFAULT_CHECKBLOCKS);
    if (request.params.size() > 0)
        nCheckDepth = request.params[0].get_int();

    return CVerifyDB().VerifyDB(pcoinsTip.get(), nCheckLevel, nCheckDepth);
}

/** Implementation of IsSuperMajority with better feedback */
static UniValue SoftForkMajorityDesc(int version, const CBlockIndex* pindex, const Consensus::Params& consensusParams)
{
    UniValue rv(UniValue::VOBJ);
    Consensus::UpgradeIndex idx;
    switch(version) {
    case 1:
    case 2:
    case 3:
        idx = Consensus::BASE_NETWORK;
        break;
    case 4:
        idx = Consensus::UPGRADE_ZC;
        break;
    case 5:
        idx = Consensus::UPGRADE_BIP65;
        break;
    case 6:
        idx = Consensus::UPGRADE_V3_4;
        break;
    case 7:
        idx = Consensus::UPGRADE_V4_0;
        break;
    default:
        rv.pushKV("status", false);
        return rv;
    }
    rv.pushKV("status", pindex && consensusParams.NetworkUpgradeActive(pindex->nHeight, idx));
    return rv;
}

static UniValue SoftForkDesc(const std::string &name, int version, const CBlockIndex* pindex)
{
    const Consensus::Params& consensus = Params().GetConsensus();
    UniValue rv(UniValue::VOBJ);
    rv.pushKV("id", name);
    rv.pushKV("version", version);
    rv.pushKV("reject", SoftForkMajorityDesc(version, pindex, consensus));
    return rv;
}

static UniValue NetworkUpgradeDesc(const Consensus::Params& consensusParams, Consensus::UpgradeIndex idx, int height)
{
    UniValue rv(UniValue::VOBJ);
    auto upgrade = NetworkUpgradeInfo[idx];
    rv.pushKV("activationheight", consensusParams.vUpgrades[idx].nActivationHeight);
    switch (NetworkUpgradeState(height, consensusParams, idx)) {
        case UPGRADE_DISABLED: rv.pushKV("status", "disabled"); break;
        case UPGRADE_PENDING: rv.pushKV("status", "pending"); break;
        case UPGRADE_ACTIVE: rv.pushKV("status", "active"); break;
    }
    rv.pushKV("info", upgrade.strInfo);
    return rv;
}

void NetworkUpgradeDescPushBack(
        UniValue& networkUpgrades,
        const Consensus::Params& consensusParams,
        Consensus::UpgradeIndex idx,
        int height)
{
    // Network upgrades with an activation height of NO_ACTIVATION_HEIGHT are
    // hidden. This is used when network upgrade implementations are merged
    // without specifying the activation height.
    if (consensusParams.vUpgrades[idx].nActivationHeight != Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT) {
        std::string name = NetworkUpgradeInfo[idx].strName;
        std::replace(name.begin(), name.end(), '_', ' '); // Beautify the name
        networkUpgrades.pushKV(name, NetworkUpgradeDesc(consensusParams, idx, height));
    }
}

UniValue getblockchaininfo(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getblockchaininfo\n"
            "Returns an object containing various state info regarding block chain processing.\n"

            "\nResult:\n"
            "{\n"
            "  \"chain\": \"xxxx\",        (string) current network name (main, test, regtest)\n"
            "  \"blocks\": xxxxxx,         (numeric) the current number of blocks processed in the server\n"
            "  \"headers\": xxxxxx,        (numeric) the current number of headers we have validated\n"
            "  \"bestblockhash\": \"...\", (string) the hash of the currently best block\n"
            "  \"difficulty\": xxxxxx,     (numeric) the current difficulty\n"
            "  \"verificationprogress\": xxxx, (numeric) estimate of verification progress [0..1]\n"
            "  \"chainwork\": \"xxxx\"     (string) total amount of work in active chain, in hexadecimal\n"
            "  \"shield_pool_value\": {  (object) Chain tip shield pool value\n"
            "    \"chainValue\":        (numeric) Total value held by the Sapling circuit up to and including the chain tip\n"
            "    \"valueDelta\":        (numeric) Change in value held by the Sapling circuit over the chain tip block\n"
            "  },\n"
            "  \"initial_block_downloading\": true|false, (boolean) whether the node is in initial block downloading state or not\n"
            "  \"softforks\": [            (array) status of softforks in progress\n"
            "     {\n"
            "        \"id\": \"xxxx\",        (string) name of softfork\n"
            "        \"version\": xx,         (numeric) block version\n"
            "        \"reject\": {           (object) progress toward rejecting pre-softfork blocks\n"
            "           \"status\": xx,       (boolean) true if threshold reached\n"
            "        },\n"
            "     }, ...\n"
            "  ],\n"
            "  \"upgrades\": {                (object) status of network upgrades\n"
            "     \"name\" : {                (string) name of upgrade\n"
            "        \"activationheight\": xxxxxx,  (numeric) block height of activation\n"
            "        \"status\": \"xxxx\",      (string) status of upgrade\n"
            "        \"info\": \"xxxx\",        (string) additional information about upgrade\n"
            "     }, ...\n"
            "}\n"
            "  \"warnings\" : \"...\",         (string) any network and blockchain warnings.\n"

            "\nExamples:\n" +
            HelpExampleCli("getblockchaininfo", "") + HelpExampleRpc("getblockchaininfo", ""));

    LOCK(cs_main);

    const Consensus::Params& consensusParams = Params().GetConsensus();
    const CBlockIndex* pChainTip = chainActive.Tip();
    int nTipHeight = pChainTip ? pChainTip->nHeight : -1;

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("chain", Params().NetworkIDString());
    obj.pushKV("blocks", nTipHeight);
    obj.pushKV("headers", pindexBestHeader ? pindexBestHeader->nHeight : -1);
    obj.pushKV("bestblockhash", pChainTip ? pChainTip->GetBlockHash().GetHex() : "");
    obj.pushKV("difficulty", (double)GetDifficulty());
    obj.pushKV("verificationprogress", Checkpoints::GuessVerificationProgress(pChainTip));
    obj.pushKV("chainwork", pChainTip ? pChainTip->nChainWork.GetHex() : "");
    // Sapling shield pool value
    obj.pushKV("shield_pool_value", pChainTip ? ValuePoolDesc(pChainTip->nChainSaplingValue, pChainTip->nSaplingValue) : 0);
    obj.pushKV("initial_block_downloading", IsInitialBlockDownload());
    UniValue softforks(UniValue::VARR);
    softforks.push_back(SoftForkDesc("bip65", 5, pChainTip));
    obj.pushKV("softforks",             softforks);
    UniValue upgrades(UniValue::VOBJ);
    for (int i = Consensus::BASE_NETWORK + 1; i < (int) Consensus::MAX_NETWORK_UPGRADES; i++) {
        NetworkUpgradeDescPushBack(upgrades, consensusParams, Consensus::UpgradeIndex(i), nTipHeight);
    }
    obj.pushKV("upgrades", upgrades);
    obj.pushKV("warnings", GetWarnings("statusbar"));
    return obj;
}

/** Comparison function for sorting the getchaintips heads.  */
struct CompareBlocksByHeight {
    bool operator()(const CBlockIndex* a, const CBlockIndex* b) const
    {
        /* Make sure that unequal blocks with the same height do not compare
           equal. Use the pointers themselves to make a distinction. */

        if (a->nHeight != b->nHeight)
            return (a->nHeight > b->nHeight);

        return a < b;
    }
};

UniValue getchaintips(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getchaintips\n"
            "Return information about all known tips in the block tree,"
            " including the main chain as well as orphaned branches.\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"height\": xxxx,         (numeric) height of the chain tip\n"
            "    \"hash\": \"xxxx\",         (string) block hash of the tip\n"
            "    \"branchlen\": 0          (numeric) zero for main chain\n"
            "    \"status\": \"active\"      (string) \"active\" for the main chain\n"
            "  },\n"
            "  {\n"
            "    \"height\": n,            (numeric) height of the chain tip\n"
            "    \"hash\": \"hash\",       (string) block hash of the tip\n"
            "    \"branchlen\": n,         (numeric) length of branch connecting the tip to the main chain\n"
            "    \"status\": \"xxxx\"      (string) status of the chain (active, valid-fork, valid-headers, headers-only, invalid)\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "Possible values for status:\n"
            "1.  \"invalid\"               This branch contains at least one invalid block\n"
            "2.  \"headers-only\"          Not all blocks for this branch are available, but the headers are valid\n"
            "3.  \"valid-headers\"         All blocks are available for this branch, but they were never fully validated\n"
            "4.  \"valid-fork\"            This branch is not part of the active chain, but is fully validated\n"
            "5.  \"active\"                This is the tip of the active main chain, which is certainly valid\n"

            "\nExamples:\n" +
            HelpExampleCli("getchaintips", "") + HelpExampleRpc("getchaintips", ""));

    LOCK(cs_main);

    /* Build up a list of chain tips.  We start with the list of all
       known blocks, and successively remove blocks that appear as pprev
       of another block.  */
    std::set<const CBlockIndex*, CompareBlocksByHeight> setTips;
    for (const std::pair<const uint256, CBlockIndex*> & item : mapBlockIndex)
        setTips.insert(item.second);
    for (const std::pair<const uint256, CBlockIndex*> & item : mapBlockIndex) {
        const CBlockIndex* pprev = item.second->pprev;
        if (pprev)
            setTips.erase(pprev);
    }

    // Always report the currently active tip.
    setTips.insert(chainActive.Tip());

    /* Construct the output array.  */
    UniValue res(UniValue::VARR);
    for (const CBlockIndex* block : setTips) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("height", block->nHeight);
        obj.pushKV("hash", block->phashBlock->GetHex());

        const int branchLen = block->nHeight - chainActive.FindFork(block)->nHeight;
        obj.pushKV("branchlen", branchLen);

        std::string status;
        if (chainActive.Contains(block)) {
            // This block is part of the currently active chain.
            status = "active";
        } else if (block->nStatus & BLOCK_FAILED_MASK) {
            // This block or one of its ancestors is invalid.
            status = "invalid";
        } else if (block->nChainTx == 0) {
            // This block cannot be connected because full block data for it or one of its parents is missing.
            status = "headers-only";
        } else if (block->IsValid(BLOCK_VALID_SCRIPTS)) {
            // This block is fully validated, but no longer part of the active chain. It was probably the active block once, but was reorganized.
            status = "valid-fork";
        } else if (block->IsValid(BLOCK_VALID_TREE)) {
            // The headers for this block are valid, but it has not been validated. It was probably never part of the most-work chain.
            status = "valid-headers";
        } else {
            // No clue.
            status = "unknown";
        }
        obj.pushKV("status", status);

        res.push_back(obj);
    }

    return res;
}

UniValue mempoolInfoToJSON()
{
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("loaded", mempool.IsLoaded());
    ret.pushKV("size", (int64_t) mempool.size());
    ret.pushKV("bytes", (int64_t) mempool.GetTotalTxSize());
    ret.pushKV("usage", (int64_t) mempool.DynamicMemoryUsage());
    size_t maxmempool = gArgs.GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000;
    ret.pushKV("mempoolminfee", ValueFromAmount(std::max(mempool.GetMinFee(maxmempool), ::minRelayTxFee).GetFeePerK()));
    ret.pushKV("minrelaytxfee", ValueFromAmount(::minRelayTxFee.GetFeePerK()));

    return ret;
}

UniValue getmempoolinfo(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getmempoolinfo\n"
            "\nReturns details on the active state of the TX memory pool.\n"

            "\nResult:\n"
            "{\n"
            "  \"loaded\": true|false         (boolean) True if the mempool is fully loaded\n"
            "  \"size\": xxxxx                (numeric) Current tx count\n"
            "  \"bytes\": xxxxx               (numeric) Sum of all tx sizes\n"
            "  \"usage\": xxxxx               (numeric) Total memory usage for the mempool\n"
            "  \"maxmempool\": xxxxx,         (numeric) Maximum memory usage for the mempool\n"
            "  \"mempoolminfee\": xxxxx       (numeric) Minimum fee rate in " + CURRENCY_UNIT + "/kB for tx to be accepted. Is the maximum of minrelaytxfee and minimum mempool fee\n"
            "  \"minrelaytxfee\": xxxxx       (numeric) Current minimum relay fee for transactions\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("getmempoolinfo", "") + HelpExampleRpc("getmempoolinfo", ""));

    return mempoolInfoToJSON();
}

UniValue invalidateblock(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "invalidateblock \"blockhash\"\n"
            "\nPermanently marks a block as invalid, as if it violated a consensus rule.\n"

            "\nArguments:\n"
            "1. blockhash   (string, required) the hash of the block to mark as invalid\n"

            "\nExamples:\n" +
            HelpExampleCli("invalidateblock", "\"blockhash\"") + HelpExampleRpc("invalidateblock", "\"blockhash\""));

    std::string strHash = request.params[0].get_str();
    uint256 hash(uint256S(strHash));
    CValidationState state;

    {
        LOCK(cs_main);
        if (mapBlockIndex.count(hash) == 0)
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");

        CBlockIndex* pblockindex = mapBlockIndex[hash];
        InvalidateBlock(state, Params(), pblockindex);
    }

    if (state.IsValid()) {
        ActivateBestChain(state);
        int nHeight = WITH_LOCK(cs_main, return chainActive.Height(); );
        g_budgetman.SetBestHeight(nHeight);
        mnodeman.SetBestHeight(nHeight);
    }

    if (!state.IsValid()) {
        throw JSONRPCError(RPC_DATABASE_ERROR, state.GetRejectReason());
    }

    return NullUniValue;
}

UniValue reconsiderblock(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "reconsiderblock \"blockhash\"\n"
            "\nRemoves invalidity status of a block and its descendants, reconsider them for activation.\n"
            "This can be used to undo the effects of invalidateblock.\n"

            "\nArguments:\n"
            "1. blockhash   (string, required) the hash of the block to reconsider\n"

            "\nExamples:\n" +
            HelpExampleCli("reconsiderblock", "\"blockhash\"") + HelpExampleRpc("reconsiderblock", "\"blockhash\""));

    std::string strHash = request.params[0].get_str();
    uint256 hash(uint256S(strHash));
    CValidationState state;

    {
        LOCK(cs_main);
        if (mapBlockIndex.count(hash) == 0)
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");

        CBlockIndex* pblockindex = mapBlockIndex[hash];
        ReconsiderBlock(state, pblockindex);
    }

    if (state.IsValid()) {
        ActivateBestChain(state);
        int nHeight = WITH_LOCK(cs_main, return chainActive.Height(); );
        g_budgetman.SetBestHeight(nHeight);
        mnodeman.SetBestHeight(nHeight);
    }

    if (!state.IsValid()) {
        throw JSONRPCError(RPC_DATABASE_ERROR, state.GetRejectReason());
    }

    return NullUniValue;
}

void validaterange(const UniValue& params, int& heightStart, int& heightEnd, int minHeightStart = 1)
{
    if (params.size() < 2) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Not enough parameters in validaterange");
    }

    int nBestHeight;
    {
        LOCK(cs_main);
        nBestHeight = chainActive.Height();
    }

    heightStart = params[0].get_int();
    if (heightStart > nBestHeight) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid starting block (%d). Out of range.", heightStart));
    }

    const int range = params[1].get_int();
    if (range < 1) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid block range. Must be strictly positive.");
    }

    heightEnd = heightStart + range - 1;

    if (heightStart < minHeightStart && heightEnd >= minHeightStart) {
        heightStart = minHeightStart;
    }

    if (heightEnd > nBestHeight) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid ending block (%d). Out of range.", heightEnd));
    }
}

UniValue getblockindexstats(const JSONRPCRequest& request) {
    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
                "getblockindexstats height range\n"
                "\nReturns aggregated BlockIndex data for blocks "
                "\n[height, height+1, height+2, ..., height+range-1]\n"

                "\nArguments:\n"
                "1. height             (numeric, required) block height where the search starts.\n"
                "2. range              (numeric, required) number of blocks to include.\n"

                "\nResult:\n"
                "{\n"
                "  \"first_block\": \"x\"            (integer) First counted block\n"
                "  \"last_block\": \"x\"             (integer) Last counted block\n"
                "  \"txcount\": xxxxx                (numeric) tx count (excluding coinbase/coinstake)\n"
                "  \"txcount_all\": xxxxx            (numeric) tx count (including coinbase/coinstake)\n"
                "  \"txbytes\": xxxxx                (numeric) Sum of the size of all txes over block range\n"
                "  \"ttlfee\": xxxxx                 (numeric) Sum of the fee amount of all txes over block range\n"
                "  \"feeperkb\": xxxxx               (numeric) Average fee per kb (excluding zc txes)\n"
                "}\n"

                "\nExamples:\n" +
                HelpExampleCli("getblockindexstats", "1200000 1000") +
                HelpExampleRpc("getblockindexstats", "1200000, 1000"));

    int heightStart, heightEnd;
    validaterange(request.params, heightStart, heightEnd);
    // return object
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("Starting block", heightStart);
    ret.pushKV("Ending block", heightEnd);

    CAmount nFees = 0;
    int64_t nBytes = 0;
    int64_t nTxCount = 0;
    int64_t nTxCount_all = 0;

    const CBlockIndex* pindex = WITH_LOCK(cs_main, return chainActive[heightEnd]);
    if (!pindex)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "invalid block height");

    while (pindex && pindex->nHeight >= heightStart) {
        CBlock block;
        if (!ReadBlockFromDisk(block, pindex)) {
            throw JSONRPCError(RPC_DATABASE_ERROR, "failed to read block from disk");
        }

        CAmount nValueIn = 0;
        CAmount nValueOut = 0;
        const int ntx = block.vtx.size();
        const int firstTxIndex = block.IsProofOfStake() ? 2 : 1;
        nTxCount_all += ntx;
        nTxCount = nTxCount + ntx - firstTxIndex;

        // loop through each tx in block and save size and fee (except for coinbase/coinstake)
        for (int idx = firstTxIndex; idx < ntx; idx++) {
            const CTransaction& tx = *(block.vtx[idx]);

            // zerocoin txes have fixed fee, don't count them here.
            if (tx.ContainsZerocoins())
                continue;

            // Transaction size
            nBytes += GetSerializeSize(tx, CLIENT_VERSION);

            // Transparent inputs
            for (unsigned int j = 0; j < tx.vin.size(); j++) {
                COutPoint prevout = tx.vin[j].prevout;
                CTransactionRef txPrev;
                uint256 hashBlock;
                if(!GetTransaction(prevout.hash, txPrev, hashBlock, true))
                    throw JSONRPCError(RPC_DATABASE_ERROR, "failed to read tx from disk");
                nValueIn += txPrev->vout[prevout.n].nValue;
            }
            // Shield inputs
            nValueIn += tx.GetShieldedValueIn();

            // Tranparent/Shield outputs
            nValueOut += tx.GetValueOut();

            // update fee
            nFees += nValueIn - nValueOut;
        }
        pindex = pindex->pprev;
    }

    // get fee rate
    CFeeRate nFeeRate = CFeeRate(nFees, nBytes);

    // return UniValue object
    ret.pushKV("txcount", (int64_t)nTxCount);
    ret.pushKV("txcount_all", (int64_t)nTxCount_all);
    ret.pushKV("txbytes", (int64_t)nBytes);
    ret.pushKV("ttlfee", FormatMoney(nFees));
    ret.pushKV("feeperkb", FormatMoney(nFeeRate.GetFeePerK()));

    return ret;
}

UniValue getfeeinfo(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "getfeeinfo blocks\n"
            "\nReturns details of transaction fees over the last n blocks.\n"

            "\nArguments:\n"
            "1. blocks     (int, required) the number of blocks to get transaction data from\n"

            "\nResult:\n"
            "{\n"
            "  \"txcount\": xxxxx                (numeric) Current tx count\n"
            "  \"txbytes\": xxxxx                (numeric) Sum of all tx sizes\n"
            "  \"ttlfee\": xxxxx                 (numeric) Sum of all fees\n"
            "  \"feeperkb\": xxxxx               (numeric) Average fee per kb over the block range\n"
            "  \"rec_highpriorityfee_perkb\": xxxxx    (numeric) Recommended fee per kb to use for a high priority tx\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("getfeeinfo", "5") + HelpExampleRpc("getfeeinfo", "5"));

    int nBlocks = request.params[0].get_int();
    int nBestHeight;
    {
        LOCK(cs_main);
        nBestHeight = chainActive.Height();
    }
    int nStartHeight = nBestHeight - nBlocks;
    if (nBlocks < 0 || nStartHeight <= 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "invalid start height");

    JSONRPCRequest newRequest;
    UniValue newParams(UniValue::VARR);
    newParams.push_back(UniValue(nStartHeight));
    newParams.push_back(UniValue(nBlocks));
    newRequest.params = newParams;

    return getblockindexstats(newRequest);
}

static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         okSafe argNames
  //  --------------------- ------------------------  -----------------------  ------ --------
    { "blockchain",         "getbestblockhash",       &getbestblockhash,       true,  {} },
    { "blockchain",         "getbestsaplinganchor",   &getbestsaplinganchor,   true,  {} },
    { "blockchain",         "getblock",               &getblock,               true,  {"blockhash","verbose"} },
    { "blockchain",         "getblockchaininfo",      &getblockchaininfo,      true,  {} },
    { "blockchain",         "getblockcount",          &getblockcount,          true,  {} },
    { "blockchain",         "getblockhash",           &getblockhash,           true,  {"height"} },
    { "blockchain",         "getblockheader",         &getblockheader,         false, {"blockhash","verbose"} },
    { "blockchain",         "getblockindexstats",     &getblockindexstats,     true,  {"height","range"} },
    { "blockchain",         "getchaintips",           &getchaintips,           true,  {} },
    { "blockchain",         "getdifficulty",          &getdifficulty,          true,  {} },
    { "blockchain",         "getfeeinfo",             &getfeeinfo,             true,  {"blocks"} },
    { "blockchain",         "getmempoolinfo",         &getmempoolinfo,         true,  {} },
    { "blockchain",         "getrawmempool",          &getrawmempool,          true,  {"verbose"} },
    { "blockchain",         "getsupplyinfo",          &getsupplyinfo,          true,  {"force_update"} },
    { "blockchain",         "gettxout",               &gettxout,               true,  {"txid","n","include_mempool"} },
    { "blockchain",         "gettxoutsetinfo",        &gettxoutsetinfo,        true,  {} },
    { "blockchain",         "verifychain",            &verifychain,            true,  {"nblocks"} },

    /* Not shown in help */
    { "hidden",             "invalidateblock",        &invalidateblock,        true,  {"blockhash"} },
    { "hidden",             "reconsiderblock",        &reconsiderblock,        true,  {"blockhash"} },
    { "hidden",             "waitforblock",           &waitforblock,           true,  {"blockhash","timeout"} },
    { "hidden",             "waitforblockheight",     &waitforblockheight,     true,  {"height","timeout"} },
    { "hidden",             "waitfornewblock",        &waitfornewblock,        true,  {"timeout"} },
    { "hidden",             "syncwithvalidationinterfacequeue", &syncwithvalidationinterfacequeue, true,  {} },


};

void RegisterBlockchainRPCCommands(CRPCTable &tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
