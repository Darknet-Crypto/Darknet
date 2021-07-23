// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "amount.h"
#include "blockassembler.h"
#include "chainparams.h"
#include "core_io.h"
#include "init.h"
#include "key_io.h"
#include "miner.h"
#include "net.h"
#include "rpc/server.h"
#include "util/blockstatecatcher.h"
#include "validationinterface.h"
#ifdef ENABLE_WALLET
#include "wallet/rpcwallet.h"
#include "wallet/db.h"
#include "wallet/wallet.h"
#endif

#include <univalue.h>

#ifdef ENABLE_WALLET
UniValue generateBlocks(const Consensus::Params& consensus,
                        CWallet* const pwallet,
                        bool fPoS,
                        const int nGenerate,
                        int nHeight,
                        int nHeightEnd,
                        CScript* coinbaseScript)
{
    UniValue blockHashes(UniValue::VARR);

    BlockStateCatcher sc(UINT256_ZERO);
    sc.registerEvent();
    while (nHeight < nHeightEnd && !ShutdownRequested()) {

        // Get available coins
        std::vector<CStakeableOutput> availableCoins;
        if (fPoS && !pwallet->StakeableCoins(&availableCoins)) {
            throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "No available coins to stake");
        }

        std::unique_ptr<CBlockTemplate> pblocktemplate(fPoS ?
                                                       BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(CScript(), pwallet, true, &availableCoins) :
                                                       CreateNewBlockWithScript(*coinbaseScript, pwallet));
        if (!pblocktemplate.get()) break;
        std::shared_ptr<CBlock> pblock = std::make_shared<CBlock>(pblocktemplate->block);

        if(!fPoS) {
            if (ShutdownRequested()) break;
            if (!SolveBlock(pblock, nHeight + 1)) continue;
        }

        sc.setBlockHash(pblock->GetHash());
        bool res = ProcessNewBlock(pblock, nullptr);
        if (!res || sc.stateErrorFound())
            throw JSONRPCError(RPC_INTERNAL_ERROR, "ProcessNewBlock, block not accepted");

        ++nHeight;
        blockHashes.push_back(pblock->GetHash().GetHex());

        // Check PoS if needed.
        if (!fPoS)
            fPoS = consensus.NetworkUpgradeActive(nHeight + 1, Consensus::UPGRADE_POS);
    }

    const int nGenerated = blockHashes.size();
    if (nGenerated == 0 || (!fPoS && nGenerated < nGenerate))
        throw JSONRPCError(RPC_INTERNAL_ERROR, "Couldn't create new blocks");

    return blockHashes;
}

UniValue generate(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 1)
        throw std::runtime_error(
            "generate nblocks\n"
            "\nMine blocks immediately (before the RPC call returns)\n"
            "\nNote: this function can only be used on the regtest network\n"

            "\nArguments:\n"
            "1. nblocks    (numeric, required) How many blocks to generate.\n"

            "\nResult\n"
            "[ blockhashes ]     (array) hashes of blocks generated\n"

            "\nExamples:\n"
            "\nGenerate 11 blocks\n"
            + HelpExampleCli("generate", "11")
        );

    if (!Params().IsRegTestNet())
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "This method can only be used on regtest");

    const int nGenerate = request.params[0].get_int();
    int nHeightEnd = 0;
    int nHeight = 0;

    {   // Don't keep cs_main locked
        LOCK(cs_main);
        nHeight = chainActive.Height();
        nHeightEnd = nHeight + nGenerate;
    }

    const Consensus::Params& consensus = Params().GetConsensus();
    bool fPoS = consensus.NetworkUpgradeActive(nHeight + 1, Consensus::UPGRADE_POS);
    std::unique_ptr<CReserveKey> reservekey;
    CScript coinbaseScript;

    if (fPoS) {
        // If we are in PoS, wallet must be unlocked.
        EnsureWalletIsUnlocked(pwallet);
    } else {
        // Coinbase key
        reservekey = MakeUnique<CReserveKey>(pwallet);
        CPubKey pubkey;
        if (!reservekey->GetReservedKey(pubkey)) throw JSONRPCError(RPC_INTERNAL_ERROR, "Error: Cannot get key from keypool");
        coinbaseScript = GetScriptForDestination(pubkey.GetID());
    }

    // Create the blocks
    UniValue blockHashes = generateBlocks(consensus,
                                          pwallet,
                                          fPoS,
                                          nGenerate,
                                          nHeight,
                                          nHeightEnd,
                                          &coinbaseScript);

    // mark key as used, only for PoW coinbases
    if (reservekey) {
        // Remove key from key pool
        reservekey->KeepKey();
    }

    return blockHashes;
}

