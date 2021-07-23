// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "activemasternode.h"
#include "chainparams.h"
#include "budget/budgetmanager.h"
#include "db.h"
#include "evo/deterministicmns.h"
#include "init.h"
#include "key_io.h"
#include "masternode-payments.h"
#include "masternode-sync.h"
#include "masternodeconfig.h"
#include "masternodeman.h"
#include "messagesigner.h"
#include "rpc/server.h"
#include "utilmoneystr.h"
#ifdef ENABLE_WALLET
#include "wallet/rpcwallet.h"
#endif

#include <univalue.h>

void budgetToJSON(const CBudgetProposal* pbudgetProposal, UniValue& bObj, int nCurrentHeight)
{
    CTxDestination address1;
    ExtractDestination(pbudgetProposal->GetPayee(), address1);

    bObj.pushKV("Name", pbudgetProposal->GetName());
    bObj.pushKV("URL", pbudgetProposal->GetURL());
    bObj.pushKV("Hash", pbudgetProposal->GetHash().ToString());
    bObj.pushKV("FeeHash", pbudgetProposal->GetFeeTXHash().ToString());
    bObj.pushKV("BlockStart", (int64_t)pbudgetProposal->GetBlockStart());
    bObj.pushKV("BlockEnd", (int64_t)pbudgetProposal->GetBlockEnd());
    bObj.pushKV("TotalPaymentCount", (int64_t)pbudgetProposal->GetTotalPaymentCount());
    bObj.pushKV("RemainingPaymentCount", (int64_t)pbudgetProposal->GetRemainingPaymentCount(nCurrentHeight));
    bObj.pushKV("PaymentAddress", EncodeDestination(address1));
    bObj.pushKV("Ratio", pbudgetProposal->GetRatio());
    bObj.pushKV("Yeas", (int64_t)pbudgetProposal->GetYeas());
    bObj.pushKV("Nays", (int64_t)pbudgetProposal->GetNays());
    bObj.pushKV("Abstains", (int64_t)pbudgetProposal->GetAbstains());
    bObj.pushKV("TotalPayment", ValueFromAmount(pbudgetProposal->GetAmount() * pbudgetProposal->GetTotalPaymentCount()));
    bObj.pushKV("MonthlyPayment", ValueFromAmount(pbudgetProposal->GetAmount()));
    bObj.pushKV("IsEstablished", pbudgetProposal->IsEstablished());
    bool fValid = pbudgetProposal->IsValid();
    bObj.pushKV("IsValid", fValid);
    if (!fValid)
        bObj.pushKV("IsInvalidReason", pbudgetProposal->IsInvalidReason());
    bObj.pushKV("Allotted", ValueFromAmount(pbudgetProposal->GetAllotted()));
}

void checkBudgetInputs(const UniValue& params, std::string &strProposalName, std::string &strURL,
                       int &nPaymentCount, int &nBlockStart, CTxDestination &address, CAmount &nAmount)
{
    strProposalName = SanitizeString(params[0].get_str());
    if (strProposalName.size() > 20)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid proposal name, limit of 20 characters.");

    strURL = SanitizeString(params[1].get_str());
    std::string strErr;
    if (!validateURL(strURL, strErr))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strErr);

    nPaymentCount = params[2].get_int();
    if (nPaymentCount < 1)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid payment count, must be more than zero.");

    int nMaxPayments = Params().GetConsensus().nMaxProposalPayments;
    if (nPaymentCount > nMaxPayments) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                strprintf("Invalid payment count, must be <= %d", nMaxPayments));
    }

    CBlockIndex* pindexPrev = GetChainTip();
    if (!pindexPrev)
        throw JSONRPCError(RPC_IN_WARMUP, "Try again after active chain is loaded");

    // Start must be in the next budget cycle or later
    const int budgetCycleBlocks = Params().GetConsensus().nBudgetCycleBlocks;
    int pHeight = pindexPrev->nHeight;

    int nBlockMin = pHeight - (pHeight % budgetCycleBlocks) + budgetCycleBlocks;

    nBlockStart = params[3].get_int();
    if ((nBlockStart < nBlockMin) || ((nBlockStart % budgetCycleBlocks) != 0))
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid block start - must be a budget cycle block. Next valid block: %d", nBlockMin));

    address = DecodeDestination(params[4].get_str());
    if (!IsValidDestination(address))
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Invalid PIVX address");

    nAmount = AmountFromValue(params[5]);
    if (nAmount < 10 * COIN)
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid amount - Payment of %s is less than minimum 10 %s allowed", FormatMoney(nAmount), CURRENCY_UNIT));

    const CAmount& nTotalBudget = g_budgetman.GetTotalBudget(nBlockStart);
    if (nAmount > nTotalBudget)
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("Invalid amount - Payment of %s more than max of %s", FormatMoney(nAmount), FormatMoney(nTotalBudget)));
}

