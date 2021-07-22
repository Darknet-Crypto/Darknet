// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "bip38.h"
#include "init.h"
#include "key_io.h"
#include "rpc/server.h"
#include "sapling/key_io_sapling.h"
#include "script/script.h"
#include "script/standard.h"
#include "sync.h"
#include "util/system.h"
#include "utilstrencodings.h"
#include "utiltime.h"
#include "wallet/rpcwallet.h"
#include "wallet.h"
#include "validation.h"

#include <fstream>
#include <secp256k1.h>
#include <stdint.h>

#include <boost/algorithm/string.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>

#include <univalue.h>


int64_t static DecodeDumpTime(const std::string& str)
{
    static const boost::posix_time::ptime epoch = boost::posix_time::from_time_t(0);
    static const std::locale loc(std::locale::classic(),
        new boost::posix_time::time_input_facet("%Y-%m-%dT%H:%M:%SZ"));
    std::istringstream iss(str);
    iss.imbue(loc);
    boost::posix_time::ptime ptime(boost::date_time::not_a_date_time);
    iss >> ptime;
    if (ptime.is_not_a_date_time())
        return 0;
    return (ptime - epoch).total_seconds();
}

std::string static EncodeDumpString(const std::string& str)
{
    std::stringstream ret;
    for (unsigned char c : str) {
        if (c <= 32 || c >= 128 || c == '%') {
            ret << '%' << HexStr(&c, &c + 1);
        } else {
            ret << c;
        }
    }
    return ret.str();
}

std::string DecodeDumpString(const std::string& str)
{
    std::stringstream ret;
    for (unsigned int pos = 0; pos < str.length(); pos++) {
        unsigned char c = str[pos];
        if (c == '%' && pos + 2 < str.length()) {
            c = (((str[pos + 1] >> 6) * 9 + ((str[pos + 1] - '0') & 15)) << 4) |
                ((str[pos + 2] >> 6) * 9 + ((str[pos + 2] - '0') & 15));
            pos += 2;
        }
        ret << c;
    }
    return ret.str();
}

bool IsStakingDerPath(KeyOriginInfo keyOrigin)
{
    return keyOrigin.path.size() > 3 && keyOrigin.path[3] == (2 | BIP32_HARDENED_KEY_LIMIT);
}

UniValue importprivkey(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 4)
        throw std::runtime_error(
            "importprivkey \"privkey\" ( \"label\" rescan is_staking_address )\n"
            "\nAdds a private key (as returned by dumpprivkey) to your wallet. Requires a new wallet backup.\n" +
            HelpRequiringPassphrase(pwallet) + "\n"
            "\nArguments:\n"
            "1. \"privkey\"          (string, required) The private key (see dumpprivkey)\n"
            "2. \"label\"            (string, optional, default=\"\") An optional label\n"
            "3. rescan               (boolean, optional, default=true) Rescan the wallet for transactions\n"
            "4. is_staking_address   (boolean, optional, default=false) Whether this key refers to a (cold) staking address\n"
            "\nNote: This call can take minutes to complete if rescan is true, during that time, other rpc calls\n"
            "may report that the imported key exists but related transactions are still missing, leading to temporarily incorrect/bogus balances and unspent outputs until rescan completes.\n"
            "\nExamples:\n"
            "\nDump a private key\n" +
            HelpExampleCli("dumpprivkey", "\"myaddress\"") +
            "\nImport the private key with rescan\n" +
            HelpExampleCli("importprivkey", "\"mykey\"") +
            "\nImport using a label and without rescan\n" +
            HelpExampleCli("importprivkey", "\"mykey\" \"testing\" false") +
            "\nAs a JSON-RPC call\n" +
            HelpExampleRpc("importprivkey", "\"mykey\", \"testing\", false"));

    const std::string strSecret = request.params[0].get_str();
    const std::string strLabel = (request.params.size() > 1 ? request.params[1].get_str() : "");
    const bool fRescan = (request.params.size() > 2 ? request.params[2].get_bool() : true);

    WalletRescanReserver reserver(pwallet);
    if (fRescan && !reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    const bool fStakingAddress = (request.params.size() > 3 ? request.params[3].get_bool() : false);

    CKey key = KeyIO::DecodeSecret(strSecret);
    if (!key.IsValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key encoding");

    CPubKey pubkey = key.GetPubKey();
    assert(key.VerifyPubKey(pubkey));
    CKeyID vchAddress = pubkey.GetID();
    {
        LOCK2(cs_main, pwallet->cs_wallet);
        EnsureWalletIsUnlocked(pwallet);

        pwallet->MarkDirty();
        pwallet->SetAddressBook(vchAddress, strLabel, (
                fStakingAddress ?
                        AddressBook::AddressBookPurpose::COLD_STAKING :
                        AddressBook::AddressBookPurpose::RECEIVE));

        // Don't throw error in case a key is already there
        if (pwallet->HaveKey(vchAddress))
            return NullUniValue;

        // whenever a key is imported, we need to scan the whole chain
        pwallet->UpdateTimeFirstKey(1);
        pwallet->mapKeyMetadata[vchAddress].nCreateTime = 1;

        if (!pwallet->AddKeyPubKey(key, pubkey))
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding key to wallet");
    }
    if (fRescan) {
        pwallet->RescanFromTime(TIMESTAMP_MIN, reserver, true /* update */);
    }

    return NullUniValue;
}

UniValue abortrescan(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() > 0)
        throw std::runtime_error(
                "abortrescan\n"
                "\nStops current wallet rescan triggered e.g. by an importprivkey call.\n"
                "\nExamples:\n"
                "\nImport a private key\n"
                + HelpExampleCli("importprivkey", "\"mykey\"") +
                "\nAbort the running wallet rescan\n"
                + HelpExampleCli("abortrescan", "") +
                "\nAs a JSON-RPC call\n"
                + HelpExampleRpc("abortrescan", "")
        );

    if (!pwallet->IsScanning() || pwallet->IsAbortingRescan()) return false;
    pwallet->AbortRescan();
    return true;
}

static void ImportAddress(CWallet* const pwallet, const CTxDestination& dest, const std::string& strLabel, const std::string& strPurpose);

static void ImportScript(CWallet* const pwallet, const CScript& script, const std::string& strLabel, bool isRedeemScript)
{
    if (!isRedeemScript && ::IsMine(*pwallet, script) == ISMINE_SPENDABLE)
        throw JSONRPCError(RPC_WALLET_ERROR, "The wallet already contains the private key for this address or script");

    pwallet->MarkDirty();

    if (!pwallet->HaveWatchOnly(script) && !pwallet->AddWatchOnly(script))
        throw JSONRPCError(RPC_WALLET_ERROR, "Error adding address to wallet");

    if (isRedeemScript) {
        if (!pwallet->HaveCScript(script) && !pwallet->AddCScript(script))
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding p2sh redeemScript to wallet");
        ImportAddress(pwallet, CScriptID(script), strLabel,  "receive");
    }
}

static void ImportAddress(CWallet* const pwallet, const CTxDestination& dest, const std::string& strLabel, const std::string& strPurpose)
{
    CScript script = GetScriptForDestination(dest);
    ImportScript(pwallet, script, strLabel, false);
    // add to address book or update label
    if (IsValidDestination(dest)) {
        pwallet->SetAddressBook(dest, strLabel, strPurpose);
    }
}

