// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "wallet/rpcwallet.h"

#include "addressbook.h"
#include "amount.h"
#include "coincontrol.h"
#include "core_io.h"
#include "destination_io.h"
#include "httpserver.h"
#include "key_io.h"
#include "masternode-sync.h"
#include "net.h"
#include "policy/feerate.h"
#include "rpc/server.h"
#include "sapling/sapling_operation.h"
#include "sapling/key_io_sapling.h"
#include "spork.h"
#include "timedata.h"
#include "utilmoneystr.h"
#include "wallet/wallet.h"
#include "wallet/walletdb.h"
#include "wallet/walletutil.h"
#include "zpivchain.h"

#include  <init.h>    // for StartShutdown

#include <stdint.h>
#include <univalue.h>


static const std::string WALLET_ENDPOINT_BASE = "/wallet/";

CWallet* GetWalletForJSONRPCRequest(const JSONRPCRequest& request)
{
    if (request.URI.substr(0, WALLET_ENDPOINT_BASE.size()) == WALLET_ENDPOINT_BASE) {
        // wallet endpoint was used
        std::string requestedWallet = urlDecode(request.URI.substr(WALLET_ENDPOINT_BASE.size()));
        for (CWalletRef pwallet : ::vpwallets) {
            if (pwallet->GetName() == requestedWallet) {
                return pwallet;
            }
        }
        throw JSONRPCError(RPC_WALLET_NOT_FOUND, "Requested wallet does not exist or is not loaded");
    }
    return ::vpwallets.size() == 1 || (request.fHelp && ::vpwallets.size() > 0) ? ::vpwallets[0] : nullptr;
}

std::string HelpRequiringPassphrase(CWallet* const pwallet)
{
    return pwallet && pwallet->IsCrypted() ? "\nRequires wallet passphrase to be set with walletpassphrase call." : "";
}

bool EnsureWalletIsAvailable(CWallet* const pwallet, bool avoidException)
{
    if (pwallet) return true;
    if (avoidException) return false;
    if (::vpwallets.empty()) {
        // Wallet RPC methods are disabled if no wallets are loaded.
        throw JSONRPCError(RPC_METHOD_NOT_FOUND, "Method not found (disabled)");
    }
    throw JSONRPCError(RPC_WALLET_NOT_SPECIFIED,
        "Wallet file not specified (must request wallet RPC through /wallet/<filename> uri-path).");
}

void EnsureWalletIsUnlocked(CWallet* const pwallet, bool fAllowAnonOnly)
{
    if (pwallet->IsLocked() || (!fAllowAnonOnly && pwallet->fWalletUnlockStaking))
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "Error: Please enter the wallet passphrase with walletpassphrase first.");
}

void WalletTxToJSON(const CWalletTx& wtx, UniValue& entry)
{
    int confirms = wtx.GetDepthInMainChain();
    entry.pushKV("confirmations", confirms);
    entry.pushKV("bcconfirmations", confirms);      // DEPRECATED in 4.3.99
    if (wtx.IsCoinBase() || wtx.IsCoinStake())
        entry.pushKV("generated", true);
    if (confirms > 0) {
        entry.pushKV("blockhash", wtx.m_confirm.hashBlock.GetHex());
        entry.pushKV("blockindex", wtx.m_confirm.nIndex);
        entry.pushKV("blocktime", mapBlockIndex[wtx.m_confirm.hashBlock]->GetBlockTime());
    } else {
        entry.pushKV("trusted", wtx.IsTrusted());
    }
    uint256 hash = wtx.GetHash();
    entry.pushKV("txid", hash.GetHex());
    UniValue conflicts(UniValue::VARR);
    for (const uint256& conflict : wtx.GetConflicts())
        conflicts.push_back(conflict.GetHex());
    entry.pushKV("walletconflicts", conflicts);
    entry.pushKV("time", wtx.GetTxTime());
    entry.pushKV("timereceived", (int64_t)wtx.nTimeReceived);
    for (const std::pair<std::string, std::string> & item : wtx.mapValue)
        entry.pushKV(item.first, item.second);
}

std::string LabelFromValue(const UniValue& value)
{
    std::string label = value.get_str();
    if (label == "*")
        throw JSONRPCError(RPC_WALLET_INVALID_LABEL_NAME, "Invalid label name");
    return label;
}

static CTxDestination GetNewAddressFromLabel(CWallet* const pwallet, const std::string purpose, const UniValue &params,
                                             const CChainParams::Base58Type addrType = CChainParams::PUBKEY_ADDRESS)
{
    LOCK2(cs_main, pwallet->cs_wallet);
    // Parse the label first so we don't generate a key if there's an error
    std::string label;
    if (!params.isNull() && params.size() > 0)
        label = LabelFromValue(params[0]);

    CTxDestination address;
    PairResult r = pwallet->getNewAddress(address, label, purpose, addrType);
    if(!r.result)
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, *r.status);
    return address;
}

/** Convert CAddressBookData to JSON record.  */
static UniValue AddressBookDataToJSON(const AddressBook::CAddressBookData& data, const bool verbose)
{
    UniValue ret(UniValue::VOBJ);
    if (verbose) {
        ret.pushKV("name", data.name);
    }
    ret.pushKV("purpose", data.purpose);
    return ret;
}

/** Checks if a CKey is in the given CWallet compressed or otherwise*/
bool HaveKey(const CWallet* wallet, const CKey& key)
{
    CKey key2;
    key2.Set(key.begin(), key.end(), !key.IsCompressed());
    return wallet->HaveKey(key.GetPubKey().GetID()) || wallet->HaveKey(key2.GetPubKey().GetID());
}

UniValue getaddressinfo(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    const std::string example_address = "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\"";

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
                "getaddressinfo ( \"address\" )\n"
                "\nReturn information about the given PIVX address.\n"
                "Some of the information will only be present if the address is in the active wallet.\n"
                "Metadata for shield addresses is available only if the wallet is unlocked.\n"
                "{Result:\n"
                "  \"address\" : \"address\",              (string) The PIVX address validated.\n"
                "  \"isshield\" : true|false,            (boolean) If the address is shield or transparent.\n"
                "  \"scriptPubKey\" : \"hex\",             (string, only if isshield=false) The hex-encoded scriptPubKey generated by the address.\n"
                "  \"ischange\" : true|false,            (boolean) If the transparent address was used for change output.\n"
                "  \"ismine\" : true|false,              (boolean) If the address is yours.\n"
                "  \"iswatchonly\" : true|false,         (boolean) If the address is watchonly.\n"
                "  \"label\" :  \"label\"                  (string) The label associated with the address, \"\" is the default label.\n"
                "  \"timestamp\" : timestamp,            (number, optional) The creation time of the key, if available, expressed in the UNIX epoch time.\n"
                "  \"hdkeypath\" : \"keypath\"             (string, optional) The HD keypath, if the key is HD and available.\n"
                "  \"hdseedid\" : \"<hash160>\"            (string, optional) The Hash160 of the HD seed.\n"
                "  \"hdmasterfingerprint\" : \"<hash160>\" (string, optional) The fingerprint of the master key.\n"
                "  \"labels\"                            (json object) An array of labels associated with the address. Currently limited to one label but returned\n"
                "                                               as an array to keep the API stable if multiple labels are enabled in the future.\n"
                "    [\n"
                "      { (json object of label data)\n"
                "        \"name\" : \"labelname\" (string) The label.\n"
                "        \"purpose\" : \"purpose\" (string) The purpose of the associated address (send or receive).\n"
                "      }\n"
                "    ]\n"
                "}\n"

                "\nExamples:\n" +
                HelpExampleCli("getaddressinfo", example_address) + HelpExampleRpc("getaddressinfo", example_address)
                );

    LOCK(pwallet->cs_wallet);

    const std::string& strAdd = request.params[0].get_str();
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("address", strAdd);

    const CWDestination& dest = Standard::DecodeDestination(strAdd);
    // Make sure the destination is valid
    if (!Standard::IsValidDestination(dest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
    }

    const CTxDestination* pTransDest = Standard::GetTransparentDestination(dest);
    ret.pushKV("isshield", pTransDest == nullptr);

    if (pTransDest) {
        CScript scriptPubKey = GetScriptForDestination(*pTransDest);
        ret.pushKV("scriptPubKey", HexStr(scriptPubKey));
        ret.pushKV("ischange", pwallet->IsChange(scriptPubKey));
    }

    isminetype mine = IsMine(*pwallet, dest);
    ret.pushKV("ismine", bool(mine & ISMINE_SPENDABLE_ALL));
    ret.pushKV("iswatchonly", bool(mine & ISMINE_WATCH_ONLY_ALL));

    // Return label field if existing. Currently only one label can be
    // associated with an address, so the label should be equivalent to the
    // value of the name key/value pair in the labels array below.
    if (pwallet->HasAddressBook(dest)) {
        ret.pushKV("label", pwallet->GetNameForAddressBookEntry(dest));
    }

    ScriptPubKeyMan* spk_man = pwallet->GetScriptPubKeyMan();
    SaplingScriptPubKeyMan* sspk_man = pwallet->GetSaplingScriptPubKeyMan();
    CKeyMetadata* meta = nullptr;

    if (spk_man && pTransDest) {
        // transparent destination
        const CKeyID* keyID = boost::get<CKeyID>(pTransDest);
        if (keyID) {
            auto it = pwallet->mapKeyMetadata.find(*keyID);
            if(it != pwallet->mapKeyMetadata.end()) {
                meta = &it->second;
            }
        }
    } else if (sspk_man && !pTransDest) {
        // shield destination
        const libzcash::SaplingPaymentAddress pa = *Standard::GetShieldedDestination(dest);
        libzcash::SaplingExtendedSpendingKey extsk;
        if (pwallet->GetSaplingExtendedSpendingKey(pa, extsk)) {
            const auto& ivk = extsk.expsk.full_viewing_key().in_viewing_key();
            auto it = sspk_man->mapSaplingZKeyMetadata.find(ivk);
            if (it != sspk_man->mapSaplingZKeyMetadata.end()) {
                meta = &it->second;
            }
        }
    }

    // Add metadata
    if (meta) {
        ret.pushKV("timestamp", meta->nCreateTime);
        if (meta->HasKeyOrigin()) {
            ret.pushKV("hdkeypath", meta->key_origin.pathToString());
            ret.pushKV("hdseedid", meta->hd_seed_id.GetHex());
            ret.pushKV("hdmasterfingerprint", HexStr(meta->key_origin.fingerprint, meta->key_origin.fingerprint + 4));
        }
    }

    // Return a `labels` array containing the label associated with the address,
    // equivalent to the `label` field above. Currently only one label can be
    // associated with an address, but we return an array so the API remains
    // stable if we allow multiple labels to be associated with an address in
    // the future.
    //
    // DEPRECATED: The previous behavior of returning an array containing a JSON
    // object of `name` and `purpose` key/value pairs has been deprecated.
    UniValue labels(UniValue::VARR);
    auto addrBookData = pwallet->GetAddressBookEntry(dest);
    if (addrBookData) {
        labels.push_back(AddressBookDataToJSON(*addrBookData, true));
    }
    ret.pushKV("labels", std::move(labels));

    return ret;
}

UniValue getaddressesbylabel(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "getaddressesbylabel \"label\"\n"
            "\nReturns the list of addresses assigned the specified label.\n"

            "\nArguments:\n"
            "1. \"label\"  (string, required) The label.\n"

            "\nResult:\n"
            "{ (json object with addresses as keys)\n"
            "  \"address\": { (json object with information about address)\n"
            "    \"purpose\": \"string\" (string)  Purpose of address (\"send\" for sending address, \"receive\" for receiving address)\n"
            "  },...\n"
            "}\n"

            "\nExamples:\n"
            + HelpExampleCli("getaddressesbylabel", "\"tabby\"")
            + HelpExampleRpc("getaddressesbylabel", "\"tabby\"")
        );

    LOCK(pwallet->cs_wallet);

    std::string label = LabelFromValue(request.params[0]);

    // Find all addresses that have the given label
    UniValue ret(UniValue::VOBJ);
    for (auto it = pwallet->NewAddressBookIterator(); it.IsValid(); it.Next()) {
        auto addrBook = it.GetValue();
        if (!addrBook.isShielded() && addrBook.name == label) {
            ret.pushKV(EncodeDestination(*it.GetCTxDestKey(), AddressBook::IsColdStakingPurpose(addrBook.purpose)), AddressBookDataToJSON(addrBook, false));
        }
    }

    if (ret.empty()) {
        throw JSONRPCError(RPC_WALLET_INVALID_LABEL_NAME, std::string("No addresses with label " + label));
    }

    return ret;
}

UniValue listlabels(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "listlabels ( \"purpose\" )\n"
            "\nReturns the list of all labels, or labels that are assigned to addresses with a specific purpose.\n"

            "\nArguments:\n"
            "1. \"purpose\"    (string, optional) Address purpose to list labels for ('send','receive', 'delegable', 'delegator', 'coldstaking', 'coldstaking_send', 'refund'). An empty string is the same as not providing this argument.\n"

            "\nResult:\n"
            "[               (json array of string)\n"
            "  \"label\",      (string) Label name\n"
            "  ...\n"
            "]\n"

            "\nExamples:\n"
            "\nList all labels\n"
            + HelpExampleCli("listlabels", "") +
            "\nList labels that have receiving addresses\n"
            + HelpExampleCli("listlabels", "receive") +
            "\nList labels that have sending addresses\n"
            + HelpExampleCli("listlabels", "send") +
            "\nAs json rpc call\n"
            + HelpExampleRpc("listlabels", "receive")
        );

    LOCK(pwallet->cs_wallet);

    std::string purpose;
    if (!request.params[0].isNull()) {
        purpose = request.params[0].get_str();
    }

    // Add to a set to sort by label name, then insert into Univalue array
    std::set<std::string> label_set;
    for (auto it = pwallet->NewAddressBookIterator(); it.IsValid(); it.Next()) {
        auto addrBook = it.GetValue();
        if (purpose.empty() || addrBook.purpose == purpose) {
            label_set.insert(addrBook.name);
        }
    }

    UniValue ret(UniValue::VARR);
    for (const std::string& name : label_set) {
        ret.push_back(name);
    }

    return ret;
}

CPubKey parseWIFKey(std::string strKey, CWallet* pwallet)
{
    CKey key = KeyIO::DecodeSecret(strKey);
    if (!key.IsValid()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key");
    }

    if (HaveKey(pwallet, key)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Already have this key (either as an HD seed or as a loose private key)");
    }
    return pwallet->GetScriptPubKeyMan()->DeriveNewSeed(key);
}

UniValue upgradewallet(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error("upgradewallet\n"
                                 "Bump the wallet features to the latest supported version. Non-HD wallets will be upgraded to HD wallet functionality. "
                                 "Marking all the previous keys as pre-split keys and managing them separately. Once the last key in the pre-split keypool gets marked as used (received balance), the wallet will automatically start using the HD generated keys.\n"
                                 "The upgraded HD wallet will have a new HD seed set so that new keys added to the keypool will be derived from this new seed.\n"
                                 "Wallets that are already runnning the latest HD version will be upgraded to Sapling support\n"
                                 "Enabling the Sapling key manager. Sapling keys will be deterministically derived by the same HD wallet seed.\n"
                                 "Wallets that are running the latest Sapling version will not be upgraded"
                                 "\nNote that you will need to MAKE A NEW BACKUP of your wallet after upgrade it.\n"
                                 + HelpRequiringPassphrase(pwallet) + "\n"
                                 + HelpExampleCli("upgradewallet", "") + HelpExampleRpc("upgradewallet", "")
        );

    LOCK2(cs_main, pwallet->cs_wallet);

    // Do not do anything to wallets already upgraded
    if (pwallet->CanSupportFeature(FEATURE_LATEST)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Cannot upgrade the wallet. The wallet is already running the latest version");
    }

    EnsureWalletIsUnlocked(pwallet);

    // Get version
    int prev_version = pwallet->GetVersion();
    // Upgrade wallet's version
    pwallet->SetMinVersion(FEATURE_LATEST);
    pwallet->SetMaxVersion(FEATURE_LATEST);

    // Upgrade to HD
    std::string upgradeError;
    if (!pwallet->Upgrade(upgradeError, prev_version)) {
        upgradeError = strprintf("Error: Cannot upgrade wallet, %s", upgradeError);
        throw JSONRPCError(RPC_WALLET_ERROR, upgradeError);
    }

    return NullUniValue;
}

UniValue sethdseed(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error("sethdseed ( newkeypool \"seed\" )\n"
               "Set or generate a new HD wallet seed. Non-HD wallets will not be upgraded to being a HD wallet. Wallets that are already\n"
               "HD will have a new HD seed set so that new keys added to the keypool will be derived from this new seed.\n"
               "\nNote that you will need to MAKE A NEW BACKUP of your wallet after setting the HD wallet seed.\n"
                + HelpRequiringPassphrase(pwallet) + "\n"

               "\nArguments:\n"
               "1. newkeypool (boolean, optional, default true): Whether to flush old unused addresses, including change addresses, from the keypool and regenerate it.\n"
               "           If true, the next address from getnewaddress and change address from getrawchangeaddress will be from this new seed.\n"
               "           If false, addresses (including change addresses if the wallet already had HD Chain Split enabled) from the existing\n"
               "           keypool will be used until it has been depleted."
               "2. \"seed\" (string, optional, default random seed): The WIF private key to use as the new HD seed.\n"
               "           The seed value can be retrieved using the dumpwallet command. It is the private key marked hdseed=1"
               + HelpExampleCli("sethdseed", "")
               + HelpExampleCli("sethdseed", "false")
               + HelpExampleCli("sethdseed", "true \"wifkey\"")
               + HelpExampleRpc("sethdseed", "true, \"wifkey\"")
        );

    if (IsInitialBlockDownload()) {
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Cannot set a new HD seed while still in Initial Block Download");
    }

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    // Do not do anything to non-HD wallets
    if (!pwallet->CanSupportFeature(FEATURE_PRE_SPLIT_KEYPOOL)) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Cannot set a HD seed on a non-HD wallet. Start with -upgradewallet in order to upgrade a non-HD wallet to HD");
    }

    EnsureWalletIsUnlocked(pwallet);

    bool flush_key_pool = true;
    if (!request.params[0].isNull()) {
        flush_key_pool = request.params[0].get_bool();
    }

    ScriptPubKeyMan* spk_man = pwallet->GetScriptPubKeyMan();
    CPubKey master_pub_key = request.params[1].isNull() ?
            spk_man->GenerateNewSeed() : parseWIFKey(request.params[1].get_str(), pwallet);

    spk_man->SetHDSeed(master_pub_key, true);
    if (flush_key_pool) spk_man->NewKeyPool();

    // Update Sapling chain
    SaplingScriptPubKeyMan* sspk_man = pwallet->CanSupportFeature(FEATURE_SAPLING) ?
                                       pwallet->GetSaplingScriptPubKeyMan() : nullptr;
    if (sspk_man) {
        sspk_man->SetHDSeed(master_pub_key, true);
    }

    return NullUniValue;
}

UniValue getnewaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "getnewaddress ( \"label\" )\n"
            "\nReturns a new PIVX address for receiving payments.\n"
            "If 'label' is specified, it is added to the address book \n"
            "so payments received with the address will be associated with 'label'.\n"

            "\nArguments:\n"
            "1. \"label\"        (string, optional) The label name for the address to be linked to. if not provided, the default label \"\" is used. It can also be set to the empty string \"\" to represent the default label. The label does not need to exist, it will be created if there is no label by the given name.\n"

            "\nResult:\n"
            "\"pivxaddress\"    (string) The new pivx address\n"

            "\nExamples:\n" +
            HelpExampleCli("getnewaddress", "") + HelpExampleRpc("getnewaddress", ""));

    return EncodeDestination(GetNewAddressFromLabel(pwallet, AddressBook::AddressBookPurpose::RECEIVE, request.params));
}

UniValue getnewstakingaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "getnewstakingaddress ( \"label\" )\n"
            "\nReturns a new PIVX cold staking address for receiving delegated cold stakes.\n"

            "\nArguments:\n"
            "1. \"label\"        (string, optional) The label name for the address to be linked to. if not provided, the default label \"\" is used. It can also be set to the empty string \"\" to represent the default label. The label does not need to exist, it will be created if there is no label by the given name.\n"


            "\nResult:\n"
            "\"pivxaddress\"    (string) The new pivx address\n"

            "\nExamples:\n" +
            HelpExampleCli("getnewstakingaddress", "") + HelpExampleRpc("getnewstakingaddress", ""));

    return EncodeDestination(GetNewAddressFromLabel(pwallet, "coldstaking", request.params, CChainParams::STAKING_ADDRESS), CChainParams::STAKING_ADDRESS);
}

UniValue getnewshieldaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || !request.params.empty())
        throw std::runtime_error(
                "getnewshieldaddress\n"
                "\nReturns a new shield address for receiving payments.\n"
                + HelpRequiringPassphrase(pwallet) + "\n"

                "\nResult:\n"
                "\"address\"    (string) The new shield address.\n"

                "\nExamples:\n"
                + HelpExampleCli("getnewshieldaddress", "")
                + HelpExampleRpc("getnewshieldaddress", "")
        );

    LOCK2(cs_main, pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    return KeyIO::EncodePaymentAddress(pwallet->GenerateNewSaplingZKey());
}