UniValue generatetoaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
                "generatetoaddress nblocks \"address\"\n"
                "\nMine blocks immediately to a specified address (before the RPC call returns)\n"
                "\nArguments:\n"
                "1. nblocks          (numeric, required) How many blocks are generated immediately.\n"
                "2. \"address\"      (string, required) The address to send the newly generated bitcoin to.\n"
                "\nResult\n"
                "[ blockhashes ]     (array) hashes of blocks generated\n"
                "\nExamples:\n"
                "\nGenerate 11 blocks to myaddress\n"
                + HelpExampleCli("generatetoaddress", "11 \"myaddress\"")
        );

    int nGenerate = request.params[0].get_int();
    CTxDestination dest(DecodeDestination(request.params[1].get_str()));
    if (!IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Error: Invalid address");
    }
    CScript coinbaseScript = GetScriptForDestination(dest);

    const Consensus::Params& consensus = Params().GetConsensus();
    int nHeightEnd = 0;
    int nHeight = 0;

    {   // Don't keep cs_main locked
        LOCK(cs_main);
        nHeight = chainActive.Height();
        nHeightEnd = nHeight + nGenerate;
    }

    bool fPoS = consensus.NetworkUpgradeActive(nHeight + 1, Consensus::UPGRADE_POS);
    return generateBlocks(consensus,
                          pwallet,
                          fPoS,
                          nGenerate,
                          nHeight,
                          nHeightEnd,
                          &coinbaseScript);
}

#endif // ENABLE_WALLET

#ifdef ENABLE_MINING_RPC
/**
 * Return average network hashes per second based on the last 'lookup' blocks,
 * or from the last difficulty change if 'lookup' is nonpositive.
 * If 'height' is nonnegative, compute the estimate at the time when a given block was found.
 */
UniValue GetNetworkHashPS(int lookup, int height)
{
    CBlockIndex *pb = chainActive.Tip();

    if (height >= 0 && height < chainActive.Height())
        pb = chainActive[height];

    if (pb == NULL || !pb->nHeight)
        return 0;

    // If lookup is -1, then use blocks since last difficulty change.
    if (lookup <= 0)
        lookup = pb->nHeight % 2016 + 1;

    // If lookup is larger than chain, then set it to chain length.
    if (lookup > pb->nHeight)
        lookup = pb->nHeight;

    CBlockIndex* pb0 = pb;
    int64_t minTime = pb0->GetBlockTime();
    int64_t maxTime = minTime;
    for (int i = 0; i < lookup; i++) {
        pb0 = pb0->pprev;
        int64_t time = pb0->GetBlockTime();
        minTime = std::min(time, minTime);
        maxTime = std::max(time, maxTime);
    }

    // In case there's a situation where minTime == maxTime, we don't want a divide by zero exception.
    if (minTime == maxTime)
        return 0;

    uint256 workDiff = pb->nChainWork - pb0->nChainWork;
    int64_t timeDiff = maxTime - minTime;

    return (int64_t)(workDiff.getdouble() / timeDiff);
}

UniValue getnetworkhashps(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            "getnetworkhashps ( nblocks height )\n"
            "\nReturns the estimated network hashes per second based on the last n blocks.\n"
            "Pass in [blocks] to override # of blocks, -1 specifies since last difficulty change.\n"
            "Pass in [height] to estimate the network speed at the time when a certain block was found.\n"

            "\nArguments:\n"
            "1. nblocks     (numeric, optional, default=120) The number of blocks, or -1 for blocks since last difficulty change.\n"
            "2. height     (numeric, optional, default=-1) To estimate at the time of the given height.\n"

            "\nResult:\n"
            "x             (numeric) Hashes per second estimated\n"
            "\nExamples:\n" +
            HelpExampleCli("getnetworkhashps", "") + HelpExampleRpc("getnetworkhashps", ""));

    LOCK(cs_main);
    return GetNetworkHashPS(request.params.size() > 0 ? request.params[0].get_int() : 120, request.params.size() > 1 ? request.params[1].get_int() : -1);
}