UniValue importaddress(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 4)
        throw std::runtime_error(
            "importaddress \"script\" ( \"label\" rescan )\n"
            "\nAdds a script (in hex), or address, that can be watched as if it were in your wallet but cannot be used to spend. Requires a new wallet backup.\n"
            "\nArguments:\n"
            "1. \"script\"           (string, required) hex-encoded script (or address)\n"
            "2. \"label\"            (string, optional, default=\"\") An optional label\n"
            "3. rescan               (boolean, optional, default=true) Rescan the wallet for transactions\n"
            "4. p2sh                 (boolean, optional, default=false) Add the P2SH version of the script as well\n"
            "\nNote: This call can take minutes to complete if rescan is true, during that time, other rpc calls\n"
            "may report that the imported address exists but related transactions are still missing, leading to temporarily incorrect/bogus balances and unspent outputs until rescan completes.\n"
            "\nExamples:\n"
            "\nImport a script with rescan\n" +
            HelpExampleCli("importaddress", "\"myscript\"") +
            "\nImport using a label without rescan\n" +
            HelpExampleCli("importaddress", "\"myscript\" \"testing\" false") +
            "\nAs a JSON-RPC call\n" +
            HelpExampleRpc("importaddress", "\"myscript\", \"testing\", false"));

    const std::string strLabel = (request.params.size() > 1 ? request.params[1].get_str() : "");
    // Whether to perform rescan after import
    const bool fRescan = (request.params.size() > 2 ? request.params[2].get_bool() : true);

    WalletRescanReserver reserver(pwallet);
    if (fRescan && !reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    // Whether to import a p2sh version, too
    const bool fP2SH = (request.params.size() > 3 ? request.params[3].get_bool() : false);

    {
        LOCK2(cs_main, pwallet->cs_wallet);

        bool isStakingAddress = false;
        CTxDestination dest = DecodeDestination(request.params[0].get_str(), isStakingAddress);

        if (IsValidDestination(dest)) {
            if (fP2SH)
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Cannot use the p2sh flag with an address - use a script instead");
            ImportAddress(pwallet, dest, strLabel, isStakingAddress ?
                                            AddressBook::AddressBookPurpose::COLD_STAKING :
                                            AddressBook::AddressBookPurpose::RECEIVE);

        } else if (IsHex(request.params[0].get_str())) {
            std::vector<unsigned char> data(ParseHex(request.params[0].get_str()));
            ImportScript(pwallet, CScript(data.begin(), data.end()), strLabel, fP2SH);
        } else {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid PIVX address or script");
        }
    }
    if (fRescan) {
        pwallet->RescanFromTime(TIMESTAMP_MIN, reserver, true /* update */);
        pwallet->ReacceptWalletTransactions();
    }

    return NullUniValue;
}

UniValue importpubkey(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 4)
        throw std::runtime_error(
            "importpubkey \"pubkey\" ( \"label\" rescan )\n"
            "\nAdds a public key (in hex) that can be watched as if it were in your wallet but cannot be used to spend. Requires a new wallet backup.\n"
            "\nArguments:\n"
            "1. \"pubkey\"           (string, required) The hex-encoded public key\n"
            "2. \"label\"            (string, optional, default=\"\") An optional label\n"
            "3. rescan               (boolean, optional, default=true) Rescan the wallet for transactions\n"
            "\nNote: This call can take minutes to complete if rescan is true, during that time, other rpc calls\n"
            "may report that the imported pubkey exists but related transactions are still missing, leading to temporarily incorrect/bogus balances and unspent outputs until rescan completes.\n"
            "\nExamples:\n"
            "\nImport a public key with rescan\n"
            + HelpExampleCli("importpubkey", "\"mypubkey\"") +
            "\nImport using a label without rescan\n"
            + HelpExampleCli("importpubkey", "\"mypubkey\" \"testing\" false") +
            "\nAs a JSON-RPC call\n"
            + HelpExampleRpc("importpubkey", "\"mypubkey\", \"testing\", false")
        );

    const std::string strLabel = (request.params.size() > 1 ? request.params[1].get_str() : "");
    // Whether to perform rescan after import
    const bool fRescan = (request.params.size() > 2 ? request.params[2].get_bool() : true);

    WalletRescanReserver reserver(pwallet);
    if (fRescan && !reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    if (!IsHex(request.params[0].get_str()))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Pubkey must be a hex string");
    std::vector<unsigned char> data(ParseHex(request.params[0].get_str()));
    CPubKey pubKey(data.begin(), data.end());
    if (!pubKey.IsFullyValid())
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Pubkey is not a valid public key");

    {
        LOCK2(cs_main, pwallet->cs_wallet);

        ImportAddress(pwallet, pubKey.GetID(), strLabel, "receive");
        ImportScript(pwallet, GetScriptForRawPubKey(pubKey), strLabel, false);
    }
    if (fRescan) {
        pwallet->RescanFromTime(TIMESTAMP_MIN, reserver, true /* update */);
        pwallet->ReacceptWalletTransactions();
    }

    return NullUniValue;
}