UniValue preparebudget(const JSONRPCRequest& request)
{
    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp || request.params.size() != 6)
        throw std::runtime_error(
            "preparebudget \"name\" \"url\" npayments start \"address\" monthly_payment\n"
            "\nPrepare proposal for network by signing and creating tx\n"

            "\nArguments:\n"
            "1. \"name\":        (string, required) Desired proposal name (20 character limit)\n"
            "2. \"url\":         (string, required) URL of proposal details (64 character limit)\n"
            "3. npayments:       (numeric, required) Total number of monthly payments\n"
            "4. start:           (numeric, required) Starting super block height\n"
            "5. \"address\":     (string, required) PIVX address to send payments to\n"
            "6. monthly_payment: (numeric, required) Monthly payment amount\n"

            "\nResult:\n"
            "\"xxxx\"       (string) proposal fee hash (if successful) or error message (if failed)\n"

            "\nExamples:\n" +
            HelpExampleCli("preparebudget", "\"test-proposal\" \"https://forum.pivx.org/t/test-proposal\" 2 820800 \"D9oc6C3dttUbv8zd7zGNq1qKBGf4ZQ1XEE\" 500") +
            HelpExampleRpc("preparebudget", "\"test-proposal\" \"https://forum.pivx.org/t/test-proposal\" 2 820800 \"D9oc6C3dttUbv8zd7zGNq1qKBGf4ZQ1XEE\" 500"));

    LOCK2(cs_main, pwallet->cs_wallet);

    EnsureWalletIsUnlocked(pwallet);

    std::string strProposalName;
    std::string strURL;
    int nPaymentCount;
    int nBlockStart;
    CTxDestination address;
    CAmount nAmount;

    checkBudgetInputs(request.params, strProposalName, strURL, nPaymentCount, nBlockStart, address, nAmount);

    // Parse PIVX address
    CScript scriptPubKey = GetScriptForDestination(address);

    // create transaction 15 minutes into the future, to allow for confirmation time
    CBudgetProposal proposal(strProposalName, strURL, nPaymentCount, scriptPubKey, nAmount, nBlockStart, UINT256_ZERO);
    const uint256& nHash = proposal.GetHash();
    if (!proposal.IsWellFormed(g_budgetman.GetTotalBudget(proposal.GetBlockStart())))
        throw std::runtime_error("Proposal is not valid " + proposal.IsInvalidReason());

    CTransactionRef wtx;
    // make our change address
    CReserveKey keyChange(pwallet);
    if (!pwallet->CreateBudgetFeeTX(wtx, nHash, keyChange, false)) { // 50 PIV collateral for proposal
        throw std::runtime_error("Error making collateral transaction for proposal. Please check your wallet balance.");
    }

    //send the tx to the network
    const CWallet::CommitResult& res = pwallet->CommitTransaction(wtx, keyChange, g_connman.get());
    if (res.status != CWallet::CommitStatus::OK)
        throw JSONRPCError(RPC_WALLET_ERROR, res.ToString());

    // Store proposal name as a comment
    assert(pwallet->mapWallet.count(wtx->GetHash()));
    pwallet->mapWallet.at(wtx->GetHash()).SetComment("Proposal: " + strProposalName);

    return wtx->GetHash().ToString();
}

UniValue submitbudget(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 7)
        throw std::runtime_error(
            "submitbudget \"name\" \"url\" npayments start \"address\" monthly_payment \"fee_txid\"\n"
            "\nSubmit proposal to the network\n"

            "\nArguments:\n"
            "1. \"name\":         (string, required) Desired proposal name (20 character limit)\n"
            "2. \"url\":          (string, required) URL of proposal details (64 character limit)\n"
            "3. npayments:        (numeric, required) Total number of monthly payments\n"
            "4. start:            (numeric, required) Starting super block height\n"
            "5. \"address\":      (string, required) PIVX address to send payments to\n"
            "6. monthly_payment:  (numeric, required) Monthly payment amount\n"
            "7. \"fee_txid\":     (string, required) Transaction hash from preparebudget command\n"

            "\nResult:\n"
            "\"xxxx\"       (string) proposal hash (if successful) or error message (if failed)\n"

            "\nExamples:\n" +
            HelpExampleCli("submitbudget", "\"test-proposal\" \"https://forum.pivx.org/t/test-proposal\" 2 820800 \"D9oc6C3dttUbv8zd7zGNq1qKBGf4ZQ1XEE\" 500") +
            HelpExampleRpc("submitbudget", "\"test-proposal\" \"https://forum.pivx.org/t/test-proposal\" 2 820800 \"D9oc6C3dttUbv8zd7zGNq1qKBGf4ZQ1XEE\" 500"));

    std::string strProposalName;
    std::string strURL;
    int nPaymentCount;
    int nBlockStart;
    CTxDestination address;
    CAmount nAmount;

    checkBudgetInputs(request.params, strProposalName, strURL, nPaymentCount, nBlockStart, address, nAmount);

    // Parse PIVX address
    CScript scriptPubKey = GetScriptForDestination(address);
    const uint256& hash = ParseHashV(request.params[6], "parameter 1");

    if (!masternodeSync.IsBlockchainSynced()) {
        throw std::runtime_error("Must wait for client to sync with masternode network. Try again in a minute or so.");
    }

    // create the proposal in case we're the first to make it
    CBudgetProposal proposal(strProposalName, strURL, nPaymentCount, scriptPubKey, nAmount, nBlockStart, hash);
    if(!g_budgetman.AddProposal(proposal)) {
        std::string strError = strprintf("invalid budget proposal - %s", proposal.IsInvalidReason());
        throw std::runtime_error(strError);
    }
    proposal.Relay();

    return proposal.GetHash().ToString();
}

static UniValue packRetStatus(const std::string& nodeType, const std::string& result, const std::string& error)
{
    UniValue statusObj(UniValue::VOBJ);
    statusObj.pushKV("node", nodeType);
    statusObj.pushKV("result", result);
    statusObj.pushKV("error", error);
    return statusObj;
}

static UniValue packErrorRetStatus(const std::string& nodeType, const std::string& error)
{
    return packRetStatus(nodeType, "failed", error);
}

static UniValue packVoteReturnValue(const UniValue& details, int success, int failed)
{
    UniValue returnObj(UniValue::VOBJ);
    returnObj.pushKV("overall", strprintf("Voted successfully %d time(s) and failed %d time(s).", success, failed));
    returnObj.pushKV("detail", details);
    return returnObj;
}

