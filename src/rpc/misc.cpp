// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "clientversion.h"
#include "httpserver.h"
#include "init.h"
#include "key_io.h"
#include "sapling/key_io_sapling.h"
#include "masternode-sync.h"
#include "net.h"
#include "netbase.h"
#include "rpc/server.h"
#include "spork.h"
#include "timedata.h"
#include "util/system.h"
#ifdef ENABLE_WALLET
#include "wallet/rpcwallet.h"
#include "wallet/wallet.h"
#include "wallet/walletdb.h"
#endif
#include "warnings.h"

#include <stdint.h>

#include <univalue.h>

extern std::vector<CSporkDef> sporkDefs;

/** getinfo depends on getsupplyinfo defined in rpc/blockchain.cpp */
UniValue getsupplyinfo(const JSONRPCRequest& request);

/**
 * @note Do not add or change anything in the information returned by this
 * method. `getinfo` exists for backwards-compatibility only. It combines
 * information from wildly different sources in the program, which is a mess,
 * and is thus planned to be deprecated eventually.
 *
 * Based on the source of the information, new information should be added to:
 * - `getblockchaininfo`,
 * - `getnetworkinfo` or
 * - `getwalletinfo`
 *
 * Or alternatively, create a specific query method for the information.
 **/
UniValue getinfo(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getinfo\n"
            "\nReturns an object containing various state info.\n"

            "\nResult:\n"
            "{\n"
            "  \"version\": xxxxx,             (numeric) the server version\n"
            "  \"protocolversion\": xxxxx,     (numeric) the protocol version\n"
            "  \"services\": \"xxxx\",         (string) The network services provided by this client\n"
            "  \"walletversion\": xxxxx,       (numeric) the wallet version\n"
            "  \"balance\": xxxxxxx,           (numeric) the total pivx balance of the wallet\n"
            "  \"staking status\": true|false, (boolean) if the wallet is staking or not\n"
            "  \"blocks\": xxxxxx,             (numeric) the current number of blocks processed in the server\n"
            "  \"timeoffset\": xxxxx,          (numeric) the time offset\n"
            "  \"connections\": xxxxx,         (numeric) the number of connections\n"
            "  \"proxy\": \"host:port\",       (string, optional) the proxy used by the server\n"
            "  \"difficulty\": xxxxxx,         (numeric) the current difficulty\n"
            "  \"testnet\": true|false,        (boolean) if the server is using testnet or not\n"
            "  \"moneysupply\": n              (numeric) The sum of transparentsupply and shieldedsupply\n"
            "  \"transparentsupply\" : n       (numeric) The sum of the value of all unspent outputs when the chainstate was\n"
            "                                            last flushed to disk (use getsupplyinfo to know the update-height, or\n"
            "                                            to trigger the money supply update/recalculation)"
            "  \"shieldsupply\": n             (numeric) Chain tip shield pool value\n"
            "  \"keypoololdest\": xxxxxx,      (numeric) the timestamp (seconds since GMT epoch) of the oldest pre-generated key in the key pool\n"
            "  \"keypoolsize\": xxxx,          (numeric) how many new keys are pre-generated\n"
            "  \"unlocked_until\": ttt,        (numeric) the timestamp in seconds since epoch (midnight Jan 1 1970 GMT) that the wallet is unlocked for transfers, or 0 if the wallet is locked\n"
            "  \"paytxfee\": x.xxxx,           (numeric) the transaction fee set in " + CURRENCY_UNIT + "/kB\n"
            "  \"relayfee\": x.xxxx,           (numeric) minimum relay fee for transactions in " + CURRENCY_UNIT + "/kB\n"
            "  \"errors\": \"...\"             (string) any error messages\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("getinfo", "") + HelpExampleRpc("getinfo", ""));

#ifdef ENABLE_WALLET
    LOCK2(cs_main, pwallet ? &pwallet->cs_wallet : NULL);
#else
    LOCK(cs_main);
#endif

    std::string services;
    for (int i = 0; i < 8; i++) {
        uint64_t check = 1 << i;
        if (g_connman->GetLocalServices() & check) {
            switch (check) {
                case NODE_NETWORK:
                    services+= "NETWORK/";
                    break;
                case NODE_BLOOM:
                case NODE_BLOOM_WITHOUT_MN:
                    services+= "BLOOM/";
                    break;
                default:
                    services+= "UNKNOWN/";
            }
        }
    }

    proxyType proxy;
    GetProxy(NET_IPV4, proxy);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("version", CLIENT_VERSION);
    obj.pushKV("protocolversion", PROTOCOL_VERSION);
    obj.pushKV("services", services);
#ifdef ENABLE_WALLET
    if (pwallet) {
        obj.pushKV("walletversion", pwallet->GetVersion());
        obj.pushKV("balance", ValueFromAmount(pwallet->GetAvailableBalance()));
        obj.pushKV("staking status", (pwallet->pStakerStatus->IsActive() ? "Staking Active" : "Staking Not Active"));
    }
#endif
    obj.pushKV("blocks", (int)chainActive.Height());
    obj.pushKV("timeoffset", GetTimeOffset());
    if(g_connman)
        obj.pushKV("connections", (int)g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL));
    obj.pushKV("proxy", (proxy.IsValid() ? proxy.proxy.ToStringIPPort() : std::string()));
    obj.pushKV("difficulty", (double)GetDifficulty());
    obj.pushKV("testnet", Params().IsTestnet());

    // Add (cached) money supply via getsupplyinfo RPC
    UniValue supply_info = getsupplyinfo(JSONRPCRequest());
    obj.pushKV("moneysupply", supply_info["totalsupply"]);
    obj.pushKV("transparentsupply", supply_info["transparentsupply"]);
    obj.pushKV("shieldsupply", supply_info["shieldsupply"]);

