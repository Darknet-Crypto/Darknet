// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "budget/budgetmanager.h"

#include "consensus/validation.h"
#include "evo/deterministicmns.h"
#include "masternode-sync.h"
#include "masternodeman.h"
#include "net_processing.h"
#include "netmessagemaker.h"
#include "validation.h"   // GetTransaction, cs_main


CBudgetManager g_budgetman;

std::map<uint256, int64_t> askedForSourceProposalOrBudget;

// Used to check both proposals and finalized-budgets collateral txes
bool CheckCollateral(const uint256& nTxCollateralHash, const uint256& nExpectedHash, std::string& strError, int64_t& nTime, int nCurrentHeight, bool fBudgetFinalization);

void CBudgetManager::CheckOrphanVotes()
{
    std::string strError = "";
    {
        LOCK(cs_votes);
        for (auto it = mapOrphanProposalVotes.begin(); it != mapOrphanProposalVotes.end();) {
            if (UpdateProposal(it->second, nullptr, strError))
                it = mapOrphanProposalVotes.erase(it);
            else
                ++it;
        }
    }
    {
        LOCK(cs_finalizedvotes);
        for (auto it = mapOrphanFinalizedBudgetVotes.begin(); it != mapOrphanFinalizedBudgetVotes.end();) {
            if (UpdateFinalizedBudget(it->second, nullptr, strError))
                it = mapOrphanFinalizedBudgetVotes.erase(it);
            else
                ++it;
        }
    }
    LogPrint(BCLog::MNBUDGET,"%s: Done\n", __func__);
}

uint256 CBudgetManager::SubmitFinalBudget()
{
    static int nSubmittedHeight = 0; // height at which final budget was submitted last time
    int nCurrentHeight = GetBestHeight();

    const int nBlocksPerCycle = Params().GetConsensus().nBudgetCycleBlocks;
    int nBlockStart = nCurrentHeight - nCurrentHeight % nBlocksPerCycle + nBlocksPerCycle;
    if (nSubmittedHeight >= nBlockStart){
        LogPrint(BCLog::MNBUDGET,"%s: nSubmittedHeight(=%ld) < nBlockStart(=%ld) condition not fulfilled.\n",
                __func__, nSubmittedHeight, nBlockStart);
        return UINT256_ZERO;
    }

     // Submit final budget during the last 2 days (2880 blocks) before payment for Mainnet, about 9 minutes (9 blocks) for Testnet
    int finalizationWindow = ((nBlocksPerCycle / 30) * 2);

    if (Params().IsTestnet()) {
        // NOTE: 9 blocks for testnet is way to short to have any masternode submit an automatic vote on the finalized(!) budget,
        //       because those votes are only submitted/relayed once every 56 blocks in CFinalizedBudget::AutoCheck()

        finalizationWindow = 64; // 56 + 4 finalization confirmations + 4 minutes buffer for propagation
    }

    int nFinalizationStart = nBlockStart - finalizationWindow;

    int nOffsetToStart = nFinalizationStart - nCurrentHeight;

    if (nBlockStart - nCurrentHeight > finalizationWindow) {
        LogPrint(BCLog::MNBUDGET,"%s: Too early for finalization. Current block is %ld, next Superblock is %ld.\n", __func__, nCurrentHeight, nBlockStart);
        LogPrint(BCLog::MNBUDGET,"%s: First possible block for finalization: %ld. Last possible block for finalization: %ld. "
                "You have to wait for %ld block(s) until Budget finalization will be possible\n", __func__, nFinalizationStart, nBlockStart, nOffsetToStart);
        return UINT256_ZERO;
    }

    std::vector<CBudgetProposal> vBudgetProposals = GetBudget();
    std::string strBudgetName = "main";
    std::vector<CTxBudgetPayment> vecTxBudgetPayments;

    for (const auto& p : vBudgetProposals) {
        CTxBudgetPayment txBudgetPayment;
        txBudgetPayment.nProposalHash = p.GetHash();
        txBudgetPayment.payee = p.GetPayee();
        txBudgetPayment.nAmount = p.GetAllotted();
        vecTxBudgetPayments.push_back(txBudgetPayment);
    }

    if (vecTxBudgetPayments.size() < 1) {
        LogPrint(BCLog::MNBUDGET,"%s: Found No Proposals For Period\n", __func__);
        return UINT256_ZERO;
    }

    CFinalizedBudget tempBudget(strBudgetName, nBlockStart, vecTxBudgetPayments, UINT256_ZERO);
    const uint256& budgetHash = tempBudget.GetHash();
    if (HaveFinalizedBudget(budgetHash)) {
        LogPrint(BCLog::MNBUDGET,"%s: Budget already exists - %s\n", __func__, budgetHash.ToString());
        nSubmittedHeight = nCurrentHeight;
        return UINT256_ZERO;
    }

    // See if collateral tx exists
    if (!mapUnconfirmedFeeTx.count(budgetHash)) {
        // create the collateral tx, send it to the network and return
        CTransactionRef wtx;
        // Get our change address
        if (vpwallets.empty() || !vpwallets[0]) {
            LogPrint(BCLog::MNBUDGET,"%s: Wallet not found\n", __func__);
            return UINT256_ZERO;
        }
        CReserveKey keyChange(vpwallets[0]);
        if (!vpwallets[0]->CreateBudgetFeeTX(wtx, budgetHash, keyChange, true)) {
            LogPrint(BCLog::MNBUDGET,"%s: Can't make collateral transaction\n", __func__);
            return UINT256_ZERO;
        }
        // Send the tx to the network
        const CWallet::CommitResult& res = vpwallets[0]->CommitTransaction(wtx, keyChange, g_connman.get());
        if (res.status == CWallet::CommitStatus::OK) {
            const uint256& collateraltxid = wtx->GetHash();
            mapUnconfirmedFeeTx.emplace(budgetHash, collateraltxid);
            LogPrint(BCLog::MNBUDGET,"%s: Collateral sent. txid: %s\n", __func__, collateraltxid.ToString());
            return budgetHash;
        }
        return UINT256_ZERO;
    }

    // Collateral tx already exists, see if it's mature enough.
    CFinalizedBudget fb(strBudgetName, nBlockStart, vecTxBudgetPayments, mapUnconfirmedFeeTx.at(budgetHash));
    if (!AddFinalizedBudget(fb)) {
        return UINT256_ZERO;
    }
    fb.Relay();
    nSubmittedHeight = nCurrentHeight;
    LogPrint(BCLog::MNBUDGET,"%s: Done! %s\n", __func__, budgetHash.ToString());
    return budgetHash;
}

void CBudgetManager::SetBudgetProposalsStr(CFinalizedBudget& finalizedBudget) const
{
    const std::vector<uint256>& vHashes = finalizedBudget.GetProposalsHashes();
    std::string strProposals = "";
    {
        LOCK(cs_proposals);
        for (const uint256& hash: vHashes) {
            const std::string token = (mapProposals.count(hash) ? mapProposals.at(hash).GetName() : hash.ToString());
            strProposals += (strProposals == "" ? "" : ", ") + token;
        }
    }
    finalizedBudget.SetProposalsStr(strProposals);
}

