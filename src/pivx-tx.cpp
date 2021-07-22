// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "clientversion.h"
#include "coins.h"
#include "core_io.h"
#include "keystore.h"
#include "key_io.h"
#include "policy/policy.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "script/sign.h"
#include <univalue.h>
#include "util/system.h"
#include "utilmoneystr.h"
#include "utilstrencodings.h"

#include <stdio.h>

#include <boost/algorithm/string.hpp>

static bool fCreateBlank;
static std::map<std::string, UniValue> registers;

static bool AppInitRawTx(int argc, char* argv[])
{
    //
    // Parameters
    //
    gArgs.ParseParameters(argc, argv);

    // Check for -testnet or -regtest parameter (Params() calls are only valid after this clause)
    try {
        SelectParams(gArgs.GetChainName());
    } catch(const std::exception& e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return false;
    }

    fCreateBlank = gArgs.GetBoolArg("-create", false);

    if (argc < 2 || gArgs.IsArgSet("-?") || gArgs.IsArgSet("-h") || gArgs.IsArgSet("-help")) {
        // First part of help message is specific to this utility
        std::string strUsage = _("Pivx Core pivx-tx utility version") + " " + FormatFullVersion() + "\n\n" +
                               _("Usage:") + "\n" +
                               "  pivx-tx [options] <hex-tx> [commands]  " + _("Update hex-encoded pivx transaction") + "\n" +
                               "  pivx-tx [options] -create [commands]   " + _("Create hex-encoded pivx transaction") + "\n" +
                               "\n";

        fprintf(stdout, "%s", strUsage.c_str());

        strUsage = HelpMessageGroup(_("Options:"));
        strUsage += HelpMessageOpt("-?", _("This help message"));
        strUsage += HelpMessageOpt("-create", _("Create new, empty TX."));
        strUsage += HelpMessageOpt("-json", _("Select JSON output"));
        strUsage += HelpMessageOpt("-txid", _("Output only the hex-encoded transaction id of the resultant transaction."));
        strUsage += HelpMessageOpt("-regtest", _("Enter regression test mode, which uses a special chain in which blocks can be solved instantly."));
        strUsage += HelpMessageOpt("-testnet", _("Use the test network"));

        fprintf(stdout, "%s", strUsage.c_str());


        strUsage = HelpMessageGroup(_("Commands:"));
        strUsage += HelpMessageOpt("delin=N", _("Delete input N from TX"));
        strUsage += HelpMessageOpt("delout=N", _("Delete output N from TX"));
        strUsage += HelpMessageOpt("in=TXID:VOUT", _("Add input to TX"));
        strUsage += HelpMessageOpt("locktime=N", _("Set TX lock time to N"));
        strUsage += HelpMessageOpt("nversion=N", _("Set TX version to N"));
        strUsage += HelpMessageOpt("outaddr=VALUE:ADDRESS", _("Add address-based output to TX"));
        strUsage += HelpMessageOpt("outscript=VALUE:SCRIPT", _("Add raw script output to TX"));
        strUsage += HelpMessageOpt("sign=SIGHASH-FLAGS", _("Add zero or more signatures to transaction") + ". " +
            _("This command requires JSON registers:") +
            _("prevtxs=JSON object") + ", " +
            _("privatekeys=JSON object") + ". " +
            _("See signrawtransaction docs for format of sighash flags, JSON objects."));
        fprintf(stdout, "%s", strUsage.c_str());

        strUsage = HelpMessageGroup(_("Register Commands:"));
        strUsage += HelpMessageOpt("load=NAME:FILENAME", _("Load JSON file FILENAME into register NAME"));
        strUsage += HelpMessageOpt("set=NAME:JSON-STRING", _("Set register NAME to given JSON-STRING"));
        fprintf(stdout, "%s", strUsage.c_str());

        return false;
    }
    return true;
}

static void RegisterSetJson(const std::string& key, const std::string& rawJson)
{
    UniValue val;
    if (!val.read(rawJson)) {
        std::string strErr = "Cannot parse JSON for key " + key;
        throw std::runtime_error(strErr);
    }

    registers[key] = val;
}