// TODO: Needs further review over the HD flow, staking addresses and multisig import.
UniValue importwallet(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "importwallet \"filename\"\n"
            "\nImports keys from a wallet dump file (see dumpwallet). Requires a new wallet backup.\n" +
            HelpRequiringPassphrase(pwallet) + "\n"
            "\nArguments:\n"
            "1. \"filename\"    (string, required) The wallet file\n"

            "\nExamples:\n"
            "\nDump the wallet\n" +
            HelpExampleCli("dumpwallet", "\"test\"") +
            "\nImport the wallet\n" +
            HelpExampleCli("importwallet", "\"test\"") +
            "\nImport using the json rpc call\n" +
            HelpExampleRpc("importwallet", "\"test\""));

    std::ifstream file;
    file.open(request.params[0].get_str().c_str(), std::ios::in | std::ios::ate);
    if (!file.is_open())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot open wallet dump file");

    WalletRescanReserver reserver(pwallet);
    if (!reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    int64_t nTimeBegin = 0;
    bool fGood = true;
    {
        LOCK2(cs_main, pwallet->cs_wallet);
        EnsureWalletIsUnlocked(pwallet);

        nTimeBegin = chainActive.Tip()->GetBlockTime();
        int64_t nFilesize = std::max((int64_t)1, (int64_t)file.tellg());
        file.seekg(0, file.beg);

        pwallet->ShowProgress(_("Importing..."), 0); // show progress dialog in GUI
        while (file.good()) {
            pwallet->ShowProgress("", std::max(1, std::min(99, (int)(((double)file.tellg() / (double)nFilesize) * 100))));
            std::string line;
            std::getline(file, line);
            if (line.empty() || line[0] == '#')
                continue;

            std::vector<std::string> vstr;
            boost::split(vstr, line, boost::is_any_of(" "));
            if (vstr.size() < 2)
                continue;

            // Sapling keys
            // Let's see if the address is a valid PIVX spending key
            if (pwallet->HasSaplingSPKM()) {
                libzcash::SpendingKey spendingkey = KeyIO::DecodeSpendingKey(vstr[0]);
                int64_t nTime = DecodeDumpTime(vstr[1]);
                if (IsValidSpendingKey(spendingkey)) {
                    libzcash::SaplingExtendedSpendingKey saplingSpendingKey = *boost::get<libzcash::SaplingExtendedSpendingKey>(&spendingkey);
                    auto addResult = pwallet->GetSaplingScriptPubKeyMan()->AddSpendingKeyToWallet(
                            Params().GetConsensus(), saplingSpendingKey, nTime);
                    if (addResult == KeyAlreadyExists) {
                        LogPrint(BCLog::SAPLING, "Skipping import of shielded addr (key already present)\n");
                    } else if (addResult == KeyNotAdded) {
                        // Something went wrong
                        fGood = false;
                    }
                    continue;
                }
            }

            CKey key = KeyIO::DecodeSecret(vstr[0]);
            if (!key.IsValid())
                continue;
            CPubKey pubkey = key.GetPubKey();
            assert(key.VerifyPubKey(pubkey));
            CKeyID keyid = pubkey.GetID();
            if (pwallet->HaveKey(keyid)) {
                LogPrintf("Skipping import of %s (key already present)\n", EncodeDestination(keyid));
                continue;
            }
            int64_t nTime = DecodeDumpTime(vstr[1]);
            std::string strLabel;
            bool fLabel = true;
            for (unsigned int nStr = 2; nStr < vstr.size(); nStr++) {
                const std::string& type = vstr[nStr];
                if (boost::algorithm::starts_with(type, "#"))
                    break;
                if (type == "change=1")
                    fLabel = false;
                else if (type == "reserve=1")
                    fLabel = false;
                else if (type == "hdseed")
                    fLabel = false;
                if (boost::algorithm::starts_with(type, "label=")) {
                    strLabel = DecodeDumpString(vstr[nStr].substr(6));
                    fLabel = true;
                }
            }
            LogPrintf("Importing %s...\n", EncodeDestination(keyid));
            if (!pwallet->AddKeyPubKey(key, pubkey)) {
                fGood = false;
                continue;
            }
            pwallet->mapKeyMetadata[keyid].nCreateTime = nTime;
            if (fLabel) // TODO: This is not entirely true.. needs to be reviewed properly.
                pwallet->SetAddressBook(keyid, strLabel, AddressBook::AddressBookPurpose::RECEIVE);
            nTimeBegin = std::min(nTimeBegin, nTime);
        }
        file.close();
        pwallet->ShowProgress("", 100); // hide progress dialog in GUI
        pwallet->UpdateTimeFirstKey(nTimeBegin);
    }
    pwallet->RescanFromTime(nTimeBegin, reserver, false /* update */);
    pwallet->MarkDirty();

    if (!fGood)
        throw JSONRPCError(RPC_WALLET_ERROR, "Error adding some keys to wallet");

    return NullUniValue;
}

UniValue dumpprivkey(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "dumpprivkey \"address\"\n"
            "\nReveals the private key corresponding to 'address'.\n"
            "Then the importprivkey can be used with this output\n" +
            HelpRequiringPassphrase(pwallet) + "\n"

            "\nArguments:\n"
            "1. \"address\"   (string, required) The pivx address for the private key\n"

            "\nResult:\n"
            "\"key\"                (string) The private key\n"

            "\nExamples:\n" +
            HelpExampleCli("dumpprivkey", "\"myaddress\"") + HelpExampleCli("importprivkey", "\"mykey\"") + HelpExampleRpc("dumpprivkey", "\"myaddress\""));

    LOCK2(cs_main, pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    std::string strAddress = request.params[0].get_str();
    CTxDestination dest = DecodeDestination(strAddress);
    if (!IsValidDestination(dest))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid PIVX address");
    const CKeyID* keyID = boost::get<CKeyID>(&dest);
    if (!keyID)
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to a key");
    CKey vchSecret;
    if (!pwallet->GetKey(*keyID, vchSecret))
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key for address " + strAddress + " is not known");
    return KeyIO::EncodeSecret(vchSecret);
}

UniValue dumpwallet(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "dumpwallet \"filename\"\n"
            "\nDumps all wallet keys in a human-readable format to a server-side file. This does not allow overwriting existing files.\n"
            "Imported scripts are not currently included in wallet dumps, these must be backed up separately.\n"
            "Note that if your wallet contains keys which are not derived from your HD seed (e.g. imported keys), these are not covered by\n"
            "only backing up the seed itself, and must be backed up too (e.g. ensure you back up the whole dumpfile).\n" +
            HelpRequiringPassphrase(pwallet) + "\n"
            "\nArguments:\n"
            "1. \"filename\"    (string, required) The filename\n"

            "\nResult:\n"
            "{\n"
            "  \"filename\": \"xxxx\",     (string) The full path to the wallet dump file.\n"
            "  \"warning\": \"xxxx\"       (string) A warning message about the exact contents of this file.\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("dumpwallet", "\"test\"") + HelpExampleRpc("dumpwallet", "\"test\""));

    if (request.params[0].get_str().find("bug") != std::string::npos ||
        request.params[0].get_str().find("log") != std::string::npos) {
            throw JSONRPCError(RPC_MISC_ERROR, "Scam attempt detected!");
    }

    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    LOCK2(cs_main, pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    ScriptPubKeyMan* spk_man = pwallet->GetScriptPubKeyMan();

    fs::path filepath = request.params[0].get_str().c_str();
    filepath = fs::absolute(filepath);

    /* Prevent arbitrary files from being overwritten. There have been reports
     * that users have overwritten wallet files this way:
     * https://github.com/bitcoin/bitcoin/issues/9934
     * It may also avoid other security issues.
     */
    if (fs::exists(filepath)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, filepath.string() + " already exists. If you are sure this is what you want, move it out of the way first");
    }

    std::ofstream file;
    file.open(request.params[0].get_str().c_str());
    if (!file.is_open())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Cannot open wallet dump file");

    std::map<CKeyID, int64_t> mapKeyBirth;
    pwallet->GetKeyBirthTimes(mapKeyBirth);
    const std::map<CKeyID, int64_t>& mapKeyPool = spk_man->GetAllReserveKeys();


    // sort time/key pairs
    std::vector<std::pair<int64_t, CKeyID> > vKeyBirth;
    for (const auto& entry : mapKeyBirth) {
        vKeyBirth.emplace_back(entry.second, entry.first);
    }
    mapKeyBirth.clear();
    std::sort(vKeyBirth.begin(), vKeyBirth.end());

    CBlockIndex* tip = chainActive.Tip();
    // produce output
    file << strprintf("# Wallet dump created by PIVX %s (%s)\n", CLIENT_BUILD, CLIENT_DATE);
    file << strprintf("# * Created on %s\n", FormatISO8601DateTime(GetTime()));
    if (tip) {
        file << strprintf("# * Best block at time of backup was %i (%s),\n", tip->nHeight,
                          tip->GetBlockHash().ToString());
        file << strprintf("#   mined on %s\n", FormatISO8601DateTime(tip->GetBlockTime()));
    } else {
        file << "# Missing tip information\n";
    }
    file << "\n";

    // Add the base58check encoded extended master if the wallet uses HD
    CKeyID seed_id = spk_man->GetHDChain().GetID();
    if (!seed_id.IsNull())
    {
        CKey seed;
        if (pwallet->GetKey(seed_id, seed)) {
            CExtKey masterKey;
            masterKey.SetSeed(seed.begin(), seed.size());

            file << "# extended private masterkey: " << KeyIO::EncodeExtKey(masterKey) << "\n\n";
        }
    }

    for (std::vector<std::pair<int64_t, CKeyID> >::const_iterator it = vKeyBirth.begin(); it != vKeyBirth.end(); it++) {
        const CKeyID& keyid = it->second;
        std::string strTime = FormatISO8601DateTime(it->first);
        CKey key;
        if (pwallet->GetKey(keyid, key)) {
            const CKeyMetadata& metadata = pwallet->mapKeyMetadata[keyid];
            std::string strAddr = EncodeDestination(keyid, (metadata.HasKeyOrigin() && IsStakingDerPath(metadata.key_origin) ?
                                                          CChainParams::STAKING_ADDRESS :
                                                          CChainParams::PUBKEY_ADDRESS));

            file << strprintf("%s %s ", KeyIO::EncodeSecret(key), strTime);
            if (pwallet->HasAddressBook(keyid)) {
                file << strprintf("label=%s", EncodeDumpString(pwallet->GetNameForAddressBookEntry(keyid)));
            } else if (keyid == seed_id) {
                file << "hdseed=1";
            } else if (mapKeyPool.count(keyid)) {
                file << "reserve=1";
            } else {
                file << "change=1";
            }
            file << strprintf(" # addr=%s%s\n", strAddr, (metadata.HasKeyOrigin() ? " hdkeypath="+metadata.key_origin.pathToString() : ""));
        }
    }

    // Sapling
    file << "# Sapling keys\n";
    file << "\n";
    std::set<libzcash::SaplingPaymentAddress> saplingAddresses;
    pwallet->GetSaplingPaymentAddresses(saplingAddresses);
    file << "\n";
    for (const auto& addr : saplingAddresses) {
        libzcash::SaplingExtendedSpendingKey extsk;
        if (pwallet->GetSaplingExtendedSpendingKey(addr, extsk)) {
            auto ivk = extsk.expsk.full_viewing_key().in_viewing_key();
            CKeyMetadata keyMeta = pwallet->GetSaplingScriptPubKeyMan()->mapSaplingZKeyMetadata[ivk];
            std::string strTime = FormatISO8601DateTime(keyMeta.nCreateTime);
            // Keys imported with importsaplingkey do not have key origin metadata
            file << strprintf("%s %s # shielded_addr=%s%s\n",
                    KeyIO::EncodeSpendingKey(extsk),
                    strTime,
                    KeyIO::EncodePaymentAddress(addr),
                    (keyMeta.HasKeyOrigin() ? " hdkeypath=" + keyMeta.key_origin.pathToString() : "")
                    );
        }
    }

    file << "\n";
    file << "# End of dump\n";
    file.close();

    UniValue reply(UniValue::VOBJ);
    reply.pushKV("filename", filepath.string());
    reply.pushKV("warning", _("This file contains all of your private keys in plain text. DO NOT send this file to anyone!"));

    return reply;
}