#ifdef ENABLE_WALLET
UniValue getgenerate(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getgenerate\n"
            "\nReturn if the server is set to generate coins or not. The default is false.\n"
            "It is set with the command line argument -gen (or pivx.conf setting gen)\n"
            "It can also be set with the setgenerate call.\n"

            "\nResult\n"
            "true|false      (boolean) If the server is set to generate coins or not\n"

            "\nExamples:\n" +
            HelpExampleCli("getgenerate", "") + HelpExampleRpc("getgenerate", ""));

    LOCK(cs_main);
    return gArgs.GetBoolArg("-gen", false);
}

UniValue setgenerate(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "setgenerate generate ( genproclimit )\n"
            "\nSet 'generate' true or false to turn generation on or off.\n"
            "Generation is limited to 'genproclimit' processors, -1 is unlimited.\n"
            "See the getgenerate call for the current setting.\n"

            "\nArguments:\n"
            "1. generate         (boolean, required) Set to true to turn on generation, false to turn off.\n"
            "2. genproclimit     (numeric, optional) Set the processor limit for when generation is on. Can be -1 for unlimited.\n"

            "\nExamples:\n"
            "\nSet the generation on with a limit of one processor\n" +
            HelpExampleCli("setgenerate", "true 1") +
            "\nCheck the setting\n" + HelpExampleCli("getgenerate", "") +
            "\nTurn off generation\n" + HelpExampleCli("setgenerate", "false") +
            "\nUsing json rpc\n" + HelpExampleRpc("setgenerate", "true, 1"));

    if (Params().IsRegTestNet())
        throw JSONRPCError(RPC_INVALID_REQUEST, "Use the generate method instead of setgenerate on regtest");

    bool fGenerate = true;
    if (request.params.size() > 0)
        fGenerate = request.params[0].get_bool();

    const int nHeight = WITH_LOCK(cs_main, return chainActive.Height() + 1);
    if (fGenerate && Params().GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_POS))
        throw JSONRPCError(RPC_INVALID_REQUEST, "Proof of Work phase has already ended");

    int nGenProcLimit = -1;
    if (request.params.size() > 1) {
        nGenProcLimit = request.params[1].get_int();
        if (nGenProcLimit == 0)
            fGenerate = false;
    }

    gArgs.GetArg("-gen", "") = (fGenerate ? "1" : "0");
    gArgs.GetArg("-genproclimit", "") = itostr(nGenProcLimit);
    GenerateBitcoins(fGenerate, pwallet, nGenProcLimit);

    return NullUniValue;
}

UniValue gethashespersec(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "gethashespersec\n"
            "\nReturns a recent hashes per second performance measurement while generating.\n"
            "See the getgenerate and setgenerate calls to turn generation on and off.\n"

            "\nResult:\n"
            "n            (numeric) The recent hashes per second when generation is on (will return 0 if generation is off)\n"

            "\nExamples:\n" +
            HelpExampleCli("gethashespersec", "") + HelpExampleRpc("gethashespersec", ""));

    if (GetTimeMillis() - nHPSTimerStart > 8000)
        return (int64_t)0;
    return (int64_t)dHashesPerSec;
}
#endif