static void RegisterSet(const std::string& strInput)
{
    // separate NAME:VALUE in string
    size_t pos = strInput.find(':');
    if ((pos == std::string::npos) ||
        (pos == 0) ||
        (pos == (strInput.size() - 1)))
        throw std::runtime_error("Register input requires NAME:VALUE");

    std::string key = strInput.substr(0, pos);
    std::string valStr = strInput.substr(pos + 1, std::string::npos);

    RegisterSetJson(key, valStr);
}

static void RegisterLoad(const std::string& strInput)
{
    // separate NAME:FILENAME in string
    size_t pos = strInput.find(':');
    if ((pos == std::string::npos) ||
        (pos == 0) ||
        (pos == (strInput.size() - 1)))
        throw std::runtime_error("Register load requires NAME:FILENAME");

    std::string key = strInput.substr(0, pos);
    std::string filename = strInput.substr(pos + 1, std::string::npos);

    FILE* f = fopen(filename.c_str(), "r");
    if (!f) {
        std::string strErr = "Cannot open file " + filename;
        throw std::runtime_error(strErr);
    }

    // load file chunks into one big buffer
    std::string valStr;
    while ((!feof(f)) && (!ferror(f))) {
        char buf[4096];
        int bread = fread(buf, 1, sizeof(buf), f);
        if (bread <= 0)
            break;

        valStr.insert(valStr.size(), buf, bread);
    }

    if (ferror(f)) {
        std::string strErr = "Error reading file " + filename;
        throw std::runtime_error(strErr);
    }

    fclose(f);

    // evaluate as JSON buffer register
    RegisterSetJson(key, valStr);
}

static CAmount ExtractAndValidateValue(const std::string& strValue)
{
    CAmount value;
    if (!ParseMoney(strValue, value))
        throw std::runtime_error("invalid TX output value");
    return value;
}

static void MutateTxVersion(CMutableTransaction& tx, const std::string& cmdVal)
{
    int64_t newVersion;
    if (!ParseInt64(cmdVal, &newVersion) || newVersion < 1 || newVersion >= CTransaction::TxVersion::TOOHIGH)
        throw std::runtime_error("Invalid TX version requested: '" + cmdVal + "'");

    tx.nVersion = (int)newVersion;
}

static void MutateTxLocktime(CMutableTransaction& tx, const std::string& cmdVal)
{
    int64_t newLocktime;
    if (!ParseInt64(cmdVal, &newLocktime) || newLocktime < 0LL || newLocktime > 0xffffffffLL)
        throw std::runtime_error("Invalid TX locktime requested: '" + cmdVal + "'");

    tx.nLockTime = (unsigned int)newLocktime;
}

static void MutateTxAddInput(CMutableTransaction& tx, const std::string& strInput)
{
    std::vector<std::string> vStrInputParts;
    boost::split(vStrInputParts, strInput, boost::is_any_of(":"));

    // separate TXID:VOUT in string
    if (vStrInputParts.size()<2)
        throw std::runtime_error("TX input missing separator");

    // extract and validate TXID
    uint256 txid;
    if (!ParseHashStr(vStrInputParts[0], txid)) {
        throw std::runtime_error("invalid TX input txid");
    }

    static const unsigned int minTxOutSz = 9;
    unsigned int nMaxSize = MAX_BLOCK_SIZE_LEGACY;
    static const unsigned int maxVout = nMaxSize / minTxOutSz;

    // extract and validate vout
    const std::string& strVout = vStrInputParts[1];
    int64_t vout;
    if (!ParseInt64(strVout, &vout) || vout < 0 || vout > static_cast<int64_t>(maxVout))
        throw std::runtime_error("invalid TX input vout '" + strVout + "'");

    // extract the optional sequence number
    uint32_t nSequenceIn = CTxIn::SEQUENCE_FINAL;
    if (vStrInputParts.size() > 2)
        nSequenceIn = std::stoul(vStrInputParts[2]);

    // append to transaction input list
    tx.vin.emplace_back(txid, vout, CScript(), nSequenceIn);
}