UniValue listshieldunspent(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 4)
        throw std::runtime_error(
                "listshieldunspent ( minconf maxconf include_watchonly [\"shield_addr\",...] )\n"
                "\nReturns array of unspent shield notes with between minconf and maxconf (inclusive) confirmations.\n"
                "Optionally filter to only include notes sent to specified addresses.\n"
                "When minconf is 0, unspent notes with zero confirmations are returned, even though they are not immediately spendable.\n"

                "\nArguments:\n"
                "1. minconf            (numeric, optional, default=1) The minimum confirmations to filter\n"
                "2. maxconf            (numeric, optional, default=9999999) The maximum confirmations to filter\n"
                "3. include_watchonly  (bool, optional, default=false) Also include watchonly addresses (see 'importsaplingviewingkey')\n"
                "4. \"addresses\"      (string) A json array of shield addrs to filter on.  Duplicate addresses not allowed.\n"
                "    [\n"
                "      \"address\"     (string) shield addr\n"
                "      ,...\n"
                "    ]\n"

                "\nResult:\n"
                "[                             (array of json object)\n"
                "  {\n"
                "    \"txid\" : \"txid\",          (string) the transaction id \n"
                "    \"outindex\" (sapling) : n,       (numeric) the output index\n"
                "    \"confirmations\" : n,       (numeric) the number of confirmations\n"
                "    \"spendable\" : true|false,  (boolean) true if note can be spent by wallet, false if address is watchonly\n"
                "    \"address\" : \"address\",    (string) the shield address\n"
                "    \"amount\": xxxxx,          (numeric) the amount of value in the note\n"
                "    \"memo\": xxxxx,            (string) hexademical string representation of memo field\n"
                "    \"change\": true|false,     (boolean) true if the address that received the note is also one of the sending addresses\n"
                "    \"nullifier\": xxxxx,       (string) the note's nullifier, hex encoded"
                "  }\n"
                "  ,...\n"
                "]\n"

                "\nExamples\n"
                + HelpExampleCli("listshieldunspent", "")
                + HelpExampleCli("listshieldunspent", "6 9999999 false \"[\\\"ptestsapling1h0w73csah2aq0a32h42kr7tq4htlt5wfn4ejxfnm56f6ehjvek7k4e244g6v8v3pgylmz5ea8jh\\\",\\\"ptestsapling1h0w73csah2aq0a32h42kr7tq4htlt5wfn4ejxfnm56f6ehjvek7k4e244g6v8v3pgylmz5ea8jh\\\"]\"")
                + HelpExampleRpc("listshieldunspent", "6 9999999 false \"[\\\"ptestsapling1h0w73csah2aq0a32h42kr7tq4htlt5wfn4ejxfnm56f6ehjvek7k4e244g6v8v3pgylmz5ea8jh\\\",\\\"ptestsapling1h0w73csah2aq0a32h42kr7tq4htlt5wfn4ejxfnm56f6ehjvek7k4e244g6v8v3pgylmz5ea8jh\\\"]\"")
        );

    RPCTypeCheck(request.params, {UniValue::VNUM, UniValue::VNUM, UniValue::VBOOL, UniValue::VARR});

    int nMinDepth = request.params.size() > 0 ? request.params[0].get_int() : 1;
    if (nMinDepth < 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Minimum number of confirmations cannot be less than 0");
    }

    int nMaxDepth = request.params.size() > 1 ? request.params[1].get_int() : 9999999;
    if (nMaxDepth < nMinDepth) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Maximum number of confirmations must be greater or equal to the minimum number of confirmations");
    }

    std::set<libzcash::PaymentAddress> shieldAddrs = {};
    bool fIncludeWatchonly = request.params.size() > 2 && request.params[2].get_bool();

    LOCK2(cs_main, pwallet->cs_wallet);

    // User has supplied shield addrs to filter on
    if (request.params.size() > 3) {
        UniValue addresses = request.params[3].get_array();
        if (addresses.size()==0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, addresses array is empty.");

        // Keep track of addresses to spot duplicates
        std::set<std::string> setAddress;

        // Sources
        for (const UniValue& o : addresses.getValues()) {
            if (!o.isStr()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected string");
            }
            std::string address = o.get_str();
            auto shieldAddr = KeyIO::DecodePaymentAddress(address);
            if (!IsValidPaymentAddress(shieldAddr)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, address is not a valid shield address: ") + address);
            }
            libzcash::SaplingPaymentAddress paymentAddress = *boost::get<libzcash::SaplingPaymentAddress>(&shieldAddr);
            bool hasSpendingKey = pwallet->HaveSpendingKeyForPaymentAddress(paymentAddress);
            if (!fIncludeWatchonly && !hasSpendingKey) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, spending key for address does not belong to wallet: ") + address);
            }
            shieldAddrs.insert(shieldAddr);

            if (setAddress.count(address)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ") + address);
            }
            setAddress.insert(address);
        }
    } else {
        // User did not provide shield addrs, so use default i.e. all addresses
        std::set<libzcash::SaplingPaymentAddress> saplingzaddrs = {};
        pwallet->GetSaplingPaymentAddresses(saplingzaddrs);
        shieldAddrs.insert(saplingzaddrs.begin(), saplingzaddrs.end());
    }

    UniValue results(UniValue::VARR);

    if (shieldAddrs.size() > 0) {
        std::vector<SaplingNoteEntry> saplingEntries;
        pwallet->GetSaplingScriptPubKeyMan()->GetFilteredNotes(saplingEntries, shieldAddrs, nMinDepth, nMaxDepth, true, !fIncludeWatchonly, false);
        std::set<std::pair<libzcash::PaymentAddress, uint256>> nullifierSet = pwallet->GetSaplingScriptPubKeyMan()->GetNullifiersForAddresses(shieldAddrs);

        for (const auto& entry : saplingEntries) {
            UniValue obj(UniValue::VOBJ);
            obj.pushKV("txid", entry.op.hash.ToString());
            obj.pushKV("outindex", (int)entry.op.n);
            obj.pushKV("confirmations", entry.confirmations);
            bool hasSaplingSpendingKey = pwallet->HaveSpendingKeyForPaymentAddress(entry.address);
            obj.pushKV("spendable", hasSaplingSpendingKey);
            obj.pushKV("address", KeyIO::EncodePaymentAddress(entry.address));
            obj.pushKV("amount", ValueFromAmount(CAmount(entry.note.value()))); // note.value() is equivalent to plaintext.value()
            obj.pushKV("memo", HexStrTrimmed(entry.memo));
            if (hasSaplingSpendingKey) {
                obj.pushKV("change", pwallet->GetSaplingScriptPubKeyMan()->IsNoteSaplingChange(nullifierSet, entry.address, entry.op));
            }
            const auto& nd = pwallet->mapWallet.at(entry.op.hash).mapSaplingNoteData.at(entry.op);
            if (nd.nullifier) {
                obj.pushKV("nullifier", nd.nullifier->ToString());
            }
            results.push_back(obj);
        }
    }

    return results;
}

UniValue delegatoradd(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "delegatoradd \"address\" ( \"label\" )\n"
            "\nAdd the provided address <address> into the allowed delegators AddressBook.\n"
            "This enables the staking of coins delegated to this wallet, owned by <addr>\n"

            "\nArguments:\n"
            "1. \"address\"     (string, required) The address to whitelist\n"
            "2. \"label\"       (string, optional) A label for the address to whitelist\n"

            "\nResult:\n"
            "true|false           (boolean) true if successful.\n"

            "\nExamples:\n" +
            HelpExampleCli("delegatoradd", "DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6") +
            HelpExampleRpc("delegatoradd", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\"") +
            HelpExampleRpc("delegatoradd", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" \"myPaperWallet\""));


    bool isStakingAddress = false;
    CTxDestination dest = DecodeDestination(request.params[0].get_str(), isStakingAddress);
    if (!IsValidDestination(dest) || isStakingAddress)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid PIVX address");

    const std::string strLabel = (request.params.size() > 1 ? request.params[1].get_str() : "");

    const CKeyID* keyID = boost::get<CKeyID>(&dest);
    if (!keyID)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unable to get KeyID from PIVX address");

    return pwallet->SetAddressBook(*keyID, strLabel, AddressBook::AddressBookPurpose::DELEGATOR);
}

UniValue delegatorremove(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "delegatorremove \"address\"\n"
            "\nUpdates the provided address <address> from the allowed delegators keystore to a \"delegable\" status.\n"
            "This disables the staking of coins delegated to this wallet, owned by <addr>\n"

            "\nArguments:\n"
            "1. \"address\"      (string, required) The address to blacklist\n"

            "\nResult:\n"
            "true|false           (boolean) true if successful.\n"

            "\nExamples:\n" +
            HelpExampleCli("delegatorremove", "DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6") +
            HelpExampleRpc("delegatorremove", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\""));

    bool isStakingAddress = false;
    CTxDestination dest = DecodeDestination(request.params[0].get_str(), isStakingAddress);
    if (!IsValidDestination(dest) || isStakingAddress)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid PIVX address");

    const CKeyID* keyID = boost::get<CKeyID>(&dest);
    if (!keyID)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unable to get KeyID from PIVX address");

    if (!pwallet->HasAddressBook(*keyID))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Unable to get PIVX address from addressBook");

    std::string label = "";
    auto optAdd = pwallet->GetAddressBookEntry(dest);
    if (optAdd) {
        label = optAdd->name;
    }

    return pwallet->SetAddressBook(*keyID, label, AddressBook::AddressBookPurpose::DELEGABLE);
}

static UniValue ListaddressesForPurpose(CWallet* const pwallet, const std::string strPurpose)
{
    const CChainParams::Base58Type addrType = (
            AddressBook::IsColdStakingPurpose(strPurpose) ?
                    CChainParams::STAKING_ADDRESS :
                    CChainParams::PUBKEY_ADDRESS);
    UniValue ret(UniValue::VARR);
    {
        LOCK(pwallet->cs_wallet);
        for (auto it = pwallet->NewAddressBookIterator(); it.IsValid(); it.Next()) {
            auto addrBook = it.GetValue();
            if (addrBook.purpose != strPurpose) continue;
            auto dest = it.GetCTxDestKey();
            if (!dest) continue;
            UniValue entry(UniValue::VOBJ);
            entry.pushKV("label", addrBook.name);
            entry.pushKV("address", EncodeDestination(*dest, addrType));
            ret.push_back(entry);
        }
    }

    return ret;
}

UniValue listdelegators(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "listdelegators ( blacklist )\n"
            "\nShows the list of allowed delegator addresses for cold staking.\n"

            "\nArguments:\n"
            "1. blacklist             (boolean, optional, default = false) Show addresses removed\n"
            "                          from the delegators whitelist\n"

            "\nResult:\n"
            "[\n"
            "   {\n"
            "   \"label\": \"yyy\",    (string) Address label\n"
            "   \"address\": \"xxx\",  (string) PIVX address string\n"
            "   }\n"
            "  ...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("listdelegators" , "") +
            HelpExampleRpc("listdelegators", ""));

    const bool fBlacklist = (request.params.size() > 0 ? request.params[0].get_bool() : false);
    return (fBlacklist ?
            ListaddressesForPurpose(pwallet, AddressBook::AddressBookPurpose::DELEGABLE) :
            ListaddressesForPurpose(pwallet, AddressBook::AddressBookPurpose::DELEGATOR));
}

UniValue liststakingaddresses(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || !request.params.empty())
        throw std::runtime_error(
            "liststakingaddresses\n"
            "\nShows the list of staking addresses for this wallet.\n"

            "\nResult:\n"
            "[\n"
            "   {\n"
            "   \"label\": \"yyy\",  (string) Address label\n"
            "   \"address\": \"xxx\",  (string) PIVX address string\n"
            "   }\n"
            "  ...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("liststakingaddresses" , "") +
            HelpExampleRpc("liststakingaddresses", ""));

    return ListaddressesForPurpose(pwallet, AddressBook::AddressBookPurpose::COLD_STAKING);
}

UniValue listshieldaddresses(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
                "listshieldaddresses ( include_watchonly )\n"
                "\nReturns the list of shield addresses belonging to the wallet.\n"

                "\nArguments:\n"
                "1. include_watchonly (bool, optional, default=false) Also include watchonly addresses (see 'importviewingkey')\n"

                "\nResult:\n"
                "[                     (json array of string)\n"
                "  \"addr\"           (string) a shield address belonging to the wallet\n"
                "  ,...\n"
                "]\n"

                "\nExamples:\n"
                + HelpExampleCli("listshieldaddresses", "")
                + HelpExampleRpc("listshieldaddresses", "")
        );

    LOCK2(cs_main, pwallet->cs_wallet);

    bool fIncludeWatchonly = false;
    if (request.params.size() > 0) {
        fIncludeWatchonly = request.params[0].get_bool();
    }

    UniValue ret(UniValue::VARR);

    std::set<libzcash::SaplingPaymentAddress> addresses;
    pwallet->GetSaplingPaymentAddresses(addresses);
    libzcash::SaplingIncomingViewingKey ivk;
    libzcash::SaplingExtendedFullViewingKey extfvk;
    for (libzcash::SaplingPaymentAddress addr : addresses) {
        if (fIncludeWatchonly || pwallet->HaveSpendingKeyForPaymentAddress(addr)) {
            ret.push_back(KeyIO::EncodePaymentAddress(addr));
        }
    }
    return ret;
}

UniValue getrawchangeaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || !request.params.empty())
        throw std::runtime_error(
            "getrawchangeaddress\n"
            "\nReturns a new PIVX address, for receiving change.\n"
            "This is for use with raw transactions, NOT normal use.\n"

            "\nResult:\n"
            "\"address\"    (string) The address\n"

            "\nExamples:\n" +
            HelpExampleCli("getrawchangeaddress", "") + HelpExampleRpc("getrawchangeaddress", ""));

    LOCK2(cs_main, pwallet->cs_wallet);

    if (!pwallet->IsLocked())
        pwallet->TopUpKeyPool();

    CReserveKey reservekey(pwallet);
    CPubKey vchPubKey;
    if (!reservekey.GetReservedKey(vchPubKey, true))
        throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, "Error: Keypool ran out, please call keypoolrefill first");

    reservekey.KeepKey();

    CKeyID keyID = vchPubKey.GetID();

    return EncodeDestination(keyID);
}


UniValue setlabel(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
            "setlabel \"address\" \"label\"\n"
            "\nSets the label associated with the given address.\n"

            "\nArguments:\n"
            "1. \"address\"       (string, required) The pivx address to be associated with a label.\n"
            "2. \"label\"         (string, required) The label to assign to the address.\n"

            "\nExamples:\n" +
            HelpExampleCli("setlabel", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" \"tabby\"") + HelpExampleRpc("setlabel", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\", \"tabby\""));

    LOCK2(cs_main, pwallet->cs_wallet);

    CTxDestination dest = DecodeDestination(request.params[0].get_str());
    if (!IsValidDestination(dest))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid PIVX address");

    std::string old_label = pwallet->GetNameForAddressBookEntry(dest);
    std::string label = LabelFromValue(request.params[1]);

    pwallet->SetAddressBook(dest, label, "");

    return NullUniValue;
}

static void SendMoney(CWallet* const pwallet, const CTxDestination& address, CAmount nValue, bool fSubtractFeeFromAmount, CTransactionRef& tx)
{
    LOCK2(cs_main, pwallet->cs_wallet);

    // Check amount
    if (nValue <= 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid amount");

    if (nValue > pwallet->GetAvailableBalance())
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");

    if (!g_connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    // Parse PIVX address
    CScript scriptPubKey = GetScriptForDestination(address);

    // Create and send the transaction
    CReserveKey reservekey(pwallet);
    CAmount nFeeRequired;
    std::string strError;
    std::vector<CRecipient> vecSend;
    int nChangePosRet = -1;
    CRecipient recipient = {scriptPubKey, nValue, fSubtractFeeFromAmount};
    vecSend.push_back(recipient);
    if (!pwallet->CreateTransaction(vecSend, tx, reservekey, nFeeRequired, nChangePosRet, strError)) {
        if (!fSubtractFeeFromAmount && nValue + nFeeRequired > pwallet->GetAvailableBalance())
            strError = strprintf("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds!", FormatMoney(nFeeRequired));
        LogPrintf("%s: %s\n", __func__, strError);
        throw JSONRPCError(RPC_WALLET_ERROR, strError);
    }
    const CWallet::CommitResult&& res = pwallet->CommitTransaction(tx, reservekey, g_connman.get());
    if (res.status != CWallet::CommitStatus::OK)
        throw JSONRPCError(RPC_WALLET_ERROR, res.ToString());
}

static SaplingOperation CreateShieldedTransaction(CWallet* const pwallet, const JSONRPCRequest& request);

/*
 * redirect sendtoaddress/sendmany inputs to shieldsendmany implementation (CreateShieldedTransaction)
 */
static UniValue ShieldSendManyTo(CWallet * const pwallet,
                                 const UniValue& sendTo,
                                 const std::string& commentStr,
                                 const std::string& toStr,
                                 int nMinDepth,
                                 bool fIncludeDelegated,
                                 UniValue subtractFeeFromAmount)
{
    // convert params to 'shieldsendmany' format
    JSONRPCRequest req;
    req.params = UniValue(UniValue::VARR);
    if (!fIncludeDelegated) {
        req.params.push_back(UniValue("from_transparent"));
    } else {
        req.params.push_back(UniValue("from_trans_cold"));
    }
    UniValue recipients(UniValue::VARR);
    for (const std::string& key : sendTo.getKeys()) {
        UniValue recipient(UniValue::VOBJ);
        recipient.pushKV("address", key);
        recipient.pushKV("amount", sendTo[key]);
        recipients.push_back(recipient);
    }
    req.params.push_back(recipients);
    req.params.push_back(nMinDepth);
    req.params.push_back(0);
    req.params.push_back(subtractFeeFromAmount);

    // send
    SaplingOperation operation = CreateShieldedTransaction(pwallet, req);
    std::string txid;
    auto res = operation.send(txid);
    if (!res)
        throw JSONRPCError(RPC_WALLET_ERROR, res.getError());

    // add comments
    const uint256& txHash = uint256S(txid);
    assert(pwallet->mapWallet.count(txHash));
    if (!commentStr.empty()) {
        pwallet->mapWallet.at(txHash).mapValue["comment"] = commentStr;
    }
    if (!toStr.empty()) {
        pwallet->mapWallet.at(txHash).mapValue["to"] = toStr;
    }

    return txid;
}

UniValue sendtoaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 5)
        throw std::runtime_error(
            "sendtoaddress \"address\" amount ( \"comment\" \"comment-to\" subtract_fee )\n"
            "\nSend an amount to a given address. The amount is a real and is rounded to the nearest 0.00000001\n" +
            HelpRequiringPassphrase(pwallet) + "\n"

            "\nArguments:\n"
            "1. \"address\"     (string, required) The pivx address to send to.\n"
            "2. \"amount\"      (numeric, required) The amount in PIV to send. eg 0.1\n"
            "3. \"comment\"     (string, optional) A comment used to store what the transaction is for. \n"
            "                             This is not part of the transaction, just kept in your wallet.\n"
            "4. \"comment-to\"  (string, optional) A comment to store the name of the person or organization \n"
            "                             to which you're sending the transaction. This is not part of the \n"
            "                             transaction, just kept in your wallet.\n"
            "5. subtract_fee    (boolean, optional, default=false) The fee will be deducted from the amount being sent.\n"
            "                             The recipient will receive less PIVs than you enter in the amount field.\n"

            "\nResult:\n"
            "\"transactionid\"  (string) The transaction id.\n"

            "\nExamples:\n" +
            HelpExampleCli("sendtoaddress", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" 0.1") +
            HelpExampleCli("sendtoaddress", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" 0.1 \"donation\" \"seans outpost\"") +
            HelpExampleCli("sendtoaddress", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" 0.1 \"\" \"\" true") +
            HelpExampleRpc("sendtoaddress", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\", 0.1, \"donation\", \"seans outpost\""));

    EnsureWalletIsUnlocked(pwallet);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    bool isStaking = false, isShielded = false;
    const std::string addrStr = request.params[0].get_str();
    const CWDestination& destination = Standard::DecodeDestination(addrStr, isStaking, isShielded);
    if (!Standard::IsValidDestination(destination) || isStaking)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid PIVX address");
    const std::string commentStr = (request.params.size() > 2 && !request.params[2].isNull()) ?
                                   request.params[2].get_str() : "";
    const std::string toStr = (request.params.size() > 3 && !request.params[3].isNull()) ?
                                   request.params[3].get_str() : "";
    bool fSubtractFeeFromAmount = request.params.size() > 4 && request.params[4].get_bool();

    if (isShielded) {
        UniValue sendTo(UniValue::VOBJ);
        sendTo.pushKV(addrStr, request.params[1]);
        UniValue subtractFeeFromAmount(UniValue::VARR);
        if (fSubtractFeeFromAmount) {
            subtractFeeFromAmount.push_back(addrStr);
        }
        return ShieldSendManyTo(pwallet, sendTo, commentStr, toStr, 1, false, subtractFeeFromAmount);
    }

    const CTxDestination& address = *Standard::GetTransparentDestination(destination);

    // Amount
    CAmount nAmount = AmountFromValue(request.params[1]);

    CTransactionRef tx;
    SendMoney(pwallet, address, nAmount, fSubtractFeeFromAmount, tx);

    // Wallet comments
    CWalletTx& wtx = pwallet->mapWallet.at(tx->GetHash());
    if (!commentStr.empty())
        wtx.mapValue["comment"] = commentStr;
    if (!toStr.empty())
        wtx.mapValue["to"] = toStr;

    return wtx.GetHash().GetHex();
}

static UniValue CreateColdStakeDelegation(CWallet* const pwallet, const UniValue& params, CTransactionRef& txNew, CReserveKey& reservekey)
{
    LOCK2(cs_main, pwallet->cs_wallet);

    // Check that Cold Staking has been enforced or fForceNotEnabled = true
    bool fForceNotEnabled = false;
    if (params.size() > 6 && !params[6].isNull())
        fForceNotEnabled = params[6].get_bool();

    if (sporkManager.IsSporkActive(SPORK_19_COLDSTAKING_MAINTENANCE) && !fForceNotEnabled) {
        std::string errMsg = "Cold Staking temporarily disabled with SPORK 19.\n"
                "You may force the stake delegation setting fForceNotEnabled to true.\n"
                "WARNING: If relayed before activation, this tx will be rejected resulting in a ban.\n";
        throw JSONRPCError(RPC_WALLET_ERROR, errMsg);
    }

    // Get Staking Address
    bool isStaking = false;
    CTxDestination stakeAddr = DecodeDestination(params[0].get_str(), isStaking);
    if (!IsValidDestination(stakeAddr) || !isStaking)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid PIVX staking address");

    CKeyID* stakeKey = boost::get<CKeyID>(&stakeAddr);
    if (!stakeKey)
        throw JSONRPCError(RPC_WALLET_ERROR, "Unable to get stake pubkey hash from stakingaddress");

    // Get Amount
    CAmount nValue = AmountFromValue(params[1]);
    if (nValue < MIN_COLDSTAKING_AMOUNT)
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid amount (%d). Min amount: %d",
                nValue, MIN_COLDSTAKING_AMOUNT));

    // include already delegated coins
    bool fUseDelegated = false;
    if (params.size() > 4 && !params[4].isNull())
        fUseDelegated = params[4].get_bool();

    // Check amount
    CAmount currBalance = pwallet->GetAvailableBalance() + (fUseDelegated ? pwallet->GetDelegatedBalance() : 0);
    if (nValue > currBalance)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Insufficient funds");

    std::string strError;

    // Get Owner Address
    std::string ownerAddressStr;
    CKeyID ownerKey;
    if (params.size() > 2 && !params[2].isNull() && !params[2].get_str().empty()) {
        // Address provided
        bool isStaking = false;
        CTxDestination dest = DecodeDestination(params[2].get_str(), isStaking);
        if (!IsValidDestination(dest) || isStaking)
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid PIVX spending address");
        ownerKey = *boost::get<CKeyID>(&dest);
        // Check that the owner address belongs to this wallet, or fForceExternalAddr is true
        bool fForceExternalAddr = params.size() > 3 && !params[3].isNull() ? params[3].get_bool() : false;
        if (!fForceExternalAddr && !pwallet->HaveKey(ownerKey)) {
            std::string errMsg = strprintf("The provided owneraddress \"%s\" is not present in this wallet.\n", params[2].get_str());
            errMsg += "Set 'fExternalOwner' argument to true, in order to force the stake delegation to an external owner address.\n"
                    "e.g. delegatestake stakingaddress amount owneraddress true.\n"
                    "WARNING: Only the owner of the key to owneraddress will be allowed to spend these coins after the delegation.";
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, errMsg);
        }
        ownerAddressStr = params[2].get_str();
    } else {
        // Get new owner address from keypool
        CTxDestination ownerAddr = GetNewAddressFromLabel(pwallet, "delegated", NullUniValue);
        CKeyID* pOwnerKey = boost::get<CKeyID>(&ownerAddr);
        assert(pOwnerKey);
        ownerKey = *pOwnerKey;
        ownerAddressStr = EncodeDestination(ownerAddr);
    }

    // Use new opcode after v6.0 enforcement (!TODO: remove after enforcement)
    bool fV6Enforced = Params().GetConsensus().NetworkUpgradeActive(chainActive.Height(), Consensus::UPGRADE_V6_0);

    // Create the transaction
    const bool fUseShielded = (params.size() > 5) && params[5].get_bool();
    if (!fUseShielded) {
        // Delegate transparent coins
        CAmount nFeeRequired;
        CScript scriptPubKey = fV6Enforced ? GetScriptForStakeDelegation(*stakeKey, ownerKey)
                                           : GetScriptForStakeDelegationLOF(*stakeKey, ownerKey);
        if (!pwallet->CreateTransaction(scriptPubKey, nValue, txNew, reservekey, nFeeRequired, strError, nullptr, (CAmount)0, fUseDelegated)) {
            if (nValue + nFeeRequired > currBalance)
                strError = strprintf("Error: This transaction requires a transaction fee of at least %s because of its amount, complexity, or use of recently received funds!", FormatMoney(nFeeRequired));
            LogPrintf("%s : %s\n", __func__, strError);
            throw JSONRPCError(RPC_WALLET_ERROR, strError);
        }
    } else {
        // Delegate shield coins
        const Consensus::Params& consensus = Params().GetConsensus();
        // Check network status
        int nextBlockHeight = chainActive.Height() + 1;
        if (sporkManager.IsSporkActive(SPORK_20_SAPLING_MAINTENANCE)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "SHIELD in maintenance (SPORK 20)");
        }
        std::vector<SendManyRecipient> recipients = {SendManyRecipient(ownerKey, *stakeKey, nValue, fV6Enforced)};
        SaplingOperation operation(consensus, nextBlockHeight, pwallet);
        OperationResult res = operation.setSelectShieldedCoins(true)
                                       ->setRecipients(recipients)
                                       ->build();
        if (!res) throw JSONRPCError(RPC_WALLET_ERROR, res.getError());
        txNew = MakeTransactionRef(operation.getFinalTx());
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("owner_address", ownerAddressStr);
    result.pushKV("staker_address", EncodeDestination(stakeAddr, true));
    return result;
}

