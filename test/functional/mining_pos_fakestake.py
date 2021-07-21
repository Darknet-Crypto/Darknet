#!/usr/bin/env python3
# Copyright (c) 2019-2020 The PIVX developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
Covers various scenarios of PoS blocks where the coinstake input is already spent
(either in a previous block, in a "future" block, or in the same block being staked).
Two nodes: nodes[0] moves the chain and checks the spam blocks, nodes[1] sends them.
Spend txes sent from nodes[1] are received by nodes[0]
Start with the PoW chache: 200 blocks.
For each test, nodes[1] sends 3 blocks.

At the beginning nodes[0] mines 50 blocks (201-250) to reach PoS activation.

** Test_1:
(Nodes[1] spams a PoS block on main chain.)
(Stake inputs spent on the same block being staked.)
--> starts at height 250
  - nodes[1] saves his mature utxos 'utxos_to_spend' at block 250
  - nodes[0] mines 5 blocks (251-255) to be used as buffer for adding the fork chain later
  - nodes[1] spams 3 blocks with height 256 --> [REJECTED]
--> ends at height 255

** Test_2
(Nodes[1] spams a PoS block on main chain.)
(Stake inputs spent earlier on main chain.)
--> starts at height 255
  - nodes[1] spends utxos_to_spend at block 256
  - nodes[0] mines 5 more blocks (256-260) to include the spends
  - nodes[1] spams 3 blocks with height 261 --> [REJECTED]
  - nodes[1] spams fork block with height 258 --> [REJECTED] (coinstake input spent on active chain before the split)
--> ends at height 260

** Test_3:
(Nodes[1] spams PoS blocks on a forked chain.)
(Stake inputs spent later on main chain.)
--> starts at height 260
  - nodes[1] spams fork block with height 251 --> [ACCEPTED]
  - nodes[1] spams fork block with height 252
    (using the same coinstake input as previous block) --> [REJECTED]
  - nodes[1] spams fork block with height 252
    (using a different coinstake input) --> [ACCEPTED]