static void MutateTxAddOutAddr(CMutableTransaction& tx, const std::string& strInput)
{
    // Separate into VALUE:ADDRESS
    std::vector<std::string> vStrInputParts;
    boost::split(vStrInputParts, strInput, boost::is_any_of(":"));

    if (vStrInputParts.size() != 2)
        throw std::runtime_error("TX output missing or too many separators");

    // Extract and validate VALUE
    CAmount value = ExtractAndValidateValue(vStrInputParts[0]);

    // extract and validate ADDRESS
    std::string strAddr = vStrInputParts[1];
    CTxDestination destination = DecodeDestination(strAddr);
    if (!IsValidDestination(destination)) {
        throw std::runtime_error("invalid TX output address");
    }
    CScript scriptPubKey = GetScriptForDestination(destination);

    // construct TxOut, append to transaction output list
    tx.vout.emplace_back(value, scriptPubKey);
}

static void MutateTxAddOutPubKey(CMutableTransaction& tx, const std::string& strInput)
{
    // Separate into VALUE:PUBKEY[:FLAGS]
    std::vector<std::string> vStrInputParts;
    boost::split(vStrInputParts, strInput, boost::is_any_of(":"));

    if (vStrInputParts.size() < 2 || vStrInputParts.size() > 3)
        throw std::runtime_error("TX output missing or too many separators");

    // Extract and validate VALUE
    CAmount value = ExtractAndValidateValue(vStrInputParts[0]);

    // Extract and validate PUBKEY
    CPubKey pubkey(ParseHex(vStrInputParts[1]));
    if (!pubkey.IsFullyValid())
        throw std::runtime_error("invalid TX output pubkey");
    CScript scriptPubKey = GetScriptForRawPubKey(pubkey);

    // Extract and validate FLAGS
    bool bScriptHash = false;
    if (vStrInputParts.size() == 3) {
        std::string flags = vStrInputParts[2];
        bScriptHash = (flags.find('S') != std::string::npos);
    }
    if (bScriptHash) {
        // Get the ID for the script, and then construct a P2SH destination for it.
        scriptPubKey = GetScriptForDestination(CScriptID(scriptPubKey));
    }

    // construct TxOut, append to transaction output list
    tx.vout.emplace_back(value, scriptPubKey);
}

static void MutateTxAddOutMultiSig(CMutableTransaction& tx, const std::string& strInput)
{
    // Separate into VALUE:REQUIRED:NUMKEYS:PUBKEY1:PUBKEY2:....[:FLAGS]
    std::vector<std::string> vStrInputParts;
    boost::split(vStrInputParts, strInput, boost::is_any_of(":"));

    // Check that there are enough parameters
    if (vStrInputParts.size() < 3)
        throw std::runtime_error("Not enough multisig parameters");

    // Extract and validate VALUE
    CAmount value = ExtractAndValidateValue(vStrInputParts[0]);

    // Extract REQUIRED
    uint32_t required = stoul(vStrInputParts[1]);

    // Extract NUMKEYS
    uint32_t numkeys = stoul(vStrInputParts[2]);

    // Validate there are the correct number of pubkeys
    if (vStrInputParts.size() < numkeys + 3)
        throw std::runtime_error("incorrect number of multisig pubkeys");

    if (required < 1 || required > MAX_PUBKEYS_PER_MULTISIG || numkeys < 1 || numkeys > MAX_PUBKEYS_PER_MULTISIG || numkeys < required)
        throw std::runtime_error("multisig parameter mismatch. Required " \
                            + std::to_string(required) + " of " + std::to_string(numkeys) + "signatures.");

    // extract and validate PUBKEYs
    std::vector<CPubKey> pubkeys;
    for(int pos = 1; pos <= int(numkeys); pos++) {
        CPubKey pubkey(ParseHex(vStrInputParts[pos + 2]));
        if (!pubkey.IsFullyValid())
            throw std::runtime_error("invalid TX output pubkey");
        pubkeys.push_back(pubkey);
    }

    // Extract FLAGS
    bool bScriptHash = false;
    if (vStrInputParts.size() == numkeys + 4) {
        std::string flags = vStrInputParts.back();
        bScriptHash = (flags.find('S') != std::string::npos);
    }
    else if (vStrInputParts.size() > numkeys + 4) {
        // Validate that there were no more parameters passed
        throw std::runtime_error("Too many parameters");
    }

    CScript scriptPubKey = GetScriptForMultisig(required, pubkeys);

    if (bScriptHash) {
        if (scriptPubKey.size() > MAX_SCRIPT_ELEMENT_SIZE) {
            throw std::runtime_error(strprintf(
                        "redeemScript exceeds size limit: %d > %d", scriptPubKey.size(), MAX_SCRIPT_ELEMENT_SIZE));
        }
        // Get the ID for the script, and then construct a P2SH destination for it.
        scriptPubKey = GetScriptForDestination(CScriptID(scriptPubKey));
    }

    // construct TxOut, append to transaction output list
    tx.vout.emplace_back(value, scriptPubKey);
}