UniValue delegatestake(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 7)
        throw std::runtime_error(
            "delegatestake \"staking_addr\" amount ( \"owner_addr\" ext_owner include_delegated from_shield force )\n"
            "\nDelegate an amount to a given address for cold staking. The amount is a real and is rounded to the nearest 0.00000001\n" +
            HelpRequiringPassphrase(pwallet) + "\n"

            "\nArguments:\n"
            "1. \"staking_addr\"      (string, required) The pivx staking address to delegate.\n"
            "2. \"amount\"            (numeric, required) The amount in PIV to delegate for staking. eg 100\n"
            "3. \"owner_addr\"        (string, optional) The pivx address corresponding to the key that will be able to spend the stake.\n"
            "                               If not provided, or empty string, a new wallet address is generated.\n"
            "4. \"ext_owner\"         (boolean, optional, default = false) use the provided 'owneraddress' anyway, even if not present in this wallet.\n"
            "                               WARNING: The owner of the keys to 'owneraddress' will be the only one allowed to spend these coins.\n"
            "5. \"include_delegated\" (boolean, optional, default = false) include already delegated inputs if needed.\n"
            "6. \"from_shield\"       (boolean, optional, default = false) delegate shield funds.\n"
            "7. \"force\"             (boolean, optional, default = false) ONLY FOR TESTING: force the creation even if SPORK 17 is disabled.\n"

            "\nResult:\n"
            "{\n"
            "   \"owner_address\": \"xxx\"   (string) The owner (delegator) owneraddress.\n"
            "   \"staker_address\": \"xxx\"  (string) The cold staker (delegate) stakingaddress.\n"
            "   \"txid\": \"xxx\"            (string) The stake delegation transaction id.\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("delegatestake", "\"S1t2a3kab9c8c71VA78xxxy4MxZg6vgeS6\" 100") +
            HelpExampleCli("delegatestake", "\"S1t2a3kab9c8c71VA78xxxy4MxZg6vgeS6\" 1000 \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg34fk\"") +
            HelpExampleRpc("delegatestake", "\"S1t2a3kab9c8c71VA78xxxy4MxZg6vgeS6\", 1000, \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg34fk\""));

    EnsureWalletIsUnlocked(pwallet);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    CTransactionRef wtx;
    CReserveKey reservekey(pwallet);
    UniValue ret = CreateColdStakeDelegation(pwallet, request.params, wtx, reservekey);

    const CWallet::CommitResult& res = pwallet->CommitTransaction(wtx, reservekey, g_connman.get());
    if (res.status != CWallet::CommitStatus::OK)
        throw JSONRPCError(RPC_WALLET_ERROR, res.ToString());

    ret.pushKV("txid", wtx->GetHash().GetHex());
    return ret;
}

UniValue rawdelegatestake(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 7)
        throw std::runtime_error(
            "rawdelegatestake \"staking_addr\" amount ( \"owner_addr\" ext_owner include_delegated from_shield )\n"
            "\nDelegate an amount to a given address for cold staking. The amount is a real and is rounded to the nearest 0.00000001\n"
            "\nDelegate transaction is returned as json object." +
            HelpRequiringPassphrase(pwallet) + "\n"

            "\nArguments:\n"
            "1. \"staking_addr\"      (string, required) The pivx staking address to delegate.\n"
            "2. \"amount\"            (numeric, required) The amount in PIV to delegate for staking. eg 100\n"
            "3. \"owner_addr\"        (string, optional) The pivx address corresponding to the key that will be able to spend the stake.\n"
            "                               If not provided, or empty string, a new wallet address is generated.\n"
            "4. \"ext_owner\"         (boolean, optional, default = false) use the provided 'owneraddress' anyway, even if not present in this wallet.\n"
            "                               WARNING: The owner of the keys to 'owneraddress' will be the only one allowed to spend these coins.\n"
            "5. \"include_delegated\" (boolean, optional, default = false) include already delegated inputs if needed.\n"
            "6. \"from_shield\"       (boolean, optional, default = false) delegate shield funds.\n"
            "7. \"force\"             (boolean, optional, default = false) ONLY FOR TESTING: force the creation even if SPORK 17 is disabled (for tests).\n"

            "\nResult:\n"
            "\"transaction\"            (string) hex string of the transaction\n"

            "\nExamples:\n" +
            HelpExampleCli("rawdelegatestake", "\"S1t2a3kab9c8c71VA78xxxy4MxZg6vgeS6\" 100") +
            HelpExampleCli("rawdelegatestake", "\"S1t2a3kab9c8c71VA78xxxy4MxZg6vgeS6\" 1000 \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg34fk\"") +
            HelpExampleRpc("rawdelegatestake", "\"S1t2a3kab9c8c71VA78xxxy4MxZg6vgeS6\", 1000, \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg34fk\""));

    EnsureWalletIsUnlocked(pwallet);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    CTransactionRef wtx;
    CReserveKey reservekey(pwallet);
    CreateColdStakeDelegation(pwallet, request.params, wtx, reservekey);

    return EncodeHexTx(*wtx);
}


static CAmount getBalanceShieldedAddr(CWallet* const pwallet, Optional<libzcash::SaplingPaymentAddress>& filterAddress, int minDepth = 1, bool ignoreUnspendable=true) {
    CAmount balance = 0;
    std::vector<SaplingNoteEntry> saplingEntries;
    LOCK2(cs_main, pwallet->cs_wallet);
    pwallet->GetSaplingScriptPubKeyMan()->GetFilteredNotes(saplingEntries, filterAddress, minDepth, true, ignoreUnspendable);
    for (auto & entry : saplingEntries) {
        balance += CAmount(entry.note.value());
    }
    return balance;
}

UniValue getshieldbalance(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 3)
        throw std::runtime_error(
                "getshieldbalance \"address\" ( minconf include_watchonly )\n"
                "\nReturn the total shield value of funds stored in the node's wallet or if an address was given,"
                "\nreturns the balance of the shield addr belonging to the node's wallet.\n"
                "\nCAUTION: If the wallet contains any addresses for which it only has incoming viewing keys,"
                "\nthe returned private balance may be larger than the actual balance, because spends cannot"
                "\nbe detected with incoming viewing keys.\n"

                "\nArguments:\n"
                "1. \"address\"        (string, optional) The selected address. If non empty nor \"*\", it must be a Sapling address\n"
                "2. minconf            (numeric, optional, default=1) Only include private and transparent transactions confirmed at least this many times.\n"
                "3. include_watchonly  (bool, optional, default=false) Also include balance in watchonly addresses (see 'importaddress' and 'importsaplingviewingkey')\n"

                "\nResult:\n"
                "amount              (numeric) the total balance of shield funds (in Sapling addresses)\n"

                "\nExamples:\n"
                "\nThe total amount in the wallet\n"
                + HelpExampleCli("getshieldbalance", "")
                + HelpExampleCli("getshieldbalance", "ptestsapling1h0w73csah2aq0a32h42kr7tq4htlt5wfn4ejxfnm56f6ehjvek7k4e244g6v8v3pgylmz5ea8jh") +
                "\nThe total amount in the wallet at least 5 blocks confirmed\n"
                + HelpExampleCli("getshieldbalance", "\"*\" \"5\"") +
                "\nAs a json rpc call\n"
                + HelpExampleRpc("getshieldbalance", "\"*\" \"5\"")
        );

    LOCK2(cs_main, pwallet->cs_wallet);

    Optional<libzcash::SaplingPaymentAddress> address;
    if (request.params.size() > 0) {
        std::string addressStr = request.params[0].get_str();
        if (addressStr.empty() || addressStr != "*") {
            address = KeyIO::DecodeSaplingPaymentAddress(addressStr);
            if (!address) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid shield address");
            }
        }
    }

    const int nMinDepth = request.params.size() > 1 ? request.params[1].get_int() : 1;
    if (nMinDepth < 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Minimum number of confirmations cannot be less than 0");
    }

    const bool fIncludeWatchonly = request.params.size() > 2 && request.params[2].get_bool();
    CAmount nBalance = getBalanceShieldedAddr(pwallet, address, nMinDepth, !fIncludeWatchonly);
    return ValueFromAmount(nBalance);
}

UniValue viewshieldtransaction(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
                "viewshieldtransaction \"txid\"\n"
                "\nGet detailed shield information about in-wallet transaction \"txid\"\n"
                + HelpRequiringPassphrase(pwallet) + "\n"

                "\nArguments:\n"
                "1. \"txid\"    (string, required) The transaction id\n"
                "\nResult:\n"
                "{\n"
                "  \"txid\" : \"transactionid\",   (string) The transaction id\n"
                "  \"fee\"  : x.xxx,               (numeric) The transaction fee in " + CURRENCY_UNIT + "\n"
                "  \"spends\" : [\n"
                "    {\n"
                "      \"spend\" : n,                    (numeric, sapling) the index of the spend within vShieldedSpend\n"
                "      \"txidPrev\" : \"transactionid\",   (string) The id for the transaction this note was created in\n"
                "      \"outputPrev\" : n,               (numeric, sapling) the index of the output within the vShieldedOutput\n"
                "      \"address\" : \"pivxaddress\",     (string) The PIVX address involved in the transaction\n"
                "      \"value\" : x.xxx                 (numeric) The amount in " + CURRENCY_UNIT + "\n"
                "      \"valueSat\" : xxxx               (numeric) The amount in satoshis\n"
                "    }\n"
                "    ,...\n"
                "  ],\n"
                "  \"outputs\" : [\n"
                "    {\n"
                "      \"output\" : n,                   (numeric, sapling) the index of the output within the vShieldedOutput\n"
                "      \"address\" : \"pivxaddress\",     (string) The PIVX address involved in the transaction\n"
                "      \"outgoing\" : true|false         (boolean, sapling) True if the output is not for an address in the wallet\n"
                "      \"value\" : x.xxx                 (numeric) The amount in " + CURRENCY_UNIT + "\n"
                "      \"valueSat\" : xxxx               (numeric) The amount in satoshis\n"
                "      \"memo\" : \"hexmemo\",             (string) Hexademical string representation of the memo field\n"
                "      \"memoStr\" : \"memo\",             (string) Only returned if memo contains valid UTF-8 text.\n"
                "    }\n"
                "    ,...\n"
                "  ],\n"
                "}\n"

                "\nExamples:\n"
                + HelpExampleCli("viewshieldtransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
                + HelpExampleRpc("viewshieldtransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
        );

    if (!pwallet->HasSaplingSPKM()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Sapling wallet not initialized.");
    }

    EnsureWalletIsUnlocked(pwallet);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    uint256 hash;
    hash.SetHex(request.params[0].get_str());

    UniValue entry(UniValue::VOBJ);
    if (!pwallet->mapWallet.count(hash))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    const CWalletTx& wtx = pwallet->mapWallet.at(hash);

    if (!wtx.tx->IsShieldedTx()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid transaction, no shield data available");
    }

    entry.pushKV("txid", hash.GetHex());

    UniValue spends(UniValue::VARR);
    UniValue outputs(UniValue::VARR);

    auto addMemo = [](UniValue& entry, const Optional<std::array<unsigned char, ZC_MEMO_SIZE>>& optMemo) {
        // empty memo
        if (!static_cast<bool>(optMemo)) {
            const std::array<unsigned char, 1> memo {0xF6};
            entry.pushKV("memo", HexStr(memo.begin(), memo.end()));
            return;
        }
        const auto& memo = *optMemo;
        auto end = FindFirstNonZero(memo.rbegin(), memo.rend());
        entry.pushKV("memo", HexStr(memo.begin(), end.base()));
        // If the leading byte is 0xF4 or lower, the memo field should be interpreted as a
        // UTF-8-encoded text string.
        if (memo[0] <= 0xf4) {
            std::string memoStr(memo.begin(), end.base());
            if (IsValidUTF8(memoStr)) {
                entry.pushKV("memoStr", memoStr);
            }
        }
    };

    auto sspkm = pwallet->GetSaplingScriptPubKeyMan();

    // Collect OutgoingViewingKeys for recovering output information
    std::set<uint256> ovks;
    // Get the common OVK for recovering t->shield outputs.
    // If not already databased, a new one will be generated from the HD seed.
    // It is safe to do it here, as the wallet is unlocked.
    ovks.insert(sspkm->getCommonOVK());

    // Sapling spends
    for (size_t i = 0; i < wtx.tx->sapData->vShieldedSpend.size(); ++i) {
        const auto& spend = wtx.tx->sapData->vShieldedSpend[i];

        // Fetch the note that is being spent
        auto res = sspkm->mapSaplingNullifiersToNotes.find(spend.nullifier);
        if (res == sspkm->mapSaplingNullifiersToNotes.end()) {
            continue;
        }
        const auto& op = res->second;
        std::string addrStr = "unknown";
        UniValue amountStr = UniValue("unknown");
        CAmount amount = 0;
        auto wtxPrevIt = pwallet->mapWallet.find(op.hash);
        if (wtxPrevIt != pwallet->mapWallet.end()) {
            const auto ndIt = wtxPrevIt->second.mapSaplingNoteData.find(op);
            if (ndIt != wtxPrevIt->second.mapSaplingNoteData.end()) {
                // get cached address and amount
                if (ndIt->second.address) {
                    addrStr = KeyIO::EncodePaymentAddress(*(ndIt->second.address));
                }
                if (ndIt->second.amount) {
                    amount = *(ndIt->second.amount);
                    amountStr = ValueFromAmount(amount);
                }
            }
        }

        UniValue entry_(UniValue::VOBJ);
        entry_.pushKV("spend", (int)i);
        entry_.pushKV("txidPrev", op.hash.GetHex());
        entry_.pushKV("outputPrev", (int)op.n);
        entry_.pushKV("address", addrStr);
        entry_.pushKV("value", amountStr);
        entry_.pushKV("valueSat", amount);
        spends.push_back(entry_);
    }

    // Sapling outputs
    for (uint32_t i = 0; i < wtx.tx->sapData->vShieldedOutput.size(); ++i) {
        auto op = SaplingOutPoint(hash, i);
        if (!wtx.mapSaplingNoteData.count(op)) continue;
        const auto& nd = wtx.mapSaplingNoteData.at(op);

        const bool isOutgoing = !nd.IsMyNote();
        std::string addrStr = "unknown";
        UniValue amountStr = UniValue("unknown");
        CAmount amount = 0;
        if (nd.address) {
            addrStr = KeyIO::EncodePaymentAddress(*(nd.address));
        }
        if (nd.amount) {
            amount = *(nd.amount);
            amountStr = ValueFromAmount(amount);
        }

        UniValue entry_(UniValue::VOBJ);
        entry_.pushKV("output", (int)op.n);
        entry_.pushKV("outgoing", isOutgoing);
        entry_.pushKV("address", addrStr);
        entry_.pushKV("value", amountStr);
        entry_.pushKV("valueSat", amount);
        addMemo(entry_, nd.memo);

        outputs.push_back(entry_);
    }

    entry.pushKV("fee", FormatMoney(pcoinsTip->GetValueIn(*wtx.tx) - wtx.tx->GetValueOut()));
    entry.pushKV("spends", spends);
    entry.pushKV("outputs", outputs);

    return entry;
}

static SaplingOperation CreateShieldedTransaction(CWallet* const pwallet, const JSONRPCRequest& request)
{
    LOCK2(cs_main, pwallet->cs_wallet);
    int nextBlockHeight = chainActive.Height() + 1;
    SaplingOperation operation(Params().GetConsensus(), nextBlockHeight, pwallet);

    // Param 0: source of funds. Can either be a valid address, sapling address,
    // or the string "from_transparent"|"from_trans_cold"|"from_shield"
    bool fromSapling  = false;
    std::string sendFromStr = request.params[0].get_str();
    if (sendFromStr == "from_transparent") {
        // send from any transparent address
        operation.setSelectTransparentCoins(true);
    } else if (sendFromStr == "from_trans_cold") {
        // send from any transparent address + delegations
        operation.setSelectTransparentCoins(true, true);
    } else if (sendFromStr == "from_shield") {
        // send from any shield address
        operation.setSelectShieldedCoins(true);
        fromSapling = true;
    } else {
        CTxDestination fromTAddressDest = DecodeDestination(sendFromStr);
        if (!IsValidDestination(fromTAddressDest)) {
            auto res = KeyIO::DecodePaymentAddress(sendFromStr);
            if (!IsValidPaymentAddress(res)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid from address, should be a taddr or shield addr.");
            }
            libzcash::SaplingPaymentAddress fromShieldedAddress = *boost::get<libzcash::SaplingPaymentAddress>(&res);
            if (!pwallet->HaveSpendingKeyForPaymentAddress(fromShieldedAddress)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "From address does not belong to this node, shield addr spending key not found.");
            }
            // send from user-supplied shield address
            operation.setFromAddress(fromShieldedAddress);
            fromSapling = true;
        } else {
            // send from user-supplied transparent address
            operation.setFromAddress(fromTAddressDest);
        }
    }

    // Param 4: subtractFeeFromAmount addresses
    const UniValue subtractFeeFromAmount = request.params[4];

    // Param 1: array of outputs
    UniValue outputs = request.params[1].get_array();
    if (outputs.empty())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, amounts array is empty.");

    // Keep track of addresses to spot duplicates
    std::set<std::string> setAddress;
    // Recipients
    std::vector<SendManyRecipient> recipients;
    CAmount nTotalOut = 0;
    bool containsSaplingOutput = false;

    for (const UniValue& o : outputs.getValues()) {
        if (!o.isObject())
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected object");

        // sanity check, report error if unknown key-value pairs
        for (const std::string& name_ : o.getKeys()) {
            if (name_ != "address" && name_ != "amount" && name_!="memo")
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, unknown key: ")+name_);
        }

        std::string address = find_value(o, "address").get_str();
        CTxDestination taddr = DecodeDestination(address);
        Optional<libzcash::SaplingPaymentAddress> saddr;

        if (!IsValidDestination(taddr)) {
            const auto& addr = KeyIO::DecodePaymentAddress(address);
            if (IsValidPaymentAddress(addr)) {
                saddr = *(boost::get<libzcash::SaplingPaymentAddress>(&addr));
                containsSaplingOutput = true;
            } else {
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, unknown address format: ")+address );
            }
        }

        if (setAddress.count(address))
            throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ")+address);
        setAddress.insert(address);

        UniValue memoValue = find_value(o, "memo");
        std::string memo;
        if (!memoValue.isNull()) {
            memo = memoValue.get_str();
            if (!saddr) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Memo cannot be used with a taddr. It can only be used with a shield addr.");
            }
            if (memo.length() > ZC_MEMO_SIZE*2) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,  strprintf("Invalid parameter, size of memo is larger than maximum allowed %d", ZC_MEMO_SIZE ));
            }
        }

        UniValue av = find_value(o, "amount");
        CAmount nAmount = AmountFromValue(av);
        if (nAmount < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, amount must be positive");

        bool fSubtractFeeFromAmount = false;
        for (unsigned int idx = 0; idx < subtractFeeFromAmount.size(); idx++) {
            const UniValue& addr = subtractFeeFromAmount[idx];
            if (addr.get_str() == address) {
                fSubtractFeeFromAmount = true;
                break;
            }
        }

        if (saddr) {
            recipients.emplace_back(*saddr, nAmount, memo, fSubtractFeeFromAmount);
        } else {
            recipients.emplace_back(taddr, nAmount, fSubtractFeeFromAmount);
        }

        nTotalOut += nAmount;
    }

    // Check network status
    if (sporkManager.IsSporkActive(SPORK_20_SAPLING_MAINTENANCE)) {
        // If Sapling is disabled, do not allow sending from or sending to Sapling addresses.
        if (fromSapling || containsSaplingOutput) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "SHIELD in maintenance (SPORK 20)");
        }
    }

    // Now check the transaction
    auto opResult = CheckTransactionSize(recipients, !fromSapling);
    if (!opResult) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, opResult.getError());
    }

    // Param 2: Minimum confirmations
    int nMinDepth = request.params.size() > 2 ? request.params[2].get_int() : 1;
    if (nMinDepth < 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Minimum number of confirmations cannot be less than 0");
    }

    // Param 3: Fee
    // If not set, SaplingOperation will set the minimum fee (based on minRelayFee and tx size)
    if (request.params.size() > 3) {
        CAmount nFee = AmountFromValue(request.params[3]);
        if (nFee < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid fee. Must be positive.");
        } else if (nFee > 0) {
            // If the user-selected fee is not enough (or too much), the build operation will fail.
            operation.setFee(nFee);
        }
        // If nFee=0 leave the default (build operation will compute the minimum fee)
    }

    if (fromSapling && nMinDepth == 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Minconf cannot be zero when sending from shield addr");
    }

    if (nMinDepth < 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Minconf cannot be negative");
    }

    // Build the send operation
    OperationResult res = operation.setMinDepth(nMinDepth)
            ->setRecipients(recipients)
            ->build();
    if (!res) throw JSONRPCError(RPC_WALLET_ERROR, res.getError());
    return operation;
}