UniValue getmininginfo(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getmininginfo\n"
            "\nReturns a json object containing mining-related information."

            "\nResult:\n"
            "{\n"
            "  \"blocks\": nnn,             (numeric) The current block\n"
            "  \"currentblocksize\": nnn,   (numeric) The last block size\n"
            "  \"currentblocktx\": nnn,     (numeric) The last block transaction\n"
            "  \"difficulty\": xxx.xxxxx    (numeric) The current difficulty\n"
            "  \"generate\": true|false     (boolean) If the generation is on or off (see getgenerate or setgenerate calls)\n"
            "  \"genproclimit\": n          (numeric) The processor limit for generation. -1 if no generation. (see getgenerate or setgenerate calls)\n"
            "  \"hashespersec\": n          (numeric) The hashes per second of the generation, or 0 if no generation.\n"
            "  \"pooledtx\": n              (numeric) The size of the mem pool\n"
            "  \"testnet\": true|false      (boolean) If using testnet or not\n"
            "  \"chain\": \"xxxx\",         (string) current network name (main, test, regtest)\n"
            "  \"warnings\": \"...\"        (string) any network and blockchain warnings\n"
            "  \"errors\": \"...\"          (string) DEPRECATED. Same as warnings. Only shown when bitcoind is started with -deprecatedrpc=getmininginfo\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("getmininginfo", "") + HelpExampleRpc("getmininginfo", ""));

    LOCK(cs_main);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("blocks", (int)chainActive.Height());
    obj.pushKV("currentblocksize", (uint64_t)nLastBlockSize);
    obj.pushKV("currentblocktx", (uint64_t)nLastBlockTx);
    obj.pushKV("difficulty", (double)GetDifficulty());
    obj.pushKV("genproclimit", (int)gArgs.GetArg("-genproclimit", -1));
    obj.pushKV("networkhashps", getnetworkhashps(request));
    obj.pushKV("pooledtx", (uint64_t)mempool.size());
    obj.pushKV("testnet", Params().IsTestnet());
    obj.pushKV("chain", Params().NetworkIDString());
    obj.pushKV("errors", GetWarnings("statusbar"));
    if (IsDeprecatedRPCEnabled("getmininginfo")) {
        obj.pushKV("errors", GetWarnings("statusbar"));
    } else {
        obj.pushKV("warnings", GetWarnings("statusbar"));
    }
#ifdef ENABLE_WALLET
    obj.pushKV("generate", getgenerate(request));
    obj.pushKV("hashespersec", gethashespersec(request));
#endif
    return obj;
}
#endif // ENABLE_MINING_RPC

// NOTE: Unlike wallet RPC (which use BTC values), mining RPCs follow GBT (BIP 22) in using satoshi amounts
UniValue prioritisetransaction(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
            "prioritisetransaction \"txid\" priority_delta fee_delta\n"
            "Accepts the transaction into mined blocks at a higher (or lower) priority\n"

            "\nArguments:\n"
            "1. \"txid\"       (string, required) The transaction id.\n"
            "2. fee_delta      (numeric, required) The fee value (in satoshis) to add (or subtract, if negative).\n"
            "                  The fee is not actually paid, only the algorithm for selecting transactions into a block\n"
            "                  considers the transaction as it would have paid a higher (or lower) fee.\n"

            "\nResult\n"
            "true              (boolean) Returns true\n"

            "\nExamples:\n" +
            HelpExampleCli("prioritisetransaction", "\"txid\" 10000") + HelpExampleRpc("prioritisetransaction", "\"txid\", 10000"));

    LOCK(cs_main);

    uint256 hash = ParseHashStr(request.params[0].get_str(), "txid");
    CAmount nAmount = request.params[1].get_int64();

    mempool.PrioritiseTransaction(hash, nAmount);
    return true;
}

// NOTE: Assumes a conclusive result; if result is inconclusive, it must be handled by caller
static UniValue BIP22ValidationResult(const CValidationState& state)
{
    if (state.IsValid())
        return NullUniValue;

    std::string strRejectReason = state.GetRejectReason();
    if (state.IsError())
        throw JSONRPCError(RPC_VERIFY_ERROR, strRejectReason);
    if (state.IsInvalid()) {
        if (strRejectReason.empty())
            return "rejected";
        return strRejectReason;
    }
    // Should be impossible
    return "valid?";
}

