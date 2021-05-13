#!/usr/bin/env python3
# Copyright (c) 2020 The PIVX developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php.

from test_framework.test_framework import PivxTier2TestFramework
from test_framework.util import (
    assert_equal,
    assert_true,
    Decimal,
)

import time

"""
Test checking:
 1) Masternodes setup/creation.
 2) Proposal creation.
 3) Vote creation.
 4) Proposal and vote broadcast.
 5) Proposal and vote sync.
"""

class Proposal:
    def __init__(self,
                 proposalName,
                 proposalLink,
                 proposalCycles,
                 proposalAddress,
                 proposalAmountPerCycle):
        self.proposalName = proposalName
        self.proposalLink = proposalLink
        self.proposalCycles = proposalCycles
        self.proposalAddress = proposalAddress
        self.proposalAmountPerCycle = proposalAmountPerCycle


class MasternodeGovernanceBasicTest(PivxTier2TestFramework):

    def check_budget_finalization_sync(self, votesCount, status):
        for i in range(0, len(self.nodes)):
            node = self.nodes[i]
            budFin = node.mnfinalbudget("show")
            assert_true(len(budFin) == 1, "MN budget finalization not synced in node" + str(i))
            budget = budFin[next(iter(budFin))]
            assert_equal(budget["VoteCount"], votesCount)
            assert_equal(budget["Status"], status)

    def broadcastbudgetfinalization(self, node, with_ping_mns=[]):
        self.log.info("suggesting the budget finalization..")
        assert (node.mnfinalbudgetsuggest() is not None)

        self.log.info("confirming the budget finalization..")
        time.sleep(1)
        self.stake(4, with_ping_mns)

        self.log.info("broadcasting the budget finalization..")
        return node.mnfinalbudgetsuggest()

    def check_proposal_existence(self, proposalName, proposalHash):
        for node in self.nodes:
            proposals = node.getbudgetinfo(proposalName)
            assert(len(proposals) > 0)
            assert_equal(proposals[0]["Hash"], proposalHash)

    def check_vote_existence(self, proposalName, mnCollateralHash, voteType):
        for i in range(0, len(self.nodes)):
            node = self.nodes[i]
            votesInfo = node.getbudgetvotes(proposalName)
            assert(len(votesInfo) > 0)
            found = False
            for voteInfo in votesInfo:
                if (voteInfo["mnId"].split("-")[0] == mnCollateralHash) :
                    assert_equal(voteInfo["Vote"], voteType)
                    found = True
            assert_true(found, "Error checking vote existence in node " + str(i))

    def get_proposal_obj(self, Name, URL, Hash, FeeHash, BlockStart, BlockEnd,
                             TotalPaymentCount, RemainingPaymentCount, PaymentAddress,
                             Ratio, Yeas, Nays, Abstains, TotalPayment, MonthlyPayment,
                             IsEstablished, IsValid, Allotted, TotalBudgetAllotted, IsInvalidReason = ""):
        obj = {}
        obj["Name"] = Name
        obj["URL"] = URL
        obj["Hash"] = Hash
        obj["FeeHash"] = FeeHash
        obj["BlockStart"] = BlockStart
        obj["BlockEnd"] = BlockEnd
        obj["TotalPaymentCount"] = TotalPaymentCount
        obj["RemainingPaymentCount"] = RemainingPaymentCount
        obj["PaymentAddress"] = PaymentAddress
        obj["Ratio"] = Ratio
        obj["Yeas"] = Yeas
        obj["Nays"] = Nays
        obj["Abstains"] = Abstains
        obj["TotalPayment"] = TotalPayment
        obj["MonthlyPayment"] = MonthlyPayment
        obj["IsEstablished"] = IsEstablished
        obj["IsValid"] = IsValid
        if IsInvalidReason != "":
            obj["IsInvalidReason"] = IsInvalidReason
        obj["Allotted"] = Allotted
        obj["TotalBudgetAllotted"] = TotalBudgetAllotted
        return obj

    def check_budgetprojection(self, expected):
        for i in range(self.num_nodes):
            assert_equal(self.nodes[i].getbudgetprojection(), expected)
            self.log.info("Budget projection valid for node %d" % i)

    def create_multisig_addr(self, node):
        addr1 = node.getnewaddress()
        addr2 = node.getnewaddress()
        addr3 = node.getnewaddress()
        sig_address_1 = node.validateaddress(addr1)
        sig_address_2 = node.validateaddress(addr2)
        sig_address_3 = node.validateaddress(addr3)
        return node.addmultisigaddress(2, [sig_address_1['pubkey'], sig_address_2['pubkey'], sig_address_3['pubkey']])

    def run_test(self):
        self.enable_mocktime()
        self.setup_2_masternodes_network()

        self.log.info("activating sporks..")
        # activate sporks
        self.activate_spork(self.minerPos, "SPORK_8_MASTERNODE_PAYMENT_ENFORCEMENT")
        self.activate_spork(self.minerPos, "SPORK_9_MASTERNODE_BUDGET_ENFORCEMENT")
        self.activate_spork(self.minerPos, "SPORK_13_ENABLE_SUPERBLOCKS")

        # Prepare the proposals
        self.log.info("preparing budget proposal..")
        firstProposal = Proposal("super-cool", "https://forum.pivx.org/t/test-proposal", 2, self.miner.getnewaddress(), 300)
        multisigAddr = self.create_multisig_addr(self.miner)
        multiSigProposal = Proposal("multi-sig", "https://forum.pivx.org/t/test-proposal-multisig", 2, multisigAddr, 300)
        nextSuperBlockHeight = self.miner.getnextsuperblock()

        proposalFeeTxId = self.miner.preparebudget(
            firstProposal.proposalName,
            firstProposal.proposalLink,
            firstProposal.proposalCycles,
            nextSuperBlockHeight,
            firstProposal.proposalAddress,
            firstProposal.proposalAmountPerCycle)

        # submit second one
        proposalFeeTxIdMultiSig = self.miner.preparebudget(
            multiSigProposal.proposalName,
            multiSigProposal.proposalLink,
            multiSigProposal.proposalCycles,
            nextSuperBlockHeight,
            multiSigProposal.proposalAddress,
            multiSigProposal.proposalAmountPerCycle)

        # generate 3 blocks to confirm the tx (and update the mnping)
        self.stake(3, [self.remoteOne, self.remoteTwo])

        txinfo = self.miner.gettransaction(proposalFeeTxId)
        assert_equal(txinfo['amount'], -50.00)

        txinfo = self.miner.gettransaction(proposalFeeTxIdMultiSig)
        assert_equal(txinfo['amount'], -50.00)
        assert(proposalFeeTxId != proposalFeeTxIdMultiSig)

        self.log.info("submitting budget proposals..")

        # submit regular proposal
        proposalHash = self.miner.submitbudget(
            firstProposal.proposalName,
            firstProposal.proposalLink,
            firstProposal.proposalCycles,
            nextSuperBlockHeight,
            firstProposal.proposalAddress,
            firstProposal.proposalAmountPerCycle,
            proposalFeeTxId)

        # submit multisig proposal
        proposalHashMultiSig = self.miner.submitbudget(
            multiSigProposal.proposalName,
            multiSigProposal.proposalLink,
            multiSigProposal.proposalCycles,
            nextSuperBlockHeight,
            multiSigProposal.proposalAddress,
            multiSigProposal.proposalAmountPerCycle,
            proposalFeeTxIdMultiSig)

        # let's wait a little bit and see if all nodes are sync
        time.sleep(1)
        self.check_proposal_existence(firstProposal.proposalName, proposalHash)
        self.check_proposal_existence(multiSigProposal.proposalName, proposalHashMultiSig)
        self.log.info("proposals broadcasted successfully!")

        # Proposal is established after 5 minutes. Mine 7 blocks
        # Proposal needs to be on the chain > 5 min.
        self.stake(7, [self.remoteOne, self.remoteTwo])

        # now let's vote for the proposal with the first MN
        self.log.info("broadcasting votes for the proposals now..")
        voteResult = self.ownerOne.mnbudgetvote("alias", proposalHash, "yes", self.masternodeOneAlias)
        assert_equal(voteResult["detail"][0]["result"], "success")

        voteResult = self.ownerOne.mnbudgetvote("alias", proposalHashMultiSig, "yes", self.masternodeOneAlias)
        assert_equal(voteResult["detail"][0]["result"], "success")

        # check that the vote was accepted everywhere
        self.stake(1, [self.remoteOne, self.remoteTwo])
        self.check_vote_existence(firstProposal.proposalName, self.mnOneTxHash, "YES")
        self.check_vote_existence(multiSigProposal.proposalName, self.mnOneTxHash, "YES")
        self.log.info("all good, MN1 vote accepted everywhere!")

        # now let's vote for the proposal with the second MN
        voteResult = self.ownerTwo.mnbudgetvote("alias", proposalHash, "yes", self.masternodeTwoAlias)
        assert_equal(voteResult["detail"][0]["result"], "success")

        # check that the vote was accepted everywhere
        self.stake(1, [self.remoteOne, self.remoteTwo])
        self.check_vote_existence(firstProposal.proposalName, self.mnTwoTxHash, "YES")
        self.log.info("all good, MN2 vote accepted everywhere!")

        # Now check the budget
        blockStart = nextSuperBlockHeight
        blockEnd = blockStart + firstProposal.proposalCycles * 145
        TotalPayment = firstProposal.proposalAmountPerCycle * firstProposal.proposalCycles
        Allotted = firstProposal.proposalAmountPerCycle
        RemainingPaymentCount = firstProposal.proposalCycles
        # 2 votes for the regular one, 1 vote for the multisig proposal
        expected_budget = [
            self.get_proposal_obj(firstProposal.proposalName, firstProposal.proposalLink, proposalHash, proposalFeeTxId, blockStart,
                                  blockEnd, firstProposal.proposalCycles, RemainingPaymentCount, firstProposal.proposalAddress, 1,
                                  2, 0, 0, Decimal(str(TotalPayment)), Decimal(str(firstProposal.proposalAmountPerCycle)),
                                  True, True, Decimal(str(Allotted)), Decimal(str(Allotted))),
            self.get_proposal_obj(multiSigProposal.proposalName, multiSigProposal.proposalLink, proposalHashMultiSig, proposalFeeTxIdMultiSig, blockStart,
                                  blockEnd, multiSigProposal.proposalCycles, RemainingPaymentCount, multiSigProposal.proposalAddress, 1,
                                  1, 0, 0, Decimal(str(TotalPayment)), Decimal(str(multiSigProposal.proposalAmountPerCycle)),
                                  True, True, Decimal(str(Allotted)), Decimal(str(Allotted * 2)))
                           ]
        self.check_budgetprojection(expected_budget)

        # Quick block count check.
        assert_equal(self.ownerOne.getblockcount(), 276)

        self.log.info("starting budget finalization sync test..")
        self.stake(5, [self.remoteOne, self.remoteTwo])

        # assert that there is no budget finalization first.
        assert_true(len(self.ownerOne.mnfinalbudget("show")) == 0)

        # suggest the budget finalization and confirm the tx (+4 blocks).
        budgetFinHash = self.broadcastbudgetfinalization(self.miner,
                                                         with_ping_mns=[self.remoteOne, self.remoteTwo])
        assert (budgetFinHash != "")
        time.sleep(1)

        self.log.info("checking budget finalization sync..")
        self.check_budget_finalization_sync(0, "OK")

        self.log.info("budget finalization synced!, now voting for the budget finalization..")

        self.ownerOne.mnfinalbudget("vote-many", budgetFinHash)
        self.ownerTwo.mnfinalbudget("vote-many", budgetFinHash)
        self.stake(2, [self.remoteOne, self.remoteTwo])

        self.log.info("checking finalization votes..")
        self.check_budget_finalization_sync(2, "OK")

        self.stake(8, [self.remoteOne, self.remoteTwo])
        addrInfo = self.miner.listreceivedbyaddress(0, False, False, firstProposal.proposalAddress)
        assert_equal(addrInfo[0]["amount"], firstProposal.proposalAmountPerCycle)

        addrInfo = self.miner.listreceivedbyaddress(0, False, False, multiSigProposal.proposalAddress)
        assert_equal(addrInfo[0]["amount"], multiSigProposal.proposalAmountPerCycle)

        self.log.info("budget proposal paid!, all good")

        # Check that the proposal info returns updated payment count
        expected_budget[0]["RemainingPaymentCount"] -= 1
        expected_budget[1]["RemainingPaymentCount"] -= 1
        self.check_budgetprojection(expected_budget)





if __name__ == '__main__':
    MasternodeGovernanceBasicTest().main()