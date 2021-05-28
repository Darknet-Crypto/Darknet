// Copyright (c) 2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef GOVERNANCEMODEL_H
#define GOVERNANCEMODEL_H

#include "clientmodel.h"
#include "operationresult.h"
#include "uint256.h"

#include <atomic>
#include <string>
#include <list>
#include <utility>

#include <QObject>

// TODO: Add Passing, Passing but not funded, Not Passing, Waiting to be approved state (proposal need confirmations)
struct ProposalInfo {
public:
    enum Status {
        WAITING_FOR_APPROVAL,
        PASSING,
        PASSING_NOT_FUNDED,
        NOT_PASSING
    };

    /** Proposal hash */
    uint256 id;
    std::string name;
    std::string url;
    int votesYes;
    int votesNo;
    /** Payment script destination */
    std::string recipientAdd;
    /** Amount of PIV paid per month */
    CAmount amount;
    /** Amount of times that the proposal will be paid */
    int totalPayments;
    /** Amount of times that the proposal was paid already */
    int remainingPayments;
    /** Proposal state */
    Status status;

    ProposalInfo() {}
    explicit ProposalInfo(const uint256& _id, std::string  _name, std::string  _url,
                          int _votesYes, int _votesNo, std::string  _recipientAdd,
                          CAmount _amount, int _totalPayments, int _remainingPayments,
                          Status _status) :
            id(_id), name(std::move(_name)), url(std::move(_url)), votesYes(_votesYes), votesNo(_votesNo),
            recipientAdd(std::move(_recipientAdd)), amount(_amount), totalPayments(_totalPayments),
            remainingPayments(_remainingPayments), status(_status) {}

    bool isEmpty() { return id.IsNull(); }

    bool operator==(const ProposalInfo& prop2) const
    {
        return id == prop2.id;
    }
};

struct VoteInfo {
    enum VoteDirection {
        ABSTAIN=0,
        YES=1,
        NO=2
    };

    explicit VoteInfo(const COutPoint _mnId, VoteDirection _vote, std::string _mnAlias) : mnVoter(_mnId), vote(_vote), mnAlias(_mnAlias) {}
    COutPoint mnVoter;
    VoteDirection vote;
    std::string mnAlias;
};

class CBudgetProposal;
class TransactionRecord;
class MNModel;
class WalletModel;

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

class GovernanceModel : public QObject
{
    static const int PROP_URL_MAX_SIZE = 100;

public:
    explicit GovernanceModel(ClientModel* _clientModel, MNModel* _mnModel);
    ~GovernanceModel() override;
    void setWalletModel(WalletModel* _walletModel);

    // Return proposals ordered by net votes
    std::list<ProposalInfo> getProposals();
    // Returns true if there is at least one proposal cached
    bool hasProposals();
    // Whether a visual refresh is needed
    bool isRefreshNeeded() { return refreshNeeded; }
    // Return the number of blocks per budget cycle
    int getNumBlocksPerBudgetCycle() const;
    // Return the budget maximum available amount for the running chain
    CAmount getMaxAvailableBudgetAmount() const;
    // Return the proposal maximum payments count for the running chain
    int getPropMaxPaymentsCount() const;
    int getNextSuperblockHeight() const;
    // Returns the sum of all of the passing proposals
    CAmount getBudgetAllocatedAmount() const { return allocatedAmount; };
    CAmount getBudgetAvailableAmount() const { return getMaxAvailableBudgetAmount() - allocatedAmount; };
    // Return the votes that the local masternodes did for the inputted proposal
    std::vector<VoteInfo> getLocalMNsVotesForProposal(const ProposalInfo& propInfo);
    // Check if the URL is valid.
    OperationResult validatePropURL(const QString& url) const;
    OperationResult validatePropAmount(CAmount amount) const;
    OperationResult validatePropPaymentCount(int paymentCount) const;
    // Whether the tier two network synchronization has finished or not
    bool isTierTwoSync();

    // Creates a proposal, crafting and broadcasting the fee transaction,
    // storing it locally to be broadcasted when the fee tx proposal depth
    // fulfills the minimum depth requirements
    OperationResult createProposal(const std::string& strProposalName,
                                   const std::string& strURL,
                                   int nPaymentCount,
                                   CAmount nAmount,
                                   const std::string& strPaymentAddr);

    OperationResult voteForProposal(const ProposalInfo& prop,
                                    bool isVotePositive,
                                    const std::vector<std::string>& mnVotingAlias);
public Q_SLOTS:
    void pollGovernanceChanged();
    void txLoaded(const QString& hash, const int txType, const int txStatus);

private:
    ClientModel* clientModel{nullptr};
    WalletModel* walletModel{nullptr};
    MNModel* mnModel{nullptr};
    std::atomic<bool> refreshNeeded{false};

    // Cached amount
    CAmount allocatedAmount{0};

    QTimer* pollTimer{nullptr};
    // Cached proposals waiting for the minimum required confirmations
    // to be broadcasted to the network.
    std::vector<CBudgetProposal> waitingPropsForConfirmations;

    void scheduleBroadcast(const CBudgetProposal& proposal);
    void stopPolling();

    // Util function to create a ProposalInfo object
    ProposalInfo buidProposalInfo(const CBudgetProposal* prop, bool isPassing, bool isPending);
};

#endif // GOVERNANCEMODEL_H