std::string CBudgetManager::GetFinalizedBudgetStatus(const uint256& nHash) const
{
    CFinalizedBudget fb;
    if (!GetFinalizedBudget(nHash, fb))
        return strprintf("ERROR: cannot find finalized budget %s\n", nHash.ToString());

    std::string retBadHashes = "";
    std::string retBadPayeeOrAmount = "";
    int nBlockStart = fb.GetBlockStart();
    int nBlockEnd = fb.GetBlockEnd();

    for (int nBlockHeight = nBlockStart; nBlockHeight <= nBlockEnd; nBlockHeight++) {
        CTxBudgetPayment budgetPayment;
        if (!fb.GetBudgetPaymentByBlock(nBlockHeight, budgetPayment)) {
            LogPrint(BCLog::MNBUDGET,"%s: Couldn't find budget payment for block %lld\n", __func__, nBlockHeight);
            continue;
        }

        CBudgetProposal bp;
        if (!GetProposal(budgetPayment.nProposalHash, bp)) {
            retBadHashes += (retBadHashes == "" ? "" : ", ") + budgetPayment.nProposalHash.ToString();
            continue;
        }

        if (bp.GetPayee() != budgetPayment.payee || bp.GetAmount() != budgetPayment.nAmount) {
            retBadPayeeOrAmount += (retBadPayeeOrAmount == "" ? "" : ", ") + budgetPayment.nProposalHash.ToString();
        }
    }

    if (retBadHashes == "" && retBadPayeeOrAmount == "") return "OK";

    if (retBadHashes != "") retBadHashes = "Unknown proposal(s) hash! Check this proposal(s) before voting: " + retBadHashes;
    if (retBadPayeeOrAmount != "") retBadPayeeOrAmount = "Budget payee/nAmount doesn't match our proposal(s)! "+ retBadPayeeOrAmount;

    return retBadHashes + " -- " + retBadPayeeOrAmount;
}

bool CBudgetManager::AddFinalizedBudget(CFinalizedBudget& finalizedBudget, CNode* pfrom)
{
    AssertLockNotHeld(cs_budgets);    // need to lock cs_main here (CheckCollateral)
    const uint256& nHash = finalizedBudget.GetHash();

    if (WITH_LOCK(cs_budgets, return mapFinalizedBudgets.count(nHash))) {
        LogPrint(BCLog::MNBUDGET,"%s: finalized budget %s already added\n", __func__, nHash.ToString());
        return false;
    }

    if (!finalizedBudget.IsWellFormed(GetTotalBudget(finalizedBudget.GetBlockStart()))) {
        LogPrint(BCLog::MNBUDGET,"%s: invalid finalized budget: %s %s\n", __func__, nHash.ToString(), finalizedBudget.IsInvalidLogStr());
        return false;
    }

    std::string strError;
    int nCurrentHeight = GetBestHeight();
    const uint256& feeTxId = finalizedBudget.GetFeeTXHash();
    if (!CheckCollateral(feeTxId, nHash, strError, finalizedBudget.nTime, nCurrentHeight, true)) {
        LogPrint(BCLog::MNBUDGET,"%s: invalid finalized budget (%s) collateral id=%s - %s\n",
                __func__, nHash.ToString(), feeTxId.ToString(), strError);
        finalizedBudget.SetStrInvalid(strError);
        return false;
    }

    // update expiration
    if (!finalizedBudget.UpdateValid(nCurrentHeight)) {
        LogPrint(BCLog::MNBUDGET,"%s: invalid finalized budget: %s %s\n", __func__, nHash.ToString(), finalizedBudget.IsInvalidLogStr());
        return false;
    }

    // Compare budget payments with existent proposals, don't care on the order, just verify proposals existence.
    std::vector<CBudgetProposal> vBudget = GetBudget();
    std::map<uint256, CBudgetProposal> mapWinningProposals;
    for (const CBudgetProposal& p: vBudget) { mapWinningProposals.emplace(p.GetHash(), p); }
    if (!finalizedBudget.CheckProposals(mapWinningProposals)) {
        finalizedBudget.SetStrInvalid("Invalid proposals");
        LogPrint(BCLog::MNBUDGET,"%s: Budget finalization does not match with winning proposals\n", __func__);
        // just for now (until v6), request proposals sync in case we are missing one of them.
        if (pfrom) {
            for (const auto& propId : finalizedBudget.GetProposalsHashes()) {
                if (!g_budgetman.HaveProposal(propId)) {
                    pfrom->PushInventory(CInv(MSG_BUDGET_PROPOSAL, propId));
                }
            }
        }
        return false;
    }

    // Add budget finalization.
    SetBudgetProposalsStr(finalizedBudget);
    ForceAddFinalizedBudget(nHash, feeTxId, finalizedBudget);

    LogPrint(BCLog::MNBUDGET,"%s: finalized budget %s [%s (%s)] added\n",
            __func__, nHash.ToString(), finalizedBudget.GetName(), finalizedBudget.GetProposalsStr());
    return true;
}

void CBudgetManager::ForceAddFinalizedBudget(const uint256& nHash, const uint256& feeTxId, const CFinalizedBudget& finalizedBudget)
{
    LOCK(cs_budgets);
    mapFinalizedBudgets.emplace(nHash, finalizedBudget);
    // Add to feeTx index
    mapFeeTxToBudget.emplace(feeTxId, nHash);
    // Remove the budget from the unconfirmed map, if it was there
    if (mapUnconfirmedFeeTx.count(nHash))
        mapUnconfirmedFeeTx.erase(nHash);
}

bool CBudgetManager::AddProposal(CBudgetProposal& budgetProposal)
{
    AssertLockNotHeld(cs_proposals);    // need to lock cs_main here (CheckCollateral)
    const uint256& nHash = budgetProposal.GetHash();

    if (WITH_LOCK(cs_proposals, return mapProposals.count(nHash))) {
        LogPrint(BCLog::MNBUDGET,"%s: proposal %s already added\n", __func__, nHash.ToString());
        return false;
    }

    if (!budgetProposal.IsWellFormed(GetTotalBudget(budgetProposal.GetBlockStart()))) {
        LogPrint(BCLog::MNBUDGET,"%s: Invalid budget proposal %s %s\n", __func__, nHash.ToString(), budgetProposal.IsInvalidLogStr());
        return false;
    }

    std::string strError;
    int nCurrentHeight = GetBestHeight();
    const uint256& feeTxId = budgetProposal.GetFeeTXHash();
    if (!CheckCollateral(feeTxId, nHash, strError, budgetProposal.nTime, nCurrentHeight, false)) {
        LogPrint(BCLog::MNBUDGET,"%s: invalid budget proposal (%s) collateral id=%s - %s\n",
                __func__, nHash.ToString(), feeTxId.ToString(), strError);
        budgetProposal.SetStrInvalid(strError);
        return false;
    }

    // update expiration / heavily-downvoted
    if (!budgetProposal.UpdateValid(nCurrentHeight)) {
        LogPrint(BCLog::MNBUDGET,"%s: Invalid budget proposal %s %s\n", __func__, nHash.ToString(), budgetProposal.IsInvalidLogStr());
        return false;
    }

    {
        LOCK(cs_proposals);
        mapProposals.emplace(nHash, budgetProposal);
        // Add to feeTx index
        mapFeeTxToProposal.emplace(feeTxId, nHash);
    }
    LogPrint(BCLog::MNBUDGET,"%s: budget proposal %s [%s] added\n", __func__, nHash.ToString(), budgetProposal.GetName());

    return true;
}