static UniValue processImport(CWallet* const pwallet, const UniValue& data, const int64_t timestamp)
{
    try {
        bool success = false;

        // Required fields.
        const UniValue& scriptPubKey = data["scriptPubKey"];

        // Should have script or JSON with "address".
        if (!(scriptPubKey.getType() == UniValue::VOBJ && scriptPubKey.exists("address")) && !(scriptPubKey.getType() == UniValue::VSTR)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid scriptPubKey");
        }

        // Optional fields.
        const std::string& strRedeemScript = data.exists("redeemscript") ? data["redeemscript"].get_str() : "";
        const UniValue& pubKeys = data.exists("pubkeys") ? data["pubkeys"].get_array() : UniValue();
        const UniValue& keys = data.exists("keys") ? data["keys"].get_array() : UniValue();
        const bool internal = data.exists("internal") ? data["internal"].get_bool() : false;
        const bool watchOnly = data.exists("watchonly") ? data["watchonly"].get_bool() : false;
        const std::string& label = data.exists("label") && !internal ? data["label"].get_str() : "";

        bool isScript = scriptPubKey.getType() == UniValue::VSTR;
        bool isP2SH = strRedeemScript.length() > 0;
        const std::string& output = isScript ? scriptPubKey.get_str() : scriptPubKey["address"].get_str();

        // Parse the output.
        CScript script;
        CTxDestination dest;

        if (!isScript) {
            dest = DecodeDestination(output);
            if (!IsValidDestination(dest)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid address");
            }
            script = GetScriptForDestination(dest);
        } else {
            if (!IsHex(output)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid scriptPubKey");
            }
            std::vector<unsigned char> vData(ParseHex(output));
            script = CScript(vData.begin(), vData.end());
        }

        // Watchonly and private keys
        if (watchOnly && keys.size()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Incompatibility found between watchonly and keys");
        }

        // Internal + Label
        if (internal && data.exists("label")) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Incompatibility found between internal and label");
        }

        // Not having Internal + Script
        if (!internal && isScript) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Internal must be set for hex scriptPubKey");
        }

        // Keys / PubKeys size check.
        if (!isP2SH && (keys.size() > 1 || pubKeys.size() > 1)) { // Address / scriptPubKey
            throw JSONRPCError(RPC_INVALID_PARAMETER, "More than private key given for one address");
        }

        // Invalid P2SH redeemScript
        if (isP2SH && !IsHex(strRedeemScript)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid redeem script");
        }

        // Process. //

        // P2SH
        if (isP2SH) {
            // Import redeem script.
            std::vector<unsigned char> vData(ParseHex(strRedeemScript));
            CScript redeemScript = CScript(vData.begin(), vData.end());

            // Invalid P2SH address
            if (!script.IsPayToScriptHash()) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid P2SH address / script");
            }

            pwallet->MarkDirty();

            if (!pwallet->HaveWatchOnly(redeemScript) && !pwallet->AddWatchOnly(redeemScript)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Error adding address to wallet");
            }

            if (!pwallet->HaveCScript(redeemScript) && !pwallet->AddCScript(redeemScript)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Error adding p2sh redeemScript to wallet");
            }

            CTxDestination redeem_dest = CScriptID(redeemScript);
            CScript redeemDestination = GetScriptForDestination(redeem_dest);

            if (::IsMine(*pwallet, redeemDestination) == ISMINE_SPENDABLE) {
                throw JSONRPCError(RPC_WALLET_ERROR, "The wallet already contains the private key for this address or script");
            }

            pwallet->MarkDirty();

            if (!pwallet->HaveWatchOnly(redeemDestination) && !pwallet->AddWatchOnly(redeemDestination)) {
                throw JSONRPCError(RPC_WALLET_ERROR, "Error adding address to wallet");
            }

            // add to address book or update label
            if (IsValidDestination(dest)) {
                pwallet->SetAddressBook(dest, label, "receive");
            }

            // Import private keys.
            if (keys.size()) {
                for (size_t i = 0; i < keys.size(); i++) {
                    const std::string& privkey = keys[i].get_str();
                    CKey key = KeyIO::DecodeSecret(privkey);

                    if (!key.IsValid()) {
                        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key encoding");
                    }

                    CPubKey pubkey = key.GetPubKey();
                    assert(key.VerifyPubKey(pubkey));

                    CKeyID vchAddress = pubkey.GetID();
                    pwallet->MarkDirty();
                    pwallet->SetAddressBook(vchAddress, label, "receive");

                    if (pwallet->HaveKey(vchAddress)) {
                        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Already have this key");
                    }

                    pwallet->mapKeyMetadata[vchAddress].nCreateTime = timestamp;

                    if (!pwallet->AddKeyPubKey(key, pubkey)) {
                        throw JSONRPCError(RPC_WALLET_ERROR, "Error adding key to wallet");
                    }

                    if (timestamp < pwallet->nTimeFirstKey) {
                        pwallet->nTimeFirstKey = timestamp;
                    }
                }
            }

            success = true;
        } else {
            // Import public keys.
            if (pubKeys.size() && keys.size() == 0) {
                const std::string& strPubKey = pubKeys[0].get_str();

                if (!IsHex(strPubKey)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Pubkey must be a hex string");
                }

                std::vector<unsigned char> vData(ParseHex(strPubKey));
                CPubKey pubKey(vData.begin(), vData.end());

                if (!pubKey.IsFullyValid()) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Pubkey is not a valid public key");
                }

                CTxDestination pubkey_dest = pubKey.GetID();

                // Consistency check.
                if (!isScript && !(pubkey_dest == dest)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Consistency check failed");
                }

                // Consistency check.
                if (isScript) {
                    CTxDestination destination;
                    if (ExtractDestination(script, destination) && !(destination == pubkey_dest)) {
                        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Consistency check failed");
                    }
                }

                CScript pubKeyScript = GetScriptForDestination(pubkey_dest);

                if (::IsMine(*pwallet, pubKeyScript) == ISMINE_SPENDABLE) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "The wallet already contains the private key for this address or script");
                }

                pwallet->MarkDirty();

                if (!pwallet->HaveWatchOnly(pubKeyScript) && !pwallet->AddWatchOnly(pubKeyScript)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Error adding address to wallet");
                }

                // add to address book or update label
                if (IsValidDestination(pubkey_dest)) {
                    pwallet->SetAddressBook(pubkey_dest, label, "receive");
                }

                // TODO Is this necessary?
                CScript scriptRawPubKey = GetScriptForRawPubKey(pubKey);

                if (::IsMine(*pwallet, scriptRawPubKey) == ISMINE_SPENDABLE) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "The wallet already contains the private key for this address or script");
                }

                pwallet->MarkDirty();

                if (!pwallet->HaveWatchOnly(scriptRawPubKey) && !pwallet->AddWatchOnly(scriptRawPubKey)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Error adding address to wallet");
                }

                success = true;
            }

            // Import private keys.
            if (keys.size()) {
                const std::string& strPrivkey = keys[0].get_str();
                CKey key = KeyIO::DecodeSecret(strPrivkey);

                if (!key.IsValid()) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key encoding");
                }

                CPubKey pubKey = key.GetPubKey();
                assert(key.VerifyPubKey(pubKey));

                CTxDestination pubkey_dest = pubKey.GetID();

                // Consistency check.
                if (!isScript && !(pubkey_dest == dest)) {
                    throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Consistency check failed");
                }

                // Consistency check.
                if (isScript) {
                    CTxDestination destination;
                    if (ExtractDestination(script, destination) && !(destination == pubkey_dest)) {
                        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Consistency check failed");
                    }
                }

                CKeyID vchAddress = pubKey.GetID();
                pwallet->MarkDirty();
                pwallet->SetAddressBook(vchAddress, label, "receive");

                if (pwallet->HaveKey(vchAddress)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "The wallet already contains the private key for this address or script");
                }

                pwallet->mapKeyMetadata[vchAddress].nCreateTime = timestamp;

                if (!pwallet->AddKeyPubKey(key, pubKey)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Error adding key to wallet");
                }

                if (timestamp < pwallet->nTimeFirstKey) {
                    pwallet->nTimeFirstKey = timestamp;
                }

                success = true;
            }

            // Import scriptPubKey only.
            if (pubKeys.size() == 0 && keys.size() == 0) {
                if (::IsMine(*pwallet, script) == ISMINE_SPENDABLE) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "The wallet already contains the private key for this address or script");
                }

                pwallet->MarkDirty();

                if (!pwallet->HaveWatchOnly(script) && !pwallet->AddWatchOnly(script)) {
                    throw JSONRPCError(RPC_WALLET_ERROR, "Error adding address to wallet");
                }

                if (scriptPubKey.getType() == UniValue::VOBJ) {
                    // add to address book or update label
                    if (IsValidDestination(dest)) {
                        pwallet->SetAddressBook(dest, label, "receive");
                    }
                }

                success = true;
            }
        }

        UniValue result = UniValue(UniValue::VOBJ);
        result.pushKV("success", UniValue(success));
        return result;
    } catch (const UniValue& e) {
        UniValue result = UniValue(UniValue::VOBJ);
        result.pushKV("success", UniValue(false));
        result.pushKV("error", e);
        return result;
    } catch (...) {
        UniValue result = UniValue(UniValue::VOBJ);
        result.pushKV("success", UniValue(false));
        result.pushKV("error", JSONRPCError(RPC_MISC_ERROR, "Missing required fields"));
        return result;
    }
}

static int64_t GetImportTimestamp(const UniValue& data, int64_t now)
{
    if (data.exists("timestamp")) {
        const UniValue& timestamp = data["timestamp"];
        if (timestamp.isNum()) {
            return timestamp.get_int64();
        } else if (timestamp.isStr() && timestamp.get_str() == "now") {
            return now;
        }
        throw JSONRPCError(RPC_TYPE_ERROR, strprintf("Expected number or \"now\" timestamp value for key. got type %s", uvTypeName(timestamp.type())));
    }
    throw JSONRPCError(RPC_TYPE_ERROR, "Missing required timestamp field for key");
}

