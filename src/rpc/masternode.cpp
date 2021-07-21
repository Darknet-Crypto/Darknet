// Copyright (c) 2009-2012 The Bitcoin developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activemasternode.h"
#include "db.h"
#include "evo/deterministicmns.h"
#include "init.h"
#include "key_io.h"
#include "masternode-payments.h"
#include "masternode-sync.h"
#include "masternodeconfig.h"
#include "masternodeman.h"
#include "netbase.h"
#include "rpc/server.h"
#ifdef ENABLE_WALLET
#include "wallet/rpcwallet.h"
#endif

#include <univalue.h>

#include <boost/tokenizer.hpp>

UniValue mnping(const JSONRPCRequest& request)
{
    if (request.fHelp || !request.params.empty()) {
        throw std::runtime_error(
            "mnping \n"
            "\nSend masternode ping. Only for remote masternodes on Regtest\n"

            "\nResult:\n"
            "{\n"
            "  \"sent\":           (string YES|NO) Whether the ping was sent and, if not, the error.\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("mnping", "") + HelpExampleRpc("mnping", ""));
    }

    if (!Params().IsRegTestNet()) {
        throw JSONRPCError(RPC_MISC_ERROR, "command available only for RegTest network");
    }

    if (!fMasterNode) {
        throw JSONRPCError(RPC_MISC_ERROR, "this is not a masternode");
    }

    UniValue ret(UniValue::VOBJ);
    std::string strError;
    ret.pushKV("sent", activeMasternode.SendMasternodePing(strError) ?
                       "YES" : strprintf("NO (%s)", strError));
    return ret;
}

UniValue initmasternode(const JSONRPCRequest& request)
{
    if (request.fHelp || (request.params.size() < 2|| request.params.size() > 3)) {
        throw std::runtime_error(
                "initmasternode ( \"privkey\" \"address\" deterministic )\n"
                "\nInitialize masternode on demand if it's not already initialized.\n"
                "\nArguments:\n"
                "1. privkey          (string, required) The masternode private key.\n"
                "2. address          (string, required) The IP:Port of this masternode.\n"
                "3. deterministic    (boolean, optional, default=false) Init as DMN.\n"

                "\nResult:\n"
                " success                      (string) if the masternode initialization succeeded.\n"

                "\nExamples:\n" +
                HelpExampleCli("initmasternode", "\"9247iC59poZmqBYt9iDh9wDam6v9S1rW5XekjLGyPnDhrDkP4AK\" \"187.24.32.124:51472\"") +
                HelpExampleRpc("initmasternode", "\"9247iC59poZmqBYt9iDh9wDam6v9S1rW5XekjLGyPnDhrDkP4AK\" \"187.24.32.124:51472\""));
    }

    std::string _strMasterNodePrivKey = request.params[0].get_str();
    std::string _strMasterNodeAddr = request.params[1].get_str();
    bool fDeterministic = request.params.size() > 2 && request.params[2].get_bool();
    if (fDeterministic) {
        if (!activeMasternodeManager) {
            activeMasternodeManager = new CActiveDeterministicMasternodeManager();
            RegisterValidationInterface(activeMasternodeManager);
        }
        auto res = activeMasternodeManager->SetOperatorKey(_strMasterNodePrivKey);
        if (!res) throw std::runtime_error(res.getError());
        activeMasternodeManager->Init();
        if (activeMasternodeManager->GetState() == CActiveDeterministicMasternodeManager::MASTERNODE_ERROR) {
            throw std::runtime_error(activeMasternodeManager->GetStatus());
        }
        return "success";
    }
    // legacy
    auto res = initMasternode(_strMasterNodePrivKey, _strMasterNodeAddr, false);
    if (!res) throw std::runtime_error(res.getError());
    return "success";
}

UniValue getcachedblockhashes(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 0)
        throw std::runtime_error(
            "getcachedblockhashes \n"
            "\nReturn the block hashes cached in the masternode manager\n"

            "\nResult:\n"
            "[\n"
            "  ...\n"
            "  \"xxxx\",   (string) hash at Index d (height modulo max cache size)\n"
            "  ...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("getcachedblockhashes", "") + HelpExampleRpc("getcachedblockhashes", ""));

    std::vector<uint256> vCacheCopy = mnodeman.GetCachedBlocks();
    UniValue ret(UniValue::VARR);
    for (int i = 0; (unsigned) i < vCacheCopy.size(); i++) {
        ret.push_back(vCacheCopy[i].ToString());
    }
    return ret;
}

static inline bool filter(const std::string& str, const std::string& strFilter)
{
    return str.find(strFilter) != std::string::npos;
}

static inline bool filterMasternode(const UniValue& dmno, const std::string& strFilter, bool fEnabled)
{
    return strFilter.empty() || (filter("ENABLED", strFilter) && fEnabled)
                             || (filter("POSE_BANNED", strFilter) && !fEnabled)
                             || (filter(dmno["proTxHash"].get_str(), strFilter))
                             || (filter(dmno["collateralHash"].get_str(), strFilter))
                             || (filter(dmno["collateralAddress"].get_str(), strFilter))
                             || (filter(dmno["dmnstate"]["ownerAddress"].get_str(), strFilter))
                             || (filter(dmno["dmnstate"]["operatorAddress"].get_str(), strFilter))
                             || (filter(dmno["dmnstate"]["votingAddress"].get_str(), strFilter));
}

UniValue listmasternodes(const JSONRPCRequest& request)
{
    if (request.fHelp || (request.params.size() > 1))
        throw std::runtime_error(
            "listmasternodes ( \"filter\" )\n"
            "\nGet a ranked list of masternodes\n"

            "\nArguments:\n"
            "1. \"filter\"    (string, optional) Filter search text. Partial match by txhash, status, or addr.\n"

            // !TODO: update for DMNs
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"rank\": n,                             (numeric) Masternode Rank (or 0 if not enabled)\n"
            "    \"type\": \"legacy\"|\"deterministic\",  (string) type of masternode\n"
            "    \"txhash\": \"hash\",                    (string) Collateral transaction hash\n"
            "    \"outidx\": n,                           (numeric) Collateral transaction output index\n"
            "    \"pubkey\": \"key\",                     (string) Masternode public key used for message broadcasting\n"
            "    \"status\": s,                           (string) Status (ENABLED/EXPIRED/REMOVE/etc)\n"
            "    \"addr\": \"addr\",                      (string) Masternode PIVX address\n"
            "    \"version\": v,                          (numeric) Masternode protocol version\n"
            "    \"lastseen\": ttt,     (numeric) The time in seconds since epoch (Jan 1 1970 GMT) of the last seen\n"
            "    \"activetime\": ttt,   (numeric) The time in seconds since epoch (Jan 1 1970 GMT) masternode has been active\n"
            "    \"lastpaid\": ttt,     (numeric) The time in seconds since epoch (Jan 1 1970 GMT) masternode was last paid\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("listmasternodes", "") + HelpExampleRpc("listmasternodes", ""));


    const std::string& strFilter = request.params.size() > 0 ? request.params[0].get_str() : "";
    UniValue ret(UniValue::VARR);

    if (deterministicMNManager->LegacyMNObsolete()) {
        auto mnList = deterministicMNManager->GetListAtChainTip();
        mnList.ForEachMN(false, [&](const CDeterministicMNCPtr& dmn) {
            UniValue obj(UniValue::VOBJ);
            dmn->ToJson(obj);
            bool fEnabled = dmn->pdmnState->nPoSeBanHeight == -1;
            if (filterMasternode(obj, strFilter, fEnabled)) {
                ret.push_back(obj);
            }
        });
        return ret;
    }

    // Legacy masternodes (!TODO: remove when transition to dmn is complete)
    const CBlockIndex* chainTip = GetChainTip();
    if (!chainTip) return "[]";
    int nHeight = chainTip->nHeight;
    auto mnList = deterministicMNManager->GetListAtChainTip();

    std::vector<std::pair<int64_t, MasternodeRef>> vMasternodeRanks = mnodeman.GetMasternodeRanks(nHeight);
    for (int pos=0; pos < (int) vMasternodeRanks.size(); pos++) {
        const auto& s = vMasternodeRanks[pos];
        UniValue obj(UniValue::VOBJ);
        const CMasternode& mn = *(s.second);

        if (!mn.mnPayeeScript.empty()) {
            // Deterministic masternode
            auto dmn = mnList.GetMNByCollateral(mn.vin.prevout);
            if (dmn) {
                UniValue obj(UniValue::VOBJ);
                dmn->ToJson(obj);
                bool fEnabled = dmn->pdmnState->nPoSeBanHeight == -1;
                if (filterMasternode(obj, strFilter, fEnabled)) {
                    // Added for backward compatibility with legacy masternodes
                    obj.pushKV("type", "deterministic");
                    obj.pushKV("txhash", obj["proTxHash"].get_str());
                    obj.pushKV("addr", obj["dmnstate"]["payoutAddress"].get_str());
                    obj.pushKV("status", fEnabled ? "ENABLED" : "POSE_BANNED");
                    obj.pushKV("rank", fEnabled ? pos : 0);
                    ret.push_back(obj);
                }
            }
            continue;
        }

        std::string strVin = mn.vin.prevout.ToStringShort();
        std::string strTxHash = mn.vin.prevout.hash.ToString();
        uint32_t oIdx = mn.vin.prevout.n;

        if (strFilter != "" && strTxHash.find(strFilter) == std::string::npos &&
            mn.Status().find(strFilter) == std::string::npos &&
            EncodeDestination(mn.pubKeyCollateralAddress.GetID()).find(strFilter) == std::string::npos) continue;

        std::string strStatus = mn.Status();
        std::string strHost;
        int port;
        SplitHostPort(mn.addr.ToString(), port, strHost);
        CNetAddr node;
        LookupHost(strHost.c_str(), node, false);
        std::string strNetwork = GetNetworkName(node.GetNetwork());

        obj.pushKV("rank", (strStatus == "ENABLED" ? pos : 0));
        obj.pushKV("type", "legacy");
        obj.pushKV("network", strNetwork);
        obj.pushKV("txhash", strTxHash);
        obj.pushKV("outidx", (uint64_t)oIdx);
        obj.pushKV("pubkey", EncodeDestination(mn.pubKeyMasternode.GetID()));
        obj.pushKV("status", strStatus);
        obj.pushKV("addr", EncodeDestination(mn.pubKeyCollateralAddress.GetID()));
        obj.pushKV("version", mn.protocolVersion);
        obj.pushKV("lastseen", (int64_t)mn.lastPing.sigTime);
        obj.pushKV("activetime", (int64_t)(mn.lastPing.sigTime - mn.sigTime));
        obj.pushKV("lastpaid", (int64_t)mnodeman.GetLastPaid(s.second, chainTip));

        ret.push_back(obj);
    }

    return ret;
}

UniValue getmasternodecount (const JSONRPCRequest& request)
{
    if (request.fHelp || (request.params.size() > 0))
        throw std::runtime_error(
            "getmasternodecount\n"
            "\nGet masternode count values\n"

            "\nResult:\n"
            "{\n"
            "  \"total\": n,        (numeric) Total masternodes\n"
            "  \"stable\": n,       (numeric) Stable count\n"
            "  \"enabled\": n,      (numeric) Enabled masternodes\n"
            "  \"inqueue\": n,      (numeric) Masternodes in queue\n"
            "  \"ipv4\": n,         (numeric) Number of IPv4 masternodes\n"
            "  \"ipv6\": n,         (numeric) Number of IPv6 masternodes\n"
            "  \"onion\": n         (numeric) Number of Tor masternodes\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("getmasternodecount", "") + HelpExampleRpc("getmasternodecount", ""));

    UniValue obj(UniValue::VOBJ);
    int nCount = 0;
    int ipv4 = 0, ipv6 = 0, onion = 0;

    const CBlockIndex* pChainTip = GetChainTip();
    if (!pChainTip) return "unknown";

    mnodeman.GetNextMasternodeInQueueForPayment(pChainTip->nHeight, true, nCount, pChainTip);
    int total = mnodeman.CountNetworks(ipv4, ipv6, onion);

    obj.pushKV("total", total);
    obj.pushKV("stable", mnodeman.stable_size());
    obj.pushKV("enabled", mnodeman.CountEnabled());
    obj.pushKV("inqueue", nCount);
    obj.pushKV("ipv4", ipv4);
    obj.pushKV("ipv6", ipv6);
    obj.pushKV("onion", onion);

    return obj;
}

UniValue masternodecurrent(const JSONRPCRequest& request)
{
    if (request.fHelp || (request.params.size() != 0))
        throw std::runtime_error(
            "masternodecurrent\n"
            "\nGet current masternode winner (scheduled to be paid next).\n"

            "\nResult:\n"
            "{\n"
            "  \"protocol\": xxxx,        (numeric) Protocol version\n"
            "  \"txhash\": \"xxxx\",      (string) Collateral transaction hash\n"
            "  \"pubkey\": \"xxxx\",      (string) MN Public key\n"
            "  \"lastseen\": xxx,         (numeric) Time since epoch of last seen\n"
            "  \"activeseconds\": xxx,    (numeric) Seconds MN has been active\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("masternodecurrent", "") + HelpExampleRpc("masternodecurrent", ""));

    const CBlockIndex* pChainTip = GetChainTip();
    if (!pChainTip) return "unknown";

    int nCount = 0;
    MasternodeRef winner = mnodeman.GetNextMasternodeInQueueForPayment(pChainTip->nHeight + 1, true, nCount, pChainTip);
    if (winner) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("protocol", (int64_t)winner->protocolVersion);
        obj.pushKV("txhash", winner->vin.prevout.hash.ToString());
        obj.pushKV("pubkey", EncodeDestination(winner->pubKeyCollateralAddress.GetID()));
        obj.pushKV("lastseen", winner->lastPing.IsNull() ? winner->sigTime : (int64_t)winner->lastPing.sigTime);
        obj.pushKV("activeseconds", winner->lastPing.IsNull() ? 0 : (int64_t)(winner->lastPing.sigTime - winner->sigTime));
        return obj;
    }

    throw std::runtime_error("unknown");
}

bool StartMasternodeEntry(UniValue& statusObjRet, CMasternodeBroadcast& mnbRet, bool& fSuccessRet, const CMasternodeConfig::CMasternodeEntry& mne, std::string& errorMessage, std::string strCommand = "")
{
    int nIndex;
    if(!mne.castOutputIndex(nIndex)) {
        return false;
    }

    CTxIn vin = CTxIn(uint256S(mne.getTxHash()), uint32_t(nIndex));
    CMasternode* pmn = mnodeman.Find(vin.prevout);
    if (pmn != NULL) {
        if (strCommand == "missing") return false;
        if (strCommand == "disabled" && pmn->IsEnabled()) return false;
    }

    fSuccessRet = CMasternodeBroadcast::Create(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), errorMessage, mnbRet);

    statusObjRet.pushKV("alias", mne.getAlias());
    statusObjRet.pushKV("result", fSuccessRet ? "success" : "failed");
    statusObjRet.pushKV("error", fSuccessRet ? "" : errorMessage);

    return true;
}

void RelayMNB(CMasternodeBroadcast& mnb, const bool fSuccess, int& successful, int& failed)
{
    if (fSuccess) {
        successful++;
        mnodeman.UpdateMasternodeList(mnb);
        mnb.Relay();
    } else {
        failed++;
    }
}

void RelayMNB(CMasternodeBroadcast& mnb, const bool fSucces)
{
    int successful = 0, failed = 0;
    return RelayMNB(mnb, fSucces, successful, failed);
}

void SerializeMNB(UniValue& statusObjRet, const CMasternodeBroadcast& mnb, const bool fSuccess, int& successful, int& failed)
{
    if(fSuccess) {
        successful++;
        CDataStream ssMnb(SER_NETWORK, PROTOCOL_VERSION);
        ssMnb << mnb;
        statusObjRet.pushKV("hex", HexStr(ssMnb));
    } else {
        failed++;
    }
}

void SerializeMNB(UniValue& statusObjRet, const CMasternodeBroadcast& mnb, const bool fSuccess)
{
    int successful = 0, failed = 0;
    return SerializeMNB(statusObjRet, mnb, fSuccess, successful, failed);
}

UniValue startmasternode(const JSONRPCRequest& request)
{
    // Skip after legacy obsolete. !TODO: remove when transition to DMN is complete
    if (deterministicMNManager->LegacyMNObsolete()) {
        throw JSONRPCError(RPC_MISC_ERROR, "startmasternode is not supported when deterministic masternode list is active (DIP3)");
    }

    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    std::string strCommand;
    if (request.params.size() >= 1) {
        strCommand = request.params[0].get_str();

        // Backwards compatibility with legacy 'masternode' super-command forwarder
        if (strCommand == "start") strCommand = "local";
        if (strCommand == "start-alias") strCommand = "alias";
        if (strCommand == "start-all") strCommand = "all";
        if (strCommand == "start-many") strCommand = "many";
        if (strCommand == "start-missing") strCommand = "missing";
        if (strCommand == "start-disabled") strCommand = "disabled";
    }

    if (request.fHelp || request.params.size() < 2 || request.params.size() > 4 ||
        (request.params.size() == 2 && (strCommand != "local" && strCommand != "all" && strCommand != "many" && strCommand != "missing" && strCommand != "disabled")) ||
        ( (request.params.size() == 3 || request.params.size() == 4) && strCommand != "alias"))
        throw std::runtime_error(
            "startmasternode \"local|all|many|missing|disabled|alias\" lockwallet ( \"alias\" reload_conf )\n"
            "\nAttempts to start one or more masternode(s)\n"

            "\nArguments:\n"
            "1. set         (string, required) Specify which set of masternode(s) to start.\n"
            "2. lockwallet  (boolean, required) Lock wallet after completion.\n"
            "3. alias       (string) Masternode alias. Required if using 'alias' as the set.\n"
            "4. reload_conf (boolean) if true and \"alias\" was selected, reload the masternodes.conf data from disk"

            "\nResult: (for 'local' set):\n"
            "\"status\"     (string) Masternode status message\n"

            "\nResult: (for other sets):\n"
            "{\n"
            "  \"overall\": \"xxxx\",     (string) Overall status message\n"
            "  \"detail\": [\n"
            "    {\n"
            "      \"node\": \"xxxx\",    (string) Node name or alias\n"
            "      \"result\": \"xxxx\",  (string) 'success' or 'failed'\n"
            "      \"error\": \"xxxx\"    (string) Error message, if failed\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("startmasternode", "\"alias\" \"0\" \"my_mn\"") + HelpExampleRpc("startmasternode", "\"alias\" \"0\" \"my_mn\""));

    bool fLock = (request.params[1].get_str() == "true" ? true : false);

    EnsureWalletIsUnlocked(pwallet);

    if (strCommand == "local") {
        if (!fMasterNode) throw std::runtime_error("you must set masternode=1 in the configuration\n");

        if (activeMasternode.GetStatus() != ACTIVE_MASTERNODE_STARTED) {
            activeMasternode.ResetStatus();
            if (fLock)
                pwallet->Lock();
        }

        return activeMasternode.GetStatusMessage();
    }

    if (strCommand == "all" || strCommand == "many" || strCommand == "missing" || strCommand == "disabled") {
        if ((strCommand == "missing" || strCommand == "disabled") &&
            (masternodeSync.RequestedMasternodeAssets <= MASTERNODE_SYNC_LIST ||
                masternodeSync.RequestedMasternodeAssets == MASTERNODE_SYNC_FAILED)) {
            throw std::runtime_error("You can't use this command until masternode list is synced\n");
        }

        int successful = 0;
        int failed = 0;

        UniValue resultsObj(UniValue::VARR);

        for (CMasternodeConfig::CMasternodeEntry mne : masternodeConfig.getEntries()) {
            UniValue statusObj(UniValue::VOBJ);
            CMasternodeBroadcast mnb;
            std::string errorMessage;
            bool fSuccess = false;
            if (!StartMasternodeEntry(statusObj, mnb, fSuccess, mne, errorMessage, strCommand))
                continue;
            resultsObj.push_back(statusObj);
            RelayMNB(mnb, fSuccess, successful, failed);
        }
        if (fLock)
            pwallet->Lock();

        UniValue returnObj(UniValue::VOBJ);
        returnObj.pushKV("overall", strprintf("Successfully started %d masternodes, failed to start %d, total %d", successful, failed, successful + failed));
        returnObj.pushKV("detail", resultsObj);

        return returnObj;
    }

    if (strCommand == "alias") {
        std::string alias = request.params[2].get_str();

        // Check reload param
        if(request.params[3].getBool()) {
            masternodeConfig.clear();
            std::string error;
            if (!masternodeConfig.read(error)) {
                throw std::runtime_error("Error reloading masternode.conf, " + error);
            }
        }

        bool found = false;

        UniValue resultsObj(UniValue::VARR);
        UniValue statusObj(UniValue::VOBJ);

        for (CMasternodeConfig::CMasternodeEntry mne : masternodeConfig.getEntries()) {
            if (mne.getAlias() == alias) {
                CMasternodeBroadcast mnb;
                found = true;
                std::string errorMessage;
                bool fSuccess = false;
                if (!StartMasternodeEntry(statusObj, mnb, fSuccess, mne, errorMessage, strCommand))
                        continue;
                RelayMNB(mnb, fSuccess);
                break;
            }
        }

        if (fLock)
            pwallet->Lock();

        if(!found) {
            statusObj.pushKV("alias", alias);
            statusObj.pushKV("result", "failed");
            statusObj.pushKV("error", "Could not find alias in config. Verify with listmasternodeconf.");
        }

        return statusObj;
    }
    return NullUniValue;
}

UniValue createmasternodekey(const JSONRPCRequest& request)
{
    if (request.fHelp || (request.params.size() != 0))
        throw std::runtime_error(
            "createmasternodekey\n"
            "\nCreate a new masternode private key\n"

            "\nResult:\n"
            "\"key\"    (string) Masternode private key\n"

            "\nExamples:\n" +
            HelpExampleCli("createmasternodekey", "") + HelpExampleRpc("createmasternodekey", ""));

    CKey secret;
    secret.MakeNewKey(false);

    return KeyIO::EncodeSecret(secret);
}

UniValue getmasternodeoutputs(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || (request.params.size() != 0))
        throw std::runtime_error(
            "getmasternodeoutputs\n"
            "\nPrint all masternode transaction outputs\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"txhash\": \"xxxx\",    (string) output transaction hash\n"
            "    \"outputidx\": n       (numeric) output index number\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("getmasternodeoutputs", "") + HelpExampleRpc("getmasternodeoutputs", ""));

    // Find possible candidates
    CWallet::AvailableCoinsFilter coinsFilter;
    coinsFilter.fIncludeDelegated = false;
    coinsFilter.nMaxOutValue = Params().GetConsensus().nMNCollateralAmt;
    coinsFilter.nMinOutValue = coinsFilter.nMaxOutValue;
    coinsFilter.fIncludeLocked = true;
    std::vector<COutput> possibleCoins;
    pwallet->AvailableCoins(&possibleCoins, nullptr, coinsFilter);

    UniValue ret(UniValue::VARR);
    for (COutput& out : possibleCoins) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("txhash", out.tx->GetHash().ToString());
        obj.pushKV("outputidx", out.i);
        ret.push_back(obj);
    }

    return ret;
}