#ifdef ENABLE_WALLET
    if (pwallet) {
        obj.pushKV("keypoololdest", pwallet->GetOldestKeyPoolTime());
        size_t kpExternalSize = pwallet->KeypoolCountExternalKeys();
        obj.pushKV("keypoolsize", (int64_t)kpExternalSize);
    }
    if (pwallet && pwallet->IsCrypted())
        obj.pushKV("unlocked_until", pwallet->nRelockTime);
    obj.pushKV("paytxfee", ValueFromAmount(payTxFee.GetFeePerK()));
#endif
    obj.pushKV("relayfee", ValueFromAmount(::minRelayTxFee.GetFeePerK()));
    obj.pushKV("errors", GetWarnings("statusbar"));
    return obj;
}

UniValue mnsync(const JSONRPCRequest& request)
{
    std::string strMode;
    if (request.params.size() == 1)
        strMode = request.params[0].get_str();

    if (request.fHelp || request.params.size() != 1 || (strMode != "status" && strMode != "reset")) {
        throw std::runtime_error(
            "mnsync \"status|reset\"\n"
            "\nReturns the sync status or resets sync.\n"

            "\nArguments:\n"
            "1. \"mode\"    (string, required) either 'status' or 'reset'\n"

            "\nResult ('status' mode):\n"
            "{\n"
            "  \"IsBlockchainSynced\": true|false,    (boolean) 'true' if blockchain is synced\n"
            "  \"lastMasternodeList\": xxxx,        (numeric) Timestamp of last MN list message\n"
            "  \"lastMasternodeWinner\": xxxx,      (numeric) Timestamp of last MN winner message\n"
            "  \"lastBudgetItem\": xxxx,            (numeric) Timestamp of last MN budget message\n"
            "  \"lastFailure\": xxxx,           (numeric) Timestamp of last failed sync\n"
            "  \"nCountFailures\": n,           (numeric) Number of failed syncs (total)\n"
            "  \"sumMasternodeList\": n,        (numeric) Number of MN list messages (total)\n"
            "  \"sumMasternodeWinner\": n,      (numeric) Number of MN winner messages (total)\n"
            "  \"sumBudgetItemProp\": n,        (numeric) Number of MN budget messages (total)\n"
            "  \"sumBudgetItemFin\": n,         (numeric) Number of MN budget finalization messages (total)\n"
            "  \"countMasternodeList\": n,      (numeric) Number of MN list messages (local)\n"
            "  \"countMasternodeWinner\": n,    (numeric) Number of MN winner messages (local)\n"
            "  \"countBudgetItemProp\": n,      (numeric) Number of MN budget messages (local)\n"
            "  \"countBudgetItemFin\": n,       (numeric) Number of MN budget finalization messages (local)\n"
            "  \"RequestedMasternodeAssets\": n, (numeric) Status code of last sync phase\n"
            "  \"RequestedMasternodeAttempt\": n, (numeric) Status code of last sync attempt\n"
            "}\n"

            "\nResult ('reset' mode):\n"
            "\"status\"     (string) 'success'\n"

            "\nExamples:\n" +
            HelpExampleCli("mnsync", "\"status\"") + HelpExampleRpc("mnsync", "\"status\""));
    }

    if (strMode == "status") {
        UniValue obj(UniValue::VOBJ);

        obj.pushKV("IsBlockchainSynced", masternodeSync.IsBlockchainSynced());
        obj.pushKV("lastMasternodeList", masternodeSync.lastMasternodeList);
        obj.pushKV("lastMasternodeWinner", masternodeSync.lastMasternodeWinner);
        obj.pushKV("lastBudgetItem", masternodeSync.lastBudgetItem);
        obj.pushKV("lastFailure", masternodeSync.lastFailure);
        obj.pushKV("nCountFailures", masternodeSync.nCountFailures);
        obj.pushKV("sumMasternodeList", masternodeSync.sumMasternodeList);
        obj.pushKV("sumMasternodeWinner", masternodeSync.sumMasternodeWinner);
        obj.pushKV("sumBudgetItemProp", masternodeSync.sumBudgetItemProp);
        obj.pushKV("sumBudgetItemFin", masternodeSync.sumBudgetItemFin);
        obj.pushKV("countMasternodeList", masternodeSync.countMasternodeList);
        obj.pushKV("countMasternodeWinner", masternodeSync.countMasternodeWinner);
        obj.pushKV("countBudgetItemProp", masternodeSync.countBudgetItemProp);
        obj.pushKV("countBudgetItemFin", masternodeSync.countBudgetItemFin);
        obj.pushKV("RequestedMasternodeAssets", masternodeSync.RequestedMasternodeAssets);
        obj.pushKV("RequestedMasternodeAttempt", masternodeSync.RequestedMasternodeAttempt);

        return obj;
    }

    if (strMode == "reset") {
        masternodeSync.Reset();
        return "success";
    }
    return "failure";
}

