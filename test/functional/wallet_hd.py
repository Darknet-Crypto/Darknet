#!/usr/bin/env python3
# Copyright (c) 2016-2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test Hierarchical Deterministic wallet function."""

import os
import shutil

from test_framework.test_framework import PivxTestFramework
from test_framework.util import (
    assert_equal,
    connect_nodes,
    assert_raises_rpc_error
)


class WalletHDTest(PivxTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [['-nuparams=v5_shield:1']] * self.num_nodes
        self.extra_args[1].append('-keypool=0')
        self.supports_cli = False

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def generate_and_fund_addresses(self, fShield, fFund, seedid, count):
        add = None
        for i in range(count):
            add = self.nodes[1].getnewshieldaddress() if fShield else self.nodes[1].getnewaddress()
            add_info = self.nodes[1].getaddressinfo(add)
            assert_equal(add_info["hdkeypath"], "m/32'/119'/%d'" % (i + 1) if fShield else "m/44'/119'/0'/0'/%d'" % i)
            assert_equal(add_info["hdseedid"], seedid)
            if fFund:
                self.nodes[0].sendtoaddress(add, 1)
                self.nodes[0].generate(1)
        return add

    def generate_and_fund_transparent_addr(self, seedid, count):
        return self.generate_and_fund_addresses(False, True, seedid, count)

    def generate_and_fund_shield_addr(self, seedid, count):
        return self.generate_and_fund_addresses(True, True, seedid, count)

    def generate_transparent_addr(self, seedid, count):
        return self.generate_and_fund_addresses(False, False, seedid, count)

    def generate_shield_addr(self, seedid, count):
        return self.generate_and_fund_addresses(True, False, seedid, count)

    def start_and_connect_node1(self):
        self.start_node(1)
        connect_nodes(self.nodes[0], 1)
        connect_nodes(self.nodes[1], 0)
        self.sync_all()

    def check_addressbook(self, old_book, new_book):
        assert_equal(len(old_book), len(new_book))
        for add in new_book:
            assert add in old_book
            assert_equal(old_book[add], new_book[add])

    def run_test(self):
        # Make sure we use hd
        if '-legacywallet' in self.nodes[0].extra_args:
            self.log.info("Exiting HD test for non-HD wallets")
            return
        # keep masterkeyid
        masterkeyid = self.nodes[1].getwalletinfo()['hdseedid']
        assert_equal(len(masterkeyid), 40)

        # create an internal key
        change_addr = self.nodes[1].getrawchangeaddress()
        change_addrV= self.nodes[1].getaddressinfo(change_addr)
        assert_equal(change_addrV["hdkeypath"], "m/44'/119'/0'/1'/0'") #first internal child key

        # Import a non-HD private key in the HD wallet
        non_hd_add = self.nodes[0].getnewaddress()
        self.nodes[1].importprivkey(self.nodes[0].dumpprivkey(non_hd_add))

        # This should be enough to keep the master key and the non-HD key
        self.nodes[1].backupwallet(os.path.join(self.nodes[1].datadir, "hd.bak"))

        # Retrieve the hd seed from the dump
        self.nodes[1].dumpwallet(os.path.join(self.nodes[1].datadir, "hd.dump"))
        hdseed = None
        with open(os.path.join(self.nodes[1].datadir, "hd.dump"), 'r', encoding='utf8') as f:
            for line in f:
                if "hdseed=1" in line:
                    hdseed = line.split(' ')[0]
                    break
        assert hdseed is not None

        # Derive some HD addresses and remember the last
        # Also send funds to each add
        self.nodes[0].generate(101)
        NUM_HD_ADDS = 10
        hd_add = self.generate_and_fund_transparent_addr(masterkeyid, NUM_HD_ADDS)
        self.nodes[0].sendtoaddress(non_hd_add, 1)
        self.nodes[0].generate(1)

        # Derive some Shield addresses and remember the last
        # Also send funds to each add
        NUM_SHIELD_ADDS = 10
        z_add = self.generate_and_fund_shield_addr(masterkeyid, NUM_SHIELD_ADDS)

        # create an internal key (again)
        change_addr = self.nodes[1].getrawchangeaddress()
        change_addrV= self.nodes[1].getaddressinfo(change_addr)
        assert_equal(change_addrV["hdkeypath"], "m/44'/119'/0'/1'/1'") #second internal child key

        self.sync_all()
        assert_equal(self.nodes[1].getbalance(), NUM_HD_ADDS + NUM_SHIELD_ADDS + 1)

        # verify address-book recovery
        self.log.info("Restore backup (with chain)...")
        addrbook_old = self.nodes[1].getaddressesbylabel("")
        self.stop_node(1)
        shutil.copyfile(os.path.join(self.nodes[1].datadir, "hd.bak"), os.path.join(self.nodes[1].datadir, "regtest", "wallet.dat"))
        self.start_node(1)
        connect_nodes(self.nodes[0], 1)
        self.sync_all()
        self.check_addressbook(addrbook_old, self.nodes[1].getaddressesbylabel(""))

        # now delete the chain and recreate the addresses
        self.log.info("Restore backup (without chain)...")
        self.stop_node(1)
        # we need to delete the complete regtest directory
        # otherwise node1 would auto-recover all funds in flag the keypool keys as used
        shutil.rmtree(os.path.join(self.nodes[1].datadir, "regtest", "blocks"))
        shutil.rmtree(os.path.join(self.nodes[1].datadir, "regtest", "chainstate"))
        shutil.copyfile(os.path.join(self.nodes[1].datadir, "hd.bak"), os.path.join(self.nodes[1].datadir, "regtest", "wallets", "wallet.dat"))
        self.start_node(1)

        # Assert that derivation is deterministic
        hd_add_2 = self.generate_transparent_addr(masterkeyid, NUM_HD_ADDS)
        assert_equal(hd_add, hd_add_2)
        z_add_2 = self.generate_shield_addr(masterkeyid, NUM_SHIELD_ADDS)
        assert_equal(z_add, z_add_2)
        # connect and sync
        connect_nodes(self.nodes[0], 1)
        connect_nodes(self.nodes[1], 0)
        self.sync_all()
        assert_equal(self.nodes[1].getbalance(), NUM_HD_ADDS + NUM_SHIELD_ADDS + 1)

        # Try rescan
        self.stop_node(1)
        self.start_node(1, extra_args=self.extra_args[1] + ['-rescan'])
        assert_equal(self.nodes[1].getbalance(), NUM_HD_ADDS + NUM_SHIELD_ADDS + 1)

        # Try a RPC based rescan
        self.stop_node(1)
        shutil.rmtree(os.path.join(self.nodes[1].datadir, "regtest", "blocks"))
        shutil.rmtree(os.path.join(self.nodes[1].datadir, "regtest", "chainstate"))
        shutil.copyfile(os.path.join(self.nodes[1].datadir, "hd.bak"), os.path.join(self.nodes[1].datadir, "regtest", "wallets", "wallet.dat"))
        self.start_and_connect_node1()
        # Wallet automatically scans blocks older than key on startup (but shielded addresses need to be regenerated)
        assert_equal(self.nodes[1].getbalance(), NUM_HD_ADDS + 1)
        # Wallet automatically scans blocks older than key on startup
        out = self.nodes[1].rescanblockchain(0, 1)
        assert_equal(out['start_height'], 0)
        assert_equal(out['stop_height'], 1)
        out = self.nodes[1].rescanblockchain()
        assert_equal(out['start_height'], 0)
        assert_equal(out['stop_height'], self.nodes[1].getblockcount())
        assert_equal(self.nodes[1].getbalance(), NUM_HD_ADDS + 1)

        # send a tx and make sure its using the internal chain for the changeoutput
        txid = self.nodes[1].sendtoaddress(self.nodes[0].getnewaddress(), 1)
        outs = self.nodes[1].decoderawtransaction(self.nodes[1].gettransaction(txid)['hex'])['vout']
        keypath = ""
        for out in outs:
            if out['value'] != 1:
                keypath = self.nodes[1].getaddressinfo(out['scriptPubKey']['addresses'][0])['hdkeypath']

        assert_equal(keypath[0:16], "m/44'/119'/0'/1'")

        # Generate a new HD seed on node 1 and make sure it is set
        orig_masterkeyid = self.nodes[1].getwalletinfo()['hdseedid']
        self.nodes[1].sethdseed()
        new_masterkeyid = self.nodes[1].getwalletinfo()['hdseedid']
        assert orig_masterkeyid != new_masterkeyid
        addr = self.nodes[1].getnewaddress()
        assert_equal(self.nodes[1].getaddressinfo(addr)['hdkeypath'], 'm/44\'/119\'/0\'/0\'/0\'') # Make sure the new address is the first from the keypool
        self.nodes[1].keypoolrefill(1) # Fill keypool with 1 key

        # Set a new HD seed on node 1 without flushing the keypool
        new_seed = self.nodes[0].dumpprivkey(self.nodes[0].getnewaddress())
        orig_masterkeyid = new_masterkeyid
        self.nodes[1].sethdseed(False, new_seed)
        new_masterkeyid = self.nodes[1].getwalletinfo()['hdseedid']
        assert orig_masterkeyid != new_masterkeyid
        addr = self.nodes[1].getnewaddress()
        assert_equal(orig_masterkeyid, self.nodes[1].getaddressinfo(addr)['hdseedid'])
        assert_equal(self.nodes[1].getaddressinfo(addr)['hdkeypath'], 'm/44\'/119\'/0\'/0\'/1\'') # Make sure the new address continues previous keypool

        # Check that the next address is from the new seed
        self.nodes[1].keypoolrefill(1)
        next_addr = self.nodes[1].getnewaddress()
        assert_equal(new_masterkeyid, self.nodes[1].getaddressinfo(next_addr)['hdseedid'])
        assert_equal(self.nodes[1].getaddressinfo(next_addr)['hdkeypath'], 'm/44\'/119\'/0\'/0\'/0\'') # Make sure the new address is not from previous keypool
        assert next_addr != addr

        # Sethdseed parameter validity
        assert_raises_rpc_error(-1, 'sethdseed', self.nodes[0].sethdseed, False, new_seed, 0)
        assert_raises_rpc_error(-5, "Invalid private key", self.nodes[1].sethdseed, False, "not_wif")
        assert_raises_rpc_error(-1, "JSON value is not a boolean as expected", self.nodes[1].sethdseed, "Not_bool")
        assert_raises_rpc_error(-1, "JSON value is not a string as expected", self.nodes[1].sethdseed, False, True)
        assert_raises_rpc_error(-5, "Already have this key", self.nodes[1].sethdseed, False, new_seed)
        assert_raises_rpc_error(-5, "Already have this key", self.nodes[1].sethdseed, False, self.nodes[1].dumpprivkey(self.nodes[1].getnewaddress()))

        # Delete wallet and recover from first seed
        self.stop_node(1)
        os.remove(os.path.join(self.nodes[1].datadir, "regtest", "wallets", "wallet.dat"))
        self.start_node(1)
        # Regenerate old shield addresses
        self.log.info("Restore from seed ...")
        self.nodes[1].sethdseed(True, hdseed)
        z_add_3 = self.generate_shield_addr(masterkeyid, NUM_SHIELD_ADDS)
        assert_equal(z_add, z_add_3)
        # Restart, zap, and check balance: 1 PIV * (NUM_HD_ADDS + NUM_SHIELD_ADDS) recovered from seed
        self.stop_node(1)
        self.start_node(1, extra_args=self.extra_args[1] + ['-zapwallettxes=1'])
        assert_equal(self.nodes[1].getbalance(), NUM_HD_ADDS + NUM_SHIELD_ADDS)

if __name__ == '__main__':
    WalletHDTest().main ()