UniValue shieldsendmany(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 5)
        throw std::runtime_error(
                "shieldsendmany \"fromaddress\" [{\"address\":... ,\"amount\":...},...] ( minconf fee subtract_fee_from )\n"
                "\nSend to many recipients. Amounts are decimal numbers with at most 8 digits of precision."
                "\nChange generated from a transparent addr flows to a new  transparent addr address, while change generated from a shield addr returns to itself."
                "\nWhen sending coinbase UTXOs to a shield addr, change is not allowed. The entire value of the UTXO(s) must be consumed."
                + HelpRequiringPassphrase(pwallet) + "\n"

                "\nArguments:\n"
                "1. \"fromaddress\"         (string, required) The transparent addr or shield addr to send the funds from.\n"
                "                             It can also be the string \"from_transparent\"|\"from_shield\" to send the funds\n"
                "                             from any transparent|shield address available.\n"
                "                             Additionally, it can be the string \"from_trans_cold\" to select transparent funds,\n"
                "                             possibly including delegated coins, if needed.\n"
                "2. \"amounts\"             (array, required) An array of json objects representing the amounts to send.\n"
                "    [{\n"
                "      \"address\":address  (string, required) The address is a transparent addr or shield addr\n"
                "      \"amount\":amount    (numeric, required) The numeric amount in " + "PIV" + " is the value\n"
                "      \"memo\":memo        (string, optional) If the address is a shield addr, message string of max 512 bytes\n"
                "    }, ... ]\n"
                "3. minconf               (numeric, optional, default=1) Only use funds confirmed at least this many times.\n"
                "4. fee                   (numeric, optional), The fee amount to attach to this transaction.\n"
                "                            If not specified, or set to 0, the wallet will try to compute the minimum possible fee for a shield TX,\n"
                "                            based on the expected transaction size and the current value of -minRelayTxFee.\n"
                "5. subtract_fee_from     (array, optional) A json array with addresses.\n"
                "                           The fee will be equally deducted from the amount of each selected address.\n"
                "                           Those recipients will receive less PIV than you enter in their corresponding amount field.\n"
                "                           If no addresses are specified here, the sender pays the fee.\n"
                "    [\n"
                "      \"address\"          (string) Subtract fee from this address\n"
                "      ,...\n"
                "    ]\n"
                "\nResult:\n"
                "\"id\"          (string) transaction hash in the network\n"
                "\nExamples:\n"
                + HelpExampleCli("shieldsendmany",
                                 "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" '[{\"address\": \"ps1ra969yfhvhp73rw5ak2xvtcm9fkuqsnmad7qln79mphhdrst3lwu9vvv03yuyqlh42p42st47qd\" ,\"amount\": 5.0}]'")
                + HelpExampleRpc("shieldsendmany",
                                 "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\", [{\"address\": \"ps1ra969yfhvhp73rw5ak2xvtcm9fkuqsnmad7qln79mphhdrst3lwu9vvv03yuyqlh42p42st47qd\" ,\"amount\": 5.0}]")
        );

    EnsureWalletIsUnlocked(pwallet);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    SaplingOperation operation = CreateShieldedTransaction(pwallet, request);
    std::string txHash;
    auto res = operation.send(txHash);
    if (!res)
        throw JSONRPCError(RPC_WALLET_ERROR, res.getError());
    return txHash;
}

UniValue rawshieldsendmany(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 4)
        throw std::runtime_error(
                "rawshieldsendmany \"fromaddress\" [{\"address\":... ,\"amount\":...},...] ( minconf fee )\n"
                "\nCreates a transaction sending to many recipients (without committing it), and returns the hex string."
                "\nAmounts are decimal numbers with at most 8 digits of precision."
                "\nChange generated from a transparent addr flows to a new  transparent addr address, while change generated from a shield addr returns to itself."
                "\nWhen sending coinbase UTXOs to a shield addr, change is not allowed. The entire value of the UTXO(s) must be consumed."
                + HelpRequiringPassphrase(pwallet) + "\n"

                "\nArguments:\n"
                "1. \"fromaddress\"         (string, required) The transparent addr or shield addr to send the funds from.\n"
                "                             It can also be the string \"from_transparent\"|\"from_shield\" to send the funds\n"
                "                             from any transparent|shield address available.\n"
                "                             Additionally, it can be the string \"from_trans_cold\" to select transparent funds,\n"
                "                             possibly including delegated coins, if needed.\n"
                "2. \"amounts\"             (array, required) An array of json objects representing the amounts to send.\n"
                "    [{\n"
                "      \"address\":address  (string, required) The address is a transparent addr or shield addr\n"
                "      \"amount\":amount    (numeric, required) The numeric amount in " + "PIV" + " is the value\n"
                "      \"memo\":memo        (string, optional) If the address is a shield addr, message string of max 512 bytes\n"
                "    }, ... ]\n"
                "3. minconf               (numeric, optional, default=1) Only use funds confirmed at least this many times.\n"
                "4. fee                   (numeric, optional), The fee amount to attach to this transaction.\n"
                "                            If not specified, the wallet will try to compute the minimum possible fee for a shield TX,\n"
                "                            based on the expected transaction size and the current value of -minRelayTxFee.\n"
                "\nResult:\n"
                "\"transaction\"            (string) hex string of the transaction\n"

                "\nExamples:\n"
                + HelpExampleCli("rawshieldsendmany",
                                 "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" '[{\"address\": \"ps1ra969yfhvhp73rw5ak2xvtcm9fkuqsnmad7qln79mphhdrst3lwu9vvv03yuyqlh42p42st47qd\" ,\"amount\": 5.0}]'")
                + HelpExampleRpc("rawshieldsendmany",
                                 "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\", [{\"address\": \"ps1ra969yfhvhp73rw5ak2xvtcm9fkuqsnmad7qln79mphhdrst3lwu9vvv03yuyqlh42p42st47qd\" ,\"amount\": 5.0}]")
        );

    EnsureWalletIsUnlocked(pwallet);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    CTransaction tx = CreateShieldedTransaction(pwallet, request).getFinalTx();
    return EncodeHexTx(tx);
}

UniValue listaddressgroupings(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || !request.params.empty())
        throw std::runtime_error(
            "listaddressgroupings\n"
            "\nLists groups of addresses which have had their common ownership\n"
            "made public by common use as inputs or as the resulting change\n"
            "in past transactions\n"

            "\nResult:\n"
            "[\n"
            "  [\n"
            "    [\n"
            "      \"pivxaddress\",     (string) The pivx address\n"
            "      amount,                 (numeric) The amount in PIV\n"
            "      \"label\"             (string, optional) The label\n"
            "    ]\n"
            "    ,...\n"
            "  ]\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("listaddressgroupings", "") + HelpExampleRpc("listaddressgroupings", ""));

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    UniValue jsonGroupings(UniValue::VARR);
    std::map<CTxDestination, CAmount> balances = pwallet->GetAddressBalances();
    for (std::set<CTxDestination> grouping : pwallet->GetAddressGroupings()) {
        UniValue jsonGrouping(UniValue::VARR);
        for (CTxDestination address : grouping) {
            UniValue addressInfo(UniValue::VARR);
            addressInfo.push_back(EncodeDestination(address));
            addressInfo.push_back(ValueFromAmount(balances[address]));
            auto optAdd = pwallet->GetAddressBookEntry(address);
            if (optAdd) {
                addressInfo.push_back(optAdd->name);
            }
            jsonGrouping.push_back(addressInfo);
        }
        jsonGroupings.push_back(jsonGrouping);
    }
    return jsonGroupings;
}

UniValue signmessage(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
            "signmessage \"address\" \"message\"\n"
            "\nSign a message with the private key of an address" +
            HelpRequiringPassphrase(pwallet) + "\n"

            "\nArguments:\n"
            "1. \"address\"     (string, required) The pivx address to use for the private key.\n"
            "2. \"message\"     (string, required) The message to create a signature of.\n"

            "\nResult:\n"
            "\"signature\"          (string) The signature of the message encoded in base 64\n"

            "\nExamples:\n"
            "\nUnlock the wallet for 30 seconds\n" +
            HelpExampleCli("walletpassphrase", "\"mypassphrase\" 30") +
            "\nCreate the signature\n" +
            HelpExampleCli("signmessage", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" \"my message\"") +
            "\nVerify the signature\n" +
            HelpExampleCli("verifymessage", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" \"signature\" \"my message\"") +
            "\nAs json rpc\n" +
            HelpExampleRpc("signmessage", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\", \"my message\""));

    LOCK2(cs_main, pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    std::string strAddress = request.params[0].get_str();
    std::string strMessage = request.params[1].get_str();

    CTxDestination dest = DecodeDestination(strAddress);
    if (!IsValidDestination(dest))
        throw JSONRPCError(RPC_TYPE_ERROR, "Invalid address");

    const CKeyID* keyID = boost::get<CKeyID>(&dest);
    if (!keyID)
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to key");

    CKey key;
    if (!pwallet->GetKey(*keyID, key))
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key not available");

    CHashWriter ss(SER_GETHASH, 0);
    ss << strMessageMagic;
    ss << strMessage;

    std::vector<unsigned char> vchSig;
    if (!key.SignCompact(ss.GetHash(), vchSig))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Sign failed");

    return EncodeBase64(vchSig);
}

UniValue getreceivedbyaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "getreceivedbyaddress \"address\" ( minconf )\n"
            "\nReturns the total amount received by the given pivxaddress in transactions with at least minconf confirmations.\n"

            "\nArguments:\n"
            "1. \"address\"    (string, required) The pivx address for transactions.\n"
            "2. minconf        (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"

            "\nResult:\n"
            "amount   (numeric) The total amount in PIV received at this address.\n"

            "\nExamples:\n"
            "\nThe amount from transactions with at least 1 confirmation\n" +
            HelpExampleCli("getreceivedbyaddress", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\"") +
            "\nThe amount including unconfirmed transactions, zero confirmations\n" +
            HelpExampleCli("getreceivedbyaddress", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" 0") +
            "\nThe amount with at least 6 confirmation, very safe\n" +
            HelpExampleCli("getreceivedbyaddress", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" 6") +
            "\nAs a json rpc call\n" +
            HelpExampleRpc("getreceivedbyaddress", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\", 6"));

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);
    int nBlockHeight = chainActive.Height();

    // pivx address
    CTxDestination address = DecodeDestination(request.params[0].get_str());
    if (!IsValidDestination(address))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid PIVX address");
    CScript scriptPubKey = GetScriptForDestination(address);
    if (!IsMine(*pwallet, scriptPubKey))
        throw JSONRPCError(RPC_WALLET_ERROR, "Address not found in wallet");

    // Minimum confirmations
    int nMinDepth = 1;
    if (request.params.size() > 1)
        nMinDepth = request.params[1].get_int();

    // Tally
    CAmount nAmount = 0;
    for (std::map<uint256, CWalletTx>::iterator it = pwallet->mapWallet.begin(); it != pwallet->mapWallet.end(); ++it) {
        const CWalletTx& wtx = (*it).second;
        if (wtx.IsCoinBase() || !IsFinalTx(wtx.tx, nBlockHeight))
            continue;

        for (const CTxOut& txout : wtx.tx->vout)
            if (txout.scriptPubKey == scriptPubKey)
                if (wtx.GetDepthInMainChain() >= nMinDepth)
                    nAmount += txout.nValue;
    }

    return ValueFromAmount(nAmount);
}


UniValue getreceivedbylabel(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "getreceivedbylabel \"label\" ( minconf )\n"
            "\nReturns the total amount received by addresses with <label> in transactions with at least [minconf] confirmations.\n"

            "\nArguments:\n"
            "1. \"label\"      (string, required) The selected label, may be the default label using \"\".\n"
            "2. minconf          (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"

            "\nResult:\n"
            "amount              (numeric) The total amount in PIV received for this label.\n"

            "\nExamples:\n"
            "\nAmount received by the default label with at least 1 confirmation\n" +
            HelpExampleCli("getreceivedbylabel", "\"\"") +
            "\nAmount received at the tabby label including unconfirmed amounts with zero confirmations\n" +
            HelpExampleCli("getreceivedbylabel", "\"tabby\" 0") +
            "\nThe amount with at least 6 confirmation, very safe\n" +
            HelpExampleCli("getreceivedbylabel", "\"tabby\" 6") +
            "\nAs a json rpc call\n" +
            HelpExampleRpc("getreceivedbylabel", "\"tabby\", 6"));

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);
    int nBlockHeight = chainActive.Height();

    // Minimum confirmations
    int nMinDepth = 1;
    if (request.params.size() > 1)
        nMinDepth = request.params[1].get_int();

    // Get the set of pub keys assigned to label
    std::string label = LabelFromValue(request.params[0]);
    std::set<CTxDestination> setAddress = pwallet->GetLabelAddresses(label);

    // Tally
    CAmount nAmount = 0;
    for (std::map<uint256, CWalletTx>::iterator it = pwallet->mapWallet.begin(); it != pwallet->mapWallet.end(); ++it) {
        const CWalletTx& wtx = (*it).second;
        if (wtx.IsCoinBase() || !IsFinalTx(wtx.tx, nBlockHeight))
            continue;

        for (const CTxOut& txout : wtx.tx->vout) {
            CTxDestination address;
            if (ExtractDestination(txout.scriptPubKey, address) && IsMine(*pwallet, address) && setAddress.count(address))
                if (wtx.GetDepthInMainChain() >= nMinDepth)
                    nAmount += txout.nValue;
        }
    }

    return (double)nAmount / (double)COIN;
}

UniValue getbalance(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || (request.params.size() > 4 ))
        throw std::runtime_error(
            "getbalance ( minconf include_watchonly include_delegated include_shield )\n"
            "\nReturns the server's total available balance.\n"
            "The available balance is what the wallet considers currently spendable, and is\n"
            "thus affected by options which limit spendability such as -spendzeroconfchange.\n"

            "\nArguments:\n"
            "1. minconf          (numeric, optional, default=0) Only include transactions confirmed at least this many times.\n"
            "2. include_watchonly (bool, optional, default=false) Also include balance in watchonly addresses (see 'importaddress')\n"
            "3. include_delegated (bool, optional, default=true) Also include balance delegated to cold stakers\n"
            "4. include_shield    (bool, optional, default=true) Also include shield balance\n"

            "\nResult:\n"
            "amount              (numeric) The total amount in PIV received for this wallet.\n"

            "\nExamples:\n"
            "\nThe total amount in the wallet\n" +
            HelpExampleCli("getbalance", "") +
            "\nThe total amount in the wallet, with at least 5 confirmations\n" +
            HelpExampleCli("getbalance", "6") +
            "\nAs a json rpc call\n" +
            HelpExampleRpc("getbalance", "6"));

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    const int paramsSize = request.params.size();
    const int nMinDepth = paramsSize > 0 ? request.params[0].get_int() : 0;
    bool fIncludeWatchOnly = paramsSize > 1 && request.params[1].get_bool();
    bool fIncludeDelegated = paramsSize <= 2 || request.params[2].get_bool();
    bool fIncludeShielded = paramsSize <= 3 || request.params[3].get_bool();

    isminefilter filter = ISMINE_SPENDABLE | (fIncludeWatchOnly ?
                                              (fIncludeShielded ? ISMINE_WATCH_ONLY_ALL : ISMINE_WATCH_ONLY) : ISMINE_NO);
    filter |= fIncludeDelegated ? ISMINE_SPENDABLE_DELEGATED : ISMINE_NO;
    filter |= fIncludeShielded ? ISMINE_SPENDABLE_SHIELDED : ISMINE_NO;
    return ValueFromAmount(pwallet->GetAvailableBalance(filter, true, nMinDepth));
}

UniValue getcoldstakingbalance(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || (request.params.size() != 0))
        throw std::runtime_error(
            "getcoldstakingbalance\n"
            "\nReturns the server's total available cold balance.\n"

            "\nResult:\n"
            "amount              (numeric) The total amount in PIV received for this wallet in P2CS contracts.\n"

            "\nExamples:\n"
            "\nThe total amount in the wallet\n" +
            HelpExampleCli("getcoldstakingbalance", "") +
            "\nAs a json rpc call\n" +
            HelpExampleRpc("getcoldstakingbalance", "\"*\""));

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    return ValueFromAmount(pwallet->GetColdStakingBalance());
}

UniValue getdelegatedbalance(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || (request.params.size() != 0))
        throw std::runtime_error(
            "getdelegatedbalance\n"
            "\nReturns the server's total available delegated balance (sum of all utxos delegated\n"
            "to a cold staking address to stake on behalf of addresses of this wallet).\n"

            "\nResult:\n"
            "amount              (numeric) The total amount in PIV received for this wallet in P2CS contracts.\n"

            "\nExamples:\n"
            "\nThe total amount in the wallet\n" +
            HelpExampleCli("getdelegatedbalance", "") +
            "\nAs a json rpc call\n" +
            HelpExampleRpc("getdelegatedbalance", "\"*\""));

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    return ValueFromAmount(pwallet->GetDelegatedBalance());
}

UniValue getunconfirmedbalance(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 0)
        throw std::runtime_error(
            "getunconfirmedbalance\n"
            "Returns the server's total unconfirmed balance\n");

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    return ValueFromAmount(pwallet->GetUnconfirmedBalance());
}

/*
 * Only used for t->t transactions (via sendmany RPC)
 */
static UniValue legacy_sendmany(CWallet* const pwallet, const UniValue& sendTo, int nMinDepth, std::string comment, bool fIncludeDelegated, const UniValue& subtractFeeFromAmount)
{
    LOCK2(cs_main, pwallet->cs_wallet);

    if (!g_connman)
        throw JSONRPCError(RPC_CLIENT_P2P_DISABLED, "Error: Peer-to-peer functionality missing or disabled");

    isminefilter filter = ISMINE_SPENDABLE | (fIncludeDelegated ? ISMINE_SPENDABLE_DELEGATED : ISMINE_NO);

    CTransactionRef txNew;
    std::set<CTxDestination> setAddress;
    std::vector<CRecipient> vecSend;

    CAmount totalAmount = 0;
    std::vector<std::string> keys = sendTo.getKeys();
    for (const std::string& name_ : keys) {
        bool isStaking = false;
        CTxDestination dest = DecodeDestination(name_,isStaking);
        if (!IsValidDestination(dest) || isStaking)
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid PIVX address: ")+name_);

        if (setAddress.count(dest))
            throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ")+name_);
        setAddress.insert(dest);

        CScript scriptPubKey = GetScriptForDestination(dest);
        CAmount nAmount = AmountFromValue(sendTo[name_]);
        totalAmount += nAmount;

        bool fSubtractFeeFromAmount = false;
        for (unsigned int idx = 0; idx < subtractFeeFromAmount.size(); idx++) {
            const UniValue& addr = subtractFeeFromAmount[idx];
            if (addr.get_str() == name_) {
                fSubtractFeeFromAmount = true;
                break;
            }
        }

        vecSend.emplace_back(scriptPubKey, nAmount, fSubtractFeeFromAmount);
    }

    // Check funds
    if (totalAmount > pwallet->GetLegacyBalance(filter, nMinDepth)) {
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Wallet has insufficient funds");
    }

    // Send
    CReserveKey keyChange(pwallet);
    CAmount nFeeRequired = 0;
    std::string strFailReason;
    int nChangePosInOut = -1;
    bool fCreated = pwallet->CreateTransaction(vecSend, txNew, keyChange, nFeeRequired, nChangePosInOut, strFailReason,
                                               nullptr,     // coinControl
                                               true,        // sign
                                               0,           // nFeePay
                                               fIncludeDelegated);
    if (!fCreated)
        throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, strFailReason);
    const CWallet::CommitResult& res = pwallet->CommitTransaction(txNew, keyChange, g_connman.get());
    if (res.status != CWallet::CommitStatus::OK)
        throw JSONRPCError(RPC_WALLET_ERROR, res.ToString());

    // Set comment
    CWalletTx& wtx = pwallet->mapWallet.at(txNew->GetHash());
    if (!comment.empty()) {
        wtx.mapValue["comment"] = comment;
    }

    return wtx.GetHash().GetHex();
}

/*
 * This function uses [legacy_sendmany] in the background.
 * If any recipient is a shield address, instead it uses [shieldsendmany "from_transparent"].
 */
UniValue sendmany(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 6)
        throw std::runtime_error(
            "sendmany \"\" {\"address\":amount,...} ( minconf \"comment\" include_delegated )\n"
            "\nSend to multiple destinations. Recipients are transparent or shield PIVX addresses.\n"
            "\nAmounts are double-precision floating point numbers.\n"
            + HelpRequiringPassphrase(pwallet) + "\n"

            "\nArguments:\n"
            "1. \"dummy\"               (string, required) Must be set to \"\" for backwards compatibility.\n"
            "2. \"amounts\"             (string, required) A json object with addresses and amounts\n"
            "    {\n"
            "      \"address\":amount   (numeric) The pivx address (either transparent or shield) is the key,\n"
            "                                     the numeric amount in PIV is the value\n"
            "      ,...\n"
            "    }\n"
            "3. minconf                 (numeric, optional, default=1) Only use the balance confirmed at least this many times.\n"
            "4. \"comment\"             (string, optional) A comment\n"
            "5. include_delegated       (bool, optional, default=false) Also include balance delegated to cold stakers\n"
            "6. subtract_fee_from       (array, optional) A json array with addresses.\n"
            "                           The fee will be equally deducted from the amount of each selected address.\n"
            "                           Those recipients will receive less PIV than you enter in their corresponding amount field.\n"
            "                           If no addresses are specified here, the sender pays the fee.\n"
            "    [\n"
            "      \"address\"          (string) Subtract fee from this address\n"
            "      ,...\n"
            "    ]\n"

            "\nResult:\n"
            "\"transactionid\"          (string) The transaction id for the send. Only 1 transaction is created regardless of \n"
            "                                    the number of addresses.\n"

            "\nExamples:\n"
            "\nSend two amounts to two different addresses:\n" +
            HelpExampleCli("sendmany", "\"\" \"{\\\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\\\":0.01,\\\"DAD3Y6ivr8nPQLT1NEPX84DxGCw9jz9Jvg\\\":0.02}\"") +
            "\nSend two amounts to two different addresses setting the confirmation and comment:\n" +
            HelpExampleCli("sendmany", "\"\" \"{\\\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\\\":0.01,\\\"DAD3Y6ivr8nPQLT1NEPX84DxGCw9jz9Jvg\\\":0.02}\" 6 \"testing\"") +
            "\nSend to shield address:\n" +
            HelpExampleCli("sendmany", "\"\" \"{\\\"ps1u87kylcmn28yclnx2uy0psnvuhs2xn608ukm6n2nshrpg2nzyu3n62ls8j77m9cgp40dx40evej\\\":10}\"") +
            "\nAs a json rpc call\n" +
            HelpExampleRpc("sendmany", "\"\", \"{\\\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\\\":0.01,\\\"DAD3Y6ivr8nPQLT1NEPX84DxGCw9jz9Jvg\\\":0.02}\", 6, \"testing\"")
        );

    EnsureWalletIsUnlocked(pwallet);

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    // Read Params
    if (!request.params[0].isNull() && !request.params[0].get_str().empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Dummy value must be set to \"\"");
    }
    const UniValue sendTo = request.params[1].get_obj();
    const int nMinDepth = request.params.size() > 2 ? request.params[2].get_int() : 1;
    const std::string comment = (request.params.size() > 3 && !request.params[3].isNull() && !request.params[3].get_str().empty()) ?
                        request.params[3].get_str() : "";
    const bool fIncludeDelegated = (request.params.size() > 4 && request.params[4].get_bool());

    UniValue subtractFeeFromAmount(UniValue::VARR);
    if (request.params.size() > 5 && !request.params[5].isNull())
        subtractFeeFromAmount = request.params[5].get_array();

    // Check  if any recipient address is shield
    bool fShieldSend = false;
    for (const std::string& key : sendTo.getKeys()) {
        bool isStaking = false, isShielded = false;
        const CWDestination& dest = Standard::DecodeDestination(key, isStaking, isShielded);
        if (isShielded) {
            fShieldSend = true;
            break;
        }
    }

    if (fShieldSend) {
        return ShieldSendManyTo(pwallet, sendTo, comment, "", nMinDepth, fIncludeDelegated, subtractFeeFromAmount);
    }

    // All recipients are transparent: use Legacy sendmany t->t
    return legacy_sendmany(pwallet, sendTo, nMinDepth, comment, fIncludeDelegated, subtractFeeFromAmount);
}