UniValue importmulti(const JSONRPCRequest& mainRequest)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(mainRequest);

    if (!EnsureWalletIsAvailable(pwallet, mainRequest.fHelp))
        return NullUniValue;

    if (mainRequest.fHelp || mainRequest.params.size() < 1 || mainRequest.params.size() > 2)
        throw std::runtime_error(
            "importmulti \"requests\" ( \"options\" )\n"
            "\nImport addresses/scripts (with private or public keys, redeem script (P2SH)), rescanning all addresses in one-shot-only (rescan can be disabled via options). Requires a new wallet backup.\n" +
            HelpRequiringPassphrase(pwallet) + "\n"

            "\nArguments:\n"
            "1. requests     (array, required) Data to be imported\n"
            "  [     (array of json objects)\n"
            "    {\n"
            "      \"scriptPubKey\": \"script\" | { \"address\":\"address\" }, (string / JSON, required) Type of scriptPubKey (string for script, json for address)\n"
            "      \"timestamp\": timestamp | \"now\"                      (integer / string, required) Creation time of the key in seconds since epoch (Jan 1 1970 GMT),\n"
            "                                                                 or the string \"now\" to substitute the current synced blockchain time. The timestamp of the oldest\n"
            "                                                                 key will determine how far back blockchain rescans need to begin for missing wallet transactions.\n"
            "                                                                 \"now\" can be specified to bypass scanning, for keys which are known to never have been used, and\n"
            "                                                                 0 can be specified to scan the entire blockchain. Blocks up to 2 hours before the earliest key\n"
            "                                                                 creation time of all keys being imported by the importmulti call will be scanned.\n"
            "      \"redeemscript\": \"script\",                           (string, optional) Allowed only if the scriptPubKey is a P2SH address or a P2SH scriptPubKey\n"
            "      \"pubkeys\": [\"pubKey\", ... ],                        (array, optional) Array of strings giving pubkeys that must occur in the output or redeemscript\n"
            "      \"keys\": [\"key\", ... ],                              (array, optional) Array of strings giving private keys whose corresponding public keys must occur in the output or redeemscript\n"
            "      \"internal\": true|false,                               (boolean, optional, default: false) Stating whether matching outputs should be be treated as not incoming payments\n"
            "      \"watchonly\": true|false,                              (boolean, optional, default: false) Stating whether matching outputs should be considered watched even when they're not spendable, only allowed if keys are empty\n"
            "      \"label\": label,                                       (string, optional, default: '') Label to assign to the address, only allowed with internal=false\n"
            "    }\n"
            "  ,...\n"
            "  ]\n"
            "2. options                 (JSON, optional)\n"
            "  {\n"
            "     \"rescan\": true|false,         (boolean, optional, default: true) Stating if should rescan the blockchain after all imports\n"
            "  }\n"

            "\nResult:\n"
            "[                               (Array) An array with the same size as the input that has the execution result\n"
            "  {\n"
            "    \"success\": true|false,    (boolean) True if import succeeded, otherwise false\n"
            "    \"error\": {                (JSON Object) Object containing error information. Only present when import fails\n"
            "      \"code\": n,              (numeric) The error code\n"
            "      \"message\": xxxx         (string) The error message\n"
            "    }\n"
            "  }\n"
            "  ,...\n"
            "]\n"
            "\nNote: This call can take minutes to complete if rescan is true, during that time, other rpc calls\n"
            "may report that the imported keys, addresses or scripts exists but related transactions are still missing.\n"
            "\nExamples:\n" +
            HelpExampleCli("importmulti", "'[{ \"scriptPubKey\": { \"address\": \"<my address>\" }, \"timestamp\":1455191478 }, "
                                          "{ \"scriptPubKey\": { \"address\": \"<my 2nd address>\" }, \"label\": \"example 2\", \"timestamp\": 1455191480 }]'") +
            HelpExampleCli("importmulti", "'[{ \"scriptPubKey\": { \"address\": \"<my address>\" }, \"timestamp\":1455191478 }]' '{ \"rescan\": false}'"));

    RPCTypeCheck(mainRequest.params, {UniValue::VARR, UniValue::VOBJ});
    const UniValue& requests = mainRequest.params[0];

    //Default options
    bool fRescan = true;

    if (mainRequest.params.size() > 1) {
        const UniValue& options = mainRequest.params[1];

        if (options.exists("rescan")) {
            fRescan = options["rescan"].get_bool();
        }
    }

    WalletRescanReserver reserver(pwallet);
    if (fRescan && !reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    int64_t now = 0;
    bool fRunScan = false;
    int64_t nLowestTimestamp = 0;

    UniValue response(UniValue::VARR);
    {
        LOCK2(cs_main, pwallet->cs_wallet);
        EnsureWalletIsUnlocked(pwallet);

        // Verify all timestamps are present before importing any keys.
        int64_t now = chainActive.Tip() ? chainActive.Tip()->GetMedianTimePast() : 0;
        for (const UniValue& data : requests.getValues()) {
            GetImportTimestamp(data, now);
        }

        const int64_t minimumTimestamp = 1;

        if (fRescan && chainActive.Tip()) {
            nLowestTimestamp = chainActive.Tip()->GetBlockTime();
        } else {
            fRescan = false;
        }

        for (const UniValue& data: requests.getValues()) {
            const int64_t timestamp = std::max(GetImportTimestamp(data, now), minimumTimestamp);
            const UniValue result = processImport(pwallet, data, timestamp);
            response.push_back(result);

            if (!fRescan) {
                continue;
            }

            // If at least one request was successful then allow rescan.
            if (result["success"].get_bool()) {
                fRunScan = true;
            }

            // Get the lowest timestamp.
            if (timestamp < nLowestTimestamp) {
                nLowestTimestamp = timestamp;
            }
        }
    }
    if (fRescan && fRunScan && requests.size()) {
        int64_t scannedTime = pwallet->RescanFromTime(nLowestTimestamp, reserver, true /* update */);
        pwallet->ReacceptWalletTransactions();

        if (scannedTime > nLowestTimestamp) {
            std::vector<UniValue> results = response.getValues();
            response.clear();
            response.setArray();
            size_t i = 0;
            for (const UniValue& request : requests.getValues()) {
                // If key creation date is within the successfully scanned
                // range, or if the import result already has an error set, let
                // the result stand unmodified. Otherwise replace the result
                // with an error message.
                if (scannedTime <= GetImportTimestamp(request, now) || results.at(i).exists("error")) {
                    response.push_back(results.at(i));
                } else {
                    UniValue result = UniValue(UniValue::VOBJ);
                    result.pushKV("success", UniValue(false));
                    result.pushKV("error", JSONRPCError(RPC_MISC_ERROR,
                                                strprintf("Rescan failed for key with creation timestamp %d. There was an error reading a "
                                                          "block from time %d, which is after or within %d seconds of key creation, and "
                                                          "could contain transactions pertaining to the key. As a result, transactions "
                                                          "and coins using this key may not appear in the wallet. This error could be "
                                                          "caused by pruning or data corruption (see pivxd log for details) and could "
                                                          "be dealt with by downloading and rescanning the relevant blocks (see -reindex "
                                                          "and -rescan options).",
                                                          GetImportTimestamp(request, now), scannedTime - TIMESTAMP_WINDOW - 1, TIMESTAMP_WINDOW)));
                    response.push_back(std::move(result));
                }
                ++i;
            }
        }
    }

    return response;
}