#ifdef ENABLE_WALLET
class DescribeAddressVisitor : public boost::static_visitor<UniValue>
{
public:
    CWallet * const pwallet;

    DescribeAddressVisitor(CWallet *_pwallet) : pwallet(_pwallet) {}

    UniValue operator()(const CNoDestination &dest) const { return UniValue(UniValue::VOBJ); }

    UniValue operator()(const CKeyID &keyID) const {
        UniValue obj(UniValue::VOBJ);
        CPubKey vchPubKey;
        obj.pushKV("isscript", false);
        if (pwallet && pwallet->GetPubKey(keyID, vchPubKey)) {
            obj.pushKV("pubkey", HexStr(vchPubKey.Raw()));
            obj.pushKV("iscompressed", vchPubKey.IsCompressed());
        }
        return obj;
    }

    UniValue operator()(const CScriptID &scriptID) const {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("isscript", true);
        CScript subscript;
        if (pwallet && pwallet->GetCScript(scriptID, subscript)) {
            std::vector<CTxDestination> addresses;
            txnouttype whichType;
            int nRequired;
            ExtractDestinations(subscript, whichType, addresses, nRequired);
            obj.pushKV("script", GetTxnOutputType(whichType));
            obj.pushKV("hex", HexStr(subscript));
            UniValue a(UniValue::VARR);
            for (const CTxDestination& addr : addresses)
                a.push_back(EncodeDestination(addr));
            obj.pushKV("addresses", a);
            if (whichType == TX_MULTISIG)
                obj.pushKV("sigsrequired", nRequired);
        }
        return obj;
    }
};
#endif