// Defined in rpc/misc.cpp
extern CScript _createmultisig_redeemScript(CWallet* const pwallet, const UniValue& params);

UniValue addmultisigaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 3)
        throw std::runtime_error(
            "addmultisigaddress nrequired [\"key\",...] ( \"label\" )\n"
            "\nAdd a nrequired-to-sign multisignature address to the wallet. Requires a new wallet backup.\n"
            "Each key is a PIVX address or hex-encoded public key.\n"
            "If 'label' is specified, assign address to that label.\n"

            "\nArguments:\n"
            "1. nrequired        (numeric, required) The number of required signatures out of the n keys or addresses.\n"
            "2. \"keys\"         (string, required) A json array of pivx addresses or hex-encoded public keys\n"
            "     [\n"
            "       \"address\"  (string) pivx address or hex-encoded public key\n"
            "       ...,\n"
            "     ]\n"
            "3. \"label\"        (string, optional) A label to assign the addresses to.\n"

            "\nResult:\n"
            "\"pivxaddress\"  (string) A pivx address associated with the keys.\n"

            "\nExamples:\n"
            "\nAdd a multisig address from 2 addresses\n" +
            HelpExampleCli("addmultisigaddress", "2 \"[\\\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\\\",\\\"DAD3Y6ivr8nPQLT1NEPX84DxGCw9jz9Jvg\\\"]\"") +
            "\nAs json rpc call\n" +
            HelpExampleRpc("addmultisigaddress", "2, \"[\\\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\\\",\\\"DAD3Y6ivr8nPQLT1NEPX84DxGCw9jz9Jvg\\\"]\""));

    LOCK2(cs_main, pwallet->cs_wallet);

    std::string label;
    if (request.params.size() > 2)
        label = LabelFromValue(request.params[2]);

    // Construct using pay-to-script-hash:
    CScript inner = _createmultisig_redeemScript(pwallet, request.params);
    CScriptID innerID(inner);
    pwallet->AddCScript(inner);

    pwallet->SetAddressBook(innerID, label, AddressBook::AddressBookPurpose::SEND);
    return EncodeDestination(innerID);
}


struct tallyitem {
    CAmount nAmount;
    int nConf;
    std::vector<uint256> txids;
    bool fIsWatchonly;
    tallyitem()
    {
        nAmount = 0;
        nConf = std::numeric_limits<int>::max();
        fIsWatchonly = false;
    }
};

static UniValue ListReceived(CWallet* const pwallet, const UniValue& params, bool by_label, int nBlockHeight)
{
    // Minimum confirmations
    int nMinDepth = 1;
    if (params.size() > 0)
        nMinDepth = params[0].get_int();

    // Whether to include empty labels
    bool fIncludeEmpty = false;
    if (params.size() > 1)
        fIncludeEmpty = params[1].get_bool();

    isminefilter filter = ISMINE_SPENDABLE_ALL;
    if (params.size() > 2)
        if (params[2].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    bool has_filtered_address = false;
    CTxDestination filtered_address = CNoDestination();
    if (!by_label && params.size() > 3) {
        CTxDestination dest = DecodeDestination(params[3].get_str());
        if (!IsValidDestination(dest)) {
            throw JSONRPCError(RPC_WALLET_ERROR, "address_filter parameter was invalid");
        }
        filtered_address = dest;
        has_filtered_address = true;
    }

    // Tally
    std::map<CTxDestination, tallyitem> mapTally;
    for (std::map<uint256, CWalletTx>::iterator it = pwallet->mapWallet.begin(); it != pwallet->mapWallet.end(); ++it) {
        const CWalletTx& wtx = (*it).second;

        if (!IsFinalTx(wtx.tx, nBlockHeight)) {
            continue;
        }

        int nDepth = wtx.GetDepthInMainChain();
        if (nDepth < nMinDepth) {
            continue;
        }

        for (const CTxOut& txout : wtx.tx->vout) {
            CTxDestination address;
            if (!ExtractDestination(txout.scriptPubKey, address)) {
                continue;
            }

            if (has_filtered_address && !(filtered_address == address)) {
                continue;
            }

            isminefilter mine = IsMine(*pwallet, address);
            if (!(mine & filter)) {
                continue;
            }

            tallyitem& item = mapTally[address];
            item.nAmount += txout.nValue;
            item.nConf = std::min(item.nConf, nDepth);
            item.txids.push_back(wtx.GetHash());
            if (mine & ISMINE_WATCH_ONLY)
                item.fIsWatchonly = true;
        }
    }

    // Create mapAddressBook iterator
    // If we aren't filtering, go from begin() to end()
    auto itAddr = pwallet->NewAddressBookIterator();
    // If we are filtering, find() the applicable entry
    if (has_filtered_address) {
        itAddr.SetFilter(filtered_address);
    }

    // Reply
    UniValue ret(UniValue::VARR);
    std::map<std::string, tallyitem> label_tally;

    for (auto& itAddrBook = itAddr; itAddrBook.IsValid(); itAddrBook.Next()) {

        auto* dest = itAddrBook.GetCTxDestKey();
        if (!dest) continue;

        const auto &address = *dest;
        const std::string &label = itAddrBook.GetValue().name;
        auto it = mapTally.find(address);
        if (it == mapTally.end() && !fIncludeEmpty) {
            continue;
        }

        CAmount nAmount = 0;
        int nConf = std::numeric_limits<int>::max();
        bool fIsWatchonly = false;
        if (it != mapTally.end()) {
            nAmount = (*it).second.nAmount;
            nConf = (*it).second.nConf;
            fIsWatchonly = (*it).second.fIsWatchonly;
        }

        if (by_label) {
            tallyitem& _item = label_tally[label];
            _item.nAmount += nAmount;
            _item.nConf = std::min(_item.nConf, nConf);
            _item.fIsWatchonly = fIsWatchonly;
        } else {
            UniValue obj(UniValue::VOBJ);
            if (fIsWatchonly)
                obj.pushKV("involvesWatchonly", true);
            obj.pushKV("address", EncodeDestination(address, AddressBook::IsColdStakingPurpose(label)));
            obj.pushKV("amount", ValueFromAmount(nAmount));
            if (nConf == std::numeric_limits<int>::max()) nConf = 0;
            obj.pushKV("confirmations", nConf);
            obj.pushKV("bcconfirmations", nConf);       // DEPRECATED in 4.3.99
            obj.pushKV("label", label);
            UniValue transactions(UniValue::VARR);
            if (it != mapTally.end()) {
                for (const uint256 &item : (*it).second.txids) {
                    transactions.push_back(item.GetHex());
                }
            }
            obj.pushKV("txids", transactions);
            ret.push_back(obj);
        }
    }

    if (by_label) {
        for (const auto& entry : label_tally) {
            CAmount nAmount = entry.second.nAmount;
            int nConf = entry.second.nConf;
            UniValue obj(UniValue::VOBJ);
            if (entry.second.fIsWatchonly)
                obj.pushKV("involvesWatchonly", true);
            obj.pushKV("amount", ValueFromAmount(nAmount));
            obj.pushKV("confirmations", (nConf == std::numeric_limits<int>::max() ? 0 : nConf));
            obj.pushKV("label", entry.first);
            ret.push_back(obj);
        }
    }

    return ret;
}

UniValue listreceivedbyaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 4)
        throw std::runtime_error(
            "listreceivedbyaddress ( minconf include_empty include_watchonly filter)\n"
            "\nList balances by receiving address.\n"

            "\nArguments:\n"
            "1. minconf           (numeric, optional, default=1) The minimum number of confirmations before payments are included.\n"
            "2. include_empty     (numeric, optional, default=false) Whether to include addresses that haven't received any payments.\n"
            "3. include_watchonly (bool, optional, default=false) Whether to include watchonly addresses (see 'importaddress').\n"
            "4. filter            (string, optional) If present, only return information on this address.\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"involvesWatchonly\" : \"true\",    (bool) Only returned if imported addresses were involved in transaction\n"
            "    \"address\" : \"receivingaddress\",  (string) The receiving address\n"
            "    \"amount\" : x.xxx,                  (numeric) The total amount in PIV received by the address\n"
            "    \"confirmations\" : n                (numeric) The number of confirmations of the most recent transaction included\n"
            "    \"bcconfirmations\" : n,             (numeric) DEPRECATED: Will be removed in a future release\n"
            "    \"label\" : \"label\",               (string) The label of the receiving address. The default label is \"\".\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("listreceivedbyaddress", "") +
            HelpExampleCli("listreceivedbyaddress", "6 true") +
            HelpExampleRpc("listreceivedbyaddress", "6, true, true") +
            HelpExampleRpc("listreceivedbyaddress", "6, true, true, \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\""));

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);
    int nBlockHeight = chainActive.Height();
    return ListReceived(pwallet, request.params, false, nBlockHeight);
}

UniValue listreceivedbyshieldaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size()==0 || request.params.size() >2)
        throw std::runtime_error(
                "listreceivedbyshieldaddress \"address\" ( minconf )\n"
                "\nReturn a list of amounts received by a shield addr belonging to the node's wallet.\n"

                "\nArguments:\n"
                "1. \"address\"      (string) The private address.\n"
                "2. minconf          (numeric, optional, default=1) Only include transactions confirmed at least this many times.\n"

                "\nResult:\n"
                "{\n"
                "  \"txid\": \"txid\",           (string) the transaction id\n"
                "  \"amount\": xxxxx,         (numeric) the amount of value in the note\n"
                "  \"memo\": xxxxx,           (string) hexadecimal string representation of memo field\n"
                "  \"confirmations\" : n,     (numeric) the number of confirmations\n"
                "  \"blockheight\": n,         (numeric) The block height containing the transaction\n"
                "  \"blockindex\": n,         (numeric) The block index containing the transaction.\n"
                "  \"blocktime\": xxx,              (numeric) The transaction time in seconds since epoch (midnight Jan 1 1970 GMT).\n"
                "  \"outindex\" (sapling) : n,     (numeric) the output index\n"
                "  \"change\": true|false,    (boolean) true if the address that received the note is also one of the sending addresses\n"
                "}\n"

                "\nExamples:\n"
                + HelpExampleCli("listreceivedbyshieldaddress", "\"ps1ra969yfhvhp73rw5ak2xvtcm9fkuqsnmad7qln79mphhdrst3lwu9vvv03yuyqlh42p42st47qd\"")
                + HelpExampleRpc("listreceivedbyshieldaddress", "\"ps1ra969yfhvhp73rw5ak2xvtcm9fkuqsnmad7qln79mphhdrst3lwu9vvv03yuyqlh42p42st47qd\"")
        );

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    int nMinDepth = 1;
    if (request.params.size() > 1) {
        nMinDepth = request.params[1].get_int();
    }
    if (nMinDepth < 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Minimum number of confirmations cannot be less than 0");
    }

    // Check that the from address is valid.
    auto fromaddress = request.params[0].get_str();

    auto zaddr = KeyIO::DecodeSaplingPaymentAddress(fromaddress);
    if (!zaddr) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid addr.");
    }
    libzcash::SaplingPaymentAddress shieldAddr = *zaddr;

    auto sspkm = pwallet->GetSaplingScriptPubKeyMan();
    // Visitor to support Sapling addrs
    if (!sspkm->PaymentAddressBelongsToWallet(shieldAddr)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "From address does not belong to this node, shield addr spending key or viewing key not found.");
    }

    UniValue result(UniValue::VARR);
    std::vector<SaplingNoteEntry> saplingEntries;
    sspkm->GetFilteredNotes(saplingEntries, zaddr, nMinDepth, false, false);

    std::set<std::pair<libzcash::PaymentAddress, uint256>> nullifierSet;
    bool hasSpendingKey = pwallet->HaveSpendingKeyForPaymentAddress(shieldAddr);
    if (hasSpendingKey) {
        nullifierSet = sspkm->GetNullifiersForAddresses({*zaddr});
    }

    for (const SaplingNoteEntry& entry : saplingEntries) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("txid", entry.op.hash.ToString());
        obj.pushKV("amount", ValueFromAmount(CAmount(entry.note.value())));
        obj.pushKV("memo", HexStrTrimmed(entry.memo));
        obj.pushKV("outindex", (int)entry.op.n);
        obj.pushKV("confirmations", entry.confirmations);

        int height = 0;
        int index = -1;
        int64_t time = 0;

        if (pwallet->mapWallet.count(entry.op.hash)) {
            const CWalletTx& wtx = pwallet->mapWallet.at(entry.op.hash);
            if (!wtx.m_confirm.hashBlock.IsNull())
                height = mapBlockIndex[wtx.m_confirm.hashBlock]->nHeight;
            index = wtx.m_confirm.nIndex;
            time = wtx.GetTxTime();
        }

        obj.pushKV("blockheight", height);
        obj.pushKV("blockindex", index);
        obj.pushKV("blocktime", time);

        if (hasSpendingKey) {
            obj.pushKV("change", sspkm->IsNoteSaplingChange(nullifierSet, entry.address, entry.op));
        }
        result.push_back(obj);
    }
    return result;
}

UniValue listreceivedbylabel(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 3)
        throw std::runtime_error(
            "listreceivedbylabel ( minconf include_empty include_watchonly)\n"
            "\nList received transactions by label.\n"

            "\nArguments:\n"
            "1. minconf           (numeric, optional, default=1) The minimum number of confirmations before payments are included.\n"
            "2. include_empty     (boolean, optional, default=false) Whether to include labels that haven't received any payments.\n"
            "3. include_watchonly (bool, optional, default=false) Whether to include watchonly addresses (see 'importaddress').\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"involvesWatchonly\" : \"true\",    (bool) Only returned if imported addresses were involved in transaction\n"
            "    \"amount\" : x.xxx,                  (numeric) The total amount received by addresses with this label\n"
            "    \"confirmations\" : n                (numeric) The number of confirmations of the most recent transaction included\n"
            "    \"bcconfirmations\" : n,             (numeric) DEPRECATED: Will be removed in a future release\n"
            "    \"label\" : \"label\"                (string) The label of the receiving address. The default label is \"\".\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("listreceivedbylabel", "") + HelpExampleCli("listreceivedbylabel", "6 true") + HelpExampleRpc("listreceivedbylabel", "6, true, true"));

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);
    int nBlockHeight = chainActive.Height();
    return ListReceived(pwallet, request.params, true, nBlockHeight);
}

UniValue listcoldutxos(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "listcoldutxos ( not_whitelisted )\n"
            "\nList P2CS unspent outputs received by this wallet as cold-staker-\n"

            "\nArguments:\n"
            "1. not_whitelisted   (boolean, optional, default=false) Whether to exclude P2CS from whitelisted delegators.\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"txid\" : \"true\",            (string) The transaction id of the P2CS utxo\n"
            "    \"txidn\" : n                 (numeric) The output number of the P2CS utxo\n"
            "    \"amount\" : x.xxx,           (numeric) The amount of the P2CS utxo\n"
            "    \"confirmations\" : n         (numeric) The number of confirmations of the P2CS utxo\n"
            "    \"cold-staker\" : \"address\"   (string) The cold-staker address of the P2CS utxo\n"
            "    \"coin-owner\" : \"address\"    (string) The coin-owner address of the P2CS utxo\n"
            "    \"whitelisted\" : \"true\"      (boolean) \"true\"/\"false\" coin-owner in delegator whitelist\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("listcoldutxos", "") + HelpExampleCli("listcoldutxos", "true"));

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    bool fExcludeWhitelisted = false;
    if (request.params.size() > 0)
        fExcludeWhitelisted = request.params[0].get_bool();
    UniValue results(UniValue::VARR);

    for (std::map<uint256, CWalletTx>::const_iterator it =
            pwallet->mapWallet.begin(); it != pwallet->mapWallet.end(); ++it) {
        const uint256& wtxid = it->first;
        const CWalletTx* pcoin = &(*it).second;
        if (!CheckFinalTx(pcoin->tx) || !pcoin->IsTrusted())
            continue;

        // if this tx has no unspent P2CS outputs for us, skip it
        if(pcoin->GetColdStakingCredit() == 0 && pcoin->GetStakeDelegationCredit() == 0)
            continue;

        for (unsigned int i = 0; i < pcoin->tx->vout.size(); i++) {
            const CTxOut& out = pcoin->tx->vout[i];
            isminetype mine = pwallet->IsMine(out);
            if (!bool(mine & ISMINE_COLD) && !bool(mine & ISMINE_SPENDABLE_DELEGATED))
                continue;
            txnouttype type;
            std::vector<CTxDestination> addresses;
            int nRequired;
            if (!ExtractDestinations(out.scriptPubKey, type, addresses, nRequired))
                continue;
            const bool fWhitelisted = pwallet->HasAddressBook(addresses[1]) > 0;
            if (fExcludeWhitelisted && fWhitelisted)
                continue;
            UniValue entry(UniValue::VOBJ);
            entry.pushKV("txid", wtxid.GetHex());
            entry.pushKV("txidn", (int)i);
            entry.pushKV("amount", ValueFromAmount(out.nValue));
            entry.pushKV("confirmations", pcoin->GetDepthInMainChain());
            entry.pushKV("cold-staker", EncodeDestination(addresses[0], CChainParams::STAKING_ADDRESS));
            entry.pushKV("coin-owner", EncodeDestination(addresses[1]));
            entry.pushKV("whitelisted", fWhitelisted ? "true" : "false");
            results.push_back(entry);
        }
    }

    return results;
}

static void MaybePushAddress(UniValue & entry, const CTxDestination &dest)
{
    if (IsValidDestination(dest))
        entry.pushKV("address", EncodeDestination(dest));
}

static void ListTransactions(CWallet* const pwallet, const CWalletTx& wtx, int nMinDepth, bool fLong, UniValue& ret, const isminefilter& filter)
{
    CAmount nFee;
    std::list<COutputEntry> listReceived;
    std::list<COutputEntry> listSent;

    wtx.GetAmounts(listReceived, listSent, nFee, filter);

    bool involvesWatchonly = wtx.IsFromMe(ISMINE_WATCH_ONLY);

    // Sent
    if ((!listSent.empty() || nFee != 0)) {
        for (const COutputEntry& s : listSent) {
            UniValue entry(UniValue::VOBJ);
            if (involvesWatchonly || (::IsMine(*pwallet, s.destination) & ISMINE_WATCH_ONLY))
                entry.pushKV("involvesWatchonly", true);
            MaybePushAddress(entry, s.destination);
            entry.pushKV("category", "send");
            entry.pushKV("amount", ValueFromAmount(-s.amount));
            if (pwallet->HasAddressBook(s.destination)) {
                entry.pushKV("label", pwallet->GetNameForAddressBookEntry(s.destination));
            }
            entry.pushKV("vout", s.vout);
            entry.pushKV("fee", ValueFromAmount(-nFee));
            if (fLong)
                WalletTxToJSON(wtx, entry);
            ret.push_back(entry);
        }
    }

    // Received
    int depth = wtx.GetDepthInMainChain();
    if (listReceived.size() > 0 && depth >= nMinDepth) {
        for (const COutputEntry& r : listReceived) {
            std::string label;
            if (pwallet->HasAddressBook(r.destination))
                label = pwallet->GetNameForAddressBookEntry(r.destination);
            UniValue entry(UniValue::VOBJ);
            if (involvesWatchonly || (::IsMine(*pwallet, r.destination) & ISMINE_WATCH_ONLY))
                entry.pushKV("involvesWatchonly", true);
            MaybePushAddress(entry, r.destination);
            if (wtx.IsCoinBase()) {
                if (depth < 1)
                    entry.pushKV("category", "orphan");
                else if (wtx.GetBlocksToMaturity() > 0)
                    entry.pushKV("category", "immature");
                else
                    entry.pushKV("category", "generate");
            } else {
                entry.pushKV("category", "receive");
            }
            entry.pushKV("amount", ValueFromAmount(r.amount));
            if (pwallet->HasAddressBook(r.destination)) {
                entry.pushKV("label", label);
            }
            entry.pushKV("vout", r.vout);
            if (fLong)
                WalletTxToJSON(wtx, entry);
            ret.push_back(entry);
        }
    }
}