// key, alias and collateral outpoint of a masternode. Struct used to sign proposal/budget votes
struct MnKeyData
{
    std::string mnAlias;
    const COutPoint* collateralOut;
    CKey key;

    MnKeyData(const std::string& _mnAlias, const COutPoint* _collateralOut, const CKey& _key):
        mnAlias(_mnAlias),
        collateralOut(_collateralOut),
        key(_key)
    {}
};

typedef std::list<MnKeyData> mnKeyList;

static UniValue voteProposal(const uint256& propHash, const CBudgetVote::VoteDirection& nVote,
                             const mnKeyList& mnKeys, UniValue resultsObj, int failed)
{
    int success = 0;
    for (const auto& k : mnKeys) {
        CBudgetVote vote(CTxIn(*k.collateralOut), propHash, nVote);
        if (!vote.Sign(k.key, k.key.GetPubKey().GetID())) {
            resultsObj.push_back(packErrorRetStatus(k.mnAlias, "Failure to sign."));
            failed++;
            continue;
        }
        CValidationState state;
        if (!g_budgetman.ProcessProposalVote(vote, nullptr, state)) {
            resultsObj.push_back(packErrorRetStatus(k.mnAlias, FormatStateMessage(state)));
            failed++;
            continue;
        }
        resultsObj.push_back(packRetStatus(k.mnAlias, "success", ""));
        success++;
    }

    return packVoteReturnValue(resultsObj, success, failed);
}

static UniValue voteFinalBudget(const uint256& budgetHash,
                                const mnKeyList& mnKeys, UniValue resultsObj, int failed)
{
    int success = 0;
    for (const auto& k : mnKeys) {
        CFinalizedBudgetVote vote(CTxIn(*k.collateralOut), budgetHash);
        if (!vote.Sign(k.key, k.key.GetPubKey().GetID())) {
            resultsObj.push_back(packErrorRetStatus(k.mnAlias, "Failure to sign."));
            failed++;
            continue;
        }
        CValidationState state;
        if (!g_budgetman.ProcessFinalizedBudgetVote(vote, nullptr, state)) {
            resultsObj.push_back(packErrorRetStatus(k.mnAlias, FormatStateMessage(state)));
            failed++;
            continue;
        }
        resultsObj.push_back(packRetStatus(k.mnAlias, "success", ""));
        success++;
    }

    return packVoteReturnValue(resultsObj, success, failed);
}

// Legacy masternodes
static mnKeyList getMNKeys(const Optional<std::string>& mnAliasFilter,
                              UniValue& resultsObj, int& failed)
{
    mnKeyList mnKeys;
    for (const CMasternodeConfig::CMasternodeEntry& mne : masternodeConfig.getEntries()) {
        if (mnAliasFilter && *mnAliasFilter != mne.getAlias()) continue;
        CKey mnKey; CPubKey mnPubKey;
        const std::string& mnAlias = mne.getAlias();
        if (!CMessageSigner::GetKeysFromSecret(mne.getPrivKey(), mnKey, mnPubKey)) {
            resultsObj.push_back(packErrorRetStatus(mnAlias, "Could not get key from masternode.conf"));
            failed++;
            continue;
        }
        CMasternode* pmn = mnodeman.Find(mnPubKey);
        if (!pmn) {
            resultsObj.push_back(packErrorRetStatus(mnAlias, "Can't find masternode by pubkey"));
            failed++;
            continue;
        }
        mnKeys.emplace_back(mnAlias, &pmn->vin.prevout, mnKey);
    }
    return mnKeys;
}

static mnKeyList getMNKeysForActiveMasternode(UniValue& resultsObj)
{
    // local node must be a masternode
    if (!fMasterNode)
        throw JSONRPCError(RPC_MISC_ERROR, _("This is not a masternode. 'local' option disabled."));

    if (activeMasternode.vin == nullopt)
        throw JSONRPCError(RPC_MISC_ERROR, _("Active Masternode not initialized."));

    CKey mnKey; CPubKey mnPubKey;
    activeMasternode.GetKeys(mnKey, mnPubKey);
    CMasternode* pmn = mnodeman.Find(mnPubKey);
    if (!pmn) {
        resultsObj.push_back(packErrorRetStatus("local", "Can't find masternode by pubkey"));
        return mnKeyList();
    }

    return {MnKeyData("local", &pmn->vin.prevout, mnKey)};
}

// Deterministic masternodes
static mnKeyList getDMNKeys(CWallet* const pwallet, const Optional<std::string>& mnAliasFilter, bool fFinal, UniValue& resultsObj, int& failed)
{
    if (!pwallet) {
        throw JSONRPCError(RPC_IN_WARMUP, "Wallet (with voting key) not found.");
    }

    auto mnList = deterministicMNManager->GetListAtChainTip();

    CDeterministicMNCPtr mnFilter{nullptr};
    if (mnAliasFilter) {
        // vote with a single masternode (identified by ProTx)
        const uint256& proTxHash = ParseHashV(*mnAliasFilter, "ProTX transaction hash");
        mnFilter = mnList.GetValidMN(proTxHash);
        if (!mnFilter) {
            resultsObj.push_back(packErrorRetStatus(*mnAliasFilter, "Invalid or unknown proTxHash"));
            failed++;
            return mnKeyList();
        }
    }

    LOCK(pwallet->cs_wallet);
    EnsureWalletIsUnlocked(pwallet);

    mnKeyList mnKeys;
    mnList.ForEachMN(true, [&](const CDeterministicMNCPtr& dmn) {
        bool filtered = mnFilter && dmn->proTxHash == mnFilter->proTxHash;
        if (!mnFilter || filtered) {
            const CKeyID& mnKeyID = fFinal ? dmn->pdmnState->keyIDOperator : dmn->pdmnState->keyIDVoting;
            CKey mnKey;
            if (pwallet->GetKey(mnKeyID, mnKey)) {
                mnKeys.emplace_back(dmn->proTxHash.ToString(), &dmn->collateralOutpoint, mnKey);
            } else if (filtered) {
                resultsObj.push_back(packErrorRetStatus(*mnAliasFilter, strprintf(
                                     "Private key for voting address %s not known by this wallet", EncodeDestination(mnKeyID))));
                failed++;
            }
        }
    });

    return mnKeys;
}

