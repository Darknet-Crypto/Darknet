// Copyright (c) 2018-2021 The Dash Core developers
// Copyright (c) 2021 The PIVX Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "core_io.h"
#include "destination_io.h"
#include "evo/deterministicmns.h"
#include "evo/specialtx.h"
#include "evo/providertx.h"
#include "key_io.h"
#include "masternode.h"
#include "messagesigner.h"
#include "netbase.h"
#include "operationresult.h"
#include "policy/policy.h"
#include "pubkey.h" // COMPACT_SIGNATURE_SIZE
#include "rpc/server.h"
#include "script/sign.h"
#include "utilmoneystr.h"
#include "validation.h"

#ifdef ENABLE_WALLET
#include "coincontrol.h"
#include "wallet/wallet.h"
#include "wallet/rpcwallet.h"

extern void TryATMP(const CMutableTransaction& mtx, bool fOverrideFees);
extern void RelayTx(const uint256& hashTx);
#endif//ENABLE_WALLET

enum ProRegParam {
    collateralAddress,
    collateralHash,
    collateralIndex,
    ipAndPort_register,
    ipAndPort_update,
    operatorAddress_register,
    operatorAddress_update,
    operatorPayoutAddress_register,
    operatorPayoutAddress_update,
    operatorReward,
    operatorKey,
    ownerAddress,
    ownerKey,
    proTxHash,
    payoutAddress_register,
    payoutAddress_update,
    revocationReason,
    votingAddress_register,
    votingAddress_update,
};

static const std::map<ProRegParam, std::string> mapParamHelp = {
        {collateralAddress,
            "%d. \"collateralAddress\"     (string, required) The PIVX address to send the collateral to.\n"
        },
        {collateralHash,
            "%d. \"collateralHash\"        (string, required) The collateral transaction hash.\n"
        },
        {collateralIndex,
            "%d. collateralIndex           (numeric, required) The collateral transaction output index.\n"
        },
        {ipAndPort_register,
            "%d. \"ipAndPort\"             (string, required) IP and port in the form \"IP:PORT\".\n"
            "                                Must be unique on the network. Can be set to 0, which will require a ProUpServTx afterwards.\n"
        },
        {ipAndPort_update,
            "%d. \"ipAndPort\"             (string, required) IP and port in the form \"IP:PORT\".\n"
            "                                If set to an empty string, the currently active ip is reused.\n"
        },
        {operatorAddress_register,
            "%d. \"operatorAddress\"       (string, required) The operator key address. The private key does not have to be known by your wallet.\n"
            "                                If set to an empty string, ownerAddr will be used.\n"
        },
        {operatorAddress_update,
            "%d. \"operatorAddress\"       (string, required) The operator key address. The private key does not have to be known by your wallet.\n"
            "                                 It has to match the private key which can be used later to update the service.\n"
            "                                 If set to an empty string, the currently active operator key is reused.\n"
        },
        {operatorKey,
            "%d. \"operatorKey\"           (string, optional) The operator key associated with the operator address of the masternode.\n"
            "                                If not specified, or set to an empty string, then the mn key must be known by your wallet, in order to sign the tx.\n"
        },
        {operatorPayoutAddress_register,
            "%d. \"operatorPayoutAddress\" (string, optional) The address used for operator reward payments.\n"
            "                                Only allowed when the ProRegTx had a non-zero operatorReward value.\n"
            "                                If set to an empty string, the operatorAddress is used.\n"
        },
        {operatorPayoutAddress_update,
            "%d. \"operatorPayoutAddress\" (string, optional) The address used for operator reward payments.\n"
            "                                Only allowed when the ProRegTx had a non-zero operatorReward value.\n"
            "                                If set to an empty string, the currently active one is reused.\n"
        },
        {operatorReward,
            "%d. \"operatorReward\"        (numeric, optional) The fraction in %% to share with the operator. The value must be\n"
            "                                between 0.00 and 100.00. If not set, it takes the default value of 0.0\n"
        },
        {ownerAddress,
            "%d. \"ownerAddress\"          (string, required) The PIVX address to use for payee updates and proposal voting.\n"
            "                                The private key belonging to this address must be known in your wallet, in order to send updates.\n"
            "                                The address must not be already registered, and must differ from the collateralAddress\n"
        },
        {ownerKey,
            "%d. \"ownerKey\"              (string, optional) The owner key associated with the operator address of the masternode.\n"
            "                                If not specified, or set to an empty string, then the mn key must be known by your wallet, in order to sign the tx.\n"
        },
        {payoutAddress_register,
            "%d. \"payoutAddress\"          (string, required) The PIVX address to use for masternode reward payments.\n"
        },
        {payoutAddress_update,
            "%d. \"payoutAddress\"          (string, required) The PIVX address to use for masternode reward payments.\n"
            "                                 If set to an empty string, the currently active payout address is reused.\n"
        },
        {proTxHash,
            "%d. \"proTxHash\"              (string, required) The hash of the initial ProRegTx.\n"
        },
        {revocationReason,
            "%d. reason                     (numeric, optional) The reason for masternode service revocation. Default: 0.\n"
            "                                 0=not_specified, 1=service_termination, 2=compromised_keys, 3=keys_change.\n"
        },
        {votingAddress_register,
            "%d. \"votingAddress\"          (string, required) The voting key address. The private key does not have to be known by your wallet.\n"
            "                                 It has to match the private key which is later used when voting on proposals.\n"
            "                                 If set to an empty string, ownerAddress will be used.\n"
        },
        {votingAddress_update,
            "%d. \"votingAddress\"          (string, required) The voting key address. The private key does not have to be known by your wallet.\n"
            "                                 It has to match the private key which is later used when voting on proposals.\n"
            "                                 If set to an empty string, the currently active voting key address is reused.\n"
        },
    };