void CBudgetManager::CheckAndRemove()
{
    int nCurrentHeight = GetBestHeight();
    std::map<uint256, CFinalizedBudget> tmpMapFinalizedBudgets;
    std::map<uint256, CBudgetProposal> tmpMapProposals;

    // Check Proposals first
    {
        LOCK(cs_proposals);
        LogPrint(BCLog::MNBUDGET, "%s: mapProposals cleanup - size before: %d\n", __func__, mapProposals.size());
        for (auto& it: mapProposals) {
            CBudgetProposal* pbudgetProposal = &(it.second);
            if (!pbudgetProposal->UpdateValid(nCurrentHeight)) {
                LogPrint(BCLog::MNBUDGET,"%s: Invalid budget proposal %s %s\n", __func__, (it.first).ToString(), pbudgetProposal->IsInvalidLogStr());
                mapFeeTxToProposal.erase(pbudgetProposal->GetFeeTXHash());
            } else {
                 LogPrint(BCLog::MNBUDGET,"%s: Found valid budget proposal: %s %s\n", __func__,
                          pbudgetProposal->GetName(), pbudgetProposal->GetFeeTXHash().ToString());
                 tmpMapProposals.emplace(pbudgetProposal->GetHash(), *pbudgetProposal);
            }
        }
        // Remove invalid entries by overwriting complete map
        mapProposals.swap(tmpMapProposals);
        LogPrint(BCLog::MNBUDGET, "%s: mapProposals cleanup - size after: %d\n", __func__, mapProposals.size());
    }

    // Then check finalized budgets
    {
        LOCK(cs_budgets);
        LogPrint(BCLog::MNBUDGET, "%s: mapFinalizedBudgets cleanup - size before: %d\n", __func__, mapFinalizedBudgets.size());
        for (auto& it: mapFinalizedBudgets) {
            CFinalizedBudget* pfinalizedBudget = &(it.second);
            if (!pfinalizedBudget->UpdateValid(nCurrentHeight)) {
                LogPrint(BCLog::MNBUDGET,"%s: Invalid finalized budget %s %s\n", __func__, (it.first).ToString(), pfinalizedBudget->IsInvalidLogStr());
                mapFeeTxToBudget.erase(pfinalizedBudget->GetFeeTXHash());
            } else {
                LogPrint(BCLog::MNBUDGET,"%s: Found valid finalized budget: %s %s\n", __func__,
                          pfinalizedBudget->GetName(), pfinalizedBudget->GetFeeTXHash().ToString());
                tmpMapFinalizedBudgets.emplace(pfinalizedBudget->GetHash(), *pfinalizedBudget);
            }
        }
        // Remove invalid entries by overwriting complete map
        mapFinalizedBudgets = tmpMapFinalizedBudgets;
        LogPrint(BCLog::MNBUDGET, "%s: mapFinalizedBudgets cleanup - size after: %d\n", __func__, mapFinalizedBudgets.size());
    }
    // Masternodes vote on valid ones
    VoteOnFinalizedBudgets();
}

void CBudgetManager::RemoveByFeeTxId(const uint256& feeTxId)
{
    {
        LOCK(cs_proposals);
        // Is this collateral related to a proposal?
        const auto& it = mapFeeTxToProposal.find(feeTxId);
        if (it != mapFeeTxToProposal.end()) {
            // Remove proposal
            CBudgetProposal* p = FindProposal(it->second);
            if (p) {
                LogPrintf("%s: Removing proposal %s (collateral disconnected, id=%s)\n", __func__, p->GetName(), feeTxId.ToString());
                {
                    // Erase seen/orhpan votes
                    LOCK(cs_votes);
                    for (const uint256& hash: p->GetVotesHashes()) {
                        mapSeenProposalVotes.erase(hash);
                        mapOrphanProposalVotes.erase(hash);
                    }
                }
                // Erase proposal object
                mapProposals.erase(it->second);
            }
            // Remove from collateral index
            mapFeeTxToProposal.erase(it);
            return;
        }
    }
    {
        LOCK(cs_budgets);
        // Is this collateral related to a finalized budget?
        const auto& it = mapFeeTxToBudget.find(feeTxId);
        if (it != mapFeeTxToBudget.end()) {
            // Remove finalized budget
            CFinalizedBudget* b = FindFinalizedBudget(it->second);
            if (b) {
                LogPrintf("%s: Removing finalized budget %s (collateral disconnected, id=%s)\n", __func__, b->GetName(), feeTxId.ToString());
                {
                    // Erase seen/orhpan votes
                    LOCK(cs_finalizedvotes);
                    for (const uint256& hash: b->GetVotesHashes()) {
                        mapSeenFinalizedBudgetVotes.erase(hash);
                        mapOrphanFinalizedBudgetVotes.erase(hash);
                    }
                }
                // Erase finalized budget object
                mapFinalizedBudgets.erase(it->second);
            }
            // Remove from collateral index
            mapFeeTxToBudget.erase(it);
        }
    }
}

const CFinalizedBudget* CBudgetManager::GetBudgetWithHighestVoteCount(int chainHeight) const
{
    LOCK(cs_budgets);
    int highestVoteCount = 0;
    const CFinalizedBudget* pHighestBudget = nullptr;
    for (const auto& it: mapFinalizedBudgets) {
        const CFinalizedBudget* pfinalizedBudget = &(it.second);
        int voteCount = pfinalizedBudget->GetVoteCount();
        if (voteCount > highestVoteCount &&
            chainHeight >= pfinalizedBudget->GetBlockStart() &&
            chainHeight <= pfinalizedBudget->GetBlockEnd()) {
            pHighestBudget = pfinalizedBudget;
            highestVoteCount = voteCount;
        }
    }
    return pHighestBudget;
}

int CBudgetManager::GetHighestVoteCount(int chainHeight) const
{
    const CFinalizedBudget* pbudget = GetBudgetWithHighestVoteCount(chainHeight);
    return (pbudget ? pbudget->GetVoteCount() : -1);
}

bool CBudgetManager::GetPayeeAndAmount(int chainHeight, CScript& payeeRet, CAmount& nAmountRet) const
{
    int nCountThreshold;
    if (!IsBudgetPaymentBlock(chainHeight, nCountThreshold))
        return false;

    const CFinalizedBudget* pfb = GetBudgetWithHighestVoteCount(chainHeight);
    return pfb && pfb->GetPayeeAndAmount(chainHeight, payeeRet, nAmountRet) && pfb->GetVoteCount() > nCountThreshold;
}

bool CBudgetManager::GetExpectedPayeeAmount(int chainHeight, CAmount& nAmountRet) const
{
    CScript payeeRet;
    return GetPayeeAndAmount(chainHeight, payeeRet, nAmountRet);
}

bool CBudgetManager::FillBlockPayee(CMutableTransaction& txCoinbase, CMutableTransaction& txCoinstake, const int nHeight, bool fProofOfStake) const
{
    if (nHeight <= 0) return false;

    CScript payee;
    CAmount nAmount = 0;

    if (!GetPayeeAndAmount(nHeight, payee, nAmount))
        return false;

    CAmount blockValue = GetBlockValue(nHeight);

    // Starting from PIVX v6.0 masternode and budgets are paid in the coinbase tx of PoS blocks
    const bool fPayCoinstake = fProofOfStake &&
                               !Params().GetConsensus().NetworkUpgradeActive(nHeight, Consensus::UPGRADE_V6_0);

    if (fProofOfStake) {
        if (fPayCoinstake) {
            unsigned int i = txCoinstake.vout.size();
            txCoinstake.vout.resize(i + 1);
            txCoinstake.vout[i].scriptPubKey = payee;
            txCoinstake.vout[i].nValue = nAmount;
        } else {
            txCoinbase.vout.resize(1);
            txCoinbase.vout[0].scriptPubKey = payee;
            txCoinbase.vout[0].nValue = nAmount;
        }
    } else {
        //miners get the full amount on these blocks
        txCoinbase.vout[0].nValue = blockValue;
        txCoinbase.vout.resize(2);

        //these are super blocks, so their value can be much larger than normal
        txCoinbase.vout[1].scriptPubKey = payee;
        txCoinbase.vout[1].nValue = nAmount;
    }

    CTxDestination address;
    ExtractDestination(payee, address);
    LogPrint(BCLog::MNBUDGET,"%s: Budget payment to %s for %lld\n", __func__, EncodeDestination(address), nAmount);
    return true;
}