static mnKeyList getDMNKeysForActiveMasternode(UniValue& resultsObj)
{
    // local node must be a masternode
    if (!activeMasternodeManager)
        throw JSONRPCError(RPC_MISC_ERROR, _("This is not a deterministic masternode. 'local' option disabled."));

    CKey dmnKey; CKeyID dmnKeyID; CDeterministicMNCPtr dmn;
    auto res = activeMasternodeManager->GetOperatorKey(dmnKey, dmnKeyID, dmn);
    if (!res) {
        resultsObj.push_back(packErrorRetStatus("local", res.getError()));
        return {};
    }

    return {MnKeyData("local", &dmn->collateralOutpoint, dmnKey)};
}

// vote on proposal (finalized budget, if fFinal=true) with all possible keys or a single mn (mnAliasFilter)
static UniValue mnBudgetVoteInner(CWallet* const pwallet, bool fLegacyMN, const uint256& budgetHash, bool fFinal,
                                  const CBudgetVote::VoteDirection& nVote, const Optional<std::string>& mnAliasFilter)
{
    UniValue resultsObj(UniValue::VARR);
    int failed = 0;

    mnKeyList mnKeys = fLegacyMN ? getMNKeys(mnAliasFilter, resultsObj, failed)
                                 : getDMNKeys(pwallet, mnAliasFilter, fFinal, resultsObj, failed);

    if (mnKeys.empty()) {
        return packVoteReturnValue(resultsObj, 0, failed);
    }

    return (fFinal ? voteFinalBudget(budgetHash, mnKeys, resultsObj, failed)
                   : voteProposal(budgetHash, nVote, mnKeys, resultsObj, failed));
}

// vote on proposal (finalized budget, if fFinal=true) with the active local masternode
// Note: for DMNs only finalized budget voting is allowed with the operator key
// (proposal voting requires the voting key)
static UniValue mnLocalBudgetVoteInner(bool fLegacyMN, const uint256& budgetHash, bool fFinal,
                                       const CBudgetVote::VoteDirection& nVote)
{
    UniValue resultsObj(UniValue::VARR);

    mnKeyList mnKeys = fLegacyMN ? getMNKeysForActiveMasternode(resultsObj)
                                 : getDMNKeysForActiveMasternode(resultsObj);

    if (mnKeys.empty()) {
        return packVoteReturnValue(resultsObj, 0, 1);
    }

    return (fFinal ? voteFinalBudget(budgetHash, mnKeys, resultsObj, 0)
                   : voteProposal(budgetHash, nVote, mnKeys, resultsObj, 0));
}

static CBudgetVote::VoteDirection parseVote(const std::string& strVote)
{
    if (strVote != "yes" && strVote != "no") throw JSONRPCError(RPC_MISC_ERROR, "You can only vote 'yes' or 'no'");
    CBudgetVote::VoteDirection nVote = CBudgetVote::VOTE_ABSTAIN;
    if (strVote == "yes") nVote = CBudgetVote::VOTE_YES;
    if (strVote == "no") nVote = CBudgetVote::VOTE_NO;
    return nVote;
}

