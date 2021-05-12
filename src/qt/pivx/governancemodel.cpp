#include "qt/pivx/governancemodel.h"

#include "budget/budgetmanager.h"
#include "destination_io.h"
#include "script/standard.h"

// Cambiar este número para crear más propuestas
const int PROPOSALS_AMOUNT = 8;

GovernanceModel::GovernanceModel(ClientModel* _clientModel) : clientModel(_clientModel)
{
}

std::list<ProposalInfo> GovernanceModel::getProposals()
{
    if (!clientModel) return {};
    std::list<ProposalInfo> ret;
    for (const auto& prop : g_budgetman.GetAllProposals()) {
        CTxDestination recipient;
        ExtractDestination(prop->GetPayee(), recipient);
        ret.emplace_back(
                prop->GetHash(),
                prop->GetName(),
                prop->GetURL(),
                prop->GetYeas(),
                prop->GetNays(),
                Standard::EncodeDestination(recipient),
                prop->GetAmount(),
                prop->GetTotalPaymentCount(),
                prop->GetRemainingPaymentCount(clientModel->getLastBlockProcessedHeight())
        );
    }
    return ret;
}

bool GovernanceModel::hasProposals()
{
    return g_budgetman.HasAnyProposal();
}