static void MutateTxAddOutScript(CMutableTransaction& tx, const std::string& strInput)
{
    // separate VALUE:SCRIPT
    std::vector<std::string> vStrInputParts;
    boost::split(vStrInputParts, strInput, boost::is_any_of(":"));
    if (vStrInputParts.size() < 2)
        throw std::runtime_error("TX output missing separator");

    // Extract and validate VALUE
    CAmount value = ExtractAndValidateValue(vStrInputParts[0]);

    // extract and validate script
    std::string strScript = vStrInputParts[1];
    CScript scriptPubKey = ParseScript(strScript);

    if (scriptPubKey.size() > MAX_SCRIPT_SIZE) {
        throw std::runtime_error(strprintf(
                    "script exceeds size limit: %d > %d", scriptPubKey.size(), MAX_SCRIPT_SIZE));
    }

    // construct TxOut, append to transaction output list
    tx.vout.emplace_back(value, scriptPubKey);
}

static void MutateTxDelInput(CMutableTransaction& tx, const std::string& strInIdx)
{
    // parse requested deletion index
    int inIdx = atoi(strInIdx);
    if (inIdx < 0 || inIdx >= (int)tx.vin.size()) {
        std::string strErr = "Invalid TX input index '" + strInIdx + "'";
        throw std::runtime_error(strErr.c_str());
    }

    // delete input from transaction
    tx.vin.erase(tx.vin.begin() + inIdx);
}

static void MutateTxDelOutput(CMutableTransaction& tx, const std::string& strOutIdx)
{
    // parse requested deletion index
    int outIdx = atoi(strOutIdx);
    if (outIdx < 0 || outIdx >= (int)tx.vout.size()) {
        std::string strErr = "Invalid TX output index '" + strOutIdx + "'";
        throw std::runtime_error(strErr.c_str());
    }

    // delete output from transaction
    tx.vout.erase(tx.vout.begin() + outIdx);
}

static const unsigned int N_SIGHASH_OPTS = 6;
static const struct {
    const char* flagStr;
    int flags;
} sighashOptions[N_SIGHASH_OPTS] = {
    {"ALL", SIGHASH_ALL},
    {"NONE", SIGHASH_NONE},
    {"SINGLE", SIGHASH_SINGLE},
    {"ALL|ANYONECANPAY", SIGHASH_ALL | SIGHASH_ANYONECANPAY},
    {"NONE|ANYONECANPAY", SIGHASH_NONE | SIGHASH_ANYONECANPAY},
    {"SINGLE|ANYONECANPAY", SIGHASH_SINGLE | SIGHASH_ANYONECANPAY},
};

static bool findSighashFlags(int& flags, const std::string& flagStr)
{
    flags = 0;

    for (unsigned int i = 0; i < N_SIGHASH_OPTS; i++) {
        if (flagStr == sighashOptions[i].flagStr) {
            flags = sighashOptions[i].flags;
            return true;
        }
    }

    return false;
}