void CBudgetManager::VoteOnFinalizedBudgets()
{
    // function called only from initialized masternodes
    if (!fMasterNode) {
        LogPrint(BCLog::MNBUDGET,"%s: Not a masternode\n", __func__);
        return;
    }

    // Do this 1 in 4 blocks -- spread out the voting activity
    // -- this function is only called every fourteenth block, so this is really 1 in 56 blocks
    if (GetRandInt(4) != 0) {
        LogPrint(BCLog::MNBUDGET,"%s: waiting\n", __func__);
        return;
    }

    // Get the active masternode (operator) key
    CKey mnKey; CKeyID mnKeyID; CTxIn mnVin;
    if (!GetActiveMasternodeKeys(mnKey, mnKeyID, mnVin)) {
        return;
    }

    std::vector<CBudgetProposal> vBudget = GetBudget();
    if (vBudget.empty()) {
        LogPrint(BCLog::MNBUDGET,"%s: No proposal can be finalized\n", __func__);
        return;
    }

    std::map<uint256, CBudgetProposal> mapWinningProposals;
    for (const CBudgetProposal& p: vBudget) {
        mapWinningProposals.emplace(p.GetHash(), p);
    }
    // Vector containing the hash of finalized budgets to sign
    std::vector<uint256> vBudgetHashes;
    {
        LOCK(cs_budgets);
        for (auto& it: mapFinalizedBudgets) {
            CFinalizedBudget* pfb = &(it.second);
            // we only need to check this once
            if (pfb->IsAutoChecked()) continue;
            pfb->SetAutoChecked(true);
            //only vote for exact matches
            if (strBudgetMode == "auto") {
                // compare budget payements with winning proposals
                if (!pfb->CheckProposals(mapWinningProposals)) {
                    continue;
                }
            }
            // exact match found. add budget hash to sign it later.
            vBudgetHashes.emplace_back(pfb->GetHash());
        }
    }

    // Sign finalized budgets
    for (const uint256& budgetHash: vBudgetHashes) {
        CFinalizedBudgetVote vote(mnVin, budgetHash);
        if (!vote.Sign(mnKey, mnKeyID)) {
            LogPrintf("%s: Failure to sign budget %s", __func__, budgetHash.ToString());
            continue;
        }
        std::string strError = "";
        if (!UpdateFinalizedBudget(vote, NULL, strError)) {
            LogPrintf("%s: Error submitting vote - %s\n", __func__, strError);
            continue;
        }
        LogPrint(BCLog::MNBUDGET, "%s: new finalized budget vote signed: %s\n", __func__, vote.GetHash().ToString());
        AddSeenFinalizedBudgetVote(vote);
        vote.Relay();
    }
}

CFinalizedBudget* CBudgetManager::FindFinalizedBudget(const uint256& nHash)
{
    AssertLockHeld(cs_budgets);

    if (mapFinalizedBudgets.count(nHash))
        return &mapFinalizedBudgets[nHash];

    return NULL;
}

const CBudgetProposal* CBudgetManager::FindProposalByName(const std::string& strProposalName) const
{
    LOCK(cs_proposals);

    int64_t nYesCountMax = std::numeric_limits<int64_t>::min();
    const CBudgetProposal* pbudgetProposal = nullptr;

    for (const auto& it: mapProposals) {
        const CBudgetProposal& proposal = it.second;
        int64_t nYesCount = proposal.GetYeas() - proposal.GetNays();
        if (proposal.GetName() == strProposalName && nYesCount > nYesCountMax) {
            pbudgetProposal = &proposal;
            nYesCountMax = nYesCount;
        }
    }

    return pbudgetProposal;
}

CBudgetProposal* CBudgetManager::FindProposal(const uint256& nHash)
{
    AssertLockHeld(cs_proposals);

    if (mapProposals.count(nHash))
        return &mapProposals[nHash];

    return nullptr;
}

bool CBudgetManager::GetProposal(const uint256& nHash, CBudgetProposal& bp) const
{
    LOCK(cs_proposals);
    if (mapProposals.count(nHash)) {
        bp = mapProposals.at(nHash);
        return true;
    }
    return false;
}

bool CBudgetManager::GetFinalizedBudget(const uint256& nHash, CFinalizedBudget& fb) const
{
    LOCK(cs_budgets);
    if (mapFinalizedBudgets.count(nHash)) {
        fb = mapFinalizedBudgets.at(nHash);
        return true;
    }
    return false;
}

bool CBudgetManager::IsBudgetPaymentBlock(int nBlockHeight, int& nCountThreshold) const
{
    int nHighestCount = GetHighestVoteCount(nBlockHeight);
    int nCountEnabled = mnodeman.CountEnabled(ActiveProtocol());
    int nFivePercent = nCountEnabled / 20;
    // threshold for highest finalized budgets (highest vote count - 10% of active masternodes)
    nCountThreshold = nHighestCount - (nCountEnabled / 10);
    // reduce the threshold if there are less than 10 enabled masternodes
    if (nCountThreshold == nHighestCount) nCountThreshold--;

    LogPrint(BCLog::MNBUDGET,"%s: nHighestCount: %lli, 5%% of Masternodes: %lli.\n",
            __func__, nHighestCount, nFivePercent);

    // If budget doesn't have 5% of the network votes, then we should pay a masternode instead
    return (nHighestCount > nFivePercent);
}

bool CBudgetManager::IsBudgetPaymentBlock(int nBlockHeight) const
{
    int nCountThreshold;
    return IsBudgetPaymentBlock(nBlockHeight, nCountThreshold);
}

TrxValidationStatus CBudgetManager::IsTransactionValid(const CTransaction& txNew, const uint256& nBlockHash, int nBlockHeight) const
{
    int nCountThreshold = 0;
    if (!IsBudgetPaymentBlock(nBlockHeight, nCountThreshold)) {
        // If budget doesn't have 5% of the network votes, then we should pay a masternode instead
        return TrxValidationStatus::InValid;
    }

    // check the highest finalized budgets (- 10% to assist in consensus)
    bool fThreshold = false;
    {
        LOCK(cs_budgets);
        // Get the finalized budget with the highest amount of votes..
        const CFinalizedBudget* highestVotesBudget = GetBudgetWithHighestVoteCount(nBlockHeight);
        if (highestVotesBudget) {
            // Need to surpass the threshold
            if (highestVotesBudget->GetVoteCount() > nCountThreshold) {
                fThreshold = true;
                if (highestVotesBudget->IsTransactionValid(txNew, nBlockHash, nBlockHeight) ==
                    TrxValidationStatus::Valid) {
                    return TrxValidationStatus::Valid;
                }
            }
            // tx not valid
            LogPrint(BCLog::MNBUDGET, "%s: ignoring budget. Out of range or tx not valid.\n", __func__);
        }
    }

    // If not enough masternodes autovoted for any of the finalized budgets or if none of the txs
    // are valid, we should pay a masternode instead
    return fThreshold ? TrxValidationStatus::InValid : TrxValidationStatus::VoteThreshold;
}

std::vector<CBudgetProposal*> CBudgetManager::GetAllProposals()
{
    LOCK(cs_proposals);

    std::vector<CBudgetProposal*> vBudgetProposalRet;

    for (auto& it: mapProposals) {
        CBudgetProposal* pbudgetProposal = &(it.second);
        RemoveStaleVotesOnProposal(pbudgetProposal);
        vBudgetProposalRet.push_back(pbudgetProposal);
    }

    std::sort(vBudgetProposalRet.begin(), vBudgetProposalRet.end(), CBudgetProposal::PtrHigherYes);

    return vBudgetProposalRet;
}