#ifdef ENABLE_MINING_RPC
UniValue getblocktemplate(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "getblocktemplate ( \"template_request\" )\n"
            "\nIf the request parameters include a 'mode' key, that is used to explicitly select between the default 'template' request or a 'proposal'.\n"
            "It returns data needed to construct a block to work on.\n"
            "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.\n"

            "\nArguments:\n"
            "1. template_request         (json object, optional) A json object in the following spec\n"
            "     {\n"
            "       \"mode\":\"template\"    (string, optional) This must be set to \"template\" or omitted\n"
            "       \"capabilities\":[       (array, optional) A list of strings\n"
            "           \"support\"           (string) client side supported feature, 'longpoll', 'coinbasetxn', 'coinbasevalue', 'proposal', 'serverlist', 'workid'\n"
            "           ,...\n"
            "         ]\n"
            "     }\n"
            "\n"

            "\nResult:\n"
            "{\n"
            "  \"version\" : n,                    (numeric) The block version\n"
            "  \"previousblockhash\" : \"xxxx\",    (string) The hash of current highest block\n"
            "  \"transactions\" : [                (array) contents of non-coinbase transactions that should be included in the next block\n"
            "      {\n"
            "         \"data\" : \"xxxx\",          (string) transaction data encoded in hexadecimal (byte-for-byte)\n"
            "         \"hash\" : \"xxxx\",          (string) hash/id encoded in little-endian hexadecimal\n"
            "         \"depends\" : [              (array) array of numbers \n"
            "             n                        (numeric) transactions before this one (by 1-based index in 'transactions' list) that must be present in the final block if this one is\n"
            "             ,...\n"
            "         ],\n"
            "         \"fee\": n,                   (numeric) difference in value between transaction inputs and outputs (in upiv); for coinbase transactions, this is a negative Number of the total collected block fees (ie, not including the block subsidy); if key is not present, fee is unknown and clients MUST NOT assume there isn't one\n"
            "         \"sigops\" : n,               (numeric) total number of SigOps, as counted for purposes of block limits; if key is not present, sigop count is unknown and clients MUST NOT assume there aren't any\n"
            "         \"required\" : true|false     (boolean) if provided and true, this transaction must be in the final block\n"
            "      }\n"
            "      ,...\n"
            "  ],\n"
            "  \"coinbaseaux\" : {                  (json object) data that should be included in the coinbase's scriptSig content\n"
            "      \"flags\" : \"flags\"            (string) \n"
            "  },\n"
            "  \"coinbasevalue\" : n,               (numeric) maximum allowable input to coinbase transaction, including the generation award and transaction fees (in upiv)\n"
            "  \"coinbasetxn\" : { ... },           (json object) information for coinbase transaction\n"
            "  \"target\" : \"xxxx\",               (string) The hash target\n"
            "  \"mintime\" : xxx,                   (numeric) The minimum timestamp appropriate for next block time in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"mutable\" : [                      (array of string) list of ways the block template may be changed \n"
            "     \"value\"                         (string) A way the block template may be changed, e.g. 'time', 'transactions', 'prevblock'\n"
            "     ,...\n"
            "  ],\n"
            "  \"noncerange\" : \"00000000ffffffff\",   (string) A range of valid nonces\n"
            "  \"sigoplimit\" : n,                 (numeric) limit of sigops in blocks\n"
            "  \"sizelimit\" : n,                  (numeric) limit of block size\n"
            "  \"curtime\" : ttt,                  (numeric) current timestamp in seconds since epoch (Jan 1 1970 GMT)\n"
            "  \"bits\" : \"xxx\",                 (string) compressed target of next block\n"
            "  \"height\" : n                      (numeric) The height of the next block\n"
            "  \"payee\" : \"xxx\",                (string) required payee for the next block\n"
            "  \"payee_amount\" : n,               (numeric) required amount to pay\n"
            "  \"votes\" : [\n                     (array) show vote candidates\n"
            "        { ... }                       (json object) vote candidate\n"
            "        ,...\n"
            "  ],\n"
            "  \"enforce_masternode_payments\" : true|false  (boolean) true, if masternode payments are enforced\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("getblocktemplate", "") + HelpExampleRpc("getblocktemplate", ""));

    LOCK(cs_main);

    std::string strMode = "template";
    UniValue lpval = NullUniValue;
    if (request.params.size() > 0) {
        const UniValue& oparam = request.params[0].get_obj();
        const UniValue& modeval = find_value(oparam, "mode");
        if (modeval.isStr())
            strMode = modeval.get_str();
        else if (modeval.isNull()) {
            /* Do nothing */
        } else
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");
        lpval = find_value(oparam, "longpollid");

        if (strMode == "proposal") {
            const UniValue& dataval = find_value(oparam, "data");
            if (!dataval.isStr())
                throw JSONRPCError(RPC_TYPE_ERROR, "Missing data String key for proposal");

            CBlock block;
            if (!DecodeHexBlk(block, dataval.get_str()))
                throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");

            uint256 hash = block.GetHash();
            BlockMap::iterator mi = mapBlockIndex.find(hash);
            if (mi != mapBlockIndex.end()) {
                CBlockIndex* pindex = mi->second;
                if (pindex->IsValid(BLOCK_VALID_SCRIPTS))
                    return "duplicate";
                if (pindex->nStatus & BLOCK_FAILED_MASK)
                    return "duplicate-invalid";
                return "duplicate-inconclusive";
            }

            CBlockIndex* const pindexPrev = chainActive.Tip();
            // TestBlockValidity only supports blocks built on the current Tip
            if (block.hashPrevBlock != pindexPrev->GetBlockHash())
                return "inconclusive-not-best-prevblk";
            CValidationState state;
            TestBlockValidity(state, block, pindexPrev, false, true);
            return BIP22ValidationResult(state);
        }
    }

    if (strMode != "template")
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid mode");

    if(!g_connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    if (g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL) == 0)
        throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "PIVX is not connected!");

    if (IsInitialBlockDownload())
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "PIVX is downloading blocks...");

    static unsigned int nTransactionsUpdatedLast;

    if (!lpval.isNull()) {
        // Wait to respond until either the best block changes, OR a minute has passed and there are more transactions
        uint256 hashWatchedChain;
        std::chrono::steady_clock::time_point checktxtime;
        unsigned int nTransactionsUpdatedLastLP;

        if (lpval.isStr()) {
            // Format: <hashBestChain><nTransactionsUpdatedLast>
            std::string lpstr = lpval.get_str();

            hashWatchedChain.SetHex(lpstr.substr(0, 64));
            nTransactionsUpdatedLastLP = atoi64(lpstr.substr(64));
        } else {
            // NOTE: Spec does not specify behaviour for non-string longpollid, but this makes testing easier
            hashWatchedChain = chainActive.Tip()->GetBlockHash();
            nTransactionsUpdatedLastLP = nTransactionsUpdatedLast;
        }

        // Release the wallet and main lock while waiting
        LEAVE_CRITICAL_SECTION(cs_main);
        {
            checktxtime = std::chrono::steady_clock::now() + std::chrono::minutes(1);

            WAIT_LOCK(g_best_block_mutex, lock);
            while (g_best_block == hashWatchedChain && IsRPCRunning()) {
                if (g_best_block_cv.wait_until(lock, checktxtime) == std::cv_status::timeout)
                {
                    // Timeout: Check transactions for update
                    if (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLastLP)
                        break;
                    checktxtime += std::chrono::seconds(10);
                }
            }
        }
        ENTER_CRITICAL_SECTION(cs_main);

        if (!IsRPCRunning())
            throw JSONRPCError(RPC_CLIENT_NOT_CONNECTED, "Shutting down");
        // TODO: Maybe recheck connections/IBD and (if something wrong) send an expires-immediately template to stop miners?
    }

    // Update block
    static CBlockIndex* pindexPrev;
    static int64_t nStart;
    static std::unique_ptr<CBlockTemplate> pblocktemplate;
    if (pindexPrev != chainActive.Tip() ||
        (mempool.GetTransactionsUpdated() != nTransactionsUpdatedLast && GetTime() - nStart > 5)) {
        // Clear pindexPrev so future calls make a new block, despite any failures from here on
        pindexPrev = NULL;

        // Store the chainActive.Tip() used before CreateNewBlock, to avoid races
        nTransactionsUpdatedLast = mempool.GetTransactionsUpdated();
        CBlockIndex* pindexPrevNew = chainActive.Tip();
        nStart = GetTime();

        // Create new block
        if (pblocktemplate) {
            pblocktemplate.release();
            pblocktemplate = NULL;
        }
        CScript scriptDummy = CScript() << OP_TRUE;
        pblocktemplate = BlockAssembler(Params(), DEFAULT_PRINTPRIORITY).CreateNewBlock(scriptDummy, pwallet, false);
        if (!pblocktemplate)
            throw JSONRPCError(RPC_OUT_OF_MEMORY, "Out of memory");

        // Need to update only after we know CreateNewBlock succeeded
        pindexPrev = pindexPrevNew;
    }
    CBlock* pblock = &pblocktemplate->block; // pointer for convenience

    // Update nTime
    UpdateTime(pblock, Params().GetConsensus(), pindexPrev);
    pblock->nNonce = 0;

    UniValue aCaps(UniValue::VARR); aCaps.push_back("proposal");

    UniValue transactions(UniValue::VARR);
    std::map<uint256, int64_t> setTxIndex;
    int i = 0;
    for (const auto& txIn : pblock->vtx) {
        const CTransaction& tx = *txIn;
        const uint256& txHash = tx.GetHash();
        setTxIndex[txHash] = i++;

        if (tx.IsCoinBase())
            continue;

        UniValue entry(UniValue::VOBJ);

        entry.pushKV("data", EncodeHexTx(tx));

        entry.pushKV("hash", txHash.GetHex());

        UniValue deps(UniValue::VARR);
        for (const CTxIn& in : tx.vin) {
            if (setTxIndex.count(in.prevout.hash))
                deps.push_back(setTxIndex[in.prevout.hash]);
        }
        entry.pushKV("depends", deps);

        int index_in_template = i - 1;
        entry.pushKV("fee", pblocktemplate->vTxFees[index_in_template]);
        entry.pushKV("sigops", pblocktemplate->vTxSigOps[index_in_template]);

        transactions.push_back(entry);
    }

    UniValue aux(UniValue::VOBJ);
    aux.pushKV("flags", HexStr(COINBASE_FLAGS.begin(), COINBASE_FLAGS.end()));

    arith_uint256& hashTarget = arith_uint256().SetCompact(pblock->nBits);

    static UniValue aMutable(UniValue::VARR);
    if (aMutable.empty()) {
        aMutable.push_back("time");
        aMutable.push_back("transactions");
        aMutable.push_back("prevblock");
    }

    UniValue aVotes(UniValue::VARR);

    UniValue result(UniValue::VOBJ);
    result.pushKV("capabilities", aCaps);
    result.pushKV("version", pblock->nVersion);
    result.pushKV("previousblockhash", pblock->hashPrevBlock.GetHex());
    result.pushKV("transactions", transactions);
    result.pushKV("coinbaseaux", aux);
    result.pushKV("coinbasevalue", (int64_t)pblock->vtx[0]->GetValueOut());
    result.pushKV("longpollid", chainActive.Tip()->GetBlockHash().GetHex() + i64tostr(nTransactionsUpdatedLast));
    result.pushKV("target", hashTarget.GetHex());
    result.pushKV("mintime", (int64_t)pindexPrev->GetMedianTimePast() + 1);
    result.pushKV("mutable", aMutable);
    result.pushKV("noncerange", "00000000ffffffff");