std::string GetHelpString(int nParamNum, ProRegParam p)
{
    auto it = mapParamHelp.find(p);
    if (it == mapParamHelp.end())
        throw std::runtime_error(strprintf("FIXME: WRONG PARAM: %d!", (int)p));

    return strprintf(it->second, nParamNum);
}

#ifdef ENABLE_WALLET
static CKey GetKeyFromWallet(CWallet* pwallet, const CKeyID& keyID)
{
    assert(pwallet);
    CKey key;
    if (!pwallet->GetKey(keyID, key)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                           strprintf("key for address %s not in wallet", EncodeDestination(keyID)));
    }
    return key;
}
#endif

static void CheckEvoUpgradeEnforcement()
{
    const int nHeight = WITH_LOCK(cs_main, return chainActive.Height(); );
    if (!Params().GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_V6_0)) {
        throw JSONRPCError(RPC_MISC_ERROR, "Evo upgrade is not active yet");
    }
}

// Allows to specify PIVX address or priv key (as strings). In case of PIVX address, the priv key is taken from the wallet
static CKey ParsePrivKey(CWallet* pwallet, const std::string &strKeyOrAddress, bool allowAddresses = true) {
    bool isStaking{false}, isShield{false};
    const CWDestination& cwdest = Standard::DecodeDestination(strKeyOrAddress, isStaking, isShield);
    if (isStaking) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "cold staking addresses not supported");
    }
    if (isShield) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "shield addresses not supported");
    }
    const CTxDestination* dest = Standard::GetTransparentDestination(cwdest);
    if (allowAddresses && IsValidDestination(*dest)) {
#ifdef ENABLE_WALLET
        if (!pwallet) {
            throw std::runtime_error("addresses not supported when wallet is disabled");
        }
        EnsureWalletIsUnlocked(pwallet);
        const CKeyID* keyID = boost::get<CKeyID>(dest);
        assert (keyID != nullptr);  // we just checked IsValidDestination
        return GetKeyFromWallet(pwallet, *keyID);
#else   // ENABLE_WALLET
        throw std::runtime_error("addresses not supported in no-wallet builds");
#endif  // ENABLE_WALLET
    }

    CKey key = KeyIO::DecodeSecret(strKeyOrAddress);
    if (!key.IsValid()) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid private key encoding");
    return key;
}

static CKeyID ParsePubKeyIDFromAddress(const std::string& strAddress)
{
    bool isStaking{false}, isShield{false};
    const CWDestination& cwdest = Standard::DecodeDestination(strAddress, isStaking, isShield);
    if (isStaking) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "cold staking addresses not supported");
    }
    if (isShield) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "shield addresses not supported");
    }
    const CKeyID* keyID = boost::get<CKeyID>(Standard::GetTransparentDestination(cwdest));
    if (!keyID) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("invalid PIVX address %s", strAddress));
    }
    return *keyID;
}

#ifdef ENABLE_WALLET

template<typename SpecialTxPayload>
static void FundSpecialTx(CWallet* pwallet, CMutableTransaction& tx, SpecialTxPayload& payload)
{
    SetTxPayload(tx, payload);

    static CTxOut dummyTxOut(0, CScript() << OP_RETURN);
    std::vector<CRecipient> vecSend;
    bool dummyTxOutAdded = false;

    if (tx.vout.empty()) {
        // add dummy txout as CreateTransaction requires at least one recipient
        tx.vout.emplace_back(dummyTxOut);
        dummyTxOutAdded = true;
    }

    CAmount nFee;
    CFeeRate feeRate = CFeeRate(0);
    int nChangePos = -1;
    std::string strFailReason;
    std::set<int> setSubtractFeeFromOutputs;
    if (!pwallet->FundTransaction(tx, nFee, false, feeRate, nChangePos, strFailReason, false, false, {}))
        throw JSONRPCError(RPC_INTERNAL_ERROR, strFailReason);

    if (dummyTxOutAdded && tx.vout.size() > 1) {
        // FundTransaction added a change output, so we don't need the dummy txout anymore
        // Removing it results in slight overpayment of fees, but we ignore this for now (as it's a very low amount)
        auto it = std::find(tx.vout.begin(), tx.vout.end(), dummyTxOut);
        assert(it != tx.vout.end());
        tx.vout.erase(it);
    }

    UpdateSpecialTxInputsHash(tx, payload);
}

template<typename SpecialTxPayload>
static void UpdateSpecialTxInputsHash(const CMutableTransaction& tx, SpecialTxPayload& payload)
{
    payload.inputsHash = CalcTxInputsHash(tx);
}

template<typename SpecialTxPayload>
static void SignSpecialTxPayloadByHash(const CMutableTransaction& tx, SpecialTxPayload& payload, const CKey& key)
{
    payload.vchSig.clear();

    uint256 hash = ::SerializeHash(payload);
    if (!CHashSigner::SignHash(hash, key, payload.vchSig)) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "failed to sign special tx payload");
    }
}

template<typename SpecialTxPayload>
static void SignSpecialTxPayloadByString(SpecialTxPayload& payload, const CKey& key)
{
    payload.vchSig.clear();

    std::string m = payload.MakeSignString();
    if (!CMessageSigner::SignMessage(m, payload.vchSig, key)) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "failed to sign special tx payload");
    }
}

static std::string TxInErrorToString(int i, const CTxIn& txin, const std::string& strError)
{
    return strprintf("Input %d (%s): %s", i, txin.prevout.ToStringShort(), strError);
}

static OperationResult SignTransaction(CWallet* const pwallet, CMutableTransaction& tx)
{
    LOCK2(cs_main, pwallet->cs_wallet);
    const CTransaction txConst(tx);
    for (unsigned int i = 0; i < tx.vin.size(); i++) {
        CTxIn& txin = tx.vin[i];
        const Coin& coin = pcoinsTip->AccessCoin(txin.prevout);
        if (coin.IsSpent()) {
            return errorOut(TxInErrorToString(i, txin, "not found or already spent"));
        }
        SigVersion sv = tx.GetRequiredSigVersion();
        txin.scriptSig.clear();
        SignatureData sigdata;
        if (!ProduceSignature(MutableTransactionSignatureCreator(pwallet, &tx, i, coin.out.nValue, SIGHASH_ALL),
                              coin.out.scriptPubKey, sigdata, sv, false)) {
            return errorOut(TxInErrorToString(i, txin, "signature failed"));
        }
        UpdateTransaction(tx, i, sigdata);
    }
    return OperationResult(true);
}

template<typename SpecialTxPayload>
static std::string SignAndSendSpecialTx(CWallet* const pwallet, CMutableTransaction& tx, const SpecialTxPayload& pl)
{
    SetTxPayload(tx, pl);

    CValidationState state;
    if (!CheckSpecialTx(tx, GetChainTip(), state)) {
        throw JSONRPCError(RPC_MISC_ERROR, FormatStateMessage(state));
    }

    const OperationResult& sigRes = SignTransaction(pwallet, tx);
    if (!sigRes) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, sigRes.getError());
    }

    TryATMP(tx, false);
    const uint256& hashTx = tx.GetHash();
    RelayTx(hashTx);
    return hashTx.GetHex();
}

// Parses inputs (starting from index paramIdx) and returns ProReg payload
static ProRegPL ParseProRegPLParams(const UniValue& params, unsigned int paramIdx)
{
    assert(params.size() > paramIdx + 4);
    assert(params.size() < paramIdx + 8);
    ProRegPL pl;

    // ip and port
    const std::string& strIpPort = params[paramIdx].get_str();
    if (!strIpPort.empty()) {
        if (!Lookup(strIpPort, pl.addr, Params().GetDefaultPort(), false)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("invalid network address %s", strIpPort));
        }
    }

    // addresses/keys
    const std::string& strAddOwner = params[paramIdx + 1].get_str();
    const std::string& strAddOperator = params[paramIdx + 2].get_str();
    const std::string& strAddVoting = params[paramIdx + 3].get_str();
    pl.keyIDOwner = ParsePubKeyIDFromAddress(strAddOwner);
    pl.keyIDOperator = pl.keyIDOwner;
    if (!strAddOperator.empty()) {
        pl.keyIDOperator = ParsePubKeyIDFromAddress(strAddOperator);
    }
    pl.keyIDVoting = pl.keyIDOwner;
    if (!strAddVoting.empty()) {
        pl.keyIDVoting = ParsePubKeyIDFromAddress(strAddVoting);
    }

    // payout script (!TODO: add support for P2CS)
    const std::string& strAddPayee = params[paramIdx + 4].get_str();
    pl.scriptPayout = GetScriptForDestination(CTxDestination(ParsePubKeyIDFromAddress(strAddPayee)));

    // operator reward
    pl.nOperatorReward = 0;
    if (params.size() > paramIdx + 5) {
        int64_t operReward = 0;
        if (!ParseFixedPoint(params[paramIdx + 5].getValStr(), 2, &operReward)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "operatorReward must be a number");
        }
        if (operReward < 0 || operReward > 10000) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "operatorReward must be between 0.00 and 100.00");
        }
        pl.nOperatorReward = (uint16_t)operReward;
        if (params.size() > paramIdx + 6) {
            // operator reward payout script
            const std::string& strAddOpPayee = params[paramIdx + 6].get_str();
            if (pl.nOperatorReward > 0 && !strAddOpPayee.empty()) {
                pl.scriptOperatorPayout = GetScriptForDestination(CTxDestination(ParsePubKeyIDFromAddress(strAddOpPayee)));
            } else if (!strAddOpPayee.empty()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "operatorPayoutAddress must be empty when operatorReward is 0");
            }
        }
    }
    return pl;
}