uint256 ParseHashUO(std::map<std::string, UniValue>& o, std::string strKey)
{
    if (!o.count(strKey))
        return UINT256_ZERO;
    return ParseHashUV(o[strKey], strKey);
}

static inline int64_t roundint64(double d)
{
    return (int64_t)(d > 0 ? d + 0.5 : d - 0.5);
}

static CAmount AmountFromValue(const UniValue& value)
{
    if (!value.isNum() && !value.isStr())
        throw std::runtime_error("Amount is not a number or string");
    double dAmount = value.get_real();
    if (dAmount <= 0.0 || dAmount > 21000000.0)
        throw std::runtime_error("Invalid amount");
    CAmount nAmount = roundint64(dAmount * COIN);
    if (!Params().GetConsensus().MoneyRange(nAmount))
        throw std::runtime_error("Amount out of range");
    return nAmount;
}

std::vector<unsigned char> ParseHexUO(std::map<std::string, UniValue>& o, std::string strKey)
{
    if (!o.count(strKey)) {
        std::vector<unsigned char> emptyVec;
        return emptyVec;
    }
    return ParseHexUV(o[strKey], strKey);
}

static void MutateTxSign(CMutableTransaction& tx, const std::string& flagStr)
{
    int nHashType = SIGHASH_ALL;

    if (flagStr.size() > 0)
        if (!findSighashFlags(nHashType, flagStr))
            throw std::runtime_error("unknown sighash flag/sign option");

    std::vector<CTransaction> txVariants;
    txVariants.push_back(tx);

    // mergedTx will end up with all the signatures; it
    // starts as a clone of the raw tx:
    CMutableTransaction mergedTx(txVariants[0]);
    bool fComplete = true;
    CCoinsView viewDummy;
    CCoinsViewCache view(&viewDummy);

    if (!registers.count("privatekeys"))
        throw std::runtime_error("privatekeys register variable must be set.");
    bool fGivenKeys = false;
    CBasicKeyStore tempKeystore;
    UniValue keysObj = registers["privatekeys"];
    fGivenKeys = true;

    for (unsigned int kidx = 0; kidx < keysObj.size(); kidx++) {
        if (!keysObj[kidx].isStr())
            throw std::runtime_error("privatekey not a string");
        CKey key = KeyIO::DecodeSecret(keysObj[kidx].getValStr());
        if (!key.IsValid()) {
            throw std::runtime_error("privatekey not valid");
        }
        tempKeystore.AddKey(key);
    }

    // Add previous txouts given in the RPC call:
    if (!registers.count("prevtxs"))
        throw std::runtime_error("prevtxs register variable must be set.");
    UniValue prevtxsObj = registers["prevtxs"];
    {
        for (unsigned int previdx = 0; previdx < prevtxsObj.size(); previdx++) {
            UniValue prevOut = prevtxsObj[previdx];
            if (!prevOut.isObject())
                throw std::runtime_error("expected prevtxs internal object");

            std::map<std::string, UniValue::VType> types = {
                {"txid", UniValue::VSTR},
                {"vout", UniValue::VNUM},
                {"scriptPubKey", UniValue::VSTR}
            };
            if (!prevOut.checkObject(types))
                throw std::runtime_error("prevtxs internal object typecheck fail");

            uint256 txid = ParseHashUV(prevOut["txid"], "txid");

            int nOut = atoi(prevOut["vout"].getValStr());
            if (nOut < 0)
                throw std::runtime_error("vout must be positive");

            COutPoint out(txid, nOut);
            std::vector<unsigned char> pkData(ParseHexUV(prevOut["scriptPubKey"], "scriptPubKey"));
            CScript scriptPubKey(pkData.begin(), pkData.end());

            {
                const Coin& coin = view.AccessCoin(out);
                if (!coin.IsSpent() && coin.out.scriptPubKey != scriptPubKey) {
                    std::string err("Previous output scriptPubKey mismatch:\n");
                    err = err + ScriptToAsmStr(coin.out.scriptPubKey) + "\nvs:\n"+
                        ScriptToAsmStr(scriptPubKey);
                    throw std::runtime_error(err);
                }

                Coin newcoin;
                newcoin.out.scriptPubKey = scriptPubKey;
                newcoin.out.nValue = 0;
                newcoin.nHeight = 1;
                if (prevOut.exists("amount")) {
                    newcoin.out.nValue = AmountFromValue(prevOut["amount"]);
                }
            }

            // if redeemScript given and private keys given,
            // add redeemScript to the tempKeystore so it can be signed:
            if (fGivenKeys && scriptPubKey.IsPayToScriptHash() &&
                prevOut.exists("redeemScript")) {
                UniValue v = prevOut["redeemScript"];
                std::vector<unsigned char> rsData(ParseHexUV(v, "redeemScript"));
                CScript redeemScript(rsData.begin(), rsData.end());
                tempKeystore.AddCScript(redeemScript);
            }
        }
    }

    const CKeyStore& keystore = tempKeystore;

    bool fHashSingle = ((nHashType & ~SIGHASH_ANYONECANPAY) == SIGHASH_SINGLE);

    // Sign what we can:
    for (unsigned int i = 0; i < mergedTx.vin.size(); i++) {
        CTxIn& txin = mergedTx.vin[i];
        const Coin& coin = view.AccessCoin(txin.prevout);
        if (coin.IsSpent()) {
            fComplete = false;
            continue;
        }

        const CScript& prevPubKey = coin.out.scriptPubKey;
        const CAmount& amount = coin.out.nValue;

        SignatureData sigdata;
        SigVersion sigversion =  mergedTx.GetRequiredSigVersion();
        // Only sign SIGHASH_SINGLE if there's a corresponding output:
        if (!fHashSingle || (i < mergedTx.vout.size()))
            ProduceSignature(
                    MutableTransactionSignatureCreator(&keystore, &mergedTx, i, amount, nHashType),
                    prevPubKey,
                    sigdata,
                    sigversion,
                    false // no cold stake
            );

        // ... and merge in other signatures:
        for (const CTransaction& txv : txVariants) {
            sigdata = CombineSignatures(prevPubKey, MutableTransactionSignatureChecker(&mergedTx, i, amount), sigdata, DataFromTransaction(txv, i));
        }
        UpdateTransaction(mergedTx, i, sigdata);
        if (!VerifyScript(txin.scriptSig, prevPubKey, STANDARD_SCRIPT_VERIFY_FLAGS,
                MutableTransactionSignatureChecker(&mergedTx, i, amount), sigversion))
            fComplete = false;
    }

    if (fComplete) {
        // do nothing... for now
        // perhaps store this for later optional JSON output
    }

    tx = mergedTx;
}