/*
    Used for updating/reading spork settings on the network
*/
UniValue spork(const JSONRPCRequest& request)
{
    if (request.params.size() == 1 && request.params[0].get_str() == "show") {
        UniValue ret(UniValue::VOBJ);
        for (const auto& sporkDef : sporkDefs) {
            ret.pushKV(sporkDef.name, sporkManager.GetSporkValue(sporkDef.sporkId));
        }
        return ret;
    } else if (request.params.size() == 1 && request.params[0].get_str() == "active") {
        UniValue ret(UniValue::VOBJ);
        for (const auto& sporkDef : sporkDefs) {
            ret.pushKV(sporkDef.name, sporkManager.IsSporkActive(sporkDef.sporkId));
        }
        return ret;
    } else if (request.params.size() == 2) {
        // advanced mode, update spork values
        SporkId nSporkID = sporkManager.GetSporkIDByName(request.params[0].get_str());
        if (nSporkID == SPORK_INVALID) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid spork name");
        }

        // SPORK VALUE
        int64_t nValue = request.params[1].get_int64();

        //broadcast new spork
        if (sporkManager.UpdateSpork(nSporkID, nValue)) {
            return "success";
        } else {
            return "failure";
        }
    }

    throw std::runtime_error(
        "spork \"name\" ( value )\n"
        "\nReturn spork values or their active state.\n"

        "\nArguments:\n"
        "1. \"name\"        (string, required)  \"show\" to show values, \"active\" to show active state.\n"
        "                       When set up as a spork signer, the name of the spork can be used to update it's value.\n"
        "2. value           (numeric, required when updating a spork) The new value for the spork.\n"

        "\nResult (show):\n"
        "{\n"
        "  \"spork_name\": nnn      (key/value) Key is the spork name, value is it's current value.\n"
        "  ,...\n"
        "}\n"

        "\nResult (active):\n"
        "{\n"
        "  \"spork_name\": true|false      (key/value) Key is the spork name, value is a boolean for it's active state.\n"
        "  ,...\n"
        "}\n"

        "\nResult (name):\n"
        " \"success|failure\"       (string) Whether or not the update succeeded.\n"

        "\nExamples:\n" +
        HelpExampleCli("spork", "show") + HelpExampleRpc("spork", "show"));
}

// Every possibly address
typedef boost::variant<libzcash::InvalidEncoding, libzcash::SaplingPaymentAddress, CTxDestination> PPaymentAddress;

class DescribePaymentAddressVisitor : public boost::static_visitor<UniValue>
{
public:
    explicit DescribePaymentAddressVisitor(CWallet *_pwallet, bool _isStaking) : pwallet(_pwallet), isStaking(_isStaking) {}
    UniValue operator()(const libzcash::InvalidEncoding &zaddr) const { return UniValue(UniValue::VOBJ); }

    UniValue operator()(const libzcash::SaplingPaymentAddress &zaddr) const {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("diversifier", HexStr(zaddr.d));
        obj.pushKV("diversifiedtransmissionkey", zaddr.pk_d.GetHex());
#ifdef ENABLE_WALLET
        if (pwallet) {
            obj.pushKV("ismine", pwallet->HaveSpendingKeyForPaymentAddress(zaddr));
        }
#endif
        return obj;
    }

    UniValue operator()(const CTxDestination &dest) const {
        UniValue ret(UniValue::VOBJ);
        CScript scriptPubKey = GetScriptForDestination(dest);
        ret.pushKV("scriptPubKey", HexStr(scriptPubKey));

#ifdef ENABLE_WALLET
        isminetype mine = pwallet ? IsMine(*pwallet, dest) : ISMINE_NO;
        ret.pushKV("ismine", bool(mine & (ISMINE_SPENDABLE_ALL | ISMINE_COLD)));
        ret.pushKV("isstaking", isStaking);
        ret.pushKV("iswatchonly", bool(mine & ISMINE_WATCH_ONLY));
        UniValue detail = boost::apply_visitor(DescribeAddressVisitor(pwallet), dest);
        ret.pushKVs(detail);
        if (pwallet && pwallet->HasAddressBook(dest))
            ret.pushKV("label", pwallet->GetNameForAddressBookEntry(dest));
#endif
        return ret;
    }

private:
    CWallet * const pwallet;
    bool isStaking{false};
};