std::vector<CBudgetProposal> CBudgetManager::GetBudget()
{
    LOCK(cs_proposals);

    int nHeight = GetBestHeight();
    if (nHeight <= 0)
        return {};

    // ------- Sort budgets by net Yes Count
    std::vector<CBudgetProposal*> vBudgetPorposalsSort;
    for (auto& it: mapProposals) {
        RemoveStaleVotesOnProposal(&it.second);
        vBudgetPorposalsSort.push_back(&it.second);
    }
    std::sort(vBudgetPorposalsSort.begin(), vBudgetPorposalsSort.end(), CBudgetProposal::PtrHigherYes);

    // ------- Grab The Budgets In Order
    std::vector<CBudgetProposal> vBudgetProposalsRet;
    CAmount nBudgetAllocated = 0;

    const int nBlocksPerCycle = Params().GetConsensus().nBudgetCycleBlocks;
    int nBlockStart = nHeight - nHeight % nBlocksPerCycle + nBlocksPerCycle;
    int nBlockEnd = nBlockStart + nBlocksPerCycle - 1;
    int mnCount = mnodeman.CountEnabled(ActiveProtocol());
    CAmount nTotalBudget = GetTotalBudget(nBlockStart);

    for (CBudgetProposal* pbudgetProposal: vBudgetPorposalsSort) {
        LogPrint(BCLog::MNBUDGET,"%s: Processing Budget %s\n", __func__, pbudgetProposal->GetName());
        //prop start/end should be inside this period
        if (pbudgetProposal->IsPassing(nBlockStart, nBlockEnd, mnCount)) {
            LogPrint(BCLog::MNBUDGET,"%s:  -   Check 1 passed: valid=%d | %ld <= %ld | %ld >= %ld | Yeas=%d Nays=%d Count=%d | established=%d\n",
                    __func__, pbudgetProposal->IsValid(), pbudgetProposal->GetBlockStart(), nBlockStart, pbudgetProposal->GetBlockEnd(),
                    nBlockEnd, pbudgetProposal->GetYeas(), pbudgetProposal->GetNays(), mnCount / 10, pbudgetProposal->IsEstablished());

            if (pbudgetProposal->GetAmount() + nBudgetAllocated <= nTotalBudget) {
                pbudgetProposal->SetAllotted(pbudgetProposal->GetAmount());
                nBudgetAllocated += pbudgetProposal->GetAmount();
                vBudgetProposalsRet.emplace_back(*pbudgetProposal);
                LogPrint(BCLog::MNBUDGET,"%s:  -     Check 2 passed: Budget added\n", __func__);
            } else {
                pbudgetProposal->SetAllotted(0);
                LogPrint(BCLog::MNBUDGET,"%s:  -     Check 2 failed: no amount allotted\n", __func__);
            }

        } else {
            LogPrint(BCLog::MNBUDGET,"%s:  -   Check 1 failed: valid=%d | %ld <= %ld | %ld >= %ld | Yeas=%d Nays=%d Count=%d | established=%d\n",
                    __func__, pbudgetProposal->IsValid(), pbudgetProposal->GetBlockStart(), nBlockStart, pbudgetProposal->GetBlockEnd(),
                    nBlockEnd, pbudgetProposal->GetYeas(), pbudgetProposal->GetNays(), mnodeman.CountEnabled(ActiveProtocol()) / 10,
                    pbudgetProposal->IsEstablished());
        }

    }

    return vBudgetProposalsRet;
}

std::vector<CFinalizedBudget*> CBudgetManager::GetFinalizedBudgets()
{
    LOCK(cs_budgets);

    std::vector<CFinalizedBudget*> vFinalizedBudgetsRet;

    // ------- Grab The Budgets In Order
    for (auto& it: mapFinalizedBudgets) {
        vFinalizedBudgetsRet.push_back(&(it.second));
    }
    std::sort(vFinalizedBudgetsRet.begin(), vFinalizedBudgetsRet.end(), CFinalizedBudget::PtrGreater);

    return vFinalizedBudgetsRet;
}

std::string CBudgetManager::GetRequiredPaymentsString(int nBlockHeight)
{
    LOCK(cs_budgets);

    std::string ret = "unknown-budget";

    std::map<uint256, CFinalizedBudget>::iterator it = mapFinalizedBudgets.begin();
    while (it != mapFinalizedBudgets.end()) {
        CFinalizedBudget* pfinalizedBudget = &((*it).second);
        if (nBlockHeight >= pfinalizedBudget->GetBlockStart() && nBlockHeight <= pfinalizedBudget->GetBlockEnd()) {
            CTxBudgetPayment payment;
            if (pfinalizedBudget->GetBudgetPaymentByBlock(nBlockHeight, payment)) {
                if (ret == "unknown-budget") {
                    ret = payment.nProposalHash.ToString();
                } else {
                    ret += ",";
                    ret += payment.nProposalHash.ToString();
                }
            } else {
                LogPrint(BCLog::MNBUDGET,"%s:  Couldn't find budget payment for block %d\n", __func__, nBlockHeight);
            }
        }

        ++it;
    }

    return ret;
}

CAmount CBudgetManager::GetTotalBudget(int nHeight)
{
    // 20% of the block value
    CAmount nSubsidy = GetBlockValue(nHeight) / 5;

    // multiplied by the number of blocks in a cycle (144 on testnet, 30*1440 on mainnet)
    return nSubsidy * Params().GetConsensus().nBudgetCycleBlocks;
}

void CBudgetManager::AddSeenProposalVote(const CBudgetVote& vote)
{
    LOCK(cs_votes);
    mapSeenProposalVotes.emplace(vote.GetHash(), vote);
}

void CBudgetManager::AddSeenFinalizedBudgetVote(const CFinalizedBudgetVote& vote)
{
    LOCK(cs_finalizedvotes);
    mapSeenFinalizedBudgetVotes.emplace(vote.GetHash(), vote);
}

void CBudgetManager::RemoveStaleVotesOnProposal(CBudgetProposal* prop)
{
    AssertLockHeld(cs_proposals);
    LogPrint(BCLog::MNBUDGET, "Cleaning proposal votes for %s. Before: YES=%d, NO=%d\n",
            prop->GetName(), prop->GetYeas(), prop->GetNays());

    auto it = prop->mapVotes.begin();
    while (it != prop->mapVotes.end()) {
        auto mnList = deterministicMNManager->GetListAtChainTip();
        auto dmn = mnList.GetMNByCollateral(it->first);
        if (dmn) {
            (*it).second.SetValid(mnList.IsMNValid(dmn));
        } else {
            // -- Legacy System (!TODO: remove after enforcement) --
            CMasternode* pmn = mnodeman.Find(it->first);
            (*it).second.SetValid(pmn && pmn->IsEnabled());
        }
        ++it;
    }

    LogPrint(BCLog::MNBUDGET, "Cleaned proposal votes for %s. After: YES=%d, NO=%d\n",
            prop->GetName(), prop->GetYeas(), prop->GetNays());
}

void CBudgetManager::RemoveStaleVotesOnFinalBudget(CFinalizedBudget* fbud)
{
    AssertLockHeld(cs_budgets);
    LogPrint(BCLog::MNBUDGET, "Cleaning finalized budget votes for [%s (%s)]. Before: %d\n",
            fbud->GetName(), fbud->GetProposalsStr(), fbud->GetVoteCount());

    auto it = fbud->mapVotes.begin();
    while (it != fbud->mapVotes.end()) {
        auto mnList = deterministicMNManager->GetListAtChainTip();
        auto dmn = mnList.GetMNByCollateral(it->first);
        if (dmn) {
            (*it).second.SetValid(mnList.IsMNValid(dmn));
        } else {
            // -- Legacy System (!TODO: remove after enforcement) --
            CMasternode* pmn = mnodeman.Find(it->first);
            (*it).second.SetValid(pmn && pmn->IsEnabled());
        }
        ++it;
    }
    LogPrint(BCLog::MNBUDGET, "Cleaned finalized budget votes for [%s (%s)]. After: %d\n",
            fbud->GetName(), fbud->GetProposalsStr(), fbud->GetVoteCount());
}

CDataStream CBudgetManager::GetProposalVoteSerialized(const uint256& voteHash) const
{
    LOCK(cs_votes);
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss.reserve(1000);
    ss << mapSeenProposalVotes.at(voteHash);
    return ss;
}

CDataStream CBudgetManager::GetProposalSerialized(const uint256& propHash) const
{
    LOCK(cs_proposals);
    return mapProposals.at(propHash).GetBroadcast();
}

CDataStream CBudgetManager::GetFinalizedBudgetVoteSerialized(const uint256& voteHash) const
{
    LOCK(cs_finalizedvotes);
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss.reserve(1000);
    ss << mapSeenFinalizedBudgetVotes.at(voteHash);
    return ss;
}

CDataStream CBudgetManager::GetFinalizedBudgetSerialized(const uint256& budgetHash) const
{
    LOCK(cs_budgets);
    return mapFinalizedBudgets.at(budgetHash).GetBroadcast();
}

bool CBudgetManager::AddAndRelayProposalVote(const CBudgetVote& vote, std::string& strError)
{
    if (UpdateProposal(vote, nullptr, strError)) {
        AddSeenProposalVote(vote);
        vote.Relay();
        return true;
    }
    return false;
}

