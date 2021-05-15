// Copyright (c) 2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/governancemodel.h"

#include "budget/budgetmanager.h"
#include "destination_io.h"
#include "script/standard.h"

#include <algorithm>

GovernanceModel::GovernanceModel(ClientModel* _clientModel) : clientModel(_clientModel)
{
}

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