UniValue mnbudgetvote(const JSONRPCRequest& request)
{
    std::string strCommand;
    if (request.params.size() >= 1) {
        strCommand = request.params[0].get_str();

        // Backwards compatibility with legacy `mnbudget` command
        if (strCommand == "vote") strCommand = "local";
        if (strCommand == "vote-many") strCommand = "many";
        if (strCommand == "vote-alias") strCommand = "alias";
    }

    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (request.fHelp || (request.params.size() == 3 && (strCommand != "local" && strCommand != "many")) || (request.params.size() == 4 && strCommand != "alias") ||
        request.params.size() > 5 || request.params.size() < 3)
        throw std::runtime_error(
            "mnbudgetvote \"local|many|alias\" \"hash\" \"yes|no\" ( \"alias\" legacy )\n"
            "\nVote on a budget proposal\n"
            "\nAfter V6 enforcement, the deterministic masternode system is used by default. Set the \"legacy\" parameter to true to vote with legacy masternodes."

            "\nArguments:\n"
            "1. \"mode\"      (string, required) The voting mode. 'local' for voting directly from a masternode, 'many' for voting with a MN controller and casting the same vote for each MN, 'alias' for voting with a MN controller and casting a vote for a single MN\n"
            "2. \"hash\"      (string, required) The budget proposal hash\n"
            "3. \"votecast\"  (string, required) Your vote. 'yes' to vote for the proposal, 'no' to vote against\n"
            "4. \"alias\"     (string, required for 'alias' mode) The MN alias to cast a vote for (for deterministic masternodes it's the hash of the proTx transaction).\n"
            "5. \"legacy\"    (boolean, optional, default=false) Use the legacy masternode system after deterministic masternodes enforcement.\n"

            "\nResult:\n"
            "{\n"
            "  \"overall\": \"xxxx\",      (string) The overall status message for the vote cast\n"
            "  \"detail\": [\n"
            "    {\n"
            "      \"node\": \"xxxx\",      (string) 'local' or the MN alias\n"
            "      \"result\": \"xxxx\",    (string) Either 'Success' or 'Failed'\n"
            "      \"error\": \"xxxx\",     (string) Error message, if vote failed\n"
            "    }\n"
            "    ,...\n"
            "  ]\n"
            "}\n"

            "\nExamples:\n" +
            HelpExampleCli("mnbudgetvote", "\"alias\" \"ed2f83cedee59a91406f5f47ec4d60bf5a7f9ee6293913c82976bd2d3a658041\" \"yes\" \"4f9de28fca1f0574a217c5d3c59cc51125ec671de82a2f80b6ceb69673115041\"") +
            HelpExampleRpc("mnbudgetvote", "\"alias\" \"ed2f83cedee59a91406f5f47ec4d60bf5a7f9ee6293913c82976bd2d3a658041\" \"yes\" \"4f9de28fca1f0574a217c5d3c59cc51125ec671de82a2f80b6ceb69673115041\""));

    const uint256& hash = ParseHashV(request.params[1], "parameter 1");
    CBudgetVote::VoteDirection nVote = parseVote(request.params[2].get_str());

    bool fLegacyMN = !deterministicMNManager->IsDIP3Enforced() || (request.params.size() > 4 && request.params[4].get_bool());

    if (strCommand == "local") {
        if (!fLegacyMN) {
            throw JSONRPCError(RPC_MISC_ERROR, _("\"local\" vote is no longer available with DMNs. Use \"alias\" from the wallet with the voting key."));
        }
        return mnLocalBudgetVoteInner(true, hash, false, nVote);
    }

    // DMN require wallet with voting key
    if (!fLegacyMN && !EnsureWalletIsAvailable(pwallet, false)) {
        return NullUniValue;
    }

    bool isAlias = false;
    if (strCommand == "many" || (isAlias = strCommand == "alias")) {
        Optional<std::string> mnAlias = isAlias ? Optional<std::string>(request.params[3].get_str()) : nullopt;
        return mnBudgetVoteInner(pwallet, fLegacyMN, hash, false, nVote, mnAlias);
    }

    return NullUniValue;
}

UniValue getbudgetvotes(const JSONRPCRequest& request)
{
    if (request.params.size() != 1)
        throw std::runtime_error(
            "getbudgetvotes \"name\"\n"
            "\nPrint vote information for a budget proposal\n"

            "\nArguments:\n"
            "1. \"name\":      (string, required) Name of the proposal\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"mnId\": \"xxxx-x\",      (string) Masternode's outpoint collateral transaction (hash-n)\n"
            "    \"nHash\": \"xxxx\",       (string) Hash of the vote\n"
            "    \"Vote\": \"YES|NO\",      (string) Vote cast ('YES' or 'NO')\n"
            "    \"nTime\": xxxx,         (numeric) Time in seconds since epoch the vote was cast\n"
            "    \"fValid\": true|false,  (boolean) 'true' if the vote is valid, 'false' otherwise\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("getbudgetvotes", "\"test-proposal\"") + HelpExampleRpc("getbudgetvotes", "\"test-proposal\""));

    std::string strProposalName = SanitizeString(request.params[0].get_str());
    const CBudgetProposal* pbudgetProposal = g_budgetman.FindProposalByName(strProposalName);
    if (pbudgetProposal == NULL) throw std::runtime_error("Unknown proposal name");
    return pbudgetProposal->GetVotesArray();
}

UniValue getnextsuperblock(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getnextsuperblock\n"
            "\nPrint the next super block height\n"

            "\nResult:\n"
            "n      (numeric) Block height of the next super block\n"

            "\nExamples:\n" +
            HelpExampleCli("getnextsuperblock", "") + HelpExampleRpc("getnextsuperblock", ""));

    int nChainHeight = WITH_LOCK(cs_main, return chainActive.Height());
    if (nChainHeight < 0) return "unknown";

    const int nBlocksPerCycle = Params().GetConsensus().nBudgetCycleBlocks;
    int nNext = nChainHeight - nChainHeight % nBlocksPerCycle + nBlocksPerCycle;
    return nNext;
}

UniValue getbudgetprojection(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "getbudgetprojection\n"
            "\nShow the projection of which proposals will be paid the next cycle\n"
            "Proposal fee tx time need to be +24hrs old from the current time. (Testnet is 5 mins)\n"
            "Net Votes needs to be above Masternode Count divided by 10\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"Name\": \"xxxx\",               (string) Proposal Name\n"
            "    \"URL\": \"xxxx\",                (string) Proposal URL\n"
            "    \"Hash\": \"xxxx\",               (string) Proposal vote hash\n"
            "    \"FeeHash\": \"xxxx\",            (string) Proposal fee hash\n"
            "    \"BlockStart\": n,              (numeric) Proposal starting block\n"
            "    \"BlockEnd\": n,                (numeric) Proposal ending block\n"
            "    \"TotalPaymentCount\": n,       (numeric) Number of payments\n"
            "    \"RemainingPaymentCount\": n,   (numeric) Number of remaining payments\n"
            "    \"PaymentAddress\": \"xxxx\",     (string) PIVX address of payment\n"
            "    \"Ratio\": x.xxx,               (numeric) Ratio of yeas vs nays\n"
            "    \"Yeas\": n,                    (numeric) Number of yea votes\n"
            "    \"Nays\": n,                    (numeric) Number of nay votes\n"
            "    \"Abstains\": n,                (numeric) Number of abstains\n"
            "    \"TotalPayment\": xxx.xxx,      (numeric) Total payment amount in PIV\n"
            "    \"MonthlyPayment\": xxx.xxx,    (numeric) Monthly payment amount in PIV\n"
            "    \"IsEstablished\": true|false,  (boolean) Proposal is considered established, 24 hrs after being submitted to network. (Testnet is 5 mins)\n"
            "    \"IsValid\": true|false,        (boolean) Valid (true) or Invalid (false)\n"
            "    \"IsInvalidReason\": \"xxxx\",  (string) Error message, if any\n"
            "    \"Allotted\": xxx.xxx,           (numeric) Amount of PIV allotted in current period\n"
            "    \"TotalBudgetAllotted\": xxx.xxx (numeric) Total PIV allotted\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("getbudgetprojection", "") + HelpExampleRpc("getbudgetprojection", ""));

    UniValue ret(UniValue::VARR);
    UniValue resultObj(UniValue::VOBJ);
    CAmount nTotalAllotted = 0;

    std::vector<CBudgetProposal> winningProps = g_budgetman.GetBudget();
    for (const CBudgetProposal& p : winningProps) {
        UniValue bObj(UniValue::VOBJ);
        budgetToJSON(&p, bObj, g_budgetman.GetBestHeight());
        nTotalAllotted += p.GetAllotted();
        bObj.pushKV("TotalBudgetAllotted", ValueFromAmount(nTotalAllotted));
        ret.push_back(bObj);
    }

    return ret;
}