UniValue validateaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "validateaddress \"pivxaddress\"\n"
            "\nReturn information about the given pivx address.\n"

            "\nArguments:\n"
            "1. \"pivxaddress\"     (string, required) The pivx address to validate\n"

            "\nResult:\n"
            "{\n"
            "  \"isvalid\" : true|false,         (boolean) If the address is valid or not. If not, this is the only property returned.\n"
            "  \"address\" : \"pivxaddress\",    (string) The pivx address validated\n"
            "  \"scriptPubKey\" : \"hex\",       (string) The hex encoded scriptPubKey generated by the address -only if is standard address-\n"
            "  \"ismine\" : true|false,          (boolean) If the address is yours or not\n"
            "  \"isstaking\" : true|false,       (boolean) If the address is a staking address for PIVX cold staking -only if is standard address-\n"
            "  \"iswatchonly\" : true|false,     (boolean) If the address is watchonly -only if standard address-\n"
            "  \"isscript\" : true|false,        (boolean) If the key is a script -only if standard address-\n"
            "  \"hex\" : \"hex\",                (string, optional) The redeemscript for the P2SH address -only if standard address-\n"
            "  \"pubkey\" : \"publickeyhex\",    (string) The hex value of the raw public key -only if standard address-\n"
            "  \"iscompressed\" : true|false,    (boolean) If the address is compressed -only if standard address-\n"
            "  \"label\" : \"label\"             (string) The label associated with the address, \"\" is the default label\n"
            // Sapling
            "  \"diversifier\" : \"hex\",       (string) [sapling] The hex value of the diversifier, d -only if is sapling address-\n"
            "  \"diversifiedtransmissionkey\" : \"hex\", (string) [sapling] The hex value of pk_d -only if is sapling address-\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("validateaddress", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\"") +
            HelpExampleCli("validateaddress", "\"ps1ra969yfhvhp73rw5ak2xvtcm9fkuqsnmad7qln79mphhdrst3lwu9vvv03yuyqlh42p42st47qd\"") +
            HelpExampleRpc("validateaddress", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\""));

#ifdef ENABLE_WALLET
    LOCK2(cs_main, pwallet ? &pwallet->cs_wallet : nullptr);
#else
    LOCK(cs_main);
#endif

    std::string strAddress = request.params[0].get_str();

    // First check if it's a regular address
    bool isStakingAddress = false;
    CTxDestination dest = DecodeDestination(strAddress, isStakingAddress);
    bool isValid = IsValidDestination(dest);

    PPaymentAddress finalAddress;
    if (!isValid) {
        isValid = KeyIO::IsValidPaymentAddressString(strAddress);
        if (isValid) finalAddress = KeyIO::DecodePaymentAddress(strAddress);
    } else {
        finalAddress = dest;
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("isvalid", isValid);
    if (isValid) {
        ret.pushKV("address", strAddress);
        UniValue detail = boost::apply_visitor(DescribePaymentAddressVisitor(pwallet, isStakingAddress), finalAddress);
        ret.pushKVs(detail);
    }

    return ret;
}

/**
 * Used by addmultisigaddress / createmultisig:
 */
CScript _createmultisig_redeemScript(CWallet * const pwallet, const UniValue& params)
{
    int nRequired = params[0].get_int();
    const UniValue& keys = params[1].get_array();

    // Gather public keys
    if (nRequired < 1)
        throw std::runtime_error("a multisignature address must require at least one key to redeem");
    if ((int)keys.size() < nRequired)
        throw std::runtime_error(
            strprintf("not enough keys supplied "
                      "(got %u keys, but need at least %d to redeem)",
                keys.size(), nRequired));
    if (keys.size() > 16)
        throw std::runtime_error("Number of addresses involved in the multisignature address creation > 16\nReduce the number");
    std::vector<CPubKey> pubkeys;
    pubkeys.resize(keys.size());
    for (unsigned int i = 0; i < keys.size(); i++) {
        const std::string& ks = keys[i].get_str();
#ifdef ENABLE_WALLET
        // Case 1: PIVX address and we have full public key:
        CTxDestination dest = DecodeDestination(ks);
        if (pwallet && IsValidDestination(dest)) {
            const CKeyID* keyID = boost::get<CKeyID>(&dest);
            if (!keyID) {
                throw std::runtime_error(
                        strprintf("%s does not refer to a key", ks));
            }
            CPubKey vchPubKey;
            if (!pwallet->GetPubKey(*keyID, vchPubKey))
                throw std::runtime_error(
                    strprintf("no full public key for address %s", ks));
            if (!vchPubKey.IsFullyValid())
                throw std::runtime_error(" Invalid public key: " + ks);
            pubkeys[i] = vchPubKey;
        }

        // Case 2: hex public key
        else
#endif
            if (IsHex(ks)) {
            CPubKey vchPubKey(ParseHex(ks));
            if (!vchPubKey.IsFullyValid())
                throw std::runtime_error(" Invalid public key: " + ks);
            pubkeys[i] = vchPubKey;
        } else {
            throw std::runtime_error(" Invalid public key: " + ks);
        }
    }
    CScript result = GetScriptForMultisig(nRequired, pubkeys);

    if (result.size() > MAX_SCRIPT_ELEMENT_SIZE)
        throw std::runtime_error(
            strprintf("redeemScript exceeds size limit: %d > %d", result.size(), MAX_SCRIPT_ELEMENT_SIZE));

    return result;
}

