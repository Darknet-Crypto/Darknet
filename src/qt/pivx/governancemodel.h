#ifndef GOVERNANCEMODEL_H
#define GOVERNANCEMODEL_H

#include "uint256.h"
#include "clientmodel.h"

#include <atomic>
#include <string>
#include <list>
#include <utility>

// TODO: Add Passing, Not Passing, Waiting to be approved state (proposal need confirmations)
struct ProposalInfo {
public:
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
    int paidPayments;

    ProposalInfo() {}
    explicit ProposalInfo(const uint256& _id, std::string  _name, std::string  _url,
                          int _votesYes, int _votesNo, std::string  _recipientAdd,
                          CAmount _amount, int _totalPayments, int _paidPayments) :
            id(_id), name(std::move(_name)), url(std::move(_url)), votesYes(_votesYes), votesNo(_votesNo),
            recipientAdd(std::move(_recipientAdd)), amount(_amount), totalPayments(_totalPayments),
            paidPayments(_paidPayments) {}

    bool isEmpty() { return id.IsNull(); }

    bool operator==(const ProposalInfo& prop2) const
    {
        return id == prop2.id;
    }
};

class GovernanceModel
{

public:
    explicit GovernanceModel(ClientModel* _clientModel);

    // Return proposals ordered by net votes
    std::list<ProposalInfo> getProposals();
    // Returns true if there is at least one proposal cached
    bool hasProposals();
    // Whether a visual refresh is needed
    bool isRefreshNeeded() { return refreshNeeded; }

private:
    ClientModel* clientModel{nullptr};
    std::atomic<bool> refreshNeeded{false};
};

#endif // GOVERNANCEMODEL_H