//    result.pushKV("sigoplimit", (int64_t)MAX_BLOCK_SIGOPS);
//    result.pushKV("sizelimit", (int64_t)MAX_BLOCK_SIZE);
    result.pushKV("curtime", pblock->GetBlockTime());
    result.pushKV("bits", strprintf("%08x", pblock->nBits));
    result.pushKV("height", (int64_t)(pindexPrev->nHeight + 1));
    result.pushKV("votes", aVotes);
    result.pushKV("enforce_masternode_payments", true);

    return result;
}
#endif // ENABLE_MINING_RPC

UniValue submitblock(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "submitblock \"hexdata\" ( \"jsonparametersobject\" )\n"
            "\nAttempts to submit new block to network.\n"
            "The 'jsonparametersobject' parameter is currently ignored.\n"
            "See https://en.bitcoin.it/wiki/BIP_0022 for full specification.\n"

            "\nArguments\n"
            "1. \"hexdata\"    (string, required) the hex-encoded block data to submit\n"
            "2. \"parameters\"     (string, optional) object of optional parameters\n"
            "    {\n"
            "      \"workid\" : \"id\"    (string, optional) if the server provided a workid, it MUST be included with submissions\n"
            "    }\n"

            "\nResult:\n"

            "\nExamples:\n" +
            HelpExampleCli("submitblock", "\"mydata\"") + HelpExampleRpc("submitblock", "\"mydata\""));

    std::shared_ptr<CBlock> blockptr = std::make_shared<CBlock>();
    CBlock& block = *blockptr;
    if (!DecodeHexBlk(block, request.params[0].get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block decode failed");

    if (block.vtx.empty() || !block.vtx[0]->IsCoinBase()) {
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Block does not start with a coinbase");
    }

    uint256 hash = block.GetHash();
    bool fBlockPresent = false;
    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(hash);
        if (mi != mapBlockIndex.end()) {
            CBlockIndex* pindex = mi->second;
            if (pindex->IsValid(BLOCK_VALID_SCRIPTS))
                return "duplicate";
            if (pindex->nStatus & BLOCK_FAILED_MASK)
                return "duplicate-invalid";
            // Otherwise, we might only have the header - process the block before returning
            fBlockPresent = true;
        }
    }

    BlockStateCatcher sc(block.GetHash());
    sc.registerEvent();
    bool fAccepted = ProcessNewBlock(blockptr, nullptr);
    if (fBlockPresent) {
        if (fAccepted && !sc.found)
            return "duplicate-inconclusive";
        return "duplicate";
    }
    if (fAccepted) {
        if (!sc.found)
            return "inconclusive";
    }
    return BIP22ValidationResult(sc.state);
}