UniValue createmultisig(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 2)
        throw std::runtime_error(
            "createmultisig nrequired [\"key\",...]\n"
            "\nCreates a multi-signature address with n signature of m keys required.\n"
            "It returns a json object with the address and redeemScript.\n"

            "\nArguments:\n"
            "1. nrequired      (numeric, required) The number of required signatures out of the n keys or addresses.\n"
            "2. \"keys\"       (string, required) A json array of keys which are pivx addresses or hex-encoded public keys\n"
            "     [\n"
            "       \"key\"    (string) pivx address or hex-encoded public key\n"
            "       ,...\n"
            "     ]\n"

            "\nResult:\n"
            "{\n"
            "  \"address\":\"multisigaddress\",  (string) The value of the new multisig address.\n"
            "  \"redeemScript\":\"script\"       (string) The string value of the hex-encoded redemption script.\n"
            "}\n"

            "\nExamples:\n"
            "\nCreate a multisig address from 2 addresses\n" +
            HelpExampleCli("createmultisig", "2 \"[\\\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\\\",\\\"DAD3Y6ivr8nPQLT1NEPX84DxGCw9jz9Jvg\\\"]\"") +
            "\nAs a json rpc call\n" +
            HelpExampleRpc("createmultisig", "2, \"[\\\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\\\",\\\"DAD3Y6ivr8nPQLT1NEPX84DxGCw9jz9Jvg\\\"]\""));

    // Construct using pay-to-script-hash:
    CScript inner = _createmultisig_redeemScript(pwallet, request.params);
    CScriptID innerID(inner);

    UniValue result(UniValue::VOBJ);
    result.pushKV("address", EncodeDestination(innerID));
    result.pushKV("redeemScript", HexStr(inner));

    return result;
}

UniValue verifymessage(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 3)
        throw std::runtime_error(
            "verifymessage \"pivxaddress\" \"signature\" \"message\"\n"
            "\nVerify a signed message\n"

            "\nArguments:\n"
            "1. \"pivxaddress\"  (string, required) The pivx address to use for the signature.\n"
            "2. \"signature\"    (string, required) The signature provided by the signer in base 64 encoding (see signmessage).\n"
            "3. \"message\"      (string, required) The message that was signed.\n"

            "\nResult:\n"
            "true|false   (boolean) If the signature is verified or not.\n"

            "\nExamples:\n"
            "\nUnlock the wallet for 30 seconds\n" +
            HelpExampleCli("walletpassphrase", "\"mypassphrase\" 30") +
            "\nCreate the signature\n" +
            HelpExampleCli("signmessage", "\"DAD3Y6ivr8nPQLT1NEPX84DxGCw9jz9Jvg\" \"my message\"") +
            "\nVerify the signature\n" +
            HelpExampleCli("verifymessage", "\"DAD3Y6ivr8nPQLT1NEPX84DxGCw9jz9Jvg\" \"signature\" \"my message\"") +
            "\nAs json rpc\n" +
            HelpExampleRpc("verifymessage", "\"DAD3Y6ivr8nPQLT1NEPX84DxGCw9jz9Jvg\", \"signature\", \"my message\""));

    LOCK(cs_main);

    std::string strAddress = request.params[0].get_str();
    std::string strSign = request.params[1].get_str();
    std::string strMessage = request.params[2].get_str();

    CTxDestination destination = DecodeDestination(strAddress);
    if (!IsValidDestination(destination))
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

    const CKeyID* keyID = boost::get<CKeyID>(&destination);
    if (!keyID) {
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");
    }

    bool fInvalid = false;
    std::vector<unsigned char> vchSig = DecodeBase64(strSign.c_str(), &fInvalid);

    if (fInvalid)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Malformed base64 encoding");

    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    CPubKey pubkey;
    if (!pubkey.RecoverCompact(ss.GetHash(), vchSig))
        return false;

    return (pubkey.GetID() == *keyID);
}