UniValue getbudgetinfo(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 1)
        throw std::runtime_error(
            "getbudgetinfo ( \"name\" )\n"
            "\nShow current masternode budgets\n"

            "\nArguments:\n"
            "1. \"name\"    (string, optional) Proposal name\n"

            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"Name\": \"xxxx\",               (string) Proposal Name\n"
            "    \"URL\": \"xxxx\",                (string) Proposal URL\n"
            "    \"Hash\": \"xxxx\",               (string) Proposal vote hash\n"
            "    \"FeeHash\": \"xxxx\",            (string) Proposal fee hash\n"
            "    \"BlockStart\": n,              (numeric) Proposal starting block\n"
            "    \"BlockEnd\": n,                (numeric) Proposal ending block\n"
            "    \"TotalPaymentCount\": n,       (numeric) Number of payments\n"
            "    \"RemainingPaymentCount\": n,   (numeric) Number of remaining payments\n"
            "    \"PaymentAddress\": \"xxxx\",     (string) PIVX address of payment\n"
            "    \"Ratio\": x.xxx,               (numeric) Ratio of yeas vs nays\n"
            "    \"Yeas\": n,                    (numeric) Number of yea votes\n"
            "    \"Nays\": n,                    (numeric) Number of nay votes\n"
            "    \"Abstains\": n,                (numeric) Number of abstains\n"
            "    \"TotalPayment\": xxx.xxx,      (numeric) Total payment amount in PIV\n"
            "    \"MonthlyPayment\": xxx.xxx,    (numeric) Monthly payment amount in PIV\n"
            "    \"IsEstablished\": true|false,  (boolean) Proposal is considered established, 24 hrs after being submitted to network. (5 mins for Testnet)\n"
            "    \"IsValid\": true|false,        (boolean) Valid (true) or Invalid (false)\n"
            "    \"IsInvalidReason\": \"xxxx\",      (string) Error message, if any\n"
            "  }\n"
            "  ,...\n"
            "]\n"

            "\nExamples:\n" +
            HelpExampleCli("getbudgetprojection", "") + HelpExampleRpc("getbudgetprojection", ""));

    UniValue ret(UniValue::VARR);
    int nCurrentHeight = g_budgetman.GetBestHeight();

    if (request.params.size() == 1) {
        std::string strProposalName = SanitizeString(request.params[0].get_str());
        const CBudgetProposal* pbudgetProposal = g_budgetman.FindProposalByName(strProposalName);
        if (pbudgetProposal == NULL) throw std::runtime_error("Unknown proposal name");
        UniValue bObj(UniValue::VOBJ);
        budgetToJSON(pbudgetProposal, bObj, nCurrentHeight);
        ret.push_back(bObj);
        return ret;
    }

    std::vector<CBudgetProposal*> winningProps = g_budgetman.GetAllProposals();
    for (CBudgetProposal* pbudgetProposal : winningProps) {
        if (!pbudgetProposal->IsValid()) continue;

        UniValue bObj(UniValue::VOBJ);
        budgetToJSON(pbudgetProposal, bObj, nCurrentHeight);
        ret.push_back(bObj);
    }

    return ret;
}

UniValue mnbudgetrawvote(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 6)
        throw std::runtime_error(
            "mnbudgetrawvote \"collat_txid\" collat_vout \"hash\" votecast time \"sig\"\n"
            "\nCompile and relay a proposal vote with provided external signature instead of signing vote internally\n"

            "\nArguments:\n"
            "1. \"collat_txid\"   (string, required) Transaction hash for the masternode collateral\n"
            "2. collat_vout       (numeric, required) Output index for the masternode collateral\n"
            "3. \"hash\"          (string, required) Budget Proposal hash\n"
            "4. \"votecast\"      (string, required) Your vote. 'yes' to vote for the proposal, 'no' to vote against\n"
            "5. time              (numeric, required) Time since epoch in seconds\n"
            "6. \"sig\"           (string, required) External signature\n"

            "\nResult:\n"
            "\"status\"     (string) Vote status or error message\n"

            "\nExamples:\n" +
            HelpExampleCli("mnbudgetrawvote", "") + HelpExampleRpc("mnbudgetrawvote", ""));

    const uint256& hashMnTx = ParseHashV(request.params[0], "mn tx hash");
    int nMnTxIndex = request.params[1].get_int();
    const CTxIn& vin = CTxIn(hashMnTx, nMnTxIndex);

    const uint256& hashProposal = ParseHashV(request.params[2], "Proposal hash");
    CBudgetVote::VoteDirection nVote = parseVote(request.params[3].get_str());

    int64_t nTime = request.params[4].get_int64();
    std::string strSig = request.params[5].get_str();
    bool fInvalid = false;
    std::vector<unsigned char> vchSig = DecodeBase64(strSig.c_str(), &fInvalid);

    if (fInvalid)
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Malformed base64 encoding");

    CMasternode* pmn = mnodeman.Find(vin.prevout);
    if (!pmn) {
        return "Failure to find masternode in list : " + vin.ToString();
    }

    CBudgetVote vote(vin, hashProposal, nVote);
    vote.SetTime(nTime);
    vote.SetVchSig(vchSig);

    if (!vote.CheckSignature(pmn->pubKeyMasternode.GetID())) {
        return "Failure to verify signature.";
    }

    std::string strError;
    if (g_budgetman.AddAndRelayProposalVote(vote, strError)) {
        return "Voted successfully";
    } else {
        return "Error voting : " + strError;
    }
}

UniValue mnfinalbudgetsuggest(const JSONRPCRequest& request)
{
    if (request.fHelp || !request.params.empty())
        throw std::runtime_error(
                "mnfinalbudgetsuggest\n"
                "\nTry to submit a budget finalization\n"
                "returns the budget hash if it was broadcasted sucessfully");

    if (!Params().IsRegTestNet()) {
        throw JSONRPCError(RPC_MISC_ERROR, "command available only for RegTest network");
    }

    const uint256& budgetHash = g_budgetman.SubmitFinalBudget();
    return (budgetHash.IsNull()) ? NullUniValue : budgetHash.ToString();
}

UniValue createrawmnfinalbudget(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 4)
        throw std::runtime_error(
                "createrawmnfinalbudget\n"
                "\nTry to submit the raw budget finalization\n"
                "returns the budget hash if it was broadcasted sucessfully"
                "\nArguments:\n"
                "1. \"budgetname\"    (string, required) finalization name\n"
                "2. \"blockstart\"    (numeric, required) superblock height\n"
                "3. \"proposals\"     (string, required) A json array of json objects\n"
                "     [\n"
                "       {\n"
                "         \"proposalid\":\"id\",  (string, required) The proposal id\n"
                "         \"payee\":n,         (hex, required) The payee script\n"
                "         \"amount\":n            (numeric, optional) The payee amount\n"
                "       }\n"
                "       ,...\n"
                "     ]\n"
                "4. \"feetxid\"    (string, optional) the transaction fee hash\n"
                ""
                "\nResult:\n"
                "{\n"
                    "\"result\"     (string) Budget suggest broadcast or error\n"
                    "\"id\"         (string) id of the fee tx or the finalized budget\n"
                "}\n"
                ); // future: add examples.

    if (!Params().IsRegTestNet()) {
        throw JSONRPCError(RPC_MISC_ERROR, "command available only for RegTest network");
    }

    // future: add inputs error checking..
    std::string budName = request.params[0].get_str();
    int nBlockStart = request.params[1].get_int();
    std::vector<CTxBudgetPayment> vecTxBudgetPayments;
    UniValue budgetVec = request.params[2].get_array();
    for (unsigned int idx = 0; idx < budgetVec.size(); idx++) {
        const UniValue& prop = budgetVec[idx].get_obj();
        uint256 propId = ParseHashO(prop, "proposalid");
        std::vector<unsigned char> scriptData(ParseHexO(prop, "payee"));
        CScript payee = CScript(scriptData.begin(), scriptData.end());
        CAmount amount = AmountFromValue(find_value(prop, "amount"));
        vecTxBudgetPayments.emplace_back(propId, payee, amount);
    }

    Optional<uint256> txFeeId = nullopt;
    if (request.params.size() > 3) {
        txFeeId = ParseHashV(request.params[3], "parameter 4");
    }

    if (!txFeeId) {
        CFinalizedBudget tempBudget(budName, nBlockStart, vecTxBudgetPayments, UINT256_ZERO);
        const uint256& budgetHash = tempBudget.GetHash();

        // create fee tx
        CTransactionRef wtx;
        CReserveKey keyChange(vpwallets[0]);
        if (!vpwallets[0]->CreateBudgetFeeTX(wtx, budgetHash, keyChange, true)) {
            throw std::runtime_error("Can't make collateral transaction");
        }
        // Send the tx to the network
        const CWallet::CommitResult& res = vpwallets[0]->CommitTransaction(wtx, keyChange, g_connman.get());
        UniValue ret(UniValue::VOBJ);
        if (res.status == CWallet::CommitStatus::OK) {
            ret.pushKV("result", "tx_fee_sent");
            ret.pushKV("id", wtx->GetHash().ToString());
        } else {
            ret.pushKV("result", "error");
        }
        return ret;
    }

    UniValue ret(UniValue::VOBJ);
    // Collateral tx already exists, see if it's mature enough.
    CFinalizedBudget fb(budName, nBlockStart, vecTxBudgetPayments, *txFeeId);
    if (g_budgetman.AddFinalizedBudget(fb)) {
        fb.Relay();
        ret.pushKV("result", "fin_budget_sent");
        ret.pushKV("id", fb.GetHash().ToString());
    } else {
        // future: add proper error
        ret.pushKV("result", "error");
    }
    return ret;
}