class Secp256k1Init
{
    ECCVerifyHandle globalVerifyHandle;

public:
    Secp256k1Init() {
        ECC_Start();
    }
    ~Secp256k1Init() {
        ECC_Stop();
    }
};

static void MutateTx(CMutableTransaction& tx, const std::string& command, const std::string& commandVal)
{
    std::unique_ptr<Secp256k1Init> ecc;
    if (command == "nversion") {
        MutateTxVersion(tx, commandVal);
    } else if (command == "locktime") {
        MutateTxLocktime(tx, commandVal);
    } else if (command == "delin") {
        MutateTxDelInput(tx, commandVal);
    } else if (command == "in") {
        MutateTxAddInput(tx, commandVal);
    } else if (command == "delout") {
        MutateTxDelOutput(tx, commandVal);
    } else if (command == "outaddr") {
        MutateTxAddOutAddr(tx, commandVal);
    } else if (command == "outpubkey") {
        ecc.reset(new Secp256k1Init());
        MutateTxAddOutPubKey(tx, commandVal);
    } else if (command == "outmultisig") {
        ecc.reset(new Secp256k1Init());
        MutateTxAddOutMultiSig(tx, commandVal);
    } else if (command == "outscript") {
        MutateTxAddOutScript(tx, commandVal);
    } else if (command == "sign") {
        ecc.reset(new Secp256k1Init());
        MutateTxSign(tx, commandVal);
    } else if (command == "load") {
        RegisterLoad(commandVal);
    } else if (command == "set") {
        RegisterSet(commandVal);
    } else {
        throw std::runtime_error("unknown command");
    }
}