UniValue setmocktime(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "setmocktime timestamp\n"
            "\nSet the local time to given timestamp (-regtest only)\n"

            "\nArguments:\n"
            "1. timestamp  (numeric, required) Unix seconds-since-epoch timestamp\n"
            "   Pass 0 to go back to using the system time.");

    if (!Params().IsRegTestNet())
        throw std::runtime_error("setmocktime for regression testing (-regtest mode) only");

    // For now, don't change mocktime if we're in the middle of validation, as
    // this could have an effect on mempool time-based eviction, as well as
    // IsCurrentForFeeEstimation() and IsInitialBlockDownload().
    // TODO: figure out the right way to synchronize around mocktime, and
    // ensure all callsites of GetTime() are accessing this safely.
    LOCK(cs_main);

    RPCTypeCheck(request.params, {UniValue::VNUM});
    SetMockTime(request.params[0].get_int64());

    return NullUniValue;
}

void EnableOrDisableLogCategories(UniValue cats, bool enable) {
    cats = cats.get_array();
    for (unsigned int i = 0; i < cats.size(); ++i) {
        std::string cat = cats[i].get_str();

        bool success;
        if (enable) {
            success = g_logger->EnableCategory(cat);
        } else {
            success = g_logger->DisableCategory(cat);
        }

        if (!success)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "unknown logging category " + cat);
    }
}

UniValue logging(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 2) {
        throw std::runtime_error(
            "logging [include,...] ( [exclude,...] )\n"
            "Gets and sets the logging configuration.\n"
            "When called without an argument, returns the list of categories that are currently being debug logged.\n"
            "When called with arguments, adds or removes categories from debug logging.\n"
            "The valid logging categories are: " + ListLogCategories() + "\n"
            "libevent logging is configured on startup and cannot be modified by this RPC during runtime."

            "Arguments:\n"
            "1. \"include\" (array of strings) add debug logging for these categories.\n"
            "2. \"exclude\" (array of strings) remove debug logging for these categories.\n"

            "\nResult:\n"
            "{                            (object): a JSON object of the logging categories that are active.\n"
            "  \"category\": fEnabled,    (key/value) Key is the category name, value is a boolean of it's active state.\n"
            "  ...,\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("logging", "\"[\\\"all\\\"]\" \"[\\\"http\\\"]\"")
            + HelpExampleRpc("logging", "[\"all\"], \"[libevent]\"")
        );
    }

    uint32_t original_log_categories = g_logger->GetCategoryMask();
    if (request.params.size() > 0 && request.params[0].isArray()) {
        EnableOrDisableLogCategories(request.params[0], true);
    }

    if (request.params.size() > 1 && request.params[1].isArray()) {
        EnableOrDisableLogCategories(request.params[1], false);
    }
    uint32_t updated_log_categories = g_logger->GetCategoryMask();
    uint32_t changed_log_categories = original_log_categories ^ updated_log_categories;

    // Update libevent logging if BCLog::LIBEVENT has changed.
    // If the library version doesn't allow it, UpdateHTTPServerLogging() returns false,
    // in which case we should clear the BCLog::LIBEVENT flag.
    // Throw an error if the user has explicitly asked to change only the libevent
    // flag and it failed.
    if (changed_log_categories & BCLog::LIBEVENT) {
        if (!UpdateHTTPServerLogging(g_logger->WillLogCategory(BCLog::LIBEVENT))) {
            g_logger->DisableCategory(BCLog::LIBEVENT);
            if (changed_log_categories == BCLog::LIBEVENT) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "libevent logging cannot be updated when using libevent before v2.1.1.");
            }
        }
    }

    UniValue result(UniValue::VOBJ);
    std::vector<CLogCategoryActive> vLogCatActive = ListActiveLogCategories();
    for (const auto& logCatActive : vLogCatActive) {
        result.pushKV(logCatActive.category, logCatActive.active);
    }

    return result;
}