UniValue mnfinalbudget(const JSONRPCRequest& request)
{
    std::string strCommand;
    if (request.params.size() >= 1)
        strCommand = request.params[0].get_str();

    CWallet * const pwallet = GetWalletForJSONRPCRequest(request);

    if (request.fHelp ||
        (strCommand != "vote-many" && strCommand != "vote" && strCommand != "show" && strCommand != "getvotes"))
        throw std::runtime_error(
            "mnfinalbudget \"command\"... ( \"passphrase\" )\n"
            "\nVote or show current budgets\n"

            "\nAvailable commands:\n"
            "  vote-many   - Vote on a finalized budget\n"
            "  vote        - Vote on a finalized budget with local masternode\n"
            "  show        - Show existing finalized budgets\n"
            "  getvotes     - Get vote information for each finalized budget\n");

    if (strCommand == "vote-many" || strCommand == "vote") {
        if (request.params.size() < 2 || request.params.size() > 3) {
            throw std::runtime_error(strprintf("Correct usage is 'mnfinalbudget %s BUDGET_HASH (fLegacy)'", strCommand));
        }
        const uint256& hash = ParseHashV(request.params[1], "BUDGET_HASH");
        bool fLegacyMN = !deterministicMNManager->IsDIP3Enforced() || (request.params.size() > 2 && request.params[2].get_bool());

        // DMN require wallet with operator keys for vote-many
        if (!fLegacyMN && strCommand == "vote-many" && !EnsureWalletIsAvailable(pwallet, false)) {
            return NullUniValue;
        }

        return (strCommand == "vote-many" ? mnBudgetVoteInner(pwallet, fLegacyMN, hash, true, CBudgetVote::VOTE_YES, nullopt)
                                          : mnLocalBudgetVoteInner(fLegacyMN, hash, true, CBudgetVote::VOTE_YES));
    }

    if (strCommand == "show") {
        UniValue resultObj(UniValue::VOBJ);

        std::vector<CFinalizedBudget*> winningFbs = g_budgetman.GetFinalizedBudgets();
        for (CFinalizedBudget* finalizedBudget : winningFbs) {
            const uint256& nHash = finalizedBudget->GetHash();
            UniValue bObj(UniValue::VOBJ);
            bObj.pushKV("FeeTX", finalizedBudget->GetFeeTXHash().ToString());
            bObj.pushKV("BlockStart", (int64_t)finalizedBudget->GetBlockStart());
            bObj.pushKV("BlockEnd", (int64_t)finalizedBudget->GetBlockEnd());
            bObj.pushKV("Proposals", finalizedBudget->GetProposalsStr());
            bObj.pushKV("VoteCount", (int64_t)finalizedBudget->GetVoteCount());
            bObj.pushKV("Status", g_budgetman.GetFinalizedBudgetStatus(nHash));

            bool fValid = finalizedBudget->IsValid();
            bObj.pushKV("IsValid", fValid);
            if (!fValid)
                bObj.pushKV("IsInvalidReason", finalizedBudget->IsInvalidReason());

            std::string strName = strprintf("%s (%s)", finalizedBudget->GetName(), nHash.ToString());
            resultObj.pushKV(strName, bObj);
        }

        return resultObj;
    }

    if (strCommand == "getvotes") {
        if (request.params.size() != 2)
            throw std::runtime_error("Correct usage is 'mnbudget getvotes budget-hash'");

        LOCK(g_budgetman.cs_budgets);
        std::string strHash = request.params[1].get_str();
        uint256 hash(uint256S(strHash));
        CFinalizedBudget* pfinalBudget = g_budgetman.FindFinalizedBudget(hash);
        if (pfinalBudget == NULL) return "Unknown budget hash";
        return pfinalBudget->GetVotesObject();
    }

    return NullUniValue;
}

UniValue checkbudgets(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0)
        throw std::runtime_error(
            "checkbudgets\n"
            "\nInitiates a budget check cycle manually\n"

            "\nExamples:\n" +
            HelpExampleCli("checkbudgets", "") + HelpExampleRpc("checkbudgets", ""));

    if (!masternodeSync.IsSynced())
        throw JSONRPCError(RPC_CLIENT_IN_INITIAL_DOWNLOAD, "Masternode/Budget sync not finished yet");

    g_budgetman.CheckAndRemove();
    return NullUniValue;
}

static const CRPCCommand commands[] =
{ //  category              name                      actor (function)         okSafe argNames
  //  --------------------- ------------------------  -----------------------  ------ --------
    { "budget",             "checkbudgets",           &checkbudgets,           true,  {} },
    { "budget",             "getbudgetinfo",          &getbudgetinfo,          true,  {"name"} },
    { "budget",             "getbudgetprojection",    &getbudgetprojection,    true,  {} },
    { "budget",             "getbudgetvotes",         &getbudgetvotes,         true,  {"name"} },
    { "budget",             "getnextsuperblock",      &getnextsuperblock,      true,  {} },
    { "budget",             "mnbudgetrawvote",        &mnbudgetrawvote,        true,  {"collat_txid","collat_vout","hash","votecast","time","sig"} },
    { "budget",             "mnbudgetvote",           &mnbudgetvote,           true,  {"mode","hash","votecast","alias","legacy"} },
    { "budget",             "mnfinalbudget",          &mnfinalbudget,          true,  {"command"} },
    { "budget",             "preparebudget",          &preparebudget,          true,  {"name","url","npayments","start","address","monthly_payment"} },
    { "budget",             "submitbudget",           &submitbudget,           true,  {"name","url","npayments","start","address","monthly_payment","fee_txid"}  },

    /* Not shown in help */
    { "hidden",             "mnfinalbudgetsuggest",   &mnfinalbudgetsuggest,   true,  {} },
    { "hidden",             "createrawmnfinalbudget", &createrawmnfinalbudget,   true,  {"budgetname", "blockstart", "proposals", "feetxid"} },

};

void RegisterBudgetRPCCommands(CRPCTable &tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
