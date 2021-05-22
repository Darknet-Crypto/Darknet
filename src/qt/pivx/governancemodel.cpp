// Copyright (c) 2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/governancemodel.h"

#include "budget/budgetmanager.h"
#include "destination_io.h"
#include "guiconstants.h"
#include "masternode-sync.h"
#include "script/standard.h"
#include "utilmoneystr.h"
#include "utilstrencodings.h"
#include "walletmodel.h"

#include <algorithm>
#include <QTimer>

GovernanceModel::GovernanceModel(ClientModel* _clientModel) : clientModel(_clientModel) {}
GovernanceModel::~GovernanceModel() {}
void GovernanceModel::setWalletModel(WalletModel* _walletModel) { walletModel = _walletModel; }

ProposalInfo GovernanceModel::buidProposalInfo(const CBudgetProposal* prop,
                              const std::vector<CBudgetProposal>& currentBudget,
                              bool isPending)
{
    CTxDestination recipient;
    ExtractDestination(prop->GetPayee(), recipient);

    // Calculate status
    int votesYes = prop->GetYeas();
    int votesNo = prop->GetNays();
    ProposalInfo::Status status;

    if (isPending) {
        // Proposal waiting for confirmation to be broadcasted.
        status = ProposalInfo::WAITING_FOR_APPROVAL;
    } else {
        if (std::find(currentBudget.begin(), currentBudget.end(), *prop) != currentBudget.end()) {
            status = ProposalInfo::PASSING;
        } else if (votesYes > votesNo) {
            status = ProposalInfo::PASSING_NOT_FUNDED;
        } else {
            status = ProposalInfo::NOT_PASSING;
        }
    }


    return ProposalInfo(prop->GetHash(),
            prop->GetName(),
            prop->GetURL(),
            votesYes,
            votesNo,
            Standard::EncodeDestination(recipient),
            prop->GetAmount(),
            prop->GetTotalPaymentCount(),
            prop->GetRemainingPaymentCount(clientModel->getLastBlockProcessedHeight()),
            status);
}

std::list<ProposalInfo> GovernanceModel::getProposals()
{
    if (!clientModel) return {};
    std::list<ProposalInfo> ret;
    std::vector<CBudgetProposal> budget = g_budgetman.GetBudget();
    for (const auto& prop : g_budgetman.GetAllProposals()) {
        ret.emplace_back(buidProposalInfo(prop, budget, false));
    }

    // Add pending proposals
    for (const auto& prop : waitingPropsForConfirmations) {
        ret.emplace_back(buidProposalInfo(&prop, budget, true));
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
    scheduleBroadcast(proposal);

    return {true};
}

void GovernanceModel::scheduleBroadcast(const CBudgetProposal& proposal)
{
    // Cache the proposal to be sent as soon as it gets the minimum required confirmations
    // without requiring user interaction
    waitingPropsForConfirmations.emplace_back(proposal);

    // Launch timer if it's not already running
    if (!pollTimer) pollTimer = new QTimer(this);
    if (!pollTimer->isActive()) {
        connect(pollTimer, &QTimer::timeout, this, &GovernanceModel::pollGovernanceChanged);
        pollTimer->start(MODEL_UPDATE_DELAY * 60 * 3.5); // Every 3.5 minutes
    }
}

void GovernanceModel::pollGovernanceChanged()
{
    if (!isTierTwoSync()) return;

    // Try to broadcast any pending for confirmations proposal
    auto it = waitingPropsForConfirmations.begin();
    while (it != waitingPropsForConfirmations.end()) {
        if (!g_budgetman.AddProposal(*it)) {
            LogPrint(BCLog::QT, "Cannot broadcast budget proposal - %s", it->IsInvalidReason());
            it++;
            continue;
        }
        it->Relay();
        it = waitingPropsForConfirmations.erase(it);
    }

    // If there are no more waiting proposals, turn the timer off.
    if (waitingPropsForConfirmations.empty()) {
        pollTimer->stop();
    }
}

void GovernanceModel::stopPolling()
{
    if (pollTimer && pollTimer->isActive()) {
        pollTimer->stop();
    }
}