static UniValue RPCLockedMemoryInfo()
{
    LockedPool::Stats stats = LockedPoolManager::Instance().stats();
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("used", uint64_t(stats.used));
    obj.pushKV("free", uint64_t(stats.free));
    obj.pushKV("total", uint64_t(stats.total));
    obj.pushKV("locked", uint64_t(stats.locked));
    obj.pushKV("chunks_used", uint64_t(stats.chunks_used));
    obj.pushKV("chunks_free", uint64_t(stats.chunks_free));
    return obj;
}

UniValue getmemoryinfo(const JSONRPCRequest& request)
{
    /* Please, avoid using the word "pool" here in the RPC interface or help,
     * as users will undoubtedly confuse it with the other "memory pool"
     */
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getmemoryinfo\n"
            "Returns an object containing information about memory usage.\n"
            "\nResult:\n"
            "{\n"
            "  \"locked\": {               (json object) Information about locked memory manager\n"
            "    \"used\": xxxxx,          (numeric) Number of bytes used\n"
            "    \"free\": xxxxx,          (numeric) Number of bytes available in current arenas\n"
            "    \"total\": xxxxxxx,       (numeric) Total number of bytes managed\n"
            "    \"locked\": xxxxxx,       (numeric) Amount of bytes that succeeded locking. If this number is smaller than total, locking pages failed at some point and key data could be swapped to disk.\n"
            "    \"chunks_used\": xxxxx,   (numeric) Number allocated chunks\n"
            "    \"chunks_free\": xxxxx,   (numeric) Number unused chunks\n"
            "  }\n"
            "}\n"
            "\nExamples:\n"
            + HelpExampleCli("getmemoryinfo", "")
            + HelpExampleRpc("getmemoryinfo", "")
        );
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("locked", RPCLockedMemoryInfo());
    return obj;
}

UniValue echo(const JSONRPCRequest& request)
{
    if (request.fHelp)
        throw std::runtime_error(
            "echo|echojson \"message\" ...\n"
            "\nSimply echo back the input arguments. This command is for testing.\n"
            "\nThe difference between echo and echojson is that echojson has argument conversion enabled in the client-side table in"
            "pivx-cli and the GUI. There is no server-side difference."
        );

    return request.params;
}

static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         okSafe argNames
  //  --------------------- ------------------------  -----------------------  ------ --------
    { "control",            "getinfo",                &getinfo,                true,  {} }, /* uses wallet if enabled */
    { "control",            "getmemoryinfo",          &getmemoryinfo,          true,  {} },
    { "control",            "mnsync",                 &mnsync,                 true,  {"mode"} },
    { "control",            "spork",                  &spork,                  true,  {"name","value"} },

    { "util",               "createmultisig",         &createmultisig,         true,  {"nrequired","keys"} },
    { "util",               "logging",                &logging,                true,  {"include", "exclude"} },
    { "util",               "validateaddress",        &validateaddress,        true,  {"pivxaddress"} }, /* uses wallet if enabled */
    { "util",               "verifymessage",          &verifymessage,          true,  {"pivxaddress","signature","message"} },

    /* Not shown in help */
    { "hidden",             "echo",                   &echo,                   true,  {"arg0","arg1","arg2","arg3","arg4","arg5","arg6","arg7","arg8","arg9"}},
    { "hidden",             "echojson",               &echo,                   true,  {"arg0","arg1","arg2","arg3","arg4","arg5","arg6","arg7","arg8","arg9"}},
    { "hidden",             "setmocktime",            &setmocktime,            true,  {"timestamp"} },
};

void RegisterMiscRPCCommands(CRPCTable &tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