UniValue listtransactions(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 6) throw std::runtime_error(
            "listtransactions ( \"dummy\" count from include_watchonly include_delegated include_cold)\n"
            "\nReturns up to 'count' most recent transactions skipping the first 'from' transactions.\n"

            "\nArguments:\n"
            "1. \"dummy\"         (string, optional) If set, should be \"*\" for backwards compatibility.\n"
            "2. count             (numeric, optional, default=10) The number of transactions to return\n"
            "3. from              (numeric, optional, default=0) The number of transactions to skip\n"
            "4. include_watchonly (bool, optional, default=false) Include transactions to watchonly addresses (see 'importaddress')\n"
            "5. include_delegated (bool, optional, default=true) Also include balance delegated to cold stakers\n"
            "6. include_cold      (bool, optional, default=true) Also include delegated balance received as cold-staker by this node\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"address\":\"pivxaddress\",    (string) The pivx address of the transaction.\n"
            "    \"category\":\"category\",      (string) The transaction category (send|receive|orphan|immature|generate).\n"
            "    \"amount\": x.xxx,          (numeric) The amount in PIV. This is negative for the 'send' category, and positive\n"
            "                                         for the 'receive' category,\n"
            "    \"vout\" : n,               (numeric) the vout value\n"
            "    \"fee\": x.xxx,             (numeric) The amount of the fee in PIV. This is negative and only available for the \n"
            "                                         'send' category of transactions.\n"
            "    \"confirmations\": n,     (numeric) The number of blockchain confirmations for the transaction. Available for 'send'\n"
            "                                         'receive' category of transactions. Negative confirmations indicate the\n"
            "                                         transaction conflicts with the block chain\n"
            "    \"bcconfirmations\" : n,         (numeric) DEPRECATED: Will be removed in a future release\n"
            "    \"trusted\": xxx            (bool) Whether we consider the outputs of this unconfirmed transaction safe to spend.\n"
            "                                          and 'receive' category of transactions.\n"
            "    \"blockhash\": \"hashvalue\", (string) The block hash containing the transaction. Available for 'send' and 'receive'\n"
            "                                          category of transactions.\n"
            "    \"blockindex\": n,          (numeric) The block index containing the transaction. Available for 'send' and 'receive'\n"
            "                                          category of transactions.\n"
            "    \"txid\": \"transactionid\", (string) The transaction id. Available for 'send' and 'receive' category of transactions.\n"
            "    \"time\": xxx,              (numeric) The transaction time in seconds since epoch (midnight Jan 1 1970 GMT).\n"
            "    \"timereceived\": xxx,      (numeric) The time received in seconds since epoch (midnight Jan 1 1970 GMT). Available \n"
            "                                          for 'send' and 'receive' category of transactions.\n"
            "    \"comment\": \"...\",       (string) If a comment is associated with the transaction.\n"
            "  }\n"
            "]\n"

            "\nExamples:\n"
            "\nList the most recent 10 transactions in the systems\n" +
            HelpExampleCli("listtransactions", "") +
            "\nList transactions 100 to 120\n" +
            HelpExampleCli("listtransactions", "\"*\" 20 100") +
            "\nAs a json rpc call\n" +
            HelpExampleRpc("listtransactions", "\"*\", 20, 100")
        );

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    if (!request.params[0].isNull() && request.params[0].get_str() != "*") {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Dummy value must be set to \"*\"");
    }
    int nCount = 10;
    if (request.params.size() > 1)
        nCount = request.params[1].get_int();
    int nFrom = 0;
    if (request.params.size() > 2)
        nFrom = request.params[2].get_int();
    isminefilter filter = ISMINE_SPENDABLE;
    if ( request.params.size() > 3 && request.params[3].get_bool() )
            filter = filter | ISMINE_WATCH_ONLY;
    if ( !(request.params.size() > 4) || request.params[4].get_bool() )
        filter = filter | ISMINE_SPENDABLE_DELEGATED;
    if ( !(request.params.size() > 5) || request.params[5].get_bool() )
        filter = filter | ISMINE_COLD;

    if (nCount < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative count");
    if (nFrom < 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Negative from");

    UniValue ret(UniValue::VARR);

    const CWallet::TxItems & txOrdered = pwallet->wtxOrdered;

    // iterate backwards until we have nCount items to return:
    for (CWallet::TxItems::const_reverse_iterator it = txOrdered.rbegin(); it != txOrdered.rend(); ++it) {
        CWalletTx* const pwtx = (*it).second;
        ListTransactions(pwallet, *pwtx, 0, true, ret, filter);
        if ((int)ret.size() >= (nCount + nFrom)) break;
    }
    // ret is newest to oldest

    if (nFrom > (int)ret.size())
        nFrom = ret.size();
    if ((nFrom + nCount) > (int)ret.size())
        nCount = ret.size() - nFrom;

    std::vector<UniValue> arrTmp = ret.getValues();

    std::vector<UniValue>::iterator first = arrTmp.begin();
    std::advance(first, nFrom);
    std::vector<UniValue>::iterator last = arrTmp.begin();
    std::advance(last, nFrom+nCount);

    if (last != arrTmp.end()) arrTmp.erase(last, arrTmp.end());
    if (first != arrTmp.begin()) arrTmp.erase(arrTmp.begin(), first);

    std::reverse(arrTmp.begin(), arrTmp.end()); // Return oldest to newest

    ret.clear();
    ret.setArray();
    ret.push_backV(arrTmp);

    return ret;
}

UniValue listsinceblock(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 3)
        throw std::runtime_error(
            "listsinceblock ( \"blockhash\" target_confirmations include_watchonly)\n"
            "\nGet all transactions in blocks since block [blockhash], or all transactions if omitted\n"

            "\nArguments:\n"
            "1. \"blockhash\"         (string, optional) The block hash to list transactions since\n"
            "2. target_confirmations: (numeric, optional) The confirmations required, must be 1 or more\n"
            "3. include_watchonly:    (bool, optional, default=false) Include transactions to watchonly addresses (see 'importaddress')"

            "\nResult:\n"
            "{\n"
            "  \"transactions\": [\n"
            "    \"address\":\"pivxaddress\",    (string) The pivx address of the transaction. Not present for move transactions (category = move).\n"
            "    \"category\":\"send|receive\",     (string) The transaction category. 'send' has negative amounts, 'receive' has positive amounts.\n"
            "    \"amount\": x.xxx,          (numeric) The amount in PIV. This is negative for the 'send' category, and for the 'move' category for moves \n"
            "                                          outbound. It is positive for the 'receive' category, and for the 'move' category for inbound funds.\n"
            "    \"vout\" : n,               (numeric) the vout value\n"
            "    \"fee\": x.xxx,             (numeric) The amount of the fee in PIV. This is negative and only available for the 'send' category of transactions.\n"
            "    \"confirmations\": n,       (numeric) The number of confirmations for the transaction. Available for 'send' and 'receive' category of transactions.\n"
            "    \"bcconfirmations\" : n,             (numeric) DEPRECATED: Will be removed in a future release\n"
            "    \"blockhash\": \"hashvalue\",     (string) The block hash containing the transaction. Available for 'send' and 'receive' category of transactions.\n"
            "    \"blockindex\": n,          (numeric) The block index containing the transaction. Available for 'send' and 'receive' category of transactions.\n"
            "    \"blocktime\": xxx,         (numeric) The block time in seconds since epoch (1 Jan 1970 GMT).\n"
            "    \"txid\": \"transactionid\",  (string) The transaction id. Available for 'send' and 'receive' category of transactions.\n"
            "    \"time\": xxx,              (numeric) The transaction time in seconds since epoch (Jan 1 1970 GMT).\n"
            "    \"timereceived\": xxx,      (numeric) The time received in seconds since epoch (Jan 1 1970 GMT). Available for 'send' and 'receive' category of transactions.\n"
            "    \"comment\": \"...\",       (string) If a comment is associated with the transaction.\n"
            "    \"label\" : \"label\"       (string) A comment for the address/transaction, if any\n"
            "    \"to\": \"...\",            (string) If a comment to is associated with the transaction.\n"
            "  ],\n"
            "  \"lastblock\": \"lastblockhash\"     (string) The hash of the last block\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("listsinceblock", "") +
            HelpExampleCli("listsinceblock", "\"000000000000000bacf66f7497b7dc45ef753ee9a7d38571037cdb1a57f663ad\" 6") +
            HelpExampleRpc("listsinceblock", "\"000000000000000bacf66f7497b7dc45ef753ee9a7d38571037cdb1a57f663ad\", 6"));

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    CBlockIndex* pindex = NULL;
    int target_confirms = 1;
    isminefilter filter = ISMINE_SPENDABLE_ALL | ISMINE_COLD;

    if (request.params.size() > 0) {
        uint256 blockId;

        blockId.SetHex(request.params[0].get_str());
        BlockMap::iterator it = mapBlockIndex.find(blockId);
        if (it != mapBlockIndex.end())
            pindex = it->second;
    }

    if (request.params.size() > 1) {
        target_confirms = request.params[1].get_int();

        if (target_confirms < 1)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter");
    }

    if (request.params.size() > 2)
        if (request.params[2].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    int depth = pindex ? (1 + chainActive.Height() - pindex->nHeight) : -1;

    UniValue transactions(UniValue::VARR);

    for (std::map<uint256, CWalletTx>::iterator it = pwallet->mapWallet.begin(); it != pwallet->mapWallet.end(); it++) {
        CWalletTx tx = (*it).second;

        if (depth == -1 || tx.GetDepthInMainChain() < depth)
            ListTransactions(pwallet, tx, 0, true, transactions, filter);
    }

    CBlockIndex* pblockLast = chainActive[chainActive.Height() + 1 - target_confirms];
    uint256 lastblock = pblockLast ? pblockLast->GetBlockHash() : UINT256_ZERO;

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("transactions", transactions);
    ret.pushKV("lastblock", lastblock.GetHex());

    return ret;
}

UniValue gettransaction(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "gettransaction \"txid\" ( include_watchonly )\n"
            "\nGet detailed information about in-wallet transaction \"txid\"\n"

            "\nArguments:\n"
            "1. \"txid\"              (string, required) The transaction id\n"
            "2. \"include_watchonly\" (bool, optional, default=false) Whether to include watchonly addresses in balance calculation and details[]\n"

            "\nResult:\n"
            "{\n"
            "  \"amount\" : x.xxx,        (numeric) The transaction amount in PIV\n"
            "  \"confirmations\" : n,     (numeric) The number of confirmations\n"
            "  \"bcconfirmations\" : n,             (numeric) DEPRECATED: Will be removed in a future release\n"
            "  \"blockhash\" : \"hash\",  (string) The block hash\n"
            "  \"blockindex\" : xx,       (numeric) The block index\n"
            "  \"blocktime\" : ttt,       (numeric) The time in seconds since epoch (1 Jan 1970 GMT)\n"
            "  \"txid\" : \"transactionid\",   (string) The transaction id.\n"
            "  \"time\" : ttt,            (numeric) The transaction time in seconds since epoch (1 Jan 1970 GMT)\n"
            "  \"timereceived\" : ttt,    (numeric) The time received in seconds since epoch (1 Jan 1970 GMT)\n"
            "  \"details\" : [\n"
            "    {\n"
            "      \"address\" : \"pivxaddress\",   (string) The pivx address involved in the transaction\n"
            "      \"category\" : \"send|receive\",    (string) The category, either 'send' or 'receive'\n"
            "      \"amount\" : x.xxx                  (numeric) The amount in PIV\n"
            "      \"label\" : \"label\",              (string) A comment for the address/transaction, if any\n"
            "      \"vout\" : n,                       (numeric) the vout value\n"
            "    }\n"
            "    ,...\n"
            "  ],\n"
            "  \"hex\" : \"data\"         (string) Raw data for transaction\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"") +
            HelpExampleCli("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\" true") +
            HelpExampleRpc("gettransaction", "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\""));

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    uint256 hash;
    hash.SetHex(request.params[0].get_str());

    isminefilter filter = ISMINE_SPENDABLE_ALL | ISMINE_COLD;
    if (request.params.size() > 1)
        if (request.params[1].get_bool())
            filter = filter | ISMINE_WATCH_ONLY;

    UniValue entry(UniValue::VOBJ);
    if (!pwallet->mapWallet.count(hash))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    const CWalletTx& wtx = pwallet->mapWallet.at(hash);

    CAmount nCredit = wtx.GetCredit(filter);
    CAmount nDebit = wtx.GetDebit(filter);
    CAmount nNet = nCredit - nDebit;
    CAmount nFee = (wtx.IsFromMe(filter) ? wtx.tx->GetValueOut() - nDebit : 0);

    entry.pushKV("amount", ValueFromAmount(nNet - nFee));
    if (wtx.IsFromMe(filter))
        entry.pushKV("fee", ValueFromAmount(nFee));

    WalletTxToJSON(wtx, entry);

    UniValue details(UniValue::VARR);
    ListTransactions(pwallet, wtx, 0, false, details, filter);
    entry.pushKV("details", details);

    std::string strHex = EncodeHexTx(*wtx.tx);
    entry.pushKV("hex", strHex);

    return entry;
}

UniValue abandontransaction(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
                "abandontransaction \"txid\"\n"
                "\nMark in-wallet transaction \"txid\" as abandoned\n"
                "This will mark this transaction and all its in-wallet descendants as abandoned which will allow\n"
                "for their inputs to be respent.  It can be used to replace \"stuck\" or evicted transactions.\n"
                "It only works on transactions which are not included in a block and are not currently in the mempool.\n"
                "It has no effect on transactions which are already abandoned.\n"
                "\nArguments:\n"
                "1. \"txid\"    (string, required) The transaction id\n"
                "\nResult:\n"
                "\nExamples:\n"
                + HelpExampleCli("abandontransaction",
                                 "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
                + HelpExampleRpc("abandontransaction",
                                 "\"1075db55d416d3ca199f55b6084e2115b9345e16c5cf302fc80e9d5fbf5d48d\"")
        );

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    uint256 hash;
    hash.SetHex(request.params[0].get_str());

    if (!pwallet->mapWallet.count(hash))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid or non-wallet transaction id");
    if (!pwallet->AbandonTransaction(hash))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Transaction not eligible for abandonment");

    return NullUniValue;
}


UniValue backupwallet(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "backupwallet \"destination\"\n"
            "\nSafely copies wallet file to destination, which can be a directory or a path with filename.\n"

            "\nArguments:\n"
            "1. \"destination\"   (string) The destination directory or file\n"

            "\nExamples:\n" +
            HelpExampleCli("backupwallet", "\"backup.dat\"") + HelpExampleRpc("backupwallet", "\"backup.dat\""));

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    std::string strDest = request.params[0].get_str();
    if (!pwallet->BackupWallet(strDest))
        throw JSONRPCError(RPC_WALLET_ERROR, "Error: Wallet backup failed!");

    return NullUniValue;
}


UniValue keypoolrefill(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "keypoolrefill ( newsize )\n"
            "\nFills the keypool." +
            HelpRequiringPassphrase(pwallet) + "\n"

            "\nArguments\n"
            "1. newsize     (numeric, optional, default=100) The new keypool size\n"

            "\nExamples:\n" +
            HelpExampleCli("keypoolrefill", "") + HelpExampleRpc("keypoolrefill", ""));

    LOCK2(cs_main, pwallet->cs_wallet);

    // 0 is interpreted by TopUpKeyPool() as the default keypool size given by -keypool
    unsigned int kpSize = 0;
    if (request.params.size() > 0) {
        if (request.params[0].get_int() < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected valid size.");
        kpSize = (unsigned int)request.params[0].get_int();
    }

    EnsureWalletIsUnlocked(pwallet);
    pwallet->TopUpKeyPool(kpSize);

    if (pwallet->GetKeyPoolSize() < kpSize)
        throw JSONRPCError(RPC_WALLET_ERROR, "Error refreshing keypool.");

    return NullUniValue;
}


static void LockWallet(CWallet* pWallet)
{
    LOCK(pWallet->cs_wallet);
    pWallet->nRelockTime = 0;
    pWallet->fWalletUnlockStaking = false;
    pWallet->Lock();
}

UniValue walletpassphrase(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 3) {
        throw std::runtime_error(
            "walletpassphrase \"passphrase\" timeout ( staking_only )\n"
            "\nStores the wallet decryption key in memory for 'timeout' seconds.\n"
            "This is needed prior to performing transactions related to private keys such as sending PIVs\n"

            "\nArguments:\n"
            "1. \"passphrase\"     (string, required) The wallet passphrase\n"
            "2. timeout            (numeric, required) The time to keep the decryption key in seconds.\n"
            "3. staking_only       (boolean, optional, default=false) If is true sending functions are disabled."

            "\nNote:\n"
            "Issuing the walletpassphrase command while the wallet is already unlocked will set a new unlock\n"
            "time that overrides the old one. A timeout of \"0\" unlocks until the wallet is closed.\n"

            "\nExamples:\n"
            "\nUnlock the wallet for 60 seconds\n" +
            HelpExampleCli("walletpassphrase", "\"my pass phrase\" 60") +
            "\nUnlock the wallet for 60 seconds but allow staking only\n" +
            HelpExampleCli("walletpassphrase", "\"my pass phrase\" 60 true") +
            "\nLock the wallet again (before 60 seconds)\n" +
            HelpExampleCli("walletlock", "") +
            "\nAs json rpc call\n" +
            HelpExampleRpc("walletpassphrase", "\"my pass phrase\", 60"));
    }

    LOCK2(cs_main, pwallet->cs_wallet);

    if (request.fHelp)
        return true;
    if (!pwallet->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrase was called.");

    // Note that the walletpassphrase is stored in params[0] which is not mlock()ed
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    strWalletPass = request.params[0].get_str().c_str();

    bool stakingOnly = false;
    if (request.params.size() == 3)
        stakingOnly = request.params[2].get_bool();

    if (!pwallet->IsLocked() && pwallet->fWalletUnlockStaking && stakingOnly)
        throw JSONRPCError(RPC_WALLET_ALREADY_UNLOCKED, "Error: Wallet is already unlocked.");

    // Get the timeout
    int64_t nSleepTime = request.params[1].get_int64();
    // Timeout cannot be negative, otherwise it will relock immediately
    if (nSleepTime < 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Timeout cannot be negative.");
    }
    // Clamp timeout
    constexpr int64_t MAX_SLEEP_TIME = 100000000; // larger values trigger a macos/libevent bug?
    if (nSleepTime > MAX_SLEEP_TIME) {
        nSleepTime = MAX_SLEEP_TIME;
    }

    if (!pwallet->Unlock(strWalletPass, stakingOnly))
        throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");

    pwallet->TopUpKeyPool();

    if (nSleepTime > 0) {
        pwallet->nRelockTime = GetTime () + nSleepTime;
        RPCRunLater (strprintf("lockwallet(%s)", pwallet->GetName()), std::bind (LockWallet, pwallet), nSleepTime);
    }

    return NullUniValue;
}


UniValue walletpassphrasechange(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 2) {
        throw std::runtime_error(
            "walletpassphrasechange \"oldpassphrase\" \"newpassphrase\"\n"
            "\nChanges the wallet passphrase from 'oldpassphrase' to 'newpassphrase'.\n"

            "\nArguments:\n"
            "1. \"oldpassphrase\"      (string) The current passphrase\n"
            "2. \"newpassphrase\"      (string) The new passphrase\n"

            "\nExamples:\n" +
            HelpExampleCli("walletpassphrasechange", "\"old one\" \"new one\"") + HelpExampleRpc("walletpassphrasechange", "\"old one\", \"new one\""));
    }

    LOCK2(cs_main, pwallet->cs_wallet);

    if (request.fHelp)
        return true;
    if (!pwallet->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletpassphrasechange was called.");

    // TODO: get rid of these .c_str() calls by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    SecureString strOldWalletPass;
    strOldWalletPass.reserve(100);
    strOldWalletPass = request.params[0].get_str().c_str();

    SecureString strNewWalletPass;
    strNewWalletPass.reserve(100);
    strNewWalletPass = request.params[1].get_str().c_str();

    if (strOldWalletPass.length() < 1 || strNewWalletPass.length() < 1)
        throw std::runtime_error(
            "walletpassphrasechange <oldpassphrase> <newpassphrase>\n"
            "Changes the wallet passphrase from <oldpassphrase> to <newpassphrase>.");

    if (!pwallet->ChangeWalletPassphrase(strOldWalletPass, strNewWalletPass))
        throw JSONRPCError(RPC_WALLET_PASSPHRASE_INCORRECT, "Error: The wallet passphrase entered was incorrect.");

    return NullUniValue;
}


UniValue walletlock(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || !request.params.empty()) {
        throw std::runtime_error(
            "walletlock\n"
            "\nRemoves the wallet encryption key from memory, locking the wallet.\n"
            "After calling this method, you will need to call walletpassphrase again\n"
            "before being able to call any methods which require the wallet to be unlocked.\n"

            "\nExamples:\n"
            "\nSet the passphrase for 2 minutes to perform a transaction\n" +
            HelpExampleCli("walletpassphrase", "\"my pass phrase\" 120") +
            "\nPerform a send (requires passphrase set)\n" +
            HelpExampleCli("sendtoaddress", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" 1.0") +
            "\nClear the passphrase since we are done before 2 minutes is up\n" +
            HelpExampleCli("walletlock", "") +
            "\nAs json rpc call\n" +
            HelpExampleRpc("walletlock", ""));
    }

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    if (request.fHelp)
        return true;
    if (!pwallet->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an unencrypted wallet, but walletlock was called.");

    pwallet->Lock();
    pwallet->nRelockTime = 0;

    return NullUniValue;
}


UniValue encryptwallet(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            "encryptwallet \"passphrase\"\n"
            "\nEncrypts the wallet with 'passphrase'. This is for first time encryption.\n"
            "After this, any calls that interact with private keys such as sending or signing \n"
            "will require the passphrase to be set prior the making these calls.\n"
            "Use the walletpassphrase call for this, and then walletlock call.\n"
            "If the wallet is already encrypted, use the walletpassphrasechange call.\n"
            "Note that this will shutdown the server.\n"

            "\nArguments:\n"
            "1. \"passphrase\"    (string) The pass phrase to encrypt the wallet with. It must be at least 1 character, but should be long.\n"

            "\nExamples:\n"
            "\nEncrypt you wallet\n" +
            HelpExampleCli("encryptwallet", "\"my pass phrase\"") +
            "\nNow set the passphrase to use the wallet, such as for signing or sending PIVs\n" +
            HelpExampleCli("walletpassphrase", "\"my pass phrase\"") +
            "\nNow we can so something like sign\n" +
            HelpExampleCli("signmessage", "\"pivxaddress\" \"test message\"") +
            "\nNow lock the wallet again by removing the passphrase\n" +
            HelpExampleCli("walletlock", "") +
            "\nAs a json rpc call\n" +
            HelpExampleRpc("encryptwallet", "\"my pass phrase\""));
    }

    LOCK2(cs_main, pwallet->cs_wallet);

    if (request.fHelp)
        return true;
    if (pwallet->IsCrypted())
        throw JSONRPCError(RPC_WALLET_WRONG_ENC_STATE, "Error: running with an encrypted wallet, but encryptwallet was called.");

    // TODO: get rid of this .c_str() by implementing SecureString::operator=(std::string)
    // Alternately, find a way to make request.params[0] mlock()'d to begin with.
    SecureString strWalletPass;
    strWalletPass.reserve(100);
    strWalletPass = request.params[0].get_str().c_str();

    if (strWalletPass.length() < 1)
        throw std::runtime_error(
            "encryptwallet <passphrase>\n"
            "Encrypts the wallet with <passphrase>.");

    if (!pwallet->EncryptWallet(strWalletPass))
        throw JSONRPCError(RPC_WALLET_ENCRYPTION_FAILED, "Error: Failed to encrypt the wallet.");

    // BDB seems to have a bad habit of writing old data into
    // slack space in .dat files; that is bad if the old data is
    // unencrypted private keys. So:
    StartShutdown();
    return "wallet encrypted; pivx server stopping, restart to run with encrypted wallet. The keypool has been flushed, you need to make a new backup.";
}