--> ends at height 260
"""

from io import BytesIO
import time
from time import sleep

from test_framework.authproxy import JSONRPCException
from test_framework.messages import COutPoint
from test_framework.test_framework import PivxTestFramework
from test_framework.util import (
    assert_equal,
    bytes_to_hex_str,
    set_node_times
)


class FakeStakeTest(PivxTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        # whitelist all peers to speed up tx relay / mempool sync
        self.extra_args = [["-whitelist=127.0.0.1"]] * self.num_nodes
        self.enable_mocktime()
        # nodes[0] moves the chain and checks the spam blocks, nodes[1] sends them

    def log_title(self):
        title = "*** Starting %s ***" % self.__class__.__name__
        underline = "-" * len(title)
        description = "Tests the 'fake stake' scenarios.\n" \
                      "1) Stake on main chain with coinstake input spent on the same block\n" \
                      "2) Stake on main and fork chain with coinstake input spent on a previous block\n" \
                      "3) Stake on a fork chain with coinstake input spent (later) in main chain\n"
        self.log.info("\n\n%s\n%s\n%s\n", title, underline, description)

    def run_test(self):
        # init custom fields
        self.mocktime = int(time.time())
        set_node_times(self.nodes, self.mocktime)
        self.recipient_0 = self.nodes[0].getnewaddress()
        self.recipient_1 = self.nodes[1].getnewaddress()
        self.init_dummy_key()

        # start test
        self.log_title()

        # nodes[0] mines 50 blocks (201-250) to reach PoS activation
        self.log.info("Mining 50 blocks to reach PoS phase...")
        for i in range(50):
            self.mocktime = self.generate_pow(0, self.mocktime)
        self.sync_blocks()

        # Check Tests 1-3
        self.test_1()
        self.test_2()
        self.test_3()


    # ** PoS block - cstake input spent on the same block
    def test_1(self):
        print()   # add blank line without log indent
        self.log.info("** Fake Stake - Test_1")

        # nodes[1] saves his mature utxos 'utxos_to_spend' at block 250
        assert_equal(self.nodes[1].getblockcount(), 250)
        self.utxos_to_spend = self.nodes[1].listunspent()
        assert_equal(len(self.utxos_to_spend), 50)
        self.log.info("50 'utxos_to_spend' collected.")

        # nodes[0] mines 5 blocks (251-255) to be used as buffer for adding the fork chain later
        self.log.info("Mining 5 blocks as fork depth...")
        for i in range(5):
            self.mocktime = self.generate_pos(0, self.mocktime)
        self.sync_blocks()

        # nodes[1] spams 3 blocks with height 256 --> [REJECTED]
        assert_equal(self.nodes[1].getblockcount(), 255)
        self.fake_stake(list(self.utxos_to_spend), fDoubleSpend=True)
        self.log.info("--> Test_1 passed")

    # ** PoS block - cstake input spent in the past
    def test_2(self):
        print()   # add blank line without log indent
        self.log.info("** Fake Stake - Test_2")

        # nodes[1] spends utxos_to_spend at block 256
        assert_equal(self.nodes[1].getblockcount(), 255)
        txid = self.spend_utxos(1, self.utxos_to_spend, self.recipient_0)[0]
        self.log.info("'utxos_to_spend' spent on txid=(%s...) on block 256" % txid[:16])
        self.sync_mempools()

        # nodes[0] mines 5 more blocks (256-260) to include the spends
        self.log.info("Mining 5 blocks to include the spends...")
        for i in range(5):
            self.mocktime = self.generate_pos(0, self.mocktime)
        self.sync_blocks()
        self.check_tx_in_chain(0, txid)
        assert_equal(self.nodes[1].getbalance(), 0)

        # nodes[1] spams 3 blocks with height 261 --> [REJECTED]
        self.log.info("Test rejection on active chain...")
        assert_equal(self.nodes[1].getblockcount(), 260)
        self.fake_stake(list(self.utxos_to_spend))

        # nodes[1] spams fork block with height 258 --> [REJECTED]
        self.log.info("Test rejection on fork chain...")
        assert_equal(self.nodes[1].getblockcount(), 260)
        prevBlockHash = self.nodes[1].getblockhash(257)
        prevModifier = self.nodes[1].getblock(prevBlockHash)['stakeModifier']
        block = self.stake_block(1, 7, 258, prevBlockHash, prevModifier, "0",
                                 self.get_prevouts(1, list(self.utxos_to_spend)), self.mocktime, "", [], False)
        self.send_block_and_check_error(block, "tx inputs spent/not-available on forked chain pre-split")
        assert_equal(self.nodes[1].getblockcount(), 260)
        self.log.info("--> Test_2 passed")

    # ** PoS block - cstake input spent in the future
    def test_3(self):
        print()  # add blank line without log indent
        self.log.info("** Fake Stake - Test_3")

        # nodes[1] spams fork block with height 251 --> [ACCEPTED]
        # nodes[1] spams fork block with height 252
        #  (using the same coinstake input as previous block) --> [REJECTED]
        # nodes[1] spams fork block with height 252
        #  (using a different coinstake input) --> [ACCEPTED]
        assert_equal(self.nodes[1].getblockcount(), 260)
        self.fake_stake(list(self.utxos_to_spend), nHeight=251)
        self.log.info("--> Test_3 passed")

    def send_block_and_check_error(self, block, error_mess):
        with self.nodes[1].assert_debug_log([error_mess]):
            self.nodes[1].submitblock(bytes_to_hex_str(block.serialize()))


    def fake_stake(self,
                   staking_utxo_list,
                   nHeight=-1,
                   fDoubleSpend=False):
        """ General method to create, send and test the spam blocks
        :param    staking_utxo_list:  (string list) utxos to use for staking
                  nHeight:            (int, optional) height of the staked block.
                                        Used only for fork chain. In main chain it's current height + 1
                  fDoubleSpend:       (bool) if true, stake input is double spent in block.vtx
        :return:
        """
        def get_prev_modifier(prevBlockHash):
            prevBlock = self.nodes[1].getblock(prevBlockHash)
            if prevBlock['height'] > 250:
                return prevBlock['stakeModifier']
            return "0"

        # Get block number, block time and prevBlock hash and modifier
        currHeight = self.nodes[1].getblockcount()
        isMainChain = (nHeight == -1)
        chainName = "main" if isMainChain else "forked"
        nTime = self.mocktime
        if isMainChain:
            nHeight = currHeight + 1
        prevBlockHash = self.nodes[1].getblockhash(nHeight - 1)
        prevModifier = get_prev_modifier(prevBlockHash)
        nTime += (nHeight - currHeight) * 60

        # New block hash, coinstake input and list of txes
        bHash = None
        stakedUtxo = None

        # For each test, send three blocks.
        # On main chain they are all the same height.
        # On fork chain, send three blocks where both the second and third block sent,
        # are built on top of the first one.
        for i in range(3):
            fMustBeAccepted = (not isMainChain and i != 1)
            block_txes = []

            # update block number and prevBlock hash on second block sent on forked chain
            if not isMainChain and i == 1:
                nHeight += 1
                nTime += 60
                prevBlockHash = bHash
                prevModifier = get_prev_modifier(prevBlockHash)

            stakeInputs = self.get_prevouts(1, staking_utxo_list)
            # Update stake inputs for second block sent on forked chain (must stake the same input)
            if not isMainChain and i == 1:
                stakeInputs = self.get_prevouts(1, [stakedUtxo])

            # Make spam txes sending the inputs to DUMMY_KEY in order to test double spends
            if fDoubleSpend:
                spending_prevouts = self.get_prevouts(1, staking_utxo_list)
                block_txes = self.make_txes(1, spending_prevouts, self.DUMMY_KEY.get_pubkey())

            # Stake the spam block
            block = self.stake_block(1, 7, nHeight, prevBlockHash, prevModifier, "0", stakeInputs,
                                     nTime, "", block_txes, fDoubleSpend)
            # Log stake input
            prevout = COutPoint()
            prevout.deserialize_uniqueness(BytesIO(block.prevoutStake))
            self.log.info("Staked input: [%s...-%s]" % ('{:x}'.format(prevout.hash)[:12], prevout.n))

            # Try submitblock and check result
            self.log.info("Trying to send block [%s...] with height=%d" % (block.hash[:16], nHeight))
            if not fMustBeAccepted:
                if fDoubleSpend:
                    reject_log = "inputs double spent in the same block"
                elif isMainChain:
                    reject_log = "tx inputs spent/not-available on main chain"
                else:
                    reject_log = "tx inputs spent on forked chain post-split"
                self.send_block_and_check_error(block, reject_log)
            else:
                var = self.nodes[1].submitblock(bytes_to_hex_str(block.serialize()))
                if (var != "inconclusive"):
                    raise AssertionError("Error, block not submitted (%s) in %s chain" % (var, chainName))
            self.log.info("Done.")

            # Sync and check block hash
            bHash = block.hash
            self.checkBlockHash(bHash, fMustBeAccepted)

            # Update curr block data
            stakedUtxo = [x for x in staking_utxo_list if COutPoint(
                int(x['txid'], 16), x['vout']).serialize_uniqueness() == block.prevoutStake][0]

            # Remove the used coinstake input (except before second block on fork chain)
            if isMainChain or i != 0:
                staking_utxo_list.remove(stakedUtxo)

        self.log.info("All blocks sent")


    def checkBlockHash(self, bHash, fMustBeAccepted):

        def strBlockCheck(bHash, fAccepted=True, fError=False):
            return "%s Block [%s...] IS %sstored on disk" % (
                "Error!" if fError else "Good.", bHash[:16], "" if fAccepted else "NOT ")
        try:
            block_ret = self.nodes[1].getblock(bHash)
            if not fMustBeAccepted:
                if block_ret is not [None, None]:
                    self.log.warning(str(block_ret))
                    raise AssertionError(strBlockCheck(bHash, True, True))
                else:
                    self.log.info(strBlockCheck(bHash, False, False))
            if fMustBeAccepted:
                if None in block_ret:
                    self.log.warning(str(block_ret))
                    raise AssertionError(strBlockCheck(bHash, False, True))
                else:
                    self.log.info(strBlockCheck(bHash, True, False))

        except JSONRPCException as e:
            exc_msg = str(e)
            if exc_msg in ["Can't read block from disk (-32603)", "Block not found (-5)"]:
                if fMustBeAccepted:
                    raise AssertionError(strBlockCheck(bHash, False, True))
                else:
                    self.log.info(strBlockCheck(bHash, False, False))
            else:
                raise


if __name__ == '__main__':
    FakeStakeTest().main()
