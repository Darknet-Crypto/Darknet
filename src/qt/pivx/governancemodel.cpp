// Copyright (c) 2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/governancemodel.h"

#include "budget/budgetmanager.h"
#include "budget/budgetutil.h"
#include "destination_io.h"
#include "guiconstants.h"
#include "masternode-sync.h"
#include "script/standard.h"
#include "qt/transactiontablemodel.h"
#include "qt/transactionrecord.h"
#include "qt/pivx/mnmodel.h"
#include "utilmoneystr.h"
#include "utilstrencodings.h"
#include "walletmodel.h"

#include <algorithm>
#include <QTimer>

GovernanceModel::GovernanceModel(ClientModel* _clientModel, MNModel* _mnModel) : clientModel(_clientModel), mnModel(_mnModel) {}
GovernanceModel::~GovernanceModel() {}

void GovernanceModel::setWalletModel(WalletModel* _walletModel)
{
    walletModel = _walletModel;
    connect(walletModel->getTransactionTableModel(), &TransactionTableModel::txLoaded, this, &GovernanceModel::txLoaded);
}

ProposalInfo GovernanceModel::buidProposalInfo(const CBudgetProposal* prop, bool isPassing, bool isPending)
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
        if (isPassing) {
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

std::list<ProposalInfo> GovernanceModel::getProposals(const ProposalInfo::Status* filterByStatus)
{
    if (!clientModel) return {};
    std::list<ProposalInfo> ret;
    std::vector<CBudgetProposal> budget = g_budgetman.GetBudget();
    for (const auto& prop : g_budgetman.GetAllProposals()) {
        bool isPassing = std::find(budget.begin(), budget.end(), *prop) != budget.end();
        ProposalInfo propInfo = buidProposalInfo(prop, isPassing, false);
        if (!filterByStatus || propInfo.status == *filterByStatus) {
            ret.emplace_back(propInfo);
        }
        if (isPassing) allocatedAmount += prop->GetAmount();
    }

    // Add pending proposals
    for (const auto& prop : waitingPropsForConfirmations) {
        ProposalInfo propInfo = buidProposalInfo(&prop, false, true);
        if (!filterByStatus || propInfo.status == *filterByStatus) {
            ret.emplace_back(propInfo);
        }
    }
    return ret;
}

bool GovernanceModel::hasProposals()
{
    return g_budgetman.HasAnyProposal() || !waitingPropsForConfirmations.empty();
}

CAmount GovernanceModel::getMaxAvailableBudgetAmount() const
{
    return g_budgetman.GetTotalBudget(clientModel->getNumBlocks());
}

int GovernanceModel::getNumBlocksPerBudgetCycle() const
{
    return Params().GetConsensus().nBudgetCycleBlocks;
}

int GovernanceModel::getPropMaxPaymentsCount() const
{
    return Params().GetConsensus().nMaxProposalPayments;
}

int GovernanceModel::getNextSuperblockHeight() const
{
    const int nBlocksPerCycle = getNumBlocksPerBudgetCycle();
    const int chainHeight = clientModel->getNumBlocks();
    return chainHeight - chainHeight % nBlocksPerCycle + nBlocksPerCycle;
}

std::vector<VoteInfo> GovernanceModel::getLocalMNsVotesForProposal(const ProposalInfo& propInfo)
{
    // First, get the local masternodes
    std::vector<std::pair<COutPoint, std::string>> vecLocalMn;
    for (int i = 0; i < mnModel->rowCount(); ++i) {
        vecLocalMn.emplace_back(std::make_pair(
                COutPoint(uint256S(mnModel->index(i, MNModel::COLLATERAL_ID, QModelIndex()).data().toString().toStdString()),
                mnModel->index(i, MNModel::COLLATERAL_OUT_INDEX, QModelIndex()).data().toInt()),
                mnModel->index(i, MNModel::ALIAS, QModelIndex()).data().toString().toStdString())
        );
    }

    std::vector<VoteInfo> localVotes;
    // Get the budget proposal, get the votes, then loop over it and return the ones that correspond to the local masternodes here.
    CBudgetProposal* prop = g_budgetman.FindProposal(propInfo.id);
    const auto& mapVotes = prop->GetVotes();
    for (const auto& it : mapVotes) {
        for (const auto& mn : vecLocalMn) {
            if (it.first == mn.first && it.second.IsValid()) {
                localVotes.emplace_back(mn.first, (VoteInfo::VoteDirection) it.second.GetDirection(), mn.second);
                break;
            }
        }
    }
    return localVotes;
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
    int nBlockStart = getNextSuperblockHeight();

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

OperationResult GovernanceModel::voteForProposal(const ProposalInfo& prop,
                                                 bool isVotePositive,
                                                 const std::vector<std::string>& mnVotingAlias)
{
    UniValue ret; // future: don't use UniValue here.
    for (const auto& mnAlias : mnVotingAlias) {
        bool fLegacyMN = true; // For now, only legacy MNs
        ret = mnBudgetVoteInner(nullptr,
                          fLegacyMN,
                          prop.id,
                          false,
                          isVotePositive ? CBudgetVote::VoteDirection::VOTE_YES : CBudgetVote::VoteDirection::VOTE_NO,
                          mnAlias);
        if (ret.exists("detail") && ret["detail"].isArray()) {
            const UniValue& obj = ret["detail"].get_array()[0];
            if (obj["result"].getValStr() != "success") {
                return {false, obj["error"].getValStr()};
            }
        }
    }
    // add more information with ret["overall"]
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
        pollTimer->start(MODEL_UPDATE_DELAY * 60 * (walletModel->isTestNetwork() ? 0.5 : 3.5)); // Every 3.5 minutes
    }
}

void GovernanceModel::pollGovernanceChanged()
{
    if (!isTierTwoSync()) return;

    int chainHeight = clientModel->getNumBlocks();
    // Try to broadcast any pending for confirmations proposal
    auto it = waitingPropsForConfirmations.begin();
    while (it != waitingPropsForConfirmations.end()) {
        // Remove expired proposals
        if (it->IsExpired(clientModel->getNumBlocks())) {
            it = waitingPropsForConfirmations.erase(it);
            continue;
        }

        // Try to add it
        if (!g_budgetman.AddProposal(*it)) {
            LogPrint(BCLog::QT, "Cannot broadcast budget proposal - %s", it->IsInvalidReason());
            // Remove proposals who due a reorg lost their fee tx
            if (it->IsInvalidReason().find("Can't find collateral tx") != std::string::npos) {
                // future: notify the user about it.
                it = waitingPropsForConfirmations.erase(it);
                continue;
            }
            // Check if the proposal didn't exceed the superblock start height
            if (it->GetBlockStart() >= chainHeight) {
                // Edge case, the proposal was never broadcasted before the next superblock, can be removed.
                // future: notify the user about it.
                it = waitingPropsForConfirmations.erase(it);
            } else {
                it++;
            }
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

void GovernanceModel::txLoaded(const QString& id, const int txType, const int txStatus)
{
    if (txType == TransactionRecord::SendToNobody) {
        // If the tx is not longer available in the mainchain, drop it.
        if (txStatus == TransactionStatus::Conflicted ||
            txStatus == TransactionStatus::NotAccepted) {
            return;
        }
        // If this is a proposal fee, parse it.
        const auto& wtx = walletModel->getTx(uint256S(id.toStdString()));
        assert(wtx);
        const auto& it = wtx->mapValue.find("proposal");
        if (it != wtx->mapValue.end()) {
            const std::vector<unsigned char> vec = ParseHex(it->second);
            if (vec.empty()) return;
            CDataStream ss(vec, SER_DISK, CLIENT_VERSION);
            CBudgetProposal proposal;
            ss >> proposal;
            if (!g_budgetman.HaveProposal(proposal.GetHash()) &&
                !proposal.IsExpired(clientModel->getNumBlocks()) &&
                proposal.GetBlockStart() < clientModel->getNumBlocks()) {
                scheduleBroadcast(proposal);
            }
        }
    }
}