void CBudgetManager::NewBlock(int height)
{
    SetBestHeight(height);

    if (masternodeSync.RequestedMasternodeAssets <= MASTERNODE_SYNC_BUDGET) return;

    if (strBudgetMode == "suggest") { //suggest the budget we see
        SubmitFinalBudget();
    }

    int nCurrentHeight = GetBestHeight();
    //this function should be called 1/14 blocks, allowing up to 100 votes per day on all proposals
    if (nCurrentHeight % 14 != 0) return;

    // incremental sync with our peers
    if (masternodeSync.IsSynced()) {
        LogPrint(BCLog::MNBUDGET,"%s:  incremental sync started\n", __func__);
        if (GetRandInt(1440) == 0) {
            ClearSeen();
            ResetSync();
        }

        CBudgetManager* manager = this;
        g_connman->ForEachNode([manager](CNode* pnode){
            if (pnode->nVersion >= ActiveProtocol())
                manager->Sync(pnode, UINT256_ZERO, true);
        });
        MarkSynced();
    }

    // remove expired/heavily downvoted budgets
    CheckAndRemove();

    //remove invalid (from non-active masternode) votes once in a while
    LogPrint(BCLog::MNBUDGET,"%s:  askedForSourceProposalOrBudget cleanup - size: %d\n", __func__, askedForSourceProposalOrBudget.size());
    for (auto it = askedForSourceProposalOrBudget.begin(); it !=  askedForSourceProposalOrBudget.end(); ) {
        if (it->second <= GetTime() - (60 * 60 * 24)) {
            it = askedForSourceProposalOrBudget.erase(it);
        } else {
            it++;
        }
    }
    {
        LOCK(cs_proposals);
        LogPrint(BCLog::MNBUDGET,"%s:  mapProposals cleanup - size: %d\n", __func__, mapProposals.size());
        for (auto& it: mapProposals) {
            RemoveStaleVotesOnProposal(&it.second);
        }
    }
    {
        LOCK(cs_budgets);
        LogPrint(BCLog::MNBUDGET,"%s:  mapFinalizedBudgets cleanup - size: %d\n", __func__, mapFinalizedBudgets.size());
        for (auto& it: mapFinalizedBudgets) {
            RemoveStaleVotesOnFinalBudget(&it.second);
        }
    }
    LogPrint(BCLog::MNBUDGET,"%s:  PASSED\n", __func__);
}

int CBudgetManager::ProcessBudgetVoteSync(const uint256& nProp, CNode* pfrom)
{
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (nProp.IsNull()) {
            if (pfrom->HasFulfilledRequest("budgetvotesync")) {
                LogPrint(BCLog::MNBUDGET, "mnvs - peer already asked me for the list\n");
                return 20;
            }
            pfrom->FulfilledRequest("budgetvotesync");
        }
    }

    Sync(pfrom, nProp);
    LogPrint(BCLog::MNBUDGET, "mnvs - Sent Masternode votes to peer %i\n", pfrom->GetId());
    return 0;
}

int CBudgetManager::ProcessProposal(CBudgetProposal& proposal)
{
    const uint256& nHash = proposal.GetHash();
    if (HaveProposal(nHash)) {
        masternodeSync.AddedBudgetItem(nHash);
        return 0;
    }
    if (!AddProposal(proposal)) {
        return 0;
    }
    proposal.Relay();
    masternodeSync.AddedBudgetItem(nHash);

    LogPrint(BCLog::MNBUDGET, "mprop (new) %s\n", nHash.ToString());
    //We might have active votes for this proposal that are valid now
    CheckOrphanVotes();
    return 0;
}

bool CBudgetManager::ProcessProposalVote(CBudgetVote& vote, CNode* pfrom, CValidationState& state)
{
    const uint256& voteID = vote.GetHash();

    if (HaveSeenProposalVote(voteID)) {
        masternodeSync.AddedBudgetItem(voteID);
        return false;
    }

    const CTxIn& voteVin = vote.GetVin();

    // See if this vote was signed with a deterministic masternode
    std::string err;
    auto mnList = deterministicMNManager->GetListAtChainTip();
    auto dmn = mnList.GetMNByCollateral(voteVin.prevout);
    if (dmn) {
        const std::string& mn_protx_id = dmn->proTxHash.ToString();

        if (!vote.CheckSignature(dmn->pdmnState->keyIDVoting)) {
            err = strprintf("invalid mvote sig from dmn: %s", mn_protx_id);
            return state.DoS(100, false, REJECT_INVALID, "bad-mvote-sig", false, err);
        }

        AddSeenProposalVote(vote);

        if (!mnList.IsMNValid(dmn)) {
            err = strprintf("masternode (%s) not valid or PoSe banned", mn_protx_id);
            return state.DoS(0, false, REJECT_INVALID, "bad-mvote", false, err);
        }

        if (!UpdateProposal(vote, pfrom, err)) {
            return state.DoS(0, false, REJECT_INVALID, "bad-mvote", false, strprintf("%s (%s)", err, mn_protx_id));
        }

        vote.Relay();
        masternodeSync.AddedBudgetItem(voteID);
        LogPrint(BCLog::MNBUDGET, "mvote - new vote (%s) for proposal %s from dmn %s\n",
                voteID.ToString(), vote.GetProposalHash().ToString(), mn_protx_id);
        return true;
    }

    // -- Legacy System (!TODO: remove after enforcement) --

    CMasternode* pmn = mnodeman.Find(voteVin.prevout);
    if (!pmn) {
        err = strprintf("unknown masternode - vin: %s", voteVin.prevout.ToString());
        mnodeman.AskForMN(pfrom, voteVin);
        return state.DoS(0, false, REJECT_INVALID, "bad-mvote", false, err);
    }

    AddSeenProposalVote(vote);

    if (!vote.CheckSignature(pmn->pubKeyMasternode.GetID())) {
        if (masternodeSync.IsSynced()) {
            err = strprintf("signature from masternode %s invalid", voteVin.prevout.ToString());
            return state.DoS(20, false, REJECT_INVALID, "bad-fbvote", false, err);
        }
        // it could just be a non-synced masternode
        mnodeman.AskForMN(pfrom, voteVin);
        return false;
    }

    if (!pmn->IsEnabled()) {
        return state.DoS(0, false, REJECT_INVALID, "bad-mvote", false, "masternode not valid");
    }
    if (!UpdateProposal(vote, pfrom, err)) {
        return state.DoS(0, false, REJECT_INVALID, "bad-mvote", false, err);
    }

    vote.Relay();
    masternodeSync.AddedBudgetItem(voteID);
    LogPrint(BCLog::MNBUDGET, "mvote - new vote (%s) for proposal %s from dmn %s\n",
            voteID.ToString(), vote.GetProposalHash().ToString(), voteVin.prevout.ToString());
    return true;
}

int CBudgetManager::ProcessFinalizedBudget(CFinalizedBudget& finalbudget, CNode* pfrom)
{

    const uint256& nHash = finalbudget.GetHash();
    if (HaveFinalizedBudget(nHash)) {
        masternodeSync.AddedBudgetItem(nHash);
        return 0;
    }
    if (!AddFinalizedBudget(finalbudget, pfrom)) {
        return 0;
    }
    finalbudget.Relay();
    masternodeSync.AddedBudgetItem(nHash);

    LogPrint(BCLog::MNBUDGET, "fbs (new) %s\n", nHash.ToString());
    //we might have active votes for this budget that are now valid
    CheckOrphanVotes();
    return 0;
}