// handles protx_register, and protx_register_prepare
static UniValue ProTxRegister(const JSONRPCRequest& request, bool fSignAndSend)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 7 || request.params.size() > 9) {
        throw std::runtime_error(
                (fSignAndSend ?
                    "protx_register \"collateralHash\" collateralIndex \"ipAndPort\" \"ownerAddress\" \"operatorAddress\" \"votingAddress\" \"payoutAddress\" (operatorReward \"operatorPayoutAddress\")\n"
                    "The collateral is specified through \"collateralHash\" and \"collateralIndex\" and must be an unspent\n"
                    "transaction output spendable by this wallet. It must also not be used by any other masternode.\n"
                        :
                    "protx_register_prepare \"collateralHash\" collateralIndex \"ipAndPort\" \"ownerAddress\" \"operatorAddress\" \"votingAddress\" \"payoutAddress\" (operatorReward \"operatorPayoutAddress\")\n"
                    "\nCreates an unsigned ProTx and returns it. The ProTx must be signed externally with the collateral\n"
                    "key and then passed to \"protx_register_submit\".\n"
                    "The collateral is specified through \"collateralHash\" and \"collateralIndex\" and must be an unspent transaction output.\n"
                )
                + HelpRequiringPassphrase(pwallet) + "\n"
                "\nArguments:\n"
                + GetHelpString(1, collateralHash)
                + GetHelpString(2, collateralIndex)
                + GetHelpString(3, ipAndPort_register)
                + GetHelpString(4, ownerAddress)
                + GetHelpString(5, operatorAddress_register)
                + GetHelpString(6, votingAddress_register)
                + GetHelpString(7, payoutAddress_register)
                + GetHelpString(8, operatorReward)
                + GetHelpString(9, operatorPayoutAddress_register) +
                "\nResult:\n" +
                (fSignAndSend ? (
                        "\"txid\"                 (string) The transaction id.\n"
                        "\nExamples:\n"
                        + HelpExampleCli("protx_register", "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\" 0 \"168.192.1.100:51472\" \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\"")
                        ) : (
                        "{                        (json object)\n"
                        "  \"tx\" :                 (string) The serialized ProTx in hex format.\n"
                        "  \"collateralAddress\" :  (string) The collateral address.\n"
                        "  \"signMessage\" :        (string) The string message that needs to be signed with the collateral key\n"
                        "}\n"
                        "\nExamples:\n"
                        + HelpExampleCli("protx_register_prepare", "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\" 0 \"168.192.1.100:51472\" \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\"")
                        )
                )
        );
    }
    if (fSignAndSend) CheckEvoUpgradeEnforcement();

    EnsureWalletIsUnlocked(pwallet);
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    const uint256& collateralHash = ParseHashV(request.params[0], "collateralHash");
    const int32_t collateralIndex = request.params[1].get_int();
    if (collateralIndex < 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("invalid collateral index (negative): %d", collateralIndex));
    }

    ProRegPL pl = ParseProRegPLParams(request.params, 2);
    pl.nVersion = ProRegPL::CURRENT_VERSION;
    pl.collateralOutpoint = COutPoint(collateralHash, (uint32_t)collateralIndex);

    CMutableTransaction tx;
    tx.nVersion = CTransaction::TxVersion::SAPLING;
    tx.nType = CTransaction::TxType::PROREG;

    // referencing unspent collateral outpoint
    Coin coin;
    {
        LOCK(cs_main);
        if (!GetUTXOCoin(pl.collateralOutpoint, coin)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("collateral not found: %s-%d", collateralHash.ToString(), collateralIndex));
        }
    }
    if (coin.out.nValue != Params().GetConsensus().nMNCollateralAmt) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("collateral %s-%d with invalid value %d", collateralHash.ToString(), collateralIndex, coin.out.nValue));
    }
    CTxDestination txDest;
    ExtractDestination(coin.out.scriptPubKey, txDest);
    const CKeyID* keyID = boost::get<CKeyID>(&txDest);
    if (!keyID) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("collateral type not supported: %s-%d", collateralHash.ToString(), collateralIndex));
    }
    CKey keyCollateral;
    if (fSignAndSend && !pwallet->GetKey(*keyID, keyCollateral)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("collateral key not in wallet: %s", EncodeDestination(txDest)));
    }

    // make sure fee calculation works
    pl.vchSig.resize(CPubKey::COMPACT_SIGNATURE_SIZE);

    FundSpecialTx(pwallet, tx, pl);

    if (fSignAndSend) {
        SignSpecialTxPayloadByString(pl, keyCollateral); // prove we own the collateral
        // check the payload, add the tx inputs sigs, and send the tx.
        return SignAndSendSpecialTx(pwallet, tx, pl);
    }
    // external signing with collateral key
    pl.vchSig.clear();
    SetTxPayload(tx, pl);
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("tx", EncodeHexTx(tx));
    ret.pushKV("collateralAddress", EncodeDestination(txDest));
    ret.pushKV("signMessage", pl.MakeSignString());
    return ret;
}

UniValue protx_register(const JSONRPCRequest& request)
{
    return ProTxRegister(request, true);
}

UniValue protx_register_prepare(const JSONRPCRequest& request)
{
    return ProTxRegister(request, false);
}

UniValue protx_register_submit(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 2) {
        throw std::runtime_error(
                "protx_register_submit \"tx\" \"sig\"\n"
                "\nSubmits the specified ProTx to the network. This command will also sign the inputs of the transaction\n"
                "which were previously added by \"protx_register_prepare\" to cover transaction fees\n"
                + HelpRequiringPassphrase(pwallet) + "\n"
                "\nArguments:\n"
                "1. \"tx\"                 (string, required) The serialized transaction previously returned by \"protx_register_prepare\"\n"
                "2. \"sig\"                (string, required) The signature signed with the collateral key. Must be in base64 format.\n"
                "\nResult:\n"
                "\"txid\"                  (string) The transaction id.\n"
                "\nExamples:\n"
                + HelpExampleCli("protx_register_submit", "\"tx\" \"sig\"")
        );
    }
    CheckEvoUpgradeEnforcement();

    EnsureWalletIsUnlocked(pwallet);
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    CMutableTransaction tx;
    if (!DecodeHexTx(tx, request.params[0].get_str())) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "transaction not deserializable");
    }
    if (tx.nType != CTransaction::TxType::PROREG) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "transaction not a ProRegTx");
    }
    ProRegPL pl;
    if (!GetTxPayload(tx, pl)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "transaction payload not deserializable");
    }
    if (!pl.vchSig.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "payload signature not empty");
    }

    pl.vchSig = DecodeBase64(request.params[1].get_str().c_str());

    // check the payload, add the tx inputs sigs, and send the tx.
    return SignAndSendSpecialTx(pwallet, tx, pl);
}

UniValue protx_register_fund(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 6 || request.params.size() > 8) {
        throw std::runtime_error(
                "protx_register_fund \"collateralAddress\" \"ipAndPort\" \"ownerAddress\" \"operatorAddress\" \"votingAddress\" \"payoutAddress\" (operatorReward \"operatorPayoutAddress\")\n"
                "\nCreates, funds and sends a ProTx to the network. The resulting transaction will move 10000 PIV\n"
                "to the address specified by collateralAddress and will then function as masternode collateral.\n"
                + HelpRequiringPassphrase(pwallet) + "\n"
                "\nArguments:\n"
                + GetHelpString(1, collateralAddress)
                + GetHelpString(2, ipAndPort_register)
                + GetHelpString(3, ownerAddress)
                + GetHelpString(4, operatorAddress_register)
                + GetHelpString(5, votingAddress_register)
                + GetHelpString(6, payoutAddress_register)
                + GetHelpString(7, operatorReward)
                + GetHelpString(8, operatorPayoutAddress_register) +
                "\nResult:\n"
                "\"txid\"                        (string) The transaction id.\n"
                "\nExamples:\n"
                + HelpExampleCli("protx_register_fund", "\"DKHHBsuU9zfxxxVaqqqQqK4MxZg6vzpf8\" \"168.192.1.100:51472\" \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\"")
        );
    }
    CheckEvoUpgradeEnforcement();

    EnsureWalletIsUnlocked(pwallet);
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    const CTxDestination& collateralDest(ParsePubKeyIDFromAddress(request.params[0].get_str()));
    const CScript& collateralScript = GetScriptForDestination(collateralDest);
    const CAmount collAmt = Params().GetConsensus().nMNCollateralAmt;

    ProRegPL pl = ParseProRegPLParams(request.params, 1);
    pl.nVersion = ProRegPL::CURRENT_VERSION;

    CMutableTransaction tx;
    tx.nVersion = CTransaction::TxVersion::SAPLING;
    tx.nType = CTransaction::TxType::PROREG;
    tx.vout.emplace_back(collAmt, collateralScript);

    FundSpecialTx(pwallet, tx, pl);

    for (uint32_t i = 0; i < tx.vout.size(); i++) {
        if (tx.vout[i].nValue == collAmt && tx.vout[i].scriptPubKey == collateralScript) {
            pl.collateralOutpoint.n = i;
            break;
        }
    }
    assert(pl.collateralOutpoint.n != (uint32_t) -1);
    // update payload on tx (with final collateral outpoint)
    pl.vchSig.clear();
    // check the payload, add the tx inputs sigs, and send the tx.
    return SignAndSendSpecialTx(pwallet, tx, pl);
}

#endif  //ENABLE_WALLET

static bool CheckWalletOwnsScript(CWallet* pwallet, const CScript& script)
{
#ifdef ENABLE_WALLET
    if (!pwallet)
        return false;
    AssertLockHeld(pwallet->cs_wallet);
    CTxDestination dest;
    if (ExtractDestination(script, dest)) {
        const CKeyID* keyID = boost::get<CKeyID>(&dest);
        if (keyID && pwallet->HaveKey(*keyID))
            return true;
        const CScriptID* scriptID = boost::get<CScriptID>(&dest);
        if (scriptID && pwallet->HaveCScript(*scriptID))
            return true;
    }
    return false;
#else
    return false;
#endif
}

static Optional<int> GetUTXOConfirmations(const COutPoint& outpoint)
{
    // nullopt means UTXO is yet unknown or already spent
    Optional<int> coinHeight = GetUTXOHeight(outpoint);
    if (coinHeight == nullopt || *coinHeight < 0)
        return nullopt;
    int nChainHeight(WITH_LOCK(cs_main, return chainActive.Height(); ));
    if (nChainHeight < 0 || *coinHeight > nChainHeight)
        return nullopt;
    return Optional<int>(nChainHeight - *coinHeight + 1);
}