UniValue listunspent(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 6)
        throw std::runtime_error(
                "listunspent ( minconf maxconf  [\"address\",...] watchonly_config [query_options] include_unsafe)\n"
                "\nReturns array of unspent transaction outputs\n"
                "with between minconf and maxconf (inclusive) confirmations.\n"
                "Optionally filter to only include txouts paid to specified addresses.\n"
                "Results are an array of Objects, each of which has:\n"
                "{txid, vout, scriptPubKey, amount, confirmations, spendable}\n"

                "\nArguments:\n"
                "1. minconf          (numeric, optional, default=1) The minimum confirmations to filter\n"
                "2. maxconf          (numeric, optional, default=9999999) The maximum confirmations to filter\n"
                "3. \"addresses\"    (string) A json array of pivx addresses to filter\n"
                "    [\n"
                "      \"address\"   (string) pivx address\n"
                "      ,...\n"
                "    ]\n"
                "4. watchonly_config (numeric, optional, default=1) 1 = list regular unspent transactions,  2 = list all unspent transactions (including watchonly)\n"
                "5. query_options    (json, optional) JSON with query options\n"
                "    {\n"
                "      \"minimumAmount\"    (numeric or string, default=0) Minimum value of each UTXO in " + CURRENCY_UNIT + "\n"
                "      \"maximumAmount\"    (numeric or string, default=unlimited) Maximum value of each UTXO in " + CURRENCY_UNIT + "\n"
                "      \"maximumCount\"     (numeric or string, default=unlimited) Maximum number of UTXOs\n"
                "      \"minimumSumAmount\" (numeric or string, default=unlimited) Minimum sum value of all UTXOs in " + CURRENCY_UNIT + "\n"
                "    }\n"
                "6. include_unsafe   (bool, optional, default=true) Include outputs that are not safe to spend\n"
                "                        See description of \"safe\" attribute below.\n"

                "\nResult\n"
                "[                   (array of json object)\n"
                "  {\n"
                "    \"txid\" : \"txid\",        (string) the transaction id\n"
                "    \"generated\" : true|false  (boolean) true if txout is a coinstake transaction output\n"
                "    \"vout\" : n,               (numeric) the vout value\n"
                "    \"address\" : \"address\",  (string) the pivx address\n"
                "    \"label\" : \"label\",      (string) The associated label, or \"\" for the default label\n"
                "    \"scriptPubKey\" : \"key\", (string) the script key\n"
                "    \"redeemScript\" : \"key\", (string) the redeemscript key\n"
                "    \"amount\" : x.xxx,         (numeric) the transaction amount in PIV\n"
                "    \"confirmations\" : n,      (numeric) The number of confirmations\n"
                "    \"spendable\" : true|false  (boolean) Whether we have the private keys to spend this output\n"
                "    \"solvable\" : xxx          (boolean) Whether we know how to spend this output, ignoring the lack of keys\n"
                "    \"safe\" : xxx              (boolean) Whether this output is considered safe to spend. Unconfirmed transactions\n"
                "                              from outside keys and unconfirmed replacement transactions are considered unsafe\n"
                "                              and are not eligible for spending by fundrawtransaction and sendtoaddress.\n"
                "  }\n"
                "  ,...\n"
                "]\n"

                "\nExamples\n" +
                HelpExampleCli("listunspent", "") + HelpExampleCli("listunspent", "6 9999999 \"[\\\"1PGFqEzfmQch1gKD3ra4k18PNj3tTUUSqg\\\",\\\"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\\\"]\"")
                + HelpExampleRpc("listunspent", "6, 9999999 \"[\\\"1PGFqEzfmQch1gKD3ra4k18PNj3tTUUSqg\\\",\\\"1LtvqCaApEdUGFkpKMM4MstjcaL4dKg8SP\\\"]\"")
                + HelpExampleCli("listunspent", "6 9999999 '[]' 1 '{ \"minimumAmount\": 0.005 }'")
                + HelpExampleCli("listunspent", "6 9999999 '[]' 1 '{ \"minimumAmount\": 0.005 }' false")
                + HelpExampleRpc("listunspent", "6, 9999999, [] , 1, { \"minimumAmount\": 0.005 } ")
                );

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    int nMinDepth = 1;
    if (request.params.size() > 0) {
        RPCTypeCheckArgument(request.params[0], UniValue::VNUM);
        nMinDepth = request.params[0].get_int();
    }

    int nMaxDepth = 9999999;
    if (request.params.size() > 1) {
        RPCTypeCheckArgument(request.params[1], UniValue::VNUM);
        nMaxDepth = request.params[1].get_int();
    }

    std::set<CTxDestination> destinations;
    if (request.params.size() > 2) {
        RPCTypeCheckArgument(request.params[2], UniValue::VARR);
        UniValue inputs = request.params[2].get_array();
        for (unsigned int inx = 0; inx < inputs.size(); inx++) {
            const UniValue& input = inputs[inx];
            CTxDestination dest = DecodeDestination(input.get_str());
            if (!IsValidDestination(dest))
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid PIVX address: ") + input.get_str());
            if (destinations.count(dest))
                throw JSONRPCError(RPC_INVALID_PARAMETER, std::string("Invalid parameter, duplicated address: ") + input.get_str());
            destinations.insert(dest);
        }
    }

    // List watch only utxo
    int nWatchonlyConfig = 1;
    if(request.params.size() > 3) {
        RPCTypeCheckArgument(request.params[3], UniValue::VNUM);
        nWatchonlyConfig = request.params[3].get_int();
        if (nWatchonlyConfig > 2 || nWatchonlyConfig < 1)
            nWatchonlyConfig = 1;
    }

    CWallet::AvailableCoinsFilter coinFilter;
    if (request.params.size() > 4) {
        const UniValue& options = request.params[4].get_obj();

        RPCTypeCheckObj(options,
            {
                    {"minimumAmount", UniValueType()},
                    {"maximumAmount", UniValueType()},
                    {"minimumSumAmount", UniValueType()},
                    {"maximumCount", UniValueType(UniValue::VNUM)},
            },
            true, true);

        if (options.exists("minimumAmount")) {
            coinFilter.nMinOutValue = AmountFromValue(options["minimumAmount"]);
        }

        if (options.exists("maximumAmount")) {
            coinFilter.nMaxOutValue = AmountFromValue(options["maximumAmount"]);
        }

        if (options.exists("minimumSumAmount")) {
            coinFilter.nMinimumSumAmount = AmountFromValue(options["minimumSumAmount"]);
        }

        if (options.exists("maximumCount"))
            coinFilter.nMaximumCount = options["maximumCount"].get_int64();
    }

    CCoinControl coinControl;
    coinControl.fAllowWatchOnly = nWatchonlyConfig == 2;

    bool include_unsafe = request.params.size() < 6 || request.params[5].get_bool();
    coinFilter.fOnlySafe = !include_unsafe;

    UniValue results(UniValue::VARR);
    std::vector<COutput> vecOutputs;

    LOCK2(cs_main, pwallet->cs_wallet);
    pwallet->AvailableCoins(&vecOutputs, &coinControl, coinFilter);
    for (const COutput& out : vecOutputs) {
        if (out.nDepth < nMinDepth || out.nDepth > nMaxDepth)
            continue;

        if (!destinations.empty()) {
            CTxDestination address;
            if (!ExtractDestination(out.tx->tx->vout[out.i].scriptPubKey, address))
                continue;

            if (!destinations.count(address))
                continue;
        }

        CAmount nValue = out.tx->tx->vout[out.i].nValue;
        const CScript& pk = out.tx->tx->vout[out.i].scriptPubKey;
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("txid", out.tx->GetHash().GetHex());
        entry.pushKV("vout", out.i);
        entry.pushKV("generated", out.tx->IsCoinStake() || out.tx->IsCoinBase());
        CTxDestination address;
        if (ExtractDestination(out.tx->tx->vout[out.i].scriptPubKey, address)) {
            entry.pushKV("address", EncodeDestination(address));
            if (pwallet->HasAddressBook(address)) {
                entry.pushKV("label", pwallet->GetNameForAddressBookEntry(address));
            }
        }
        entry.pushKV("scriptPubKey", HexStr(pk.begin(), pk.end()));
        if (pk.IsPayToScriptHash()) {
            CTxDestination address;
            if (ExtractDestination(pk, address)) {
                const CScriptID& hash = boost::get<CScriptID>(address);
                CScript redeemScript;
                if (pwallet->GetCScript(hash, redeemScript))
                    entry.pushKV("redeemScript", HexStr(redeemScript.begin(), redeemScript.end()));
            }
        }
        entry.pushKV("amount", ValueFromAmount(nValue));
        entry.pushKV("confirmations", out.nDepth);
        entry.pushKV("spendable", out.fSpendable);
        entry.pushKV("solvable", out.fSolvable);
        entry.pushKV("safe", out.fSafe);
        results.push_back(entry);
    }

    return results;
}

UniValue fundrawtransaction(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "fundrawtransaction \"hexstring\" ( options )\n"
            "\nAdd inputs to a transaction until it has enough in value to meet its out value.\n"
            "This will not modify existing inputs, and will add one change output to the outputs.\n"
            "Note that inputs which were signed may need to be resigned after completion since in/outputs have been added.\n"
            "The inputs added will not be signed, use signrawtransaction for that.\n"
            "Note that all existing inputs must have their previous output transaction be in the wallet.\n"
            "Note that all inputs selected must be of standard form and P2SH scripts must be "
            "in the wallet using importaddress or addmultisigaddress (to calculate fees).\n"
            "You can see whether this is the case by checking the \"solvable\" field in the listunspent output.\n"
            "Only pay-to-pubkey, multisig, and P2SH versions thereof are currently supported for watch-only\n"

            "\nArguments:\n"
            "1. \"hexstring\"    (string, required) The hex string of the raw transaction\n"
            "2. options          (object, optional)\n"
            "   {\n"
            "     \"changeAddress\"     (string, optional, default pool address) The PIVX address to receive the change\n"
            "     \"changePosition\"    (numeric, optional, default random) The index of the change output\n"
            "     \"includeWatching\"   (boolean, optional, default false) Also select inputs which are watch only\n"
            "     \"lockUnspents\"      (boolean, optional, default false) Lock selected unspent outputs\n"
            "     \"feeRate\"           (numeric, optional, default 0=estimate) Set a specific feerate (PIV per KB)\n"
            "     \"subtractFeeFromOutputs\" (array, optional) A json array of integers.\n"
            "                              The fee will be equally deducted from the amount of each specified output.\n"
            "                              The outputs are specified by their zero-based index, before any change output is added.\n"
            "                              Those recipients will receive less PIV than you enter in their corresponding amount field.\n"
            "                              If no outputs are specified here, the sender pays the fee.\n"
            "                                  [vout_index,...]\n"
            "   }\n"
            "\nResult:\n"
            "{\n"
            "  \"hex\":       \"value\", (string)  The resulting raw transaction (hex-encoded string)\n"
            "  \"fee\":       n,         (numeric) The fee added to the transaction\n"
            "  \"changepos\": n          (numeric) The position of the added change output, or -1\n"
            "}\n"
            "\"hex\"             \n"
            "\nExamples:\n"
            "\nCreate a transaction with no inputs\n"
            + HelpExampleCli("createrawtransaction", "\"[]\" \"{\\\"myaddress\\\":0.01}\"") +
            "\nAdd sufficient unsigned inputs to meet the output value\n"
            + HelpExampleCli("fundrawtransaction", "\"rawtransactionhex\"") +
            "\nSign the transaction\n"
            + HelpExampleCli("signrawtransaction", "\"fundedtransactionhex\"") +
            "\nSend the transaction\n"
            + HelpExampleCli("sendrawtransaction", "\"signedtransactionhex\"")
            );

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    RPCTypeCheck(request.params, {UniValue::VSTR});

    CTxDestination changeAddress = CNoDestination();
    int changePosition = -1;
    bool includeWatching = false;
    bool lockUnspents = false;
    UniValue subtractFeeFromOutputs;
    std::set<int> setSubtractFeeFromOutputs;
    CFeeRate feeRate = CFeeRate(0);
    bool overrideEstimatedFeerate = false;

    if (request.params.size() > 1) {
        RPCTypeCheck(request.params, {UniValue::VSTR, UniValue::VOBJ});
        UniValue options = request.params[1];
        RPCTypeCheckObj(options,
            {
                {"changeAddress", UniValueType(UniValue::VSTR)},
                {"changePosition", UniValueType(UniValue::VNUM)},
                {"includeWatching", UniValueType(UniValue::VBOOL)},
                {"lockUnspents", UniValueType(UniValue::VBOOL)},
                {"feeRate", UniValueType()}, // will be checked below
                {"subtractFeeFromOutputs", UniValueType(UniValue::VARR)},
            },
            true, true);

        if (options.exists("changeAddress")) {
            changeAddress = DecodeDestination(options["changeAddress"].get_str());

            if (!IsValidDestination(changeAddress))
                throw JSONRPCError(RPC_INVALID_PARAMETER, "changeAddress must be a valid PIVX address");
        }

        if (options.exists("changePosition"))
            changePosition = options["changePosition"].get_int();

        if (options.exists("includeWatching"))
            includeWatching = options["includeWatching"].get_bool();

        if (options.exists("lockUnspents"))
            lockUnspents = options["lockUnspents"].get_bool();

        if (options.exists("feeRate")) {
            feeRate = CFeeRate(AmountFromValue(options["feeRate"]));
            overrideEstimatedFeerate = true;
        }

        if (options.exists("subtractFeeFromOutputs")) {
            subtractFeeFromOutputs = options["subtractFeeFromOutputs"].get_array();
        }
    }

    // parse hex string from parameter
    CMutableTransaction origTx;
    if (!DecodeHexTx(origTx, request.params[0].get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "TX decode failed");

    if (origTx.vout.size() == 0)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "TX must have at least one output");

    if (changePosition != -1 && (changePosition < 0 || (unsigned int) changePosition > origTx.vout.size()))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "changePosition out of bounds");

    for (unsigned int idx = 0; idx < subtractFeeFromOutputs.size(); idx++) {
        int pos = subtractFeeFromOutputs[idx].get_int();
        if (setSubtractFeeFromOutputs.count(pos))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, duplicated position: %d", pos));
        if (pos < 0)
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, negative position: %d", pos));
        if (pos >= int(origTx.vout.size()))
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid parameter, position too large: %d", pos));
        setSubtractFeeFromOutputs.insert(pos);
    }

    CMutableTransaction tx(origTx);
    CAmount nFeeOut;
    std::string strFailReason;
    if(!pwallet->FundTransaction(tx, nFeeOut, overrideEstimatedFeerate, feeRate,
                                 changePosition, strFailReason, includeWatching,
                                 lockUnspents, setSubtractFeeFromOutputs, changeAddress)) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, strFailReason);
    }

    UniValue result(UniValue::VOBJ);
    result.pushKV("hex", EncodeHexTx(tx));
    result.pushKV("changepos", changePosition);
    result.pushKV("fee", ValueFromAmount(nFeeOut));

    return result;
}

UniValue lockunspent(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2)
        throw std::runtime_error(
            "lockunspent unlock [{\"txid\":\"txid\",\"vout\":n},...]\n"
            "\nUpdates list of temporarily unspendable outputs.\n"
            "Temporarily lock (unlock=false) or unlock (unlock=true) specified transaction outputs.\n"
            "A locked transaction output will not be chosen by automatic coin selection, when spending PIVs.\n"
            "Locks are stored in memory only. Nodes start with zero locked outputs, and the locked output list\n"
            "is always cleared (by virtue of process exit) when a node stops or fails.\n"
            "Also see the listunspent call\n"

            "\nArguments:\n"
            "1. unlock            (boolean, required) Whether to unlock (true) or lock (false) the specified transactions\n"
            "2. \"transactions\"  (string, required) A json array of objects. Each object the txid (string) vout (numeric)\n"
            "     [           (json array of json objects)\n"
            "       {\n"
            "         \"txid\":\"id\",    (string) The transaction id\n"
            "         \"vout\": n         (numeric) The output number\n"
            "       }\n"
            "       ,...\n"
            "     ]\n"

            "\nResult:\n"
            "true|false    (boolean) Whether the command was successful or not\n"

            "\nExamples:\n"
            "\nList the unspent transactions\n" +
            HelpExampleCli("listunspent", "") +
            "\nLock an unspent transaction\n" +
            HelpExampleCli("lockunspent", "false \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nList the locked transactions\n" +
            HelpExampleCli("listlockunspent", "") +
            "\nUnlock the transaction again\n" +
            HelpExampleCli("lockunspent", "true \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nAs a json rpc call\n" +
            HelpExampleRpc("lockunspent", "false, \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\""));

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    if (request.params.size() == 1)
        RPCTypeCheck(request.params, {UniValue::VBOOL});
    else
        RPCTypeCheck(request.params, {UniValue::VBOOL, UniValue::VARR});

    bool fUnlock = request.params[0].get_bool();

    if (request.params.size() == 1) {
        if (fUnlock)
            pwallet->UnlockAllCoins();
        return true;
    }

    UniValue output_request = request.params[1].get_array();

    // Create and validate the COutPoints first.
    std::vector<COutPoint> outputs;
    outputs.reserve(output_request.size());

    for (unsigned int idx = 0; idx < output_request.size(); idx++) {
        const UniValue& output = output_request[idx];
        if (!output.isObject())
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected object");
        const UniValue& o = output.get_obj();

        RPCTypeCheckObj(o,
            {
                {"txid", UniValueType(UniValue::VSTR)},
                {"vout", UniValueType(UniValue::VNUM)},
            });

        const std::string& txid = find_value(o, "txid").get_str();
        if (!IsHex(txid)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected hex txid");
        }

        const int nOutput = find_value(o, "vout").get_int();
        if (nOutput < 0) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout must be positive");
        }

        const COutPoint outpt(uint256S(txid), nOutput);

        const auto it = pwallet->mapWallet.find(outpt.hash);
        if (it == pwallet->mapWallet.end()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, unknown transaction");
        }

        const CWalletTx& wtx = it->second;

        if (outpt.n >= wtx.tx->vout.size()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, vout index out of bounds");
        }

        if (pwallet->IsSpent(outpt.hash, outpt.n)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected unspent output");
        }

        const bool is_locked = pwallet->IsLockedCoin(outpt.hash, outpt.n);

        if (fUnlock && !is_locked) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, expected locked output");
        }

        if (!fUnlock && is_locked) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid parameter, output already locked");
        }

        outputs.push_back(outpt);
    }

    // Atomically set (un)locked status for the outputs.
    for (const COutPoint& outpt : outputs) {
        if (fUnlock) pwallet->UnlockCoin(outpt);
        else pwallet->LockCoin(outpt);
    }

    return true;
}

UniValue listlockunspent(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 0)
        throw std::runtime_error(
            "listlockunspent\n"
            "\nReturns list of temporarily unspendable outputs.\n"
            "See the lockunspent call to lock and unlock transactions for spending.\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"txid\" : \"transactionid\",     (string) The transaction id locked\n"
            "    \"vout\" : n                      (numeric) The vout value\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n"
            "\nList the unspent transactions\n" +
            HelpExampleCli("listunspent", "") +
            "\nLock an unspent transaction\n" +
            HelpExampleCli("lockunspent", "false \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nList the locked transactions\n" +
            HelpExampleCli("listlockunspent", "") +
            "\nUnlock the transaction again\n" +
            HelpExampleCli("lockunspent", "true \"[{\\\"txid\\\":\\\"a08e6907dbbd3d809776dbfc5d82e371b764ed838b5655e72f463568df1aadf0\\\",\\\"vout\\\":1}]\"") +
            "\nAs a json rpc call\n" +
            HelpExampleRpc("listlockunspent", ""));

    LOCK2(cs_main, pwallet->cs_wallet);

    std::set<COutPoint> vOutpts = pwallet->ListLockedCoins();
    UniValue ret(UniValue::VARR);

    for (const COutPoint& outpt : vOutpts) {
        UniValue o(UniValue::VOBJ);

        o.pushKV("txid", outpt.hash.GetHex());
        o.pushKV("vout", (int)outpt.n);
        ret.push_back(o);
    }

    return ret;
}

UniValue settxfee(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 1)
        throw std::runtime_error(
            "settxfee amount\n"
            "\nSet the transaction fee per kB.\n"

            "\nArguments:\n"
            "1. amount         (numeric, required) The transaction fee in PIV/kB rounded to the nearest 0.00000001\n"

            "\nResult\n"
            "true|false        (boolean) Returns true if successful\n"
            "\nExamples:\n" +
            HelpExampleCli("settxfee", "0.00001") + HelpExampleRpc("settxfee", "0.00001"));

    LOCK2(cs_main, pwallet->cs_wallet);

    // Amount
    CAmount nAmount = 0;
    if (request.params[0].get_real() != 0.0)
        nAmount = AmountFromValue(request.params[0]); // rejects 0.0 amounts

    payTxFee = CFeeRate(nAmount, 1000);
    return true;
}

UniValue getwalletinfo(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getwalletinfo\n"
            "Returns an object containing various wallet state info.\n"

            "\nResult:\n"
            "{\n"
            "  \"walletname\": xxxxx,                     (string) the wallet name\n"
            "  \"walletversion\": xxxxx,                  (numeric) the wallet version\n"
            "  \"balance\": xxxxxxx,                      (numeric) the total PIV balance of the wallet (cold balance excluded)\n"
            "  \"delegated_balance\": xxxxx,              (numeric) the PIV balance held in P2CS (cold staking) contracts\n"
            "  \"cold_staking_balance\": xx,              (numeric) the PIV balance held in cold staking addresses\n"
            "  \"unconfirmed_balance\": xxx,              (numeric) the total unconfirmed balance of the wallet in PIV\n"
            "  \"immature_delegated_balance\": xxxxxx,    (numeric) the delegated immature balance of the wallet in PIV\n"
            "  \"immature_cold_staking_balance\": xxxxxx, (numeric) the cold-staking immature balance of the wallet in PIV\n"
            "  \"immature_balance\": xxxxxx,              (numeric) the total immature balance of the wallet in PIV\n"
            "  \"txcount\": xxxxxxx,                      (numeric) the total number of transactions in the wallet\n"
            "  \"autocombine_enabled\": true|false,       (boolean) true if autocombine is enabled, otherwise false\n"
            "  \"autocombine_threshold\": x.xxx,          (numeric) the current autocombine threshold in PIV\n"
            "  \"keypoololdest\": xxxxxx,                 (numeric) the timestamp (seconds since GMT epoch) of the oldest pre-generated key in the key pool\n"
            "  \"keypoolsize\": xxxx,                     (numeric) how many new keys are pre-generated (only counts external keys)\n"
            "  \"keypoolsize_hd_internal\": xxxx,         (numeric) how many new keys are pre-generated for internal use (used for change outputs, only appears if the wallet is using this feature, otherwise external keys are used)\n"
            "  \"keypoolsize_hd_staking\": xxxx,          (numeric) how many new keys are pre-generated for staking use (used for staking contracts, only appears if the wallet is using this feature)\n"
            "  \"unlocked_until\": ttt,                   (numeric) the timestamp in seconds since epoch (midnight Jan 1 1970 GMT) that the wallet is unlocked for transfers, or 0 if the wallet is locked\n"
            "  \"paytxfee\": x.xxxx                       (numeric) the transaction fee configuration, set in PIV/kB\n"
            "  \"hdseedid\": \"<hash160>\"                (string, optional) the Hash160 of the HD seed (only present when HD is enabled)\n"
            "  \"last_processed_block\": xxxxx,          (numeric) the last block processed block height\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("getwalletinfo", "") + HelpExampleRpc("getwalletinfo", ""));

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    UniValue obj(UniValue::VOBJ);
    obj.pushKV("walletname", pwallet->GetName());
    obj.pushKV("walletversion", pwallet->GetVersion());
    obj.pushKV("balance", ValueFromAmount(pwallet->GetAvailableBalance()));
    obj.pushKV("delegated_balance", ValueFromAmount(pwallet->GetDelegatedBalance()));
    obj.pushKV("cold_staking_balance", ValueFromAmount(pwallet->GetColdStakingBalance()));
    obj.pushKV("unconfirmed_balance", ValueFromAmount(pwallet->GetUnconfirmedBalance()));
    obj.pushKV("immature_balance",    ValueFromAmount(pwallet->GetImmatureBalance()));
    obj.pushKV("immature_delegated_balance",    ValueFromAmount(pwallet->GetImmatureDelegatedBalance()));
    obj.pushKV("immature_cold_staking_balance",    ValueFromAmount(pwallet->GetImmatureColdStakingBalance()));
    obj.pushKV("txcount", (int)pwallet->mapWallet.size());

    // Autocombine settings
    obj.pushKV("autocombine_enabled", pwallet->fCombineDust);
    obj.pushKV("autocombine_threshold", ValueFromAmount(pwallet->nAutoCombineThreshold));

    // Keypool information
    obj.pushKV("keypoololdest", pwallet->GetOldestKeyPoolTime());
    size_t kpExternalSize = pwallet->KeypoolCountExternalKeys();
    obj.pushKV("keypoolsize", (int64_t)kpExternalSize);

    ScriptPubKeyMan* spk_man = pwallet->GetScriptPubKeyMan();
    if (spk_man) {
        const CKeyID& seed_id = spk_man->GetHDChain().GetID();
        if (!seed_id.IsNull()) {
            obj.pushKV("hdseedid", seed_id.GetHex());
        }
    }
    if (pwallet->IsHDEnabled()) {
        obj.pushKV("keypoolsize_hd_internal",   (int64_t)(pwallet->GetKeyPoolSize() - kpExternalSize));
        obj.pushKV("keypoolsize_hd_staking",   (int64_t)(pwallet->GetStakingKeyPoolSize()));
    }

    if (pwallet->IsCrypted())
        obj.pushKV("unlocked_until", pwallet->nRelockTime);
    obj.pushKV("paytxfee", ValueFromAmount(payTxFee.GetFeePerK()));
    obj.pushKV("last_processed_block", pwallet->GetLastBlockHeight());
    return obj;
}