bool CBudgetManager::ProcessFinalizedBudgetVote(CFinalizedBudgetVote& vote, CNode* pfrom, CValidationState& state)
{
    const uint256& voteID = vote.GetHash();

    if (HaveSeenFinalizedBudgetVote(voteID)) {
        masternodeSync.AddedBudgetItem(voteID);
        return false;
    }

    const CTxIn& voteVin = vote.GetVin();

    // See if this vote was signed with a deterministic masternode
    std::string err;
    auto mnList = deterministicMNManager->GetListAtChainTip();
    auto dmn = mnList.GetMNByCollateral(voteVin.prevout);
    if (dmn) {
        const std::string& mn_protx_id = dmn->proTxHash.ToString();

        if (!vote.CheckSignature(dmn->pdmnState->keyIDOperator)) {
            err = strprintf("invalid fbvote sig from dmn: %s", mn_protx_id);
            return state.DoS(100, false, REJECT_INVALID, "bad-fbvote-sig", false, err);
        }

        AddSeenFinalizedBudgetVote(vote);

        if (!mnList.IsMNValid(dmn)) {
            err = strprintf("masternode (%s) not valid or PoSe banned", mn_protx_id);
            return state.DoS(0, false, REJECT_INVALID, "bad-fbvote", false, err);
        }
        if (!UpdateFinalizedBudget(vote, pfrom, err)) {
            return state.DoS(0, false, REJECT_INVALID, "bad-fbvote", false, strprintf("%s (%s)", err, mn_protx_id));
        }

        vote.Relay();
        masternodeSync.AddedBudgetItem(voteID);
        LogPrint(BCLog::MNBUDGET, "fbvote - new vote (%s) for budget %s from dmn %s\n",
                voteID.ToString(), vote.GetBudgetHash().ToString(), mn_protx_id);
        return true;
    }

    // -- Legacy System (!TODO: remove after enforcement) --
    CMasternode* pmn = mnodeman.Find(voteVin.prevout);
    if (!pmn) {
        err = strprintf("unknown masternode - vin: %s", voteVin.prevout.ToString());
        mnodeman.AskForMN(pfrom, voteVin);
        return state.DoS(0, false, REJECT_INVALID, "bad-fbvote", false, err);
    }

    AddSeenFinalizedBudgetVote(vote);

    if (!vote.CheckSignature(pmn->pubKeyMasternode.GetID())) {
        if (masternodeSync.IsSynced()) {
            err = strprintf("signature from masternode %s invalid", voteVin.prevout.ToString());
            return state.DoS(20, false, REJECT_INVALID, "bad-fbvote", false, err);
        }
        // it could just be a non-synced masternode
        mnodeman.AskForMN(pfrom, voteVin);
        return false;
    }

    if (!pmn->IsEnabled()) {
        return state.DoS(0, false, REJECT_INVALID, "bad-fbvote", false, "masternode not valid");
    }
    if (!UpdateFinalizedBudget(vote, pfrom, err)) {
        return state.DoS(0, false, REJECT_INVALID, "bad-fbvote", false, err);
    }

    vote.Relay();
    masternodeSync.AddedBudgetItem(voteID);
    LogPrint(BCLog::MNBUDGET, "fbvote - new vote (%s) for budget %s from mn %s\n",
            voteID.ToString(), vote.GetBudgetHash().ToString(), voteVin.prevout.ToString());
    return true;
}

void CBudgetManager::ProcessMessage(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    int banScore = ProcessMessageInner(pfrom, strCommand, vRecv);
    if (banScore > 0) {
        LOCK(cs_main);
        Misbehaving(pfrom->GetId(), banScore);
    }
}

int CBudgetManager::ProcessMessageInner(CNode* pfrom, std::string& strCommand, CDataStream& vRecv)
{
    // lite mode is not supported
    if (fLiteMode) return 0;
    if (!masternodeSync.IsBlockchainSynced()) return 0;

    if (strCommand == NetMsgType::BUDGETVOTESYNC) {
        // Masternode vote sync
        uint256 nProp;
        vRecv >> nProp;
        return ProcessBudgetVoteSync(nProp, pfrom);
    }

    if (strCommand == NetMsgType::BUDGETPROPOSAL) {
        // Masternode Proposal
        CBudgetProposal proposal;
        if (!proposal.ParseBroadcast(vRecv)) {
            return 20;
        }
        return ProcessProposal(proposal);
    }

    if (strCommand == NetMsgType::BUDGETVOTE) {
        CBudgetVote vote;
        vRecv >> vote;
        vote.SetValid(true);
        CValidationState state;
        if (!ProcessProposalVote(vote, pfrom, state)) {
            int nDos = 0;
            if(state.IsInvalid(nDos) && nDos > 0) {
                LogPrint(BCLog::NET, "%s: %s\n", __func__, FormatStateMessage(state));
                return nDos;
            }
        }
        return 0;
    }

    if (strCommand == NetMsgType::FINALBUDGET) {
        // Finalized Budget Suggestion
        CFinalizedBudget finalbudget;
        if (!finalbudget.ParseBroadcast(vRecv)) {
            return 20;
        }
        return ProcessFinalizedBudget(finalbudget, pfrom);
    }

    if (strCommand == NetMsgType::FINALBUDGETVOTE) {
        CFinalizedBudgetVote vote;
        vRecv >> vote;
        vote.SetValid(true);
        CValidationState state;
        if (!ProcessFinalizedBudgetVote(vote, pfrom, state)) {
            int nDos = 0;
            if(state.IsInvalid(nDos) && nDos > 0) {
                LogPrint(BCLog::NET, "%s: %s\n", __func__, FormatStateMessage(state));
                return nDos;
            }
        }
        return 0;
    }

    // nothing was done
    return 0;
}

void CBudgetManager::SetSynced(bool synced)
{
    {
        LOCK(cs_proposals);
        for (auto& it: mapProposals) {
            CBudgetProposal* pbudgetProposal = &(it.second);
            if (pbudgetProposal && pbudgetProposal->IsValid()) {
                //mark votes
                pbudgetProposal->SetSynced(synced);
            }
        }
    }
    {
        LOCK(cs_budgets);
        for (auto& it: mapFinalizedBudgets) {
            CFinalizedBudget* pfinalizedBudget = &(it.second);
            if (pfinalizedBudget && pfinalizedBudget->IsValid()) {
                //mark votes
                pfinalizedBudget->SetSynced(synced);
            }
        }
    }
}

void CBudgetManager::Sync(CNode* pfrom, const uint256& nProp, bool fPartial)
{
    CNetMsgMaker msgMaker(pfrom->GetSendVersion());
    int nInvCount = 0;
    {
        LOCK(cs_proposals);
        for (auto& it: mapProposals) {
            CBudgetProposal* pbudgetProposal = &(it.second);
            if (pbudgetProposal && pbudgetProposal->IsValid() && (nProp.IsNull() || it.first == nProp)) {
                pfrom->PushInventory(CInv(MSG_BUDGET_PROPOSAL, it.second.GetHash()));
                nInvCount++;
                pbudgetProposal->SyncVotes(pfrom, fPartial, nInvCount);
            }
        }
    }
    g_connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::SYNCSTATUSCOUNT, MASTERNODE_SYNC_BUDGET_PROP, nInvCount));
    LogPrint(BCLog::MNBUDGET, "%s: sent %d items\n", __func__, nInvCount);

    nInvCount = 0;
    {
        LOCK(cs_budgets);
        for (auto& it: mapFinalizedBudgets) {
            CFinalizedBudget* pfinalizedBudget = &(it.second);
            if (pfinalizedBudget && pfinalizedBudget->IsValid() && (nProp.IsNull() || it.first == nProp)) {
                pfrom->PushInventory(CInv(MSG_BUDGET_FINALIZED, it.second.GetHash()));
                nInvCount++;
                pfinalizedBudget->SyncVotes(pfrom, fPartial, nInvCount);
            }
        }
    }
    g_connman->PushMessage(pfrom, msgMaker.Make(NetMsgType::SYNCSTATUSCOUNT, MASTERNODE_SYNC_BUDGET_FIN, nInvCount));
    LogPrint(BCLog::MNBUDGET, "%s: sent %d items\n", __func__, nInvCount);
}