UniValue estimatefee(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "estimatefee nblocks\n"
            "\nEstimates the approximate fee per kilobyte\n"
            "needed for a transaction to begin confirmation\n"
            "within nblocks blocks.\n"

            "\nArguments:\n"
            "1. nblocks     (numeric)\n"

            "\nResult:\n"
            "n :    (numeric) estimated fee-per-kilobyte\n"
            "\n"
            "-1.0 is returned if not enough transactions and\n"
            "blocks have been observed to make an estimate.\n"

            "\nExample:\n" +
            HelpExampleCli("estimatefee", "6"));

    RPCTypeCheck(request.params, {UniValue::VNUM});

    int nBlocks = request.params[0].get_int();
    if (nBlocks < 1)
        nBlocks = 1;

    CFeeRate feeRate = mempool.estimateFee(nBlocks);
    if (feeRate == CFeeRate(0))
        return -1.0;

    return ValueFromAmount(feeRate.GetFeePerK());
}

UniValue estimatesmartfee(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
                "estimatesmartfee nblocks\n"
                "\nDEPRECATED. WARNING: This interface is unstable and may disappear or change!\n"
                "\nEstimates the approximate fee per kilobyte needed for a transaction to begin\n"
                "confirmation within nblocks blocks if possible and return the number of blocks\n"
                "for which the estimate is valid.\n"
                "\nArguments:\n"
                "1. nblocks     (numeric)\n"
                "\nResult:\n"
                "{\n"
                "  \"feerate\" : x.x,     (numeric) estimate fee-per-kilobyte (in BTC)\n"
                "  \"blocks\" : n         (numeric) block number where estimate was found\n"
                "}\n"
                "\n"
                "A negative value is returned if not enough transactions and blocks\n"
                "have been observed to make an estimate for any number of blocks.\n"
                "However it will not return a value below the mempool reject fee.\n"
                "\nExample:\n"
                + HelpExampleCli("estimatesmartfee", "6")
        );

    RPCTypeCheck(request.params, {UniValue::VNUM});

    int nBlocks = request.params[0].get_int();

    UniValue result(UniValue::VOBJ);
    int answerFound;
    CFeeRate feeRate = mempool.estimateSmartFee(nBlocks, &answerFound);
    result.pushKV("feerate", feeRate == CFeeRate(0) ? -1.0 : ValueFromAmount(feeRate.GetFeePerK()));
    result.pushKV("blocks", answerFound);
    return result;
}