UniValue listwallets(const JSONRPCRequest& request)
{
    if (request.fHelp || !request.params.empty())
        throw std::runtime_error(
            "listwallets\n"
            "Returns a list of currently loaded wallets.\n"
            "For full information on the wallet, use \"getwalletinfo\"\n"
            "\nResult:\n"
            "[                         (json array of strings)\n"
            "  \"walletname\"            (string) the wallet name\n"
            "   ...\n"
            "]\n"
            "\nExamples:\n"
            + HelpExampleCli("listwallets", "")
            + HelpExampleRpc("listwallets", "")
        );

    UniValue obj(UniValue::VARR);

    for (CWalletRef pwallet : vpwallets) {
        if (!EnsureWalletIsAvailable(pwallet, request.fHelp)) {
            return NullUniValue;
        }

        LOCK(pwallet->cs_wallet);
        obj.push_back(pwallet->GetName());
    }

    return obj;
}

UniValue getstakingstatus(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getstakingstatus\n"
            "\nReturns an object containing various staking information.\n"

            "\nResult:\n"
            "{\n"
            "  \"staking_status\": true|false,      (boolean) whether the wallet is staking or not\n"
            "  \"staking_enabled\": true|false,     (boolean) whether staking is enabled/disabled in pivx.conf\n"
            "  \"coldstaking_enabled\": true|false, (boolean) whether cold-staking is enabled/disabled in pivx.conf\n"
            "  \"haveconnections\": true|false,     (boolean) whether network connections are present\n"
            "  \"mnsync\": true|false,              (boolean) whether the required masternode/spork data is synced\n"
            "  \"walletunlocked\": true|false,      (boolean) whether the wallet is unlocked\n"
            "  \"stakeablecoins\": n                (numeric) number of stakeable UTXOs\n"
            "  \"stakingbalance\": d                (numeric) PIV value of the stakeable coins (minus reserve balance, if any)\n"
            "  \"stakesplitthreshold\": d           (numeric) value of the current threshold for stake split\n"
            "  \"lastattempt_age\": n               (numeric) seconds since last stake attempt\n"
            "  \"lastattempt_depth\": n             (numeric) depth of the block on top of which the last stake attempt was made\n"
            "  \"lastattempt_hash\": xxx            (hex string) hash of the block on top of which the last stake attempt was made\n"
            "  \"lastattempt_coins\": n             (numeric) number of stakeable coins available during last stake attempt\n"
            "  \"lastattempt_tries\": n             (numeric) number of stakeable coins checked during last stake attempt\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("getstakingstatus", "") + HelpExampleRpc("getstakingstatus", ""));


    if (!pwallet)
        throw JSONRPCError(RPC_IN_WARMUP, "Try again after active chain is loaded");
    {
        LOCK2(cs_main, &pwallet->cs_wallet);
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("staking_status", pwallet->pStakerStatus->IsActive());
        obj.pushKV("staking_enabled", gArgs.GetBoolArg("-staking", DEFAULT_STAKING));
        bool fColdStaking = gArgs.GetBoolArg("-coldstaking", true);
        obj.pushKV("coldstaking_enabled", fColdStaking);
        obj.pushKV("haveconnections", (g_connman->GetNodeCount(CConnman::CONNECTIONS_ALL) > 0));
        obj.pushKV("mnsync", !masternodeSync.NotCompleted());
        obj.pushKV("walletunlocked", !pwallet->IsLocked());
        std::vector<CStakeableOutput> vCoins;
        pwallet->StakeableCoins(&vCoins);
        obj.pushKV("stakeablecoins", (int)vCoins.size());
        obj.pushKV("stakingbalance", ValueFromAmount(pwallet->GetStakingBalance(fColdStaking)));
        obj.pushKV("stakesplitthreshold", ValueFromAmount(pwallet->nStakeSplitThreshold));
        CStakerStatus* ss = pwallet->pStakerStatus;
        if (ss) {
            obj.pushKV("lastattempt_age", (int)(GetTime() - ss->GetLastTime()));
            obj.pushKV("lastattempt_depth", (chainActive.Height() - ss->GetLastHeight()));
            obj.pushKV("lastattempt_hash", ss->GetLastHash().GetHex());
            obj.pushKV("lastattempt_coins", ss->GetLastCoins());
            obj.pushKV("lastattempt_tries", ss->GetLastTries());
        }
        return obj;
    }
}

UniValue setstakesplitthreshold(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "setstakesplitthreshold value\n\n"
            "This will set the stake-split threshold value.\n"
            "Whenever a successful stake is found, the stake amount is split across as many outputs (each with a value\n"
            "higher than the threshold) as possible.\n"
            "E.g. If the coinstake input + the block reward is 2000, and the split threshold is 499, the corresponding\n"
            "coinstake transaction will have 4 outputs (of 500 PIV each)."
            + HelpRequiringPassphrase(pwallet) + "\n"

            "\nArguments:\n"
            "1. value                   (numeric, required) Threshold value (in PIV).\n"
            "                                     Set to 0 to disable stake-splitting\n"
            "                                     If > 0, it must be >= " + FormatMoney(CWallet::minStakeSplitThreshold) + "\n"

            "\nResult:\n"
            "{\n"
            "  \"threshold\": n,        (numeric) Threshold value set\n"
            "  \"saved\": true|false    (boolean) 'true' if successfully saved to the wallet file\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("setstakesplitthreshold", "500.12") + HelpExampleRpc("setstakesplitthreshold", "500.12"));

    CAmount nStakeSplitThreshold = AmountFromValue(request.params[0]);
    if (nStakeSplitThreshold > 0 && nStakeSplitThreshold < CWallet::minStakeSplitThreshold)
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf(_("The threshold value cannot be less than %s"),
                FormatMoney(CWallet::minStakeSplitThreshold)));

    UniValue result(UniValue::VOBJ);
    result.pushKV("threshold", request.params[0]);
    result.pushKV("saved", pwallet->SetStakeSplitThreshold(nStakeSplitThreshold));
    return result;
}

UniValue getstakesplitthreshold(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getstakesplitthreshold\n"
            "Returns the threshold for stake splitting\n"

            "\nResult:\n"
            "n      (numeric) Threshold value\n"

            "\nExamples:\n" +
            HelpExampleCli("getstakesplitthreshold", "") + HelpExampleRpc("getstakesplitthreshold", ""));

    return ValueFromAmount(pwallet->GetStakeSplitThreshold());
}

UniValue autocombinerewards(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    bool fEnable = !request.params.empty() && request.params[0].get_bool();

    if (request.fHelp || request.params.empty() || (fEnable && request.params.size() != 2) || request.params.size() > 2)
        throw std::runtime_error(
            "autocombinerewards enable ( threshold )\n"
            "\nDEPRECATED!!! This command has been replaced with setautocombinethreshold and getautocombinethreshold and will be removed in a future version!!!\n"
            "\nWallet will automatically monitor for any coins with value below the threshold amount, and combine them if they reside with the same PIVX address\n"
            "When autocombinerewards runs it will create a transaction, and therefore will be subject to transaction fees.\n"

            "\nArguments:\n"
            "1. enable          (boolean, required) Enable auto combine (true) or disable (false)\n"
            "2. threshold       (numeric, optional, required if enable=True) Threshold amount (default: 0)\n"

            "\nExamples:\n" +
            HelpExampleCli("autocombinerewards", "true 500") + HelpExampleRpc("autocombinerewards", "true 500"));

    CWalletDB walletdb(pwallet->GetDBHandle());
    CAmount nThreshold = 0;

    if (fEnable && request.params.size() > 1) {
        nThreshold = AmountFromValue(request.params[1]);
        if (nThreshold < COIN)
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The threshold value cannot be less than %s", FormatMoney(COIN)));
    }

    pwallet->fCombineDust = fEnable;
    pwallet->nAutoCombineThreshold = nThreshold;

    if (!walletdb.WriteAutoCombineSettings(fEnable, nThreshold))
        throw std::runtime_error("Changed settings in wallet but failed to save to database\n");

    return NullUniValue;
}

UniValue setautocombinethreshold(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.empty() || request.params.size() > 2)
        throw std::runtime_error(
            "setautocombinethreshold enable ( value )\n"
            "\nThis will set the auto-combine threshold value.\n"
            "\nWallet will automatically monitor for any coins with value below the threshold amount, and combine them if they reside with the same PIVX address\n"
            "When auto-combine runs it will create a transaction, and therefore will be subject to transaction fees.\n"

            "\nArguments:\n"
            "1. enable          (boolean, required) Enable auto combine (true) or disable (false).\n"
            "2. threshold       (numeric, optional. required if enable is true) Threshold amount. Must be greater than 1.\n"

            "\nResult:\n"
            "{\n"
            "  \"enabled\": true|false,      (boolean) true if auto-combine is enabled, otherwise false\n"
            "  \"threshold\": n.nnn,         (numeric) auto-combine threshold in PIV\n"
            "  \"saved\": true|false         (boolean) true if setting was saved to the database, otherwise false\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("setautocombinethreshold", "500.12") + HelpExampleRpc("setautocombinethreshold", "500.12"));

    RPCTypeCheck(request.params, {UniValue::VBOOL, UniValue::VNUM});

    bool fEnable = request.params[0].get_bool();
    CAmount nThreshold = 0;

    if (fEnable) {
        if (request.params.size() < 2) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Missing threshold value");
        }
        nThreshold = AmountFromValue(request.params[1]);
        if (nThreshold < COIN)
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("The threshold value cannot be less than %s", FormatMoney(COIN)));
    }

    CWalletDB walletdb(pwallet->GetDBHandle());

    {
        LOCK(pwallet->cs_wallet);
        pwallet->fCombineDust = fEnable;
        pwallet->nAutoCombineThreshold = nThreshold;

        UniValue result(UniValue::VOBJ);
        result.pushKV("enabled", fEnable);
        result.pushKV("threshold", ValueFromAmount(pwallet->nAutoCombineThreshold));
        if (walletdb.WriteAutoCombineSettings(fEnable, nThreshold)) {
            result.pushKV("saved", "true");
        } else {
            result.pushKV("saved", "false");
        }

        return result;
    }
}

UniValue getautocombinethreshold(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || !request.params.empty())
        throw std::runtime_error(
            "getautocombinethreshold\n"
            "\nReturns the current threshold for auto combining UTXOs, if any\n"

            "\nResult:\n"
            "{\n"
            "  \"enabled\": true|false,        (boolean) true if auto-combine is enabled, otherwise false\n"
            "  \"threshold\": n.nnn            (numeric) the auto-combine threshold amount in PIV\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("getautocombinethreshold", "") + HelpExampleRpc("getautocombinethreshold", ""));

    LOCK(pwallet->cs_wallet);

    UniValue result(UniValue::VOBJ);
    result.pushKV("enabled", pwallet->fCombineDust);
    result.pushKV("threshold", ValueFromAmount(pwallet->nAutoCombineThreshold));

    return result;
}

UniValue getsaplingnotescount(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
                "getsaplingnotescount ( minconf )\n"
                "Returns the number of sapling notes available in the wallet.\n"

                "\nArguments:\n"
                "1. minconf      (numeric, optional, default=1) Only include notes in transactions confirmed at least this many times.\n"

                "\nResult:\n"
                "num             (numeric) the number of sapling notes in the wallet\n"

                "\nExamples:\n"
                + HelpExampleCli("getsaplingnotescount", "0")
                + HelpExampleRpc("getsaplingnotescount", "0")
        );

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    int nMinDepth = !request.params.empty() ? request.params[0].get_int() : 1;
    int count = 0;
    for (const auto& wtx : pwallet->mapWallet) {
        if (wtx.second.GetDepthInMainChain() >= nMinDepth) {
            for (const auto& nd : wtx.second.mapSaplingNoteData) {
                if (nd.second.IsMyNote()) count++;
            }
        }
    }
    return count;
}

UniValue rescanblockchain(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 2) {
        throw std::runtime_error(
                "rescanblockchain (start_height) (stop_height)\n"
                "\nRescan the local blockchain for wallet related transactions.\n"
                "\nArguments:\n"
                "1. start_height    (numeric, optional) block height where the rescan should start\n"
                "2. stop_height     (numeric, optional) the last block height that should be scanned\n"
                "\nResult:\n"
                "{\n"
                "  start_height     (numeric) The block height where the rescan has started. If omitted, rescan started from the genesis block.\n"
                "  stop_height      (numeric) The height of the last rescanned block. If omitted, rescan stopped at the chain tip.\n"
                "}\n"
                "\nExamples:\n"
                + HelpExampleCli("rescanblockchain", "100000 120000")
                + HelpExampleRpc("rescanblockchain", "100000 120000")
        );
    }

    WalletRescanReserver reserver(pwallet);
    if (!reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    CBlockIndex *pindexStart = nullptr;
    CBlockIndex *pindexStop = nullptr;
    CBlockIndex *pChainTip = nullptr;
    {
        LOCK(cs_main);
        pindexStart = chainActive.Genesis();
        pChainTip = chainActive.Tip();

        if (!request.params[0].isNull()) {
            pindexStart = chainActive[request.params[0].get_int()];
            if (!pindexStart) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid start_height");
            }
        }

        if (!request.params[1].isNull()) {
            pindexStop = chainActive[request.params[1].get_int()];
            if (!pindexStop) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid stop_height");
            }
            else if (pindexStop->nHeight < pindexStart->nHeight) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "stop_height must be greater then start_height");
            }
        }
    }

    CBlockIndex *stopBlock = pwallet->ScanForWalletTransactions(pindexStart, pindexStop, reserver, true);
    if (!stopBlock) {
        if (pwallet->IsAbortingRescan()) {
            throw JSONRPCError(RPC_MISC_ERROR, "Rescan aborted.");
        }
        // if we got a nullptr returned, ScanForWalletTransactions did rescan up to the requested stopindex
        stopBlock = pindexStop ? pindexStop : pChainTip;
    }
    else {
        throw JSONRPCError(RPC_MISC_ERROR, "Rescan failed. Potentially corrupted data files.");
    }

    UniValue response(UniValue::VOBJ);
    response.pushKV("start_height", pindexStart->nHeight);
    response.pushKV("stop_height", stopBlock->nHeight);
    return response;
}

extern UniValue abortrescan(const JSONRPCRequest& request); // in rpcdump.cpp
extern UniValue dumpprivkey(const JSONRPCRequest& request); // in rpcdump.cpp
extern UniValue importprivkey(const JSONRPCRequest& request);
extern UniValue importaddress(const JSONRPCRequest& request);
extern UniValue importpubkey(const JSONRPCRequest& request);
extern UniValue dumpwallet(const JSONRPCRequest& request);
extern UniValue importwallet(const JSONRPCRequest& request);
extern UniValue importmulti(const JSONRPCRequest& request);
extern UniValue bip38encrypt(const JSONRPCRequest& request);
extern UniValue bip38decrypt(const JSONRPCRequest& request);

extern UniValue exportsaplingkey(const JSONRPCRequest& request);
extern UniValue importsaplingkey(const JSONRPCRequest& request);
extern UniValue importsaplingviewingkey(const JSONRPCRequest& request);
extern UniValue exportsaplingviewingkey(const JSONRPCRequest& request);

static const CRPCCommand commands[] =
{ //  category              name                        actor (function)           okSafe argNames
  //  --------------------- ------------------------    -----------------------    ------ --------
    { "wallet",             "getaddressinfo",           &getaddressinfo,           true,  {"address"} },
    { "wallet",             "autocombinerewards",       &autocombinerewards,       false, {"enable","threshold"} },
    { "wallet",             "setautocombinethreshold",  &setautocombinethreshold,  false, {"enable","threshold"} },
    { "wallet",             "getautocombinethreshold",  &getautocombinethreshold,  false, {} },
    { "wallet",             "abandontransaction",       &abandontransaction,       false, {"txid"} },
    { "wallet",             "abortrescan",              &abortrescan,              false, {} },
    { "wallet",             "addmultisigaddress",       &addmultisigaddress,       true,  {"nrequired","keys","label"} },
    { "wallet",             "backupwallet",             &backupwallet,             true,  {"destination"} },
    { "wallet",             "delegatestake",            &delegatestake,            false, {"staking_addr","amount","owner_addr","ext_owner","include_delegated","from_shield","force"} },
    { "wallet",             "dumpprivkey",              &dumpprivkey,              true,  {"address"} },
    { "wallet",             "dumpwallet",               &dumpwallet,               true,  {"filename"} },
    { "wallet",             "encryptwallet",            &encryptwallet,            true,  {"passphrase"} },
    { "wallet",             "fundrawtransaction",       &fundrawtransaction,       false, {"hexstring","options"} },
    { "wallet",             "getbalance",               &getbalance,               false, {"minconf","include_watchonly","include_delegated","include_shield"} },
    { "wallet",             "getcoldstakingbalance",    &getcoldstakingbalance,    false, {} },
    { "wallet",             "getdelegatedbalance",      &getdelegatedbalance,      false, {} },
    { "wallet",             "upgradewallet",            &upgradewallet,            true,  {} },
    { "wallet",             "sethdseed",                &sethdseed,                true,  {"newkeypool","seed"} },
    { "wallet",             "getnewaddress",            &getnewaddress,            true,  {"label"} },
    { "wallet",             "getnewstakingaddress",     &getnewstakingaddress,     true,  {"label"}  },
    { "wallet",             "getrawchangeaddress",      &getrawchangeaddress,      true,  {} },
    { "wallet",             "getreceivedbyaddress",     &getreceivedbyaddress,     false, {"address","minconf"} },
    { "wallet",             "gettransaction",           &gettransaction,           false, {"txid","include_watchonly"} },
    { "wallet",             "getstakesplitthreshold",   &getstakesplitthreshold,   false, {} },
    { "wallet",             "getunconfirmedbalance",    &getunconfirmedbalance,    false, {} },
    { "wallet",             "getwalletinfo",            &getwalletinfo,            false, {} },
    { "wallet",             "getstakingstatus",         &getstakingstatus,         false, {} },
    { "wallet",             "importprivkey",            &importprivkey,            true,  {"privkey","label","rescan","is_staking_address"} },
    { "wallet",             "importwallet",             &importwallet,             true,  {"filename"} },
    { "wallet",             "importaddress",            &importaddress,            true,  {"address","label","rescan","p2sh"} },
    { "wallet",             "importpubkey",             &importpubkey,             true,  {"pubkey","label","rescan"} },
    { "wallet",             "importmulti",              &importmulti,              true,  {"requests","options"} },
    { "wallet",             "keypoolrefill",            &keypoolrefill,            true,  {"newsize"} },
    { "wallet",             "listaddressgroupings",     &listaddressgroupings,     false, {} },
    { "wallet",             "listdelegators",           &listdelegators,           false, {"blacklist"} },
    { "wallet",             "liststakingaddresses",     &liststakingaddresses,     false, {} },
    { "wallet",             "listcoldutxos",            &listcoldutxos,            false, {"not_whitelisted"} },
    { "wallet",             "listlockunspent",          &listlockunspent,          false, {} },
    { "wallet",             "listreceivedbyaddress",    &listreceivedbyaddress,    false, {"minconf","include_empty","include_watchonly","filter"} },
    { "wallet",             "listsinceblock",           &listsinceblock,           false, {"blockhash","target_confirmations","include_watchonly"} },
    { "wallet",             "listtransactions",         &listtransactions,         false, {"dummy","count","from","include_watchonly","include_delegated","include_cold"} },
    { "wallet",             "listunspent",              &listunspent,              false, {"minconf","maxconf","addresses","watchonly_config","query_options","include_unsafe" } },
    { "wallet",             "listwallets",              &listwallets,              true,  {} },
    { "wallet",             "lockunspent",              &lockunspent,              true,  {"unlock","transactions"} },
    { "wallet",             "rawdelegatestake",         &rawdelegatestake,         false, {"staking_addr","amount","owner_addr","ext_owner","include_delegated","from_shield","force"} },
    { "wallet",             "sendmany",                 &sendmany,                 false, {"dummy","amounts","minconf","comment","include_delegated","subtract_fee_from"} },
    { "wallet",             "sendtoaddress",            &sendtoaddress,            false, {"address","amount","comment","comment-to","subtract_fee"} },
    { "wallet",             "settxfee",                 &settxfee,                 true,  {"amount"} },
    { "wallet",             "setstakesplitthreshold",   &setstakesplitthreshold,   false, {"value"} },
    { "wallet",             "signmessage",              &signmessage,              true,  {"address","message"} },
    { "wallet",             "walletlock",               &walletlock,               true,  {} },
    { "wallet",             "walletpassphrasechange",   &walletpassphrasechange,   true,  {"oldpassphrase","newpassphrase"} },
    { "wallet",             "walletpassphrase",         &walletpassphrase,         true,  {"passphrase","timeout","staking_only"} },
    { "wallet",             "rescanblockchain",         &rescanblockchain,         true,  {"start_height","stop_height"} },
    { "wallet",             "delegatoradd",             &delegatoradd,             true,  {"address","label"} },
    { "wallet",             "delegatorremove",          &delegatorremove,          true,  {"address"} },
    { "wallet",             "bip38encrypt",             &bip38encrypt,             true,  {"address","passphrase"} },
    { "wallet",             "bip38decrypt",             &bip38decrypt,             true,  {"encrypted_key","passphrase"} },

    /** Sapling functions */
    { "wallet",             "getnewshieldaddress",           &getnewshieldaddress,            true,  {} },
    { "wallet",             "listshieldaddresses",           &listshieldaddresses,            false, {"include_watchonly"} },
    { "wallet",             "exportsaplingkey",              &exportsaplingkey,               true,  {"shield_addr"} },
    { "wallet",             "importsaplingkey",              &importsaplingkey,               true,  {"key","rescan","height"} },
    { "wallet",             "importsaplingviewingkey",       &importsaplingviewingkey,        true,  {"vkey","rescan","height"}  },
    { "wallet",             "exportsaplingviewingkey",       &exportsaplingviewingkey,        true,  {"shield_addr"} },
    { "wallet",             "getshieldbalance",              &getshieldbalance,               false, {"address","minconf","include_watchonly"} },
    { "wallet",             "listshieldunspent",             &listshieldunspent,              false, {"minconf","maxconf","include_watchonly","addresses"} },
    { "wallet",             "rawshieldsendmany",             &rawshieldsendmany,              false, {"fromaddress","amounts","minconf","fee"} },
    { "wallet",             "shieldsendmany",                &shieldsendmany,                 false, {"fromaddress","amounts","minconf","fee","subtract_fee_from"} },
    { "wallet",             "listreceivedbyshieldaddress",   &listreceivedbyshieldaddress,    false, {"address","minconf"} },
    { "wallet",             "viewshieldtransaction",         &viewshieldtransaction,          false, {"txid"} },
    { "wallet",             "getsaplingnotescount",          &getsaplingnotescount,           false, {"minconf"} },

    /** Label functions (to replace non-balance account functions) */
    { "wallet",             "getaddressesbylabel",      &getaddressesbylabel,      true,  {"label"} },
    { "wallet",             "getreceivedbylabel",       &getreceivedbylabel,       false, {"label","minconf"} },
    { "wallet",             "listlabels",               &listlabels,               false, {"purpose"} },
    { "wallet",             "listreceivedbylabel",      &listreceivedbylabel,      false, {"minconf","include_empty","include_watchonly"} },
    { "wallet",             "setlabel",                 &setlabel,                 true,  {"address","label"} },
};

void RegisterWalletRPCCommands(CRPCTable &tableRPC)
{
    if (gArgs.GetBoolArg("-disablewallet", false)) {
        return;
    }
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++) {
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
    }
}