bool CBudgetManager::UpdateProposal(const CBudgetVote& vote, CNode* pfrom, std::string& strError)
{
    LOCK(cs_proposals);

    const uint256& nProposalHash = vote.GetProposalHash();
    if (!mapProposals.count(nProposalHash)) {
        if (pfrom) {
            // only ask for missing items after our syncing process is complete --
            //   otherwise we'll think a full sync succeeded when they return a result
            if (!masternodeSync.IsSynced()) return false;

            LogPrint(BCLog::MNBUDGET,"%s: Unknown proposal %d, asking for source proposal\n", __func__, nProposalHash.ToString());
            WITH_LOCK(cs_votes, mapOrphanProposalVotes[nProposalHash] = vote; );

            if (!askedForSourceProposalOrBudget.count(nProposalHash)) {
                g_connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::BUDGETVOTESYNC, nProposalHash));
                askedForSourceProposalOrBudget[nProposalHash] = GetTime();
            }
        }

        strError = "Proposal not found!";
        return false;
    }


    return mapProposals[nProposalHash].AddOrUpdateVote(vote, strError);
}

bool CBudgetManager::UpdateFinalizedBudget(CFinalizedBudgetVote& vote, CNode* pfrom, std::string& strError)
{
    LOCK(cs_budgets);

    const uint256& nBudgetHash = vote.GetBudgetHash();
    if (!mapFinalizedBudgets.count(nBudgetHash)) {
        if (pfrom) {
            // only ask for missing items after our syncing process is complete --
            //   otherwise we'll think a full sync succeeded when they return a result
            if (!masternodeSync.IsSynced()) return false;

            LogPrint(BCLog::MNBUDGET,"%s: Unknown Finalized Proposal %s, asking for source budget\n", __func__, nBudgetHash.ToString());
            WITH_LOCK(cs_finalizedvotes, mapOrphanFinalizedBudgetVotes[nBudgetHash] = vote; );

            if (!askedForSourceProposalOrBudget.count(nBudgetHash)) {
                g_connman->PushMessage(pfrom, CNetMsgMaker(pfrom->GetSendVersion()).Make(NetMsgType::BUDGETVOTESYNC, nBudgetHash));
                askedForSourceProposalOrBudget[nBudgetHash] = GetTime();
            }
        }

        strError = "Finalized Budget " + nBudgetHash.ToString() +  " not found!";
        return false;
    }
    LogPrint(BCLog::MNBUDGET,"%s: Finalized Proposal %s added\n", __func__, nBudgetHash.ToString());
    return mapFinalizedBudgets[nBudgetHash].AddOrUpdateVote(vote, strError);
}

std::string CBudgetManager::ToString() const
{
    unsigned int nProposals = WITH_LOCK(cs_proposals, return mapProposals.size(); );
    unsigned int nBudgets = WITH_LOCK(cs_budgets, return mapFinalizedBudgets.size(); );

    unsigned int nSeenVotes = 0, nOrphanVotes = 0;
    {
        LOCK(cs_votes);
        nSeenVotes = mapSeenProposalVotes.size();
        nOrphanVotes = mapOrphanProposalVotes.size();
    }

    unsigned int nSeenFinalizedVotes = 0, nOrphanFinalizedVotes = 0;
    {
        LOCK(cs_finalizedvotes);
        nSeenFinalizedVotes = mapSeenFinalizedBudgetVotes.size();
        nOrphanFinalizedVotes = mapOrphanFinalizedBudgetVotes.size();
    }

    return strprintf("Proposals: %d - Finalized Budgets: %d - "
            "Proposal Votes: %d (orphan: %d) - "
            "Finalized Budget Votes: %d (orphan: %d)",
            nProposals, nBudgets,
            nSeenVotes, nOrphanVotes, nSeenFinalizedVotes, nOrphanFinalizedVotes);
}


/*
 * Check Collateral
 */
bool CheckCollateralConfs(const uint256& nTxCollateralHash, int nCurrentHeight, int nProposalHeight, std::string& strError)
{
    const int nRequiredConfs = Params().GetConsensus().nBudgetFeeConfirmations;
    const int nConf = nCurrentHeight - nProposalHeight + 1;

    if (nConf < nRequiredConfs) {
        strError = strprintf("Collateral requires at least %d confirmations - %d confirmations (current height: %d, fee tx height: %d)",
                nRequiredConfs, nConf, nCurrentHeight, nProposalHeight);
        LogPrint(BCLog::MNBUDGET,"%s: %s\n", __func__, strError);
        return false;
    }
    return true;
}

bool CheckCollateral(const uint256& nTxCollateralHash, const uint256& nExpectedHash, std::string& strError, int64_t& nTime, int nCurrentHeight, bool fBudgetFinalization)
{
    CTransactionRef txCollateral;
    uint256 nBlockHash;
    if (!GetTransaction(nTxCollateralHash, txCollateral, nBlockHash, true)) {
        strError = strprintf("Can't find collateral tx %s", nTxCollateralHash.ToString());
        return false;
    }

    if (txCollateral->vout.size() < 1) return false;
    if (txCollateral->nLockTime != 0) return false;

    CScript findScript;
    findScript << OP_RETURN << ToByteVector(nExpectedHash);

    bool foundOpReturn = false;
    for (const CTxOut &o : txCollateral->vout) {
        if (!o.scriptPubKey.IsPayToPublicKeyHash() && !o.scriptPubKey.IsUnspendable()) {
            strError = strprintf("Invalid Script %s", txCollateral->ToString());
            return false;
        }
        if (fBudgetFinalization) {
            // Collateral for budget finalization
            // Note: there are still old valid budgets out there, but the check for the new 5 PIV finalization collateral
            //       will also cover the old 50 PIV finalization collateral.
            LogPrint(BCLog::MNBUDGET, "Final Budget: o.scriptPubKey(%s) == findScript(%s) ?\n", HexStr(o.scriptPubKey), HexStr(findScript));
            if (o.scriptPubKey == findScript) {
                LogPrint(BCLog::MNBUDGET, "Final Budget: o.nValue(%ld) >= BUDGET_FEE_TX(%ld) ?\n", o.nValue, BUDGET_FEE_TX);
                if(o.nValue >= BUDGET_FEE_TX) {
                    foundOpReturn = true;
                    break;
                }
            }
        } else {
            // Collateral for normal budget proposal
            LogPrint(BCLog::MNBUDGET, "Normal Budget: o.scriptPubKey(%s) == findScript(%s) ?\n", HexStr(o.scriptPubKey), HexStr(findScript));
            if (o.scriptPubKey == findScript) {
                LogPrint(BCLog::MNBUDGET, "Normal Budget: o.nValue(%ld) >= PROPOSAL_FEE_TX(%ld) ?\n", o.nValue, PROPOSAL_FEE_TX);
                if(o.nValue >= PROPOSAL_FEE_TX) {
                    foundOpReturn = true;
                    break;
                }
            }
        }
    }

    if (!foundOpReturn) {
        strError = strprintf("Couldn't find opReturn %s in %s", nExpectedHash.ToString(), txCollateral->ToString());
        return false;
    }

    // Retrieve block height (checking that it's in the active chain) and time
    // both get set in CBudgetProposal/CFinalizedBudget by the caller (AddProposal/AddFinalizedBudget)
    if (nBlockHash.IsNull()) {
        strError = strprintf("Collateral transaction %s is unconfirmed", nTxCollateralHash.ToString());
        return false;
    }
    nTime = 0;
    int nProposalHeight = 0;
    {
        LOCK(cs_main);
        BlockMap::iterator mi = mapBlockIndex.find(nBlockHash);
        if (mi != mapBlockIndex.end() && (*mi).second) {
            CBlockIndex* pindex = (*mi).second;
            if (chainActive.Contains(pindex)) {
                nProposalHeight = pindex->nHeight;
                nTime = pindex->nTime;
            }
        }
    }

    if (!nProposalHeight) {
        strError = strprintf("Collateral transaction %s not in Active chain", nTxCollateralHash.ToString());
        return false;
    }

    return CheckCollateralConfs(nTxCollateralHash, nCurrentHeight, nProposalHeight, strError);
}