UniValue listmasternodeconf(const JSONRPCRequest& request)
{
    std::string strFilter = "";

    if (request.params.size() == 1) strFilter = request.params[0].get_str();

    if (request.fHelp || (request.params.size() > 1))
        throw std::runtime_error(
            "listmasternodeconf ( \"filter\" )\n"
            "\nPrint masternode.conf in JSON format\n"

            "\nArguments:\n"
            "1. \"filter\"    (string, optional) Filter search text. Partial match on alias, address, txHash, or status.\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"alias\": \"xxxx\",        (string) masternode alias\n"
            "    \"address\": \"xxxx\",      (string) masternode IP address\n"
            "    \"privateKey\": \"xxxx\",   (string) masternode private key\n"
            "    \"txHash\": \"xxxx\",       (string) transaction hash\n"
            "    \"outputIndex\": n,       (numeric) transaction output index\n"
            "    \"status\": \"xxxx\"        (string) masternode status\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("listmasternodeconf", "") + HelpExampleRpc("listmasternodeconf", ""));

    std::vector<CMasternodeConfig::CMasternodeEntry> mnEntries;
    mnEntries = masternodeConfig.getEntries();

    UniValue ret(UniValue::VARR);

    for (CMasternodeConfig::CMasternodeEntry mne : masternodeConfig.getEntries()) {
        int nIndex;
        if(!mne.castOutputIndex(nIndex))
            continue;
        CTxIn vin = CTxIn(uint256S(mne.getTxHash()), uint32_t(nIndex));
        CMasternode* pmn = mnodeman.Find(vin.prevout);

        std::string strStatus = pmn ? pmn->Status() : "MISSING";

        if (strFilter != "" && mne.getAlias().find(strFilter) == std::string::npos &&
            mne.getIp().find(strFilter) == std::string::npos &&
            mne.getTxHash().find(strFilter) == std::string::npos &&
            strStatus.find(strFilter) == std::string::npos) continue;

        UniValue mnObj(UniValue::VOBJ);
        mnObj.pushKV("alias", mne.getAlias());
        mnObj.pushKV("address", mne.getIp());
        mnObj.pushKV("privateKey", mne.getPrivKey());
        mnObj.pushKV("txHash", mne.getTxHash());
        mnObj.pushKV("outputIndex", mne.getOutputIndex());
        mnObj.pushKV("status", strStatus);
        ret.push_back(mnObj);
    }

    return ret;
}

UniValue getmasternodestatus(const JSONRPCRequest& request)
{
    if (request.fHelp || (request.params.size() != 0))
        throw std::runtime_error(
            "getmasternodestatus\n"
            "\nPrint masternode status\n"

            "\nResult (if legacy masternode):\n"
            "{\n"
            "  \"txhash\": \"xxxx\",      (string) Collateral transaction hash\n"
            "  \"outputidx\": n,          (numeric) Collateral transaction output index number\n"
            "  \"netaddr\": \"xxxx\",     (string) Masternode network address\n"
            "  \"addr\": \"xxxx\",        (string) PIVX address for masternode payments\n"
            "  \"status\": \"xxxx\",      (string) Masternode status\n"
            "  \"message\": \"xxxx\"      (string) Masternode status message\n"
            "}\n"
            "\n"
            "\nResult (if deterministic masternode):\n"
            "{\n"
            "... !TODO ...\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("getmasternodestatus", "") + HelpExampleRpc("getmasternodestatus", ""));

    if (!fMasterNode)
        throw JSONRPCError(RPC_MISC_ERROR, _("This is not a masternode."));

    bool fLegacyMN = (activeMasternode.vin != nullopt);
    bool fDeterministicMN = (activeMasternodeManager != nullptr);

    if (!fLegacyMN && !fDeterministicMN) {
        throw JSONRPCError(RPC_MISC_ERROR, _("Active Masternode not initialized."));
    }

    if (fDeterministicMN) {
        if (!deterministicMNManager->IsDIP3Enforced()) {
            // this should never happen as ProTx transactions are not accepted yet
            throw JSONRPCError(RPC_MISC_ERROR, _("Deterministic masternodes are not enforced yet"));
        }
        const CActiveMasternodeInfo* amninfo = activeMasternodeManager->GetInfo();
        UniValue mnObj(UniValue::VOBJ);
        auto dmn = deterministicMNManager->GetListAtChainTip().GetMNByOperatorKey(amninfo->keyIDOperator);
        if (dmn) {
            dmn->ToJson(mnObj);
        }
        mnObj.pushKV("netaddr", amninfo->service.ToString());
        mnObj.pushKV("status", activeMasternodeManager->GetStatus());
        return mnObj;
    }

    // Legacy code !TODO: remove when transition to DMN is complete
    if (deterministicMNManager->LegacyMNObsolete()) {
        throw JSONRPCError(RPC_MISC_ERROR, _("Legacy Masternode is obsolete."));
    }

    CMasternode* pmn = mnodeman.Find(activeMasternode.vin->prevout);

    if (pmn) {
        UniValue mnObj(UniValue::VOBJ);
        mnObj.pushKV("txhash", activeMasternode.vin->prevout.hash.ToString());
        mnObj.pushKV("outputidx", (uint64_t)activeMasternode.vin->prevout.n);
        mnObj.pushKV("netaddr", activeMasternode.service.ToString());
        mnObj.pushKV("addr", EncodeDestination(pmn->pubKeyCollateralAddress.GetID()));
        mnObj.pushKV("status", activeMasternode.GetStatus());
        mnObj.pushKV("message", activeMasternode.GetStatusMessage());
        return mnObj;
    }
    throw std::runtime_error("Masternode not found in the list of available masternodes. Current status: "
                        + activeMasternode.GetStatusMessage());
}

UniValue getmasternodewinners(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 2)
        throw std::runtime_error(
            "getmasternodewinners ( blocks \"filter\" )\n"
            "\nPrint the masternode winners for the last n blocks\n"

            "\nArguments:\n"
            "1. blocks      (numeric, optional) Number of previous blocks to show (default: 10)\n"
            "2. filter      (string, optional) Search filter matching MN address\n"

            "\nResult (single winner):\n"
            "[\n"
            "  {\n"
            "    \"nHeight\": n,           (numeric) block height\n"
            "    \"winner\": {\n"
            "      \"address\": \"xxxx\",    (string) PIVX MN Address\n"
            "      \"nVotes\": n,          (numeric) Number of votes for winner\n"
            "    }\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nResult (multiple winners):\n"
            "[\n"
            "  {\n"
            "    \"nHeight\": n,           (numeric) block height\n"
            "    \"winner\": [\n"
            "      {\n"
            "        \"address\": \"xxxx\",  (string) PIVX MN Address\n"
            "        \"nVotes\": n,        (numeric) Number of votes for winner\n"
            "      }\n"
            "      ,...\n"
            "    ]\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("getmasternodewinners", "") + HelpExampleRpc("getmasternodewinners", ""));

    int nHeight = WITH_LOCK(cs_main, return chainActive.Height());
    if (nHeight < 0) return "[]";

    int nLast = 10;
    std::string strFilter = "";

    if (request.params.size() >= 1)
        nLast = atoi(request.params[0].get_str());

    if (request.params.size() == 2)
        strFilter = request.params[1].get_str();

    UniValue ret(UniValue::VARR);

    for (int i = nHeight - nLast; i < nHeight + 20; i++) {
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("nHeight", i);

        std::string strPayment = GetRequiredPaymentsString(i);
        if (strFilter != "" && strPayment.find(strFilter) == std::string::npos) continue;

        if (strPayment.find(',') != std::string::npos) {
            UniValue winner(UniValue::VARR);
            boost::char_separator<char> sep(",");
            boost::tokenizer< boost::char_separator<char> > tokens(strPayment, sep);
            for (const std::string& t : tokens) {
                UniValue addr(UniValue::VOBJ);
                std::size_t pos = t.find(":");
                std::string strAddress = t.substr(0,pos);
                uint64_t nVotes = atoi(t.substr(pos+1));
                addr.pushKV("address", strAddress);
                addr.pushKV("nVotes", nVotes);
                winner.push_back(addr);
            }
            obj.pushKV("winner", winner);
        } else if (strPayment.find("Unknown") == std::string::npos) {
            UniValue winner(UniValue::VOBJ);
            std::size_t pos = strPayment.find(":");
            std::string strAddress = strPayment.substr(0,pos);
            uint64_t nVotes = atoi(strPayment.substr(pos+1));
            winner.pushKV("address", strAddress);
            winner.pushKV("nVotes", nVotes);
            obj.pushKV("winner", winner);
        } else {
            UniValue winner(UniValue::VOBJ);
            winner.pushKV("address", strPayment);
            winner.pushKV("nVotes", 0);
            obj.pushKV("winner", winner);
        }

            ret.push_back(obj);
    }

    return ret;
}

UniValue getmasternodescores(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "getmasternodescores ( blocks )\n"
            "\nPrint list of winning masternode by score\n"

            "\nArguments:\n"
            "1. blocks      (numeric, optional) Show the last n blocks (default 10)\n"

            "\nResult:\n"
            "{\n"
            "  xxxx: \"xxxx\"   (numeric : string) Block height : Masternode hash\n"
            "  ,...\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("getmasternodescores", "") + HelpExampleRpc("getmasternodescores", ""));

    int nLast = 10;

    if (request.params.size() == 1) {
        try {
            nLast = std::stoi(request.params[0].get_str());
        } catch (const std::invalid_argument&) {
            throw std::runtime_error("Exception on param 2");
        }
    }

    std::vector<std::pair<MasternodeRef, int>> vMnScores = mnodeman.GetMnScores(nLast);
    if (vMnScores.empty()) return "unknown";

    UniValue obj(UniValue::VOBJ);
    for (const auto& p : vMnScores) {
        const MasternodeRef& mn = p.first;
        const int nHeight = p.second;
        obj.pushKV(strprintf("%d", nHeight), mn->vin.prevout.hash.ToString().c_str());
    }
    return obj;
}

bool DecodeHexMnb(CMasternodeBroadcast& mnb, std::string strHexMnb) {

    if (!IsHex(strHexMnb))
        return false;

    std::vector<unsigned char> mnbData(ParseHex(strHexMnb));
    CDataStream ssData(mnbData, SER_NETWORK, PROTOCOL_VERSION);
    try {
        ssData >> mnb;
    }
    catch (const std::exception&) {
        return false;
    }

    return true;
}
UniValue createmasternodebroadcast(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    std::string strCommand;
    if (request.params.size() >= 1)
        strCommand = request.params[0].get_str();
    if (request.fHelp || (strCommand != "alias" && strCommand != "all") || (strCommand == "alias" && request.params.size() < 2))
        throw std::runtime_error(
            "createmasternodebroadcast \"command\" ( \"alias\")\n"
            "\nCreates a masternode broadcast message for one or all masternodes configured in masternode.conf\n" +
            HelpRequiringPassphrase(pwallet) + "\n"

            "\nArguments:\n"
            "1. \"command\"      (string, required) \"alias\" for single masternode, \"all\" for all masternodes\n"
            "2. \"alias\"        (string, required if command is \"alias\") Alias of the masternode\n"

            "\nResult (all):\n"
            "{\n"
            "  \"overall\": \"xxx\",        (string) Overall status message indicating number of successes.\n"
            "  \"detail\": [                (array) JSON array of broadcast objects.\n"
            "    {\n"
            "      \"alias\": \"xxx\",      (string) Alias of the masternode.\n"
            "      \"success\": true|false, (boolean) Success status.\n"
            "      \"hex\": \"xxx\"         (string, if success=true) Hex encoded broadcast message.\n"
            "      \"error_message\": \"xxx\"   (string, if success=false) Error message, if any.\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"

            "\nResult (alias):\n"
            "{\n"
            "  \"alias\": \"xxx\",      (string) Alias of the masternode.\n"
            "  \"success\": true|false, (boolean) Success status.\n"
            "  \"hex\": \"xxx\"         (string, if success=true) Hex encoded broadcast message.\n"
            "  \"error_message\": \"xxx\"   (string, if success=false) Error message, if any.\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("createmasternodebroadcast", "alias mymn1") + HelpExampleRpc("createmasternodebroadcast", "alias mymn1"));

    EnsureWalletIsUnlocked(pwallet);

    if (strCommand == "alias")
    {
        // wait for reindex and/or import to finish
        if (fImporting || fReindex)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Wait for reindex and/or import to finish");

        std::string alias = request.params[1].get_str();
        bool found = false;

        UniValue statusObj(UniValue::VOBJ);
        statusObj.pushKV("alias", alias);

        for (CMasternodeConfig::CMasternodeEntry mne : masternodeConfig.getEntries()) {
            if(mne.getAlias() == alias) {
                CMasternodeBroadcast mnb;
                found = true;
                std::string errorMessage;
                bool fSuccess = false;
                if (!StartMasternodeEntry(statusObj, mnb, fSuccess, mne, errorMessage, strCommand))
                        continue;
                SerializeMNB(statusObj, mnb, fSuccess);
                break;
            }
        }

        if(!found) {
            statusObj.pushKV("success", false);
            statusObj.pushKV("error_message", "Could not find alias in config. Verify with listmasternodeconf.");
        }

        return statusObj;
    }

    if (strCommand == "all")
    {
        // wait for reindex and/or import to finish
        if (fImporting || fReindex)
            throw JSONRPCError(RPC_INTERNAL_ERROR, "Wait for reindex and/or import to finish");

        int successful = 0;
        int failed = 0;

        UniValue resultsObj(UniValue::VARR);

        for (CMasternodeConfig::CMasternodeEntry mne : masternodeConfig.getEntries()) {
            UniValue statusObj(UniValue::VOBJ);
            CMasternodeBroadcast mnb;
            std::string errorMessage;
            bool fSuccess = false;
            if (!StartMasternodeEntry(statusObj, mnb, fSuccess, mne, errorMessage, strCommand))
                    continue;
            SerializeMNB(statusObj, mnb, fSuccess, successful, failed);
            resultsObj.push_back(statusObj);
        }

        UniValue returnObj(UniValue::VOBJ);
        returnObj.pushKV("overall", strprintf("Successfully created broadcast messages for %d masternodes, failed to create %d, total %d", successful, failed, successful + failed));
        returnObj.pushKV("detail", resultsObj);

        return returnObj;
    }
    return NullUniValue;
}

UniValue decodemasternodebroadcast(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "decodemasternodebroadcast \"hexstring\"\n"
            "\nCommand to decode masternode broadcast messages\n"

            "\nArgument:\n"
            "1. \"hexstring\"        (string) The hex encoded masternode broadcast message\n"

            "\nResult:\n"
            "{\n"
            "  \"vin\": \"xxxx\"                (string) The unspent output which is holding the masternode collateral\n"
            "  \"addr\": \"xxxx\"               (string) IP address of the masternode\n"
            "  \"pubkeycollateral\": \"xxxx\"   (string) Collateral address's public key\n"
            "  \"pubkeymasternode\": \"xxxx\"   (string) Masternode's public key\n"
            "  \"vchsig\": \"xxxx\"             (string) Base64-encoded signature of this message (verifiable via pubkeycollateral)\n"
            "  \"sigtime\": \"nnn\"             (numeric) Signature timestamp\n"
            "  \"sigvalid\": \"xxx\"            (string) \"true\"/\"false\" whether or not the mnb signature checks out.\n"
            "  \"protocolversion\": \"nnn\"     (numeric) Masternode's protocol version\n"
            "  \"nMessVersion\": \"nnn\"        (numeric) MNB Message version number\n"
            "  \"lastping\" : {                 (object) JSON object with information about the masternode's last ping\n"
            "      \"vin\": \"xxxx\"            (string) The unspent output of the masternode which is signing the message\n"
            "      \"blockhash\": \"xxxx\"      (string) Current chaintip blockhash minus 12\n"
            "      \"sigtime\": \"nnn\"         (numeric) Signature time for this ping\n"
            "      \"sigvalid\": \"xxx\"        (string) \"true\"/\"false\" whether or not the mnp signature checks out.\n"
            "      \"vchsig\": \"xxxx\"         (string) Base64-encoded signature of this ping (verifiable via pubkeymasternode)\n"
            "      \"nMessVersion\": \"nnn\"    (numeric) MNP Message version number\n"
            "  }\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("decodemasternodebroadcast", "hexstring") + HelpExampleRpc("decodemasternodebroadcast", "hexstring"));

    CMasternodeBroadcast mnb;

    if (!DecodeHexMnb(mnb, request.params[0].get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Masternode broadcast message decode failed");

    UniValue resultObj(UniValue::VOBJ);

    resultObj.pushKV("vin", mnb.vin.prevout.ToString());
    resultObj.pushKV("addr", mnb.addr.ToString());
    resultObj.pushKV("pubkeycollateral", EncodeDestination(mnb.pubKeyCollateralAddress.GetID()));
    resultObj.pushKV("pubkeymasternode", EncodeDestination(mnb.pubKeyMasternode.GetID()));
    resultObj.pushKV("vchsig", mnb.GetSignatureBase64());
    resultObj.pushKV("sigtime", mnb.sigTime);
    resultObj.pushKV("sigvalid", mnb.CheckSignature() ? "true" : "false");
    resultObj.pushKV("protocolversion", mnb.protocolVersion);
    resultObj.pushKV("nMessVersion", mnb.nMessVersion);

    UniValue lastPingObj(UniValue::VOBJ);
    lastPingObj.pushKV("vin", mnb.lastPing.vin.prevout.ToString());
    lastPingObj.pushKV("blockhash", mnb.lastPing.blockHash.ToString());
    lastPingObj.pushKV("sigtime", mnb.lastPing.sigTime);
    lastPingObj.pushKV("sigvalid", mnb.lastPing.CheckSignature(mnb.pubKeyMasternode.GetID()) ? "true" : "false");
    lastPingObj.pushKV("vchsig", mnb.lastPing.GetSignatureBase64());
    lastPingObj.pushKV("nMessVersion", mnb.lastPing.nMessVersion);

    resultObj.pushKV("lastping", lastPingObj);

    return resultObj;
}

UniValue relaymasternodebroadcast(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "relaymasternodebroadcast \"hexstring\"\n"
            "\nCommand to relay masternode broadcast messages\n"

            "\nArguments:\n"
            "1. \"hexstring\"        (string) The hex encoded masternode broadcast message\n"

            "\nExamples:\n" +
            HelpExampleCli("relaymasternodebroadcast", "hexstring") + HelpExampleRpc("relaymasternodebroadcast", "hexstring"));


    CMasternodeBroadcast mnb;

    if (!DecodeHexMnb(mnb, request.params[0].get_str()))
        throw JSONRPCError(RPC_DESERIALIZATION_ERROR, "Masternode broadcast message decode failed");

    if(!mnb.CheckSignature())
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Masternode broadcast signature verification failed");

    mnodeman.UpdateMasternodeList(mnb);
    mnb.Relay();

    return strprintf("Masternode broadcast sent (service %s, vin %s)", mnb.addr.ToString(), mnb.vin.ToString());
}

static const CRPCCommand commands[] =
{ //  category              name                         actor (function)            okSafe argNames
  //  --------------------- ---------------------------  --------------------------  ------ --------
    { "masternode",         "createmasternodebroadcast", &createmasternodebroadcast, true,  {"command","alias"} },
    { "masternode",         "createmasternodekey",       &createmasternodekey,       true,  {} },
    { "masternode",         "decodemasternodebroadcast", &decodemasternodebroadcast, true,  {"hexstring"} },
    { "masternode",         "getmasternodecount",        &getmasternodecount,        true,  {} },
    { "masternode",         "getmasternodeoutputs",      &getmasternodeoutputs,      true,  {} },
    { "masternode",         "getmasternodescores",       &getmasternodescores,       true,  {"blocks"} },
    { "masternode",         "getmasternodestatus",       &getmasternodestatus,       true,  {} },
    { "masternode",         "getmasternodewinners",      &getmasternodewinners,      true,  {"blocks","filter"} },
    { "masternode",         "initmasternode",            &initmasternode,            true,  {"privkey","address","deterministic"} },
    { "masternode",         "listmasternodeconf",        &listmasternodeconf,        true,  {"filter"} },
    { "masternode",         "listmasternodes",           &listmasternodes,           true,  {"filter"} },
    { "masternode",         "masternodecurrent",         &masternodecurrent,         true,  {} },
    { "masternode",         "relaymasternodebroadcast",  &relaymasternodebroadcast,  true,  {"hexstring"}  },
    { "masternode",         "startmasternode",           &startmasternode,           true,  {"set","lockwallet","alias","reload_conf"} },

    /* Not shown in help */
    { "hidden",             "getcachedblockhashes",      &getcachedblockhashes,      true,  {} },
    { "hidden",             "mnping",                    &mnping,                    true,  {} },
};

void RegisterMasternodeRPCCommands(CRPCTable &tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