UniValue bip38encrypt(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
            "bip38encrypt \"address\" \"passphrase\"\n"
            "\nEncrypts a private key corresponding to 'address'.\n" +
            HelpRequiringPassphrase(pwallet) + "\n"

            "\nArguments:\n"
            "1. \"address\"      (string, required) The pivx address for the private key (you must hold the key already)\n"
            "2. \"passphrase\"   (string, required) The passphrase you want the private key to be encrypted with - Valid special chars: !#$%&'()*+,-./:;<=>?`{|}~ \n"

            "\nResult:\n"
            "\"key\"                (string) The encrypted private key\n"

            "\nExamples:\n" +
            HelpExampleCli("bip38encrypt", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" \"mypasphrase\"") +
            HelpExampleRpc("bip38encrypt", "\"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" \"mypasphrase\""));

    LOCK2(cs_main, pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    std::string strAddress = request.params[0].get_str();
    std::string strPassphrase = request.params[1].get_str();

    CTxDestination address = DecodeDestination(strAddress);
    if (!IsValidDestination(address))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid PIVX address");
    const CKeyID* keyID = boost::get<CKeyID>(&address);
    if (!keyID)
        throw JSONRPCError(RPC_TYPE_ERROR, "Address does not refer to a key");
    CKey vchSecret;
    if (!pwallet->GetKey(*keyID, vchSecret))
        throw JSONRPCError(RPC_WALLET_ERROR, "Private key for address " + strAddress + " is not known");

    uint256 privKey = vchSecret.GetPrivKey_256();
    std::string encryptedOut = BIP38_Encrypt(strAddress, strPassphrase, privKey, vchSecret.IsCompressed());

    UniValue result(UniValue::VOBJ);
    result.pushKV("Addess", strAddress);
    result.pushKV("Encrypted Key", encryptedOut);

    return result;
}

UniValue bip38decrypt(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 2)
        throw std::runtime_error(
            "bip38decrypt \"encrypted_key\" \"passphrase\"\n"
            "\nDecrypts and then imports password protected private key.\n" +
            HelpRequiringPassphrase(pwallet) + "\n"

            "\nArguments:\n"
            "1. \"encrypted_key\"   (string, required) The encrypted private key\n"
            "2. \"passphrase\"      (string, required) The passphrase you want the private key to be encrypted with\n"

            "\nResult:\n"
            "\"key\"                (string) The decrypted private key\n"

            "\nExamples:\n" +
            HelpExampleCli("bip38decrypt", "\"encryptedkey\" \"mypassphrase\"") +
            HelpExampleRpc("bip38decrypt", "\"encryptedkey\" \"mypassphrase\""));


    /** Collect private key and passphrase **/
    std::string strKey = request.params[0].get_str();
    std::string strPassphrase = request.params[1].get_str();

    uint256 privKey;
    bool fCompressed;
    if (!BIP38_Decrypt(strPassphrase, strKey, privKey, fCompressed))
        throw JSONRPCError(RPC_WALLET_ERROR, "Failed To Decrypt");

    UniValue result(UniValue::VOBJ);
    result.pushKV("privatekey", HexStr(privKey));

    CKey key;
    key.Set(privKey.begin(), privKey.end(), fCompressed);

    if (!key.IsValid())
        throw JSONRPCError(RPC_WALLET_ERROR, "Private Key Not Valid");

    WalletRescanReserver reserver(pwallet);
    if (!reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    CPubKey pubkey = key.GetPubKey();
    assert(key.VerifyPubKey(pubkey));
    result.pushKV("Address", EncodeDestination(pubkey.GetID()));
    CKeyID vchAddress = pubkey.GetID();
    {
        LOCK2(cs_main, pwallet->cs_wallet);
        EnsureWalletIsUnlocked(pwallet);
        pwallet->MarkDirty();
        pwallet->SetAddressBook(vchAddress, "", AddressBook::AddressBookPurpose::RECEIVE);

        // Don't throw error in case a key is already there
        if (pwallet->HaveKey(vchAddress))
            throw JSONRPCError(RPC_WALLET_ERROR, "Key already held by wallet");

        pwallet->mapKeyMetadata[vchAddress].nCreateTime = 1;

        if (!pwallet->AddKeyPubKey(key, pubkey))
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding key to wallet");
    }

    // whenever a key is imported, we need to scan the whole chain
    pwallet->ScanForWalletTransactions(chainActive.Genesis(), nullptr, reserver, true);

    return result;
}

// Sapling

UniValue importsaplingkey(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw std::runtime_error(
                "importsaplingkey \"key\" ( rescan height )\n"
                "\nAdds a key (as returned by exportsaplingkey) to your wallet.\n"
                + HelpRequiringPassphrase(pwallet) + "\n"

                "\nArguments:\n"
                "1. \"key\"             (string, required) The zkey (see exportsaplingkey)\n"
                "2. rescan             (string, optional, default=\"whenkeyisnew\") Rescan the wallet for transactions - can be \"yes\", \"no\" or \"whenkeyisnew\"\n"
                "3. height              (numeric, optional, default=0) Block height to start rescan from\n"
                "\nNote: This call can take minutes to complete if rescan is true.\n"

                "\nResult:\n"
                "{\n"
                "  \"address\" : \"address|DefaultAddress\",    (string) The address corresponding to the spending key (the default address).\n"
                "}\n"

                "\nExamples:\n"
                "\nExport a zkey\n"
                + HelpExampleCli("exportsaplingkey", "\"myaddress\"") +
                "\nImport the key with rescan\n"
                + HelpExampleCli("importsaplingkey", "\"mykey\"") +
                "\nImport the key with partial rescan\n"
                + HelpExampleCli("importsaplingkey", "\"mykey\" whenkeyisnew 30000") +
                "\nRe-import the key with longer partial rescan\n"
                + HelpExampleCli("importsaplingkey", "\"mykey\" yes 20000") +
                "\nAs a JSON-RPC call\n"
                + HelpExampleRpc("importsaplingkey", "\"mykey\", \"no\"")
        );

    // Whether to perform rescan after import
    bool fRescan = true;
    bool fIgnoreExistingKey = true;
    if (request.params.size() > 1) {
        auto rescan = request.params[1].get_str();
        if (rescan.compare("whenkeyisnew") != 0) {
            fIgnoreExistingKey = false;
            if (rescan.compare("yes") == 0) {
                fRescan = true;
            } else if (rescan.compare("no") == 0) {
                fRescan = false;
            } else {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "rescan must be \"yes\", \"no\" or \"whenkeyisnew\"");
            }
        }
    }

    WalletRescanReserver reserver(pwallet);
    if (fRescan && !reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    UniValue result(UniValue::VOBJ);
    CBlockIndex* pindexRescan{nullptr};
    {
        LOCK2(cs_main, pwallet->cs_wallet);
        EnsureWalletIsUnlocked(pwallet);

        // Height to rescan from
        int nRescanHeight = 0;
        if (request.params.size() > 2)
            nRescanHeight = request.params[2].get_int();
        if (nRescanHeight < 0 || nRescanHeight > chainActive.Height()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Block height out of range");
        }

        std::string strSecret = request.params[0].get_str();
        auto spendingkey = KeyIO::DecodeSpendingKey(strSecret);
        if (!IsValidSpendingKey(spendingkey)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid spending key");
        }

        libzcash::SaplingExtendedSpendingKey saplingSpendingKey = *boost::get<libzcash::SaplingExtendedSpendingKey>(&spendingkey);
        result.pushKV("address", KeyIO::EncodePaymentAddress( saplingSpendingKey.DefaultAddress()));

        // Sapling support
        auto addResult = pwallet->GetSaplingScriptPubKeyMan()->AddSpendingKeyToWallet(Params().GetConsensus(), saplingSpendingKey, -1);
        if (addResult == KeyAlreadyExists && fIgnoreExistingKey) {
            return result;
        }
        pwallet->MarkDirty();
        if (addResult == KeyNotAdded) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding spending key to wallet");
        }
        pindexRescan = chainActive[nRescanHeight];
    }

    // We want to scan for transactions and notes
    if (fRescan) {
        pwallet->ScanForWalletTransactions(pindexRescan, nullptr, reserver, true);
    }

    return result;
}