static void AddDMNEntryToList(UniValue& ret, CWallet* pwallet, const CDeterministicMNCPtr& dmn, bool fVerbose, bool fFromWallet)
{
    assert(!fFromWallet || pwallet);
    assert(ret.isArray());

    bool hasOwnerKey{false};
    bool hasOperatorKey{false};
    bool hasVotingKey{false};
    bool ownsCollateral{false};
    bool ownsPayeeScript{false};

    // No need to check wallet if not wallet_only and not verbose
    bool skipWalletCheck = !fFromWallet && !fVerbose;

    if (pwallet && !skipWalletCheck) {
        LOCK(pwallet->cs_wallet);
        hasOwnerKey = pwallet->HaveKey(dmn->pdmnState->keyIDOwner);
        hasOperatorKey = pwallet->HaveKey(dmn->pdmnState->keyIDOperator);
        hasVotingKey = pwallet->HaveKey(dmn->pdmnState->keyIDVoting);
        ownsPayeeScript = CheckWalletOwnsScript(pwallet, dmn->pdmnState->scriptPayout);
        CTransactionRef collTx;
        uint256 hashBlock;
        if (GetTransaction(dmn->collateralOutpoint.hash, collTx, hashBlock, true)) {
            ownsCollateral = CheckWalletOwnsScript(pwallet, collTx->vout[dmn->collateralOutpoint.n].scriptPubKey);
        }
    }

    if (fFromWallet && !hasOwnerKey && !hasOperatorKey  && !hasVotingKey && !ownsCollateral && !ownsPayeeScript) {
        // not one of ours
        return;
    }

    if (fVerbose) {
        UniValue o(UniValue::VOBJ);
        dmn->ToJson(o);
        Optional<int> confirmations = GetUTXOConfirmations(dmn->collateralOutpoint);
        o.pushKV("confirmations", confirmations ? *confirmations : -1);
        o.pushKV("hasOwnerKey", hasOwnerKey);
        o.pushKV("hasOperatorKey", hasOperatorKey);
        o.pushKV("hasVotingKey", hasVotingKey);
        o.pushKV("ownsCollateral", ownsCollateral);
        o.pushKV("ownsPayeeScript", ownsPayeeScript);
        ret.push_back(o);
    } else {
        ret.push_back(dmn->proTxHash.ToString());
    }
}

UniValue protx_list(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 4) {
        throw std::runtime_error(
                "protx_list (detailed wallet_only valid_only height)\n"
                "\nLists all ProTxs.\n"
                "\nArguments:\n"
                "1. \"detailed\"               (bool, optional, default=true) Return detailed information about each protx.\n"
                "                                 If set to false, return only the list of txids.\n"
                "2. \"wallet_only\"            (bool, optional, default=false) If set to true, return only protx which involves\n"
                "                                 keys from this wallet (collateral, owner, operator, voting, or payout addresses).\n"
                "3. \"valid_only\"             (bool, optional, default=false) If set to true, return only ProTx which are active/valid\n"
                "                                 at the height specified.\n"
                "4. \"height\"                 (numeric, optional) If height is not specified, it defaults to the current chain-tip.\n"
                "\nResult:\n"
                "[...]                         (list) List of protx txids or, if detailed=true, list of json objects.\n"
                "\nExamples:\n"
                + HelpExampleCli("protx_list", "")
                + HelpExampleCli("protx_list", "true false false 200000")
        );
    }

    CheckEvoUpgradeEnforcement();

#ifdef ENABLE_WALLET
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
#else
    CWallet* const pwallet = nullptr;
#endif

    const bool fVerbose = (request.params.size() == 0 || request.params[0].get_bool());
    const bool fFromWallet = (request.params.size() > 1 && request.params[1].get_bool());
    const bool fValidOnly = (request.params.size() > 2 && request.params[2].get_bool());

    if (fFromWallet && !pwallet) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "wallet_only not supported when wallet is disabled");
    }

    // Get a reference to the block index at the specified height (or at the chain tip)
    const CBlockIndex* pindex;
    {
        LOCK(cs_main);
        const CBlockIndex* pindexTip = chainActive.Tip();
        if (request.params.size() > 3) {
            const int height = request.params[3].get_int();
            if (height <= 0 || height > pindexTip->nHeight) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("height must be between 1 and %d", pindexTip->nHeight));
            }
            pindexTip = chainActive[height];
        }
        pindex = mapBlockIndex.at(pindexTip->GetBlockHash());
    }

    // Get the deterministic mn list at the index
    CDeterministicMNList mnList = deterministicMNManager->GetListForBlock(pindex);

    // Build/filter the list
    UniValue ret(UniValue::VARR);
    mnList.ForEachMN(fValidOnly, [&](const CDeterministicMNCPtr& dmn) {
        AddDMNEntryToList(ret, pwallet, dmn, fVerbose, fFromWallet);
    });
    return ret;
}