static void OutputTxJSON(const CTransaction& tx)
{
    UniValue entry(UniValue::VOBJ);
    TxToUniv(tx, UINT256_ZERO, entry);

    std::string jsonOutput = entry.write(4);
    fprintf(stdout, "%s\n", jsonOutput.c_str());
}

static void OutputTxHash(const CTransaction& tx)
{
    std::string strHexHash = tx.GetHash().GetHex(); // the hex-encoded transaction hash (aka the transaction id)

    fprintf(stdout, "%s\n", strHexHash.c_str());
}

static void OutputTxHex(const CTransaction& tx)
{
    std::string strHex = EncodeHexTx(tx);

    fprintf(stdout, "%s\n", strHex.c_str());
}

static void OutputTx(const CTransaction& tx)
{
    if (gArgs.GetBoolArg("-json", false))
        OutputTxJSON(tx);
    else if (gArgs.GetBoolArg("-txid", false))
        OutputTxHash(tx);
    else
        OutputTxHex(tx);
}

static std::string readStdin()
{
    char buf[4096];
    std::string ret;

    while (!feof(stdin)) {
        size_t bread = fread(buf, 1, sizeof(buf), stdin);
        ret.append(buf, bread);
        if (bread < sizeof(buf))
            break;
    }

    if (ferror(stdin))
        throw std::runtime_error("error reading stdin");

    boost::algorithm::trim_right(ret);

    return ret;
}

static int CommandLineRawTx(int argc, char* argv[])
{
    std::string strPrint;
    int nRet = 0;
    try {
        // Skip switches; Permit common stdin convention "-"
        while (argc > 1 && IsSwitchChar(argv[1][0]) &&
               (argv[1][1] != 0)) {
            argc--;
            argv++;
        }

        CMutableTransaction tx;
        int startArg;

        if (!fCreateBlank) {
            // require at least one param
            if (argc < 2)
                throw std::runtime_error("too few parameters");

            // param: hex-encoded pivx transaction
            std::string strHexTx(argv[1]);
            if (strHexTx == "-") // "-" implies standard input
                strHexTx = readStdin();

            if (!DecodeHexTx(tx, strHexTx))
                throw std::runtime_error("invalid transaction encoding");

            startArg = 2;
        } else
            startArg = 1;

        for (int i = startArg; i < argc; i++) {
            std::string arg = argv[i];
            std::string key, value;
            size_t eqpos = arg.find('=');
            if (eqpos == std::string::npos)
                key = arg;
            else {
                key = arg.substr(0, eqpos);
                value = arg.substr(eqpos + 1);
            }

            MutateTx(tx, key, value);
        }

        OutputTx(tx);
    }

    catch (const boost::thread_interrupted&) {
        throw;
    } catch (const std::exception& e) {
        strPrint = std::string("error: ") + e.what();
        nRet = EXIT_FAILURE;
    } catch (...) {
        PrintExceptionContinue(NULL, "CommandLineRawTx()");
        throw;
    }

    if (strPrint != "") {
        fprintf((nRet == 0 ? stdout : stderr), "%s\n", strPrint.c_str());
    }
    return nRet;
}

int main(int argc, char* argv[])
{
    SetupEnvironment();

    try {
        if (!AppInitRawTx(argc, argv))
            return EXIT_FAILURE;
    } catch (const std::exception& e) {
        PrintExceptionContinue(&e, "AppInitRawTx()");
        return EXIT_FAILURE;
    } catch (...) {
        PrintExceptionContinue(NULL, "AppInitRawTx()");
        return EXIT_FAILURE;
    }

    int ret = EXIT_FAILURE;
    try {
        ret = CommandLineRawTx(argc, argv);
    } catch (const std::exception& e) {
        PrintExceptionContinue(&e, "CommandLineRawTx()");
    } catch (...) {
        PrintExceptionContinue(NULL, "CommandLineRawTx()");
    }
    return ret;
}