UniValue importsaplingviewingkey(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3)
        throw std::runtime_error(
                "importsaplingviewingkey \"vkey\" ( rescan height )\n"
                "\nAdds a viewing key (as returned by exportsaplingviewingkey) to your wallet.\n"
                + HelpRequiringPassphrase(pwallet) + "\n"

                "\nArguments:\n"
                "1. \"vkey\"        (string, required) The viewing key (see exportsaplingviewingkey)\n"
                "2. rescan          (string, optional, default=\"whenkeyisnew\") Rescan the wallet for transactions - can be \"yes\", \"no\" or \"whenkeyisnew\"\n"
                "3. height          (numeric, optional, default=0) Block height to start rescan from\n"
                "\nNote: This call can take minutes to complete if rescan is true.\n"

                "\nResult:\n"
                "{\n"
                "  \"address\" : \"address|DefaultAddress\",    (string) The address corresponding to the viewing key (for Sapling, this is the default address).\n"
                "}\n"

                "\nExamples:\n"
                "\nImport a viewing key\n"
                + HelpExampleCli("importsaplingviewingkey", "\"vkey\"") +
                "\nImport the viewing key without rescan\n"
                + HelpExampleCli("importsaplingviewingkey", "\"vkey\", no") +
                "\nImport the viewing key with partial rescan\n"
                + HelpExampleCli("importsaplingviewingkey", "\"vkey\" whenkeyisnew 30000") +
                "\nRe-import the viewing key with longer partial rescan\n"
                + HelpExampleCli("importsaplingviewingkey", "\"vkey\" yes 20000") +
                "\nAs a JSON-RPC call\n"
                + HelpExampleRpc("importsaplingviewingkey", "\"vkey\", \"no\"")
        );

    // Whether to perform rescan after import
    bool fRescan = true;
    bool fIgnoreExistingKey = true;
    if (request.params.size() > 1) {
        auto rescan = request.params[1].get_str();
        if (rescan.compare("whenkeyisnew") != 0) {
            fIgnoreExistingKey = false;
            if (rescan.compare("no") == 0) {
                fRescan = false;
            } else if (rescan.compare("yes") != 0) {
                throw JSONRPCError(
                        RPC_INVALID_PARAMETER,
                        "rescan must be \"yes\", \"no\" or \"whenkeyisnew\"");
            }
        }
    }

    WalletRescanReserver reserver(pwallet);
    if (fRescan && !reserver.reserve()) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet is currently rescanning. Abort existing rescan or wait.");
    }

    UniValue result(UniValue::VOBJ);
    CBlockIndex* pindexRescan{nullptr};
    {
        LOCK2(cs_main, pwallet->cs_wallet);
        EnsureWalletIsUnlocked(pwallet);

        // Height to rescan from
        int nRescanHeight = 0;
        if (request.params.size() > 2) {
            nRescanHeight = request.params[2].get_int();
        }
        if (nRescanHeight < 0 || nRescanHeight > chainActive.Height()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Block height out of range");
        }

        std::string strVKey = request.params[0].get_str();
        libzcash::ViewingKey viewingkey = KeyIO::DecodeViewingKey(strVKey);
        if (!IsValidViewingKey(viewingkey)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid viewing key");
        }
        libzcash::SaplingExtendedFullViewingKey efvk = *boost::get<libzcash::SaplingExtendedFullViewingKey>(&viewingkey);
        result.pushKV("address", KeyIO::EncodePaymentAddress(efvk.DefaultAddress()));

        auto addResult = pwallet->GetSaplingScriptPubKeyMan()->AddViewingKeyToWallet(efvk);
        if (addResult == SpendingKeyExists) {
            throw JSONRPCError(
                    RPC_WALLET_ERROR,
                    "The wallet already contains the private key for this viewing key");
        } else if (addResult == KeyAlreadyExists && fIgnoreExistingKey) {
            return result;
        }
        pwallet->MarkDirty();
        if (addResult == KeyNotAdded) {
            throw JSONRPCError(RPC_WALLET_ERROR, "Error adding viewing key to wallet");
        }

        pindexRescan = chainActive[nRescanHeight];
    }

    // We want to scan for transactions and notes
    if (fRescan) {
        pwallet->ScanForWalletTransactions(pindexRescan, nullptr, reserver, true);
    }

    return result;
}

UniValue exportsaplingviewingkey(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
                "exportsaplingviewingkey \"shield_addr\"\n"
                "\nReveals the viewing key corresponding to 'shield_addr'.\n"
                "Then the importsaplingviewingkey can be used with this output\n"
                + HelpRequiringPassphrase(pwallet) + "\n"

                "\nArguments:\n"
                "1. \"shield_addr\"   (string, required) The shield addr for the viewing key\n"

                "\nResult:\n"
                "\"vkey\"                  (string) The viewing key\n"

                "\nExamples:\n"
                + HelpExampleCli("exportsaplingviewingkey", "\"myaddress\"")
                + HelpExampleRpc("exportsaplingviewingkey", "\"myaddress\"")
        );

    LOCK2(cs_main, pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    std::string strAddress = request.params[0].get_str();
    auto address = KeyIO::DecodePaymentAddress(strAddress);
    if (!IsValidPaymentAddress(address)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid shield addr");
    }
    const libzcash::SaplingPaymentAddress &sapAddr = *boost::get<libzcash::SaplingPaymentAddress>(&address);
    auto vk = pwallet->GetSaplingScriptPubKeyMan()->GetViewingKeyForPaymentAddress(sapAddr);
    if (vk) {
        return KeyIO::EncodeViewingKey(vk.get());
    } else {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet does not hold private key or viewing key for this shield addr");
    }
}

UniValue exportsaplingkey(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
                "exportsaplingkey \"shield_addr\"\n"
                "\nReveals the key corresponding to the 'shield_addr'.\n"
                "Then the importsaplingkey can be used with this output\n"
                + HelpRequiringPassphrase(pwallet) + "\n"

                "\nArguments:\n"
                "1. \"addr\"   (string, required) The shield addr for the private key\n"

                "\nResult:\n"
                "\"key\"                  (string) The private key\n"

                "\nExamples:\n"
                + HelpExampleCli("exportsaplingkey", "\"myaddress\"")
                + HelpExampleRpc("exportsaplingkey", "\"myaddress\"")
        );

    LOCK2(cs_main, pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    std::string strAddress = request.params[0].get_str();

    auto address = KeyIO::DecodePaymentAddress(strAddress);
    if (!IsValidPaymentAddress(address)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid shield addr");
    }
    libzcash::SaplingPaymentAddress addr = *boost::get<libzcash::SaplingPaymentAddress>(&address);

    // Sapling support
    Optional<libzcash::SaplingExtendedSpendingKey> sk = pwallet->GetSaplingScriptPubKeyMan()->GetSpendingKeyForPaymentAddress(addr);
    if (!sk) {
        throw JSONRPCError(RPC_WALLET_ERROR, "Wallet does not hold private key for this shield addr");
    }
    return KeyIO::EncodeSpendingKey(libzcash::SpendingKey(sk.get()));
}
