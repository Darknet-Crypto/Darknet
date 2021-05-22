// Copyright (c) 2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/governancemodel.h"

#include "budget/budgetmanager.h"
#include "destination_io.h"
#include "masternode-sync.h"
#include "script/standard.h"
#include "utilmoneystr.h"
#include "utilstrencodings.h"
#include "walletmodel.h"

#include <algorithm>

GovernanceModel::GovernanceModel(ClientModel* _clientModel) : clientModel(_clientModel) {}
void GovernanceModel::setWalletModel(WalletModel* _walletModel) { walletModel = _walletModel; }

std::list<ProposalInfo> GovernanceModel::getProposals()
{
    if (!clientModel) return {};
    std::list<ProposalInfo> ret;
    std::vector<CBudgetProposal> budget = g_budgetman.GetBudget();
    for (const auto& prop : g_budgetman.GetAllProposals()) {
        CTxDestination recipient;
        ExtractDestination(prop->GetPayee(), recipient);

        // Calculate status
        int votesYes = prop->GetYeas();
        int votesNo = prop->GetNays();
        ProposalInfo::Status status;
        if (std::find(budget.begin(), budget.end(), *prop) != budget.end()) {
            status = ProposalInfo::PASSING;
        } else if (votesYes > votesNo){
            status = ProposalInfo::PASSING_NOT_FUNDED;
        } else {
            status = ProposalInfo::NOT_PASSING;
        }

        ret.emplace_back(
                prop->GetHash(),
                prop->GetName(),
                prop->GetURL(),
                votesYes,
                votesNo,
                Standard::EncodeDestination(recipient),
                prop->GetAmount(),
                prop->GetTotalPaymentCount(),
                prop->GetRemainingPaymentCount(clientModel->getLastBlockProcessedHeight()),
                status
        );
    }
    return ret;
}

bool GovernanceModel::hasProposals()
{
    return g_budgetman.HasAnyProposal();
}

CAmount GovernanceModel::getMaxAvailableBudgetAmount() const
{
    return Params().GetConsensus().nBudgetCycleBlocks * COIN;
}

int GovernanceModel::getNumBlocksPerBudgetCycle() const
{
    return Params().GetConsensus().nBudgetCycleBlocks;
}

int GovernanceModel::getPropMaxPaymentsCount() const
{
    return Params().GetConsensus().nMaxProposalPayments;
}

OperationResult GovernanceModel::validatePropURL(const QString& url) const
{
    std::string strError;
    return {validateURL(url.toStdString(), strError, PROP_URL_MAX_SIZE), strError};
}

OperationResult GovernanceModel::validatePropAmount(CAmount amount) const
{
    if (amount > getMaxAvailableBudgetAmount()) {
        return {false, strprintf("Amount exceeding the maximum available budget amount of %s PIV", FormatMoney(amount))};
    }
    return {true};
}

OperationResult GovernanceModel::validatePropPaymentCount(int paymentCount) const
{
    if (paymentCount < 1) return { false, "Invalid payment count, must be greater than zero."};
    int nMaxPayments = getPropMaxPaymentsCount();
    if (paymentCount > nMaxPayments) {
        return { false, strprintf("Invalid payment count, cannot be greater than %d", nMaxPayments)};
    }
    return {true};
}

bool GovernanceModel::isTierTwoSync()
{
    return masternodeSync.IsBlockchainSynced();
}

OperationResult GovernanceModel::createProposal(const std::string& strProposalName,
                                                const std::string& strURL,
                                                int nPaymentCount,
                                                CAmount nAmount,
                                                const std::string& strPaymentAddr)
{
    // First get the next superblock height
    const int nBlocksPerCycle = getNumBlocksPerBudgetCycle();
    const int chainHeight = clientModel->getNumBlocks();
    int nBlockStart = chainHeight - chainHeight % nBlocksPerCycle + nBlocksPerCycle;


    // Parse address
    const CTxDestination* dest = Standard::GetTransparentDestination(Standard::DecodeDestination(strPaymentAddr));
    if (!dest) return {false, "invalid recipient address for the proposal"};
    CScript scriptPubKey = GetScriptForDestination(*dest);

    // Validate proposal
    CBudgetProposal proposal(strProposalName, strURL, nPaymentCount, scriptPubKey, nAmount, nBlockStart, UINT256_ZERO);
    if (!proposal.IsWellFormed(g_budgetman.GetTotalBudget(proposal.GetBlockStart()))) {
        return {false, strprintf("Proposal is not valid %s", proposal.IsInvalidReason())};
    }

    // Craft and send transaction.
    auto opRes = walletModel->createAndSendProposalFeeTx(proposal);
    if (!opRes) return opRes;
}