static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         okSafe argNames
  //  --------------------- ------------------------  -----------------------  ------ --------
    { "util",               "estimatefee",            &estimatefee,            true,  {"nblocks"} },
    { "util",               "estimatesmartfee",       &estimatesmartfee,       true,  {"nblocks"} },
    { "mining",             "prioritisetransaction",  &prioritisetransaction,  true,  {"txid","priority_delta","fee_delta"} },

    /* Not shown in help */
#ifdef ENABLE_WALLET
    { "hidden",             "generate",               &generate,               true,  {"nblocks"} },
    { "hidden",             "generatetoaddress",      &generatetoaddress,      true,  {"nblocks","address"} },
#endif
    { "hidden",             "submitblock",            &submitblock,            true,  {"hexdata","parameters"} },
#ifdef ENABLE_MINING_RPC
    { "hidden",             "getblocktemplate",       &getblocktemplate,       true,  {"template_request"} },
    { "hidden",             "getnetworkhashps",       &getnetworkhashps,       true,  {"nblocks","height"} },
    { "hidden",             "getmininginfo",          &getmininginfo,          true,  {} },
#ifdef ENABLE_WALLET
    { "hidden",             "getgenerate",            &getgenerate,            true,  {} },
    { "hidden",             "gethashespersec",        &gethashespersec,        true,  {} },
    { "hidden",             "setgenerate",            &setgenerate,            true,  {"generate","genproclimit"} },
#endif // ENABLE_WALLET
#endif // ENABLE_MINING_RPC

    };

void RegisterMiningRPCCommands(CRPCTable &tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