#ifdef ENABLE_WALLET
UniValue protx_update_service(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 4) {
        throw std::runtime_error(
                "protx_update_service \"proTxHash\" \"ipAndPort\" (\"operatorPayoutAddress\" \"operatorKey\")\n"
                "\nCreates and sends a ProUpServTx to the network. This will update the IP address\n"
                "of a masternode, and/or the operator payout address.\n"
                "If the IP is changed for a masternode that got PoSe-banned, the ProUpServTx will also revive this masternode.\n"
                + HelpRequiringPassphrase(pwallet) + "\n"
                "\nArguments:\n"
                + GetHelpString(1, proTxHash)
                + GetHelpString(2, ipAndPort_update)
                + GetHelpString(3, operatorPayoutAddress_update)
                + GetHelpString(4, operatorKey) +
                "\nResult:\n"
                "\"txid\"                        (string) The transaction id.\n"
                "\nExamples:\n"
                + HelpExampleCli("protx_update_service", "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\" \"168.192.1.100:51472\"")
        );
    }
    CheckEvoUpgradeEnforcement();

    EnsureWalletIsUnlocked(pwallet);
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    ProUpServPL pl;
    pl.nVersion = ProUpServPL::CURRENT_VERSION;
    pl.proTxHash = ParseHashV(request.params[0], "proTxHash");

    auto dmn = deterministicMNManager->GetListAtChainTip().GetMN(pl.proTxHash);
    if (!dmn) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("masternode with hash %s not found", pl.proTxHash.ToString()));
    }
    const std::string& addrStr = request.params[1].get_str();
    if (!addrStr.empty()) {
        if (!Lookup(addrStr.c_str(), pl.addr, Params().GetDefaultPort(), false)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("invalid network address %s", addrStr));
        }
    } else {
        pl.addr = dmn->pdmnState->addr;
    }
    pl.scriptOperatorPayout = dmn->pdmnState->scriptOperatorPayout;
    if (request.params.size() > 2) {
        const std::string& strAddOpPayee = request.params[2].get_str();
        if (!strAddOpPayee.empty()) {
            if (dmn->nOperatorReward > 0) {
                pl.scriptOperatorPayout = GetScriptForDestination(CTxDestination(ParsePubKeyIDFromAddress(strAddOpPayee)));
            } else {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Operator reward is 0. Cannot set operator payout address"));
            }
        }
    }

    const std::string& strOpKey = request.params.size() > 3 ? request.params[3].get_str() : "";
    const CKey& operatorKey = strOpKey.empty() ? GetKeyFromWallet(pwallet, dmn->pdmnState->keyIDOperator)
                                               : ParsePrivKey(pwallet, strOpKey, false);

    CMutableTransaction tx;
    tx.nVersion = CTransaction::TxVersion::SAPLING;
    tx.nType = CTransaction::TxType::PROUPSERV;

    // make sure fee calculation works
    pl.vchSig.resize(CPubKey::COMPACT_SIGNATURE_SIZE);
    FundSpecialTx(pwallet, tx, pl);
    SignSpecialTxPayloadByHash(tx, pl, operatorKey);

    return SignAndSendSpecialTx(pwallet, tx, pl);
}

UniValue protx_update_registrar(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 4 || request.params.size() > 5) {
        throw std::runtime_error(
                "protx update_registrar \"proTxHash\" \"operatorAddress\" \"votingAddress\" \"payoutAddress\" (\"ownerKey\")\n"
                "\nCreates and sends a ProUpRegTx to the network. This will update the operator key, voting key and payout\n"
                "address of the masternode specified by \"proTxHash\".\n"
                "The owner key of this masternode must be known to your wallet.\n"
                + HelpRequiringPassphrase(pwallet) + "\n"
                "\nArguments:\n"
                + GetHelpString(1, proTxHash)
                + GetHelpString(2, operatorAddress_update)
                + GetHelpString(3, votingAddress_update)
                + GetHelpString(4, payoutAddress_update)
                + GetHelpString(5, ownerKey) +
                "\nResult:\n"
                "\"txid\"                        (string) The transaction id.\n"
                "\nExamples:\n"
                + HelpExampleCli("protx_update_registrar", "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\" \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\" \"DMJRSsuU9zfyrvxVaAEFQqK4MxZg6vgeS6\"")
        );
    }
    CheckEvoUpgradeEnforcement();

    if (!deterministicMNManager->LegacyMNObsolete()) {
        throw JSONRPCError(RPC_MISC_ERROR, "Legacy masternode system still active. ProUpReg transactions are not accepted yet.");
    }

    EnsureWalletIsUnlocked(pwallet);
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    ProUpRegPL pl;
    pl.nVersion = ProUpServPL::CURRENT_VERSION;
    pl.proTxHash = ParseHashV(request.params[0], "proTxHash");

    auto dmn = deterministicMNManager->GetListAtChainTip().GetMN(pl.proTxHash);
    if (!dmn) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("masternode with hash %s not found", pl.proTxHash.ToString()));
    }
    const std::string& strOperatorAddress = request.params[1].get_str();
    pl.keyIDOperator = strOperatorAddress.empty() ? dmn->pdmnState->keyIDOperator
                                                  : ParsePubKeyIDFromAddress(strOperatorAddress);

    const std::string& strVotingAddress = request.params[2].get_str();
    pl.keyIDVoting = strVotingAddress.empty() ? dmn->pdmnState->keyIDVoting
                                              : ParsePubKeyIDFromAddress(strVotingAddress);

    const std::string& strPayee = request.params[3].get_str();
    pl.scriptPayout = strPayee.empty() ? pl.scriptPayout = dmn->pdmnState->scriptPayout
                                       : GetScriptForDestination(CTxDestination(ParsePubKeyIDFromAddress(strPayee)));

    const std::string& strOwnKey = request.params.size() > 4 ? request.params[4].get_str() : "";
    const CKey& ownerKey = strOwnKey.empty() ? GetKeyFromWallet(pwallet, dmn->pdmnState->keyIDOwner)
                                             : ParsePrivKey(pwallet, strOwnKey, false);

    CMutableTransaction tx;
    tx.nVersion = CTransaction::TxVersion::SAPLING;
    tx.nType = CTransaction::TxType::PROUPREG;

    // make sure fee calculation works
    pl.vchSig.resize(CPubKey::COMPACT_SIGNATURE_SIZE);
    FundSpecialTx(pwallet, tx, pl);
    SignSpecialTxPayloadByHash(tx, pl, ownerKey);

    return SignAndSendSpecialTx(pwallet, tx, pl);
}

UniValue protx_revoke(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() < 1 || request.params.size() > 3) {
        throw std::runtime_error(
                "protx_update_revoke \"proTxHash\" (\"operatorKey\" reason)\n"
                "\nCreates and sends a ProUpRevTx to the network. This will revoke the operator key of the masternode and\n"
                "put it into the PoSe-banned state. It will also set the service field of the masternode\n"
                "to zero. Use this in case your operator key got compromised or you want to stop providing your service\n"
                "to the masternode owner.\n"
                + HelpRequiringPassphrase(pwallet) + "\n"
                "\nArguments:\n"
                + GetHelpString(1, proTxHash)
                + GetHelpString(2, operatorKey)
                + GetHelpString(3, revocationReason) +
                "\nResult:\n"
                "\"txid\"                        (string) The transaction id.\n"
                "\nExamples:\n"
                + HelpExampleCli("protx_revoke", "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"")
                + HelpExampleCli("protx_revoke", "\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\" \"\" 2")
        );
    }
    CheckEvoUpgradeEnforcement();

    EnsureWalletIsUnlocked(pwallet);
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    pwallet->BlockUntilSyncedToCurrentChain();

    ProUpRevPL pl;
    pl.nVersion = ProUpServPL::CURRENT_VERSION;
    pl.proTxHash = ParseHashV(request.params[0], "proTxHash");

    auto dmn = deterministicMNManager->GetListAtChainTip().GetMN(pl.proTxHash);
    if (!dmn) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("masternode with hash %s not found", pl.proTxHash.ToString()));
    }

    const std::string& strOpKey = request.params.size() > 1 ? request.params[1].get_str() : "";
    const CKey& operatorKey = strOpKey.empty() ? GetKeyFromWallet(pwallet, dmn->pdmnState->keyIDOperator)
                                               : ParsePrivKey(pwallet, strOpKey, false);

    pl.nReason = ProUpRevPL::RevocationReason::REASON_NOT_SPECIFIED;
    if (request.params.size() > 2) {
        int nReason = request.params[2].get_int();
        if (nReason < 0 || nReason > ProUpRevPL::RevocationReason::REASON_LAST) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("invalid reason %d, must be between 0 and %d",
                                                                nReason, ProUpRevPL::RevocationReason::REASON_LAST));
        }
        pl.nReason = (uint16_t)nReason;
    }

    CMutableTransaction tx;
    tx.nVersion = CTransaction::TxVersion::SAPLING;
    tx.nType = CTransaction::TxType::PROUPREV;

    // make sure fee calculation works
    pl.vchSig.resize(CPubKey::COMPACT_SIGNATURE_SIZE);
    FundSpecialTx(pwallet, tx, pl);
    SignSpecialTxPayloadByHash(tx, pl, operatorKey);

    return SignAndSendSpecialTx(pwallet, tx, pl);
}
#endif

static const CRPCCommand commands[] =
{ //  category       name                              actor (function)         okSafe argNames
  //  -------------- --------------------------------- ------------------------ ------ --------
    { "evo",         "protx_list",                     &protx_list,             true,  {"detailed","wallet_only","valid_only","height"}  },
#ifdef ENABLE_WALLET
    { "evo",         "protx_register",                 &protx_register,         true,  {"collateralHash","collateralIndex","ipAndPort","ownerAddress","operatorAddress","votingAddress","payoutAddress","operatorReward","operatorPayoutAddress"} },
    { "evo",         "protx_register_fund",            &protx_register_fund,    true,  {"collateralAddress","ipAndPort","ownerAddress","operatorAddress","votingAddress","payoutAddress","operatorReward","operatorPayoutAddress"} },
    { "evo",         "protx_register_prepare",         &protx_register_prepare, true,  {"collateralHash","collateralIndex","ipAndPort","ownerAddress","operatorAddress","votingAddress","payoutAddress","operatorReward","operatorPayoutAddress"} },
    { "evo",         "protx_register_submit",          &protx_register_submit,  true,  {"tx","sig"}  },
    { "evo",         "protx_revoke",                   &protx_revoke,           true,  {"proTxHash","operatorKey","reason"}  },
    { "evo",         "protx_update_registrar",         &protx_update_registrar, true,  {"proTxHash","operatorAddress","votingAddress","payoutAddress","ownerKey"}  },
    { "evo",         "protx_update_service",           &protx_update_service,   true,  {"proTxHash","ipAndPort","operatorPayoutAddress","operatorKey"}  },
#endif  //ENABLE_WALLET
};

void RegisterEvoRPCCommands(CRPCTable &tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++) {
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
    }
}
