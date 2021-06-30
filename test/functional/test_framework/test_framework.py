#!/usr/bin/env python3
# Copyright (c) 2014-2017 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Base class for RPC testing."""

from enum import Enum
from io import BytesIO
import logging
import optparse
import os
import pdb
import shutil
from struct import pack
import sys
import tempfile
import time

from . import coverage
from .address import wif_to_privkey
from .authproxy import JSONRPCException
from .blocktools import (
    create_block,
    create_coinbase_pos,
    create_transaction_from_outpoint,
)
from .key import CECKey
from .messages import (
    COIN,
    COutPoint,
    CTransaction,
    CTxIn,
    CTxOut,
    hash256,
)
from .script import (
    CScript,
    OP_CHECKSIG,
)
from .test_node import TestNode
from .mininode import NetworkThread
from .util import (
    MAX_NODES,
    PortSeed,
    assert_equal,
    assert_greater_than,
    check_json_precision,
    connect_nodes,
    connect_nodes_clique,
    disconnect_nodes,
    get_collateral_vout,
    Decimal,
    DEFAULT_FEE,
    get_datadir_path,
    hex_str_to_bytes,
    bytes_to_hex_str,
    initialize_datadir,
    is_coin_locked_by,
    create_new_dmn,
    p2p_port,
    set_node_times,
    SPORK_ACTIVATION_TIME,
    SPORK_DEACTIVATION_TIME,
    satoshi_round
)

class TestStatus(Enum):
    PASSED = 1
    FAILED = 2
    SKIPPED = 3

TEST_EXIT_PASSED = 0
TEST_EXIT_FAILED = 1
TEST_EXIT_SKIPPED = 77

TMPDIR_PREFIX = "pivx_func_test_"


class PivxTestFramework():
    """Base class for a pivx test script.

    Individual pivx test scripts should subclass this class and override the set_test_params() and run_test() methods.

    Individual tests can also override the following methods to customize the test setup:

    - add_options()
    - setup_chain()
    - setup_network()
    - setup_nodes()

    The __init__() and main() methods should not be overridden.

    This class also contains various public and private helper methods."""

    def __init__(self):
        """Sets test framework defaults. Do not override this method. Instead, override the set_test_params() method"""
        self.setup_clean_chain = False
        self.nodes = []
        self.network_thread = None
        self.mocktime = 0
        self.rpc_timewait = 600  # Wait for up to 600 seconds for the RPC server to respond
        self.supports_cli = False
        self.set_test_params()

        assert hasattr(self, "num_nodes"), "Test must set self.num_nodes in set_test_params()"

    def main(self):
        """Main function. This should not be overridden by the subclass test scripts."""

        parser = optparse.OptionParser(usage="%prog [options]")
        parser.add_option("--nocleanup", dest="nocleanup", default=False, action="store_true",
                          help="Leave pivxds and test.* datadir on exit or error")
        parser.add_option("--noshutdown", dest="noshutdown", default=False, action="store_true",
                          help="Don't stop pivxds after the test execution")
        parser.add_option("--srcdir", dest="srcdir", default=os.path.normpath(os.path.dirname(os.path.realpath(__file__))+"/../../../src"),
                          help="Source directory containing pivxd/pivx-cli (default: %default)")
        parser.add_option("--cachedir", dest="cachedir", default=os.path.normpath(os.path.dirname(os.path.realpath(__file__)) + "/../../cache"),
                          help="Directory for caching pregenerated datadirs")
        parser.add_option("--tmpdir", dest="tmpdir", help="Root directory for datadirs")
        parser.add_option("-l", "--loglevel", dest="loglevel", default="INFO",
                          help="log events at this level and higher to the console. Can be set to DEBUG, INFO, WARNING, ERROR or CRITICAL. Passing --loglevel DEBUG will output all logs to console. Note that logs at all levels are always written to the test_framework.log file in the temporary test directory.")
        parser.add_option("--tracerpc", dest="trace_rpc", default=False, action="store_true",
                          help="Print out all RPC calls as they are made")
        parser.add_option("--portseed", dest="port_seed", default=os.getpid(), type='int',
                          help="The seed to use for assigning port numbers (default: current process id)")
        parser.add_option("--coveragedir", dest="coveragedir",
                          help="Write tested RPC commands into this directory")
        parser.add_option("--configfile", dest="configfile",
                          help="Location of the test framework config file")
        parser.add_option('--legacywallet', dest="legacywallet", default=False, action="store_true",
                          help='create pre-HD wallets only')
        parser.add_option('--tiertwo', dest="tiertwo", default=False, action="store_true",
                          help='run tier two tests only')
        parser.add_option('--sapling', dest="sapling", default=False, action="store_true",
                          help='run tier two tests only')
        parser.add_option("--pdbonfailure", dest="pdbonfailure", default=False, action="store_true",
                          help="Attach a python debugger if test fails")
        parser.add_option("--usecli", dest="usecli", default=False, action="store_true",
                          help="use pivx-cli instead of RPC for all commands")
        self.add_options(parser)
        (self.options, self.args) = parser.parse_args()

        PortSeed.n = self.options.port_seed

        os.environ['PATH'] = self.options.srcdir + ":" + self.options.srcdir + "/qt:" + os.environ['PATH']

        check_json_precision()

        self.options.cachedir = os.path.abspath(self.options.cachedir)

        # Set up temp directory and start logging
        if self.options.tmpdir:
            self.options.tmpdir = os.path.abspath(self.options.tmpdir)
            os.makedirs(self.options.tmpdir, exist_ok=False)
        else:
            self.options.tmpdir = tempfile.mkdtemp(prefix=TMPDIR_PREFIX)
        self._start_logging()

        self.log.debug('Setting up network thread')
        self.network_thread = NetworkThread()
        self.network_thread.start()

        success = TestStatus.FAILED

        try:
            if self.options.usecli and not self.supports_cli:
                raise SkipTest("--usecli specified but test does not support using CLI")
            self.setup_chain()
            self.setup_network()
            time.sleep(5)
            self.run_test()
            success = TestStatus.PASSED
        except JSONRPCException as e:
            self.log.exception("JSONRPC error")
        except SkipTest as e:
            self.log.warning("Test Skipped: %s" % e.message)
            success = TestStatus.SKIPPED
        except AssertionError as e:
            self.log.exception("Assertion failed")
        except KeyError as e:
            self.log.exception("Key error")
        except Exception as e:
            self.log.exception("Unexpected exception caught during testing")
        except KeyboardInterrupt as e:
            self.log.warning("Exiting after keyboard interrupt")

        if success == TestStatus.FAILED and self.options.pdbonfailure:
            print("Testcase failed. Attaching python debugger. Enter ? for help")
            pdb.set_trace()

        self.log.debug('Closing down network thread')
        self.network_thread.close()
        if not self.options.noshutdown:
            self.log.info("Stopping nodes")
            if self.nodes:
                self.stop_nodes()
        else:
            for node in self.nodes:
                node.cleanup_on_exit = False
            self.log.info("Note: pivxds were not stopped and may still be running")

        if not self.options.nocleanup and not self.options.noshutdown and success != TestStatus.FAILED:
            self.log.info("Cleaning up")
            shutil.rmtree(self.options.tmpdir)
        else:
            self.log.warning("Not cleaning up dir %s" % self.options.tmpdir)

        if success == TestStatus.PASSED:
            self.log.info("Tests successful")
            exit_code = TEST_EXIT_PASSED
        elif success == TestStatus.SKIPPED:
            self.log.info("Test skipped")
            exit_code = TEST_EXIT_SKIPPED
        else:
            self.log.error("Test failed. Test logging available at %s/test_framework.log", self.options.tmpdir)
            self.log.error("Hint: Call {} '{}' to consolidate all logs".format(os.path.normpath(os.path.dirname(os.path.realpath(__file__)) + "/../combine_logs.py"), self.options.tmpdir))
            exit_code = TEST_EXIT_FAILED
        logging.shutdown()
        sys.exit(exit_code)

    # Methods to override in subclass test scripts.
    def set_test_params(self):
        """Tests must this method to change default values for number of nodes, topology, etc"""
        raise NotImplementedError

    def add_options(self, parser):
        """Override this method to add command-line options to the test"""
        pass

    def setup_chain(self):
        """Override this method to customize blockchain setup"""
        self.log.info("Initializing test directory " + self.options.tmpdir)
        if self.setup_clean_chain:
            self._initialize_chain_clean()
        else:
            self._initialize_chain()

    def setup_network(self):
        """Override this method to customize test network topology"""
        self.setup_nodes()

        # Connect the nodes as a "chain".  This allows us
        # to split the network between nodes 1 and 2 to get
        # two halves that can work on competing chains.
        #
        # Topology looks like this:
        # node0 <-- node1 <-- node2 <-- node3
        #
        # If all nodes are in IBD (clean chain from genesis), node0 is assumed to be the source of blocks (miner). To
        # ensure block propagation, all nodes will establish outgoing connections toward node0.
        # See fPreferredDownload in net_processing.
        #
        # If further outbound connections are needed, they can be added at the beginning of the test with e.g.
        # connect_nodes(self.nodes[1], 2)
        for i in range(self.num_nodes - 1):
            connect_nodes(self.nodes[i + 1], i)
        self.sync_all()

    def setup_nodes(self):
        """Override this method to customize test node setup"""
        extra_args = None
        if hasattr(self, "extra_args"):
            extra_args = self.extra_args
        self.add_nodes(self.num_nodes, extra_args)
        self.start_nodes()

    def run_test(self):
        """Tests must override this method to define test logic"""
        raise NotImplementedError

    # Public helper methods. These can be accessed by the subclass test scripts.

    def add_nodes(self, num_nodes, extra_args=None, *, rpchost=None, binary=None):
        """Instantiate TestNode objects"""

        if extra_args is None:
            extra_args = [[]] * num_nodes
        # Check wallet version
        if self.options.legacywallet:
            for arg in extra_args:
                arg.append('-legacywallet')
            self.log.info("Running test with legacy (pre-HD) wallet")
        if binary is None:
            binary = [None] * num_nodes
        assert_equal(len(extra_args), num_nodes)
        assert_equal(len(binary), num_nodes)
        for i in range(num_nodes):
            self.nodes.append(TestNode(i, self.options.tmpdir, extra_args[i], rpchost, timewait=self.rpc_timewait, binary=binary[i], stderr=None, mocktime=self.mocktime, coverage_dir=self.options.coveragedir, use_cli=self.options.usecli))

    def start_node(self, i, *args, **kwargs):
        """Start a pivxd"""

        node = self.nodes[i]

        node.start(*args, **kwargs)
        node.wait_for_rpc_connection()

        time.sleep(10)

        if self.options.coveragedir is not None:
            coverage.write_all_rpc_commands(self.options.coveragedir, node.rpc)

    def start_nodes(self, extra_args=None, *args, **kwargs):
        """Start multiple pivxds"""

        if extra_args is None:
            extra_args = [None] * self.num_nodes
        assert_equal(len(extra_args), self.num_nodes)
        try:
            for i, node in enumerate(self.nodes):
                node.start(extra_args[i], *args, **kwargs)
            for node in self.nodes:
                node.wait_for_rpc_connection()
        except:
            # If one node failed to start, stop the others
            self.stop_nodes()
            raise

        time.sleep(10)

        if self.options.coveragedir is not None:
            for node in self.nodes:
                coverage.write_all_rpc_commands(self.options.coveragedir, node.rpc)

    def stop_node(self, i):
        """Stop a pivxd test node"""
        self.nodes[i].stop_node()
        self.nodes[i].wait_until_stopped()

    def stop_nodes(self):
        """Stop multiple pivxd test nodes"""
        for node in self.nodes:
            # Issue RPC to stop nodes
            node.stop_node()

        for node in self.nodes:
            # Wait for nodes to stop
            time.sleep(5)
            node.wait_until_stopped()

    def restart_node(self, i, extra_args=None):
        """Stop and start a test node"""
        self.stop_node(i)
        self.start_node(i, extra_args)

    def assert_start_raises_init_error(self, i, extra_args=None, expected_msg=None, *args, **kwargs):
        with tempfile.SpooledTemporaryFile(max_size=2**16) as log_stderr:
            try:
                self.start_node(i, extra_args, stderr=log_stderr, *args, **kwargs)
                self.nodes[i].wait_for_rpc_connection()
                self.stop_node(i)
            except Exception as e:
                assert 'pivxd exited' in str(e)  # node must have shutdown
                self.nodes[i].running = False
                self.nodes[i].process = None
                if expected_msg is not None:
                    log_stderr.seek(0)
                    stderr = log_stderr.read().decode('utf-8')
                    if expected_msg not in stderr:
                        raise AssertionError("Expected error \"" + expected_msg + "\" not found in:\n" + stderr)
            else:
                if expected_msg is None:
                    assert_msg = "pivxd should have exited with an error"
                else:
                    assert_msg = "pivxd should have exited with expected error " + expected_msg
                raise AssertionError(assert_msg)

    def wait_for_node_exit(self, i, timeout):
        self.nodes[i].process.wait(timeout)

    def split_network(self):
        """
        Split the network of four nodes into nodes 0/1 and 2/3.
        """
        disconnect_nodes(self.nodes[1], 2)
        disconnect_nodes(self.nodes[2], 1)
        self.sync_all(self.nodes[:2])
        self.sync_all(self.nodes[2:])

    def join_network(self):
        """
        Join the (previously split) network halves together.
        """
        connect_nodes(self.nodes[1], 2)
        self.sync_all()

    def sync_blocks(self, nodes=None, wait=1, timeout=60):
        """
        Wait until everybody has the same tip.
        sync_blocks needs to be called with an rpc_connections set that has least
        one node already synced to the latest, stable tip, otherwise there's a
        chance it might return before all nodes are stably synced.
        """
        rpc_connections = nodes or self.nodes
        stop_time = time.time() + timeout
        while time.time() <= stop_time:
            best_hash = [x.getbestblockhash() for x in rpc_connections]
            if best_hash.count(best_hash[0]) == len(rpc_connections):
                return
            # Check that each peer has at least one connection
            assert (all([len(x.getpeerinfo()) for x in rpc_connections]))
            time.sleep(wait)
        raise AssertionError("Block sync timed out after {}s:{}".format(
            timeout,
            "".join("\n  {!r}".format(b) for b in best_hash),
        ))

    def sync_mempools(self, nodes=None, wait=1, timeout=60, flush_scheduler=True):
        """
        Wait until everybody has the same transactions in their memory
        pools
        """
        rpc_connections = nodes or self.nodes
        stop_time = time.time() + timeout
        while time.time() <= stop_time:
            pool = [set(r.getrawmempool()) for r in rpc_connections]
            if pool.count(pool[0]) == len(rpc_connections):
                if flush_scheduler:
                    for r in rpc_connections:
                        r.syncwithvalidationinterfacequeue()
                return
            # Check that each peer has at least one connection
            assert (all([len(x.getpeerinfo()) for x in rpc_connections]))
            time.sleep(wait)
        raise AssertionError("Mempool sync timed out after {}s:{}".format(
            timeout,
            "".join("\n  {!r}".format(m) for m in pool),
        ))

    def sync_all(self, nodes=None):
        self.sync_blocks(nodes)
        self.sync_mempools(nodes)

    def enable_mocktime(self):
        """Enable mocktime for the script.

        mocktime may be needed for scripts that use the cached version of the
        blockchain.  If the cached version of the blockchain is used without
        mocktime then the mempools will not sync due to IBD.

        Sets mocktime to Tuesday, October 31, 2017 6:21:20 PM GMT (1572546080)
        """
        self.mocktime = 1572546080

    def disable_mocktime(self):
        self.mocktime = 0

    # Private helper methods. These should not be accessed by the subclass test scripts.

    def _start_logging(self):
        # Add logger and logging handlers
        self.log = logging.getLogger('TestFramework')
        self.log.setLevel(logging.DEBUG)
        # Create file handler to log all messages
        fh = logging.FileHandler(self.options.tmpdir + '/test_framework.log')
        fh.setLevel(logging.DEBUG)
        # Create console handler to log messages to stderr. By default this logs only error messages, but can be configured with --loglevel.
        ch = logging.StreamHandler(sys.stdout)
        # User can provide log level as a number or string (eg DEBUG). loglevel was caught as a string, so try to convert it to an int
        ll = int(self.options.loglevel) if self.options.loglevel.isdigit() else self.options.loglevel.upper()
        ch.setLevel(ll)
        # Format logs the same as pivxd's debug.log with microprecision (so log files can be concatenated and sorted)
        formatter = logging.Formatter(fmt='%(asctime)s.%(msecs)03d000Z %(name)s (%(levelname)s): %(message)s', datefmt='%Y-%m-%dT%H:%M:%S')
        formatter.converter = time.gmtime
        fh.setFormatter(formatter)
        ch.setFormatter(formatter)
        # add the handlers to the logger
        self.log.addHandler(fh)
        self.log.addHandler(ch)

        if self.options.trace_rpc:
            rpc_logger = logging.getLogger("BitcoinRPC")
            rpc_logger.setLevel(logging.DEBUG)
            rpc_handler = logging.StreamHandler(sys.stdout)
            rpc_handler.setLevel(logging.DEBUG)
            rpc_logger.addHandler(rpc_handler)

    def _initialize_chain(self):
        """Initialize a pre-mined blockchain for use by the test."""

        def create_cachedir(cachedir):
            if os.path.isdir(cachedir):
                shutil.rmtree(cachedir)
            os.makedirs(cachedir)

        def copy_cachedir(origin, destination, num_nodes=MAX_NODES):
            for i in range(num_nodes):
                from_dir = get_datadir_path(origin, i)
                to_dir = get_datadir_path(destination, i)
                shutil.copytree(from_dir, to_dir)
                initialize_datadir(destination, i)  # Overwrite port/rpcport in pivx.conf

        def clone_cache_from_node_1(cachedir, from_num=4):
            """ Clones cache subdir from node 1 to nodes from 'from_num' to MAX_NODES"""
            def copy_and_overwrite(from_path, to_path):
                if os.path.exists(to_path):
                    shutil.rmtree(to_path)
                shutil.copytree(from_path, to_path)
            assert from_num < MAX_NODES
            node_0_datadir = os.path.join(get_datadir_path(cachedir, 0), "regtest")
            for i in range(from_num, MAX_NODES):
                node_i_datadir = os.path.join(get_datadir_path(cachedir, i), "regtest")
                for subdir in ["blocks", "chainstate", "evodb", "sporks", "zerocoin"]:
                    copy_and_overwrite(os.path.join(node_0_datadir, subdir),
                                    os.path.join(node_i_datadir, subdir))
                initialize_datadir(cachedir, i)  # Overwrite port/rpcport in pivx.conf

        def cachedir_valid(cachedir):
            for i in range(MAX_NODES):
                if not os.path.isdir(get_datadir_path(cachedir, i)):
                    return False
            # nodes directories exist. check if the first one has the .incomplete flagfile
            return (not os.path.exists(os.path.join(get_datadir_path(cachedir, 0), ".incomplete")))

        def clean_cache_subdir(cachedir):
            os.remove(os.path.join(get_datadir_path(cachedir, 0), ".incomplete"))

            def cache_path(n, *paths):
                return os.path.join(get_datadir_path(cachedir, n), "regtest", *paths)

            for i in range(MAX_NODES):
                for entry in os.listdir(cache_path(i)):
                    if entry not in ['wallet.dat', 'chainstate', 'blocks', 'sporks', 'evodb', 'zerocoin', 'backups', "wallets"]:
                        os.remove(cache_path(i, entry))

        def clean_cache_dir():
            if os.path.isdir(self.options.cachedir):
                # migrate old cache dir
                if cachedir_valid(self.options.cachedir):
                    powcachedir = os.path.join(self.options.cachedir, "pow")
                    self.log.info("Found old cachedir. Migrating to %s" % str(powcachedir))
                    copy_cachedir(self.options.cachedir, powcachedir)
                # remove everything except pow subdir
                for entry in os.listdir(self.options.cachedir):
                    if entry != 'pow':
                        entry_path = os.path.join(self.options.cachedir, entry)
                        if os.path.isfile(entry_path):
                            os.remove(entry_path)
                        elif os.path.isdir(entry_path):
                            shutil.rmtree(entry_path)
            # no cachedir found
            else:
                os.makedirs(self.options.cachedir)

        def start_nodes_from_dir(ddir, num_nodes=MAX_NODES):
            self.log.info("Starting %d nodes..." % num_nodes)
            for i in range(num_nodes):
                datadir = initialize_datadir(ddir, i)
                if i == 0:
                    # Add .incomplete flagfile
                    # (removed at the end during clean_cache_subdir)
                    open(os.path.join(datadir, ".incomplete"), 'a').close()
                args = [os.getenv("BITCOIND", "pivxd"), "-spendzeroconfchange=1", "-server", "-keypool=1",
                        "-datadir=" + datadir, "-discover=0"]
                self.nodes.append(
                    TestNode(i, ddir, extra_args=[], rpchost=None, timewait=self.rpc_timewait, binary=None, stderr=None,
                             mocktime=self.mocktime, coverage_dir=None))
                self.nodes[i].args = args
                self.start_node(i)
                self.log.info("Node %d started." % i)
            # Wait for RPC connections to be ready
            self.log.info("Nodes started. Waiting for RPC connections...")
            for node in range(4):
                self.nodes[node].wait_for_rpc_connection()
            self.log.info("Connecting nodes")
            connect_nodes_clique(self.nodes)

        def stop_and_clean_cache_dir(ddir):
            self.stop_nodes()
            self.nodes = []
            # Copy cache for nodes 5 to MAX_NODES
            self.log.info("Copying cache dir to non-started nodes")
            clone_cache_from_node_1(ddir)
            self.log.info("Cleaning up.")
            clean_cache_subdir(ddir)

        def generate_pow_cache():
            ### POW Cache ###
            # Create a 200-block-long chain; each of the 4 first nodes
            # gets 25 mature blocks and 25 immature.
            # Note: To preserve compatibility with older versions of
            # initialize_chain, only 4 nodes will generate coins.
            #
            # blocks are created with timestamps 1 minutes apart
            # starting from 331 minutes in the past

            # Create cache directories, run pivxds:
            create_cachedir(powcachedir)
            self.log.info("Creating 'PoW-chain': 200 blocks")
            start_nodes_from_dir(powcachedir, 4)

            # Mine the blocks
            self.log.info("Mining 200 blocks")
            self.enable_mocktime()
            block_time = self.mocktime - (331 * 60)
            for i in range(2):
                for peer in range(4):
                    for j in range(25):
                        set_node_times(self.nodes, block_time)
                        self.nodes[peer].generate(1)
                        block_time += 60
                    # Must sync before next peer starts generating blocks
                    self.sync_blocks()

            # Shut them down, and clean up cache directories:
            self.log.info("Stopping nodes")
            stop_and_clean_cache_dir(powcachedir)
            self.log.info("---> pow cache created")
            self.disable_mocktime()


        assert self.num_nodes <= MAX_NODES

        clean_cache_dir()
        powcachedir = os.path.join(self.options.cachedir, "pow")
        is_powcache_valid = cachedir_valid(powcachedir)

        if not is_powcache_valid:
            self.log.info("PoW-CACHE NOT FOUND or INVALID.")
            self.log.info("Creating new cached blockchain data.")
            generate_pow_cache()
        else:
            self.log.info("CACHE FOUND.")

        # Copy requested cache to tempdir
        self.log.info("Copying datadir from %s to %s" % (powcachedir, self.options.tmpdir))
        copy_cachedir(powcachedir, self.options.tmpdir, self.num_nodes)



    def _initialize_chain_clean(self):
        """Initialize empty blockchain for use by the test.

        Create an empty blockchain and num_nodes wallets.
        Useful if a test case wants complete control over initialization."""
        for i in range(self.num_nodes):
            initialize_datadir(self.options.tmpdir, i)


    ### PIVX Specific TestFramework ###
    ###################################
    def init_dummy_key(self):
        self.DUMMY_KEY = CECKey()
        self.DUMMY_KEY.set_secretbytes(hash256(pack('<I', 0xffff)))

    def get_prevouts(self, node_id, utxo_list):
        """ get prevouts (map) for each utxo in a list
        :param   node_id:          (int) index of the CTestNode used as rpc connection. Must own the utxos.
                 utxo_list:        (JSON list) utxos returned from listunspent used as input
        :return: prevouts:         ({bytes --> (int, bytes, int)} dictionary)
                                   maps CStake "uniqueness" (i.e. serialized COutPoint)
                                   to (amount, prevScript, timeBlockFrom).
        """
        assert_greater_than(len(self.nodes), node_id)
        rpc_conn = self.nodes[node_id]
        prevouts = {}

        for utxo in utxo_list:
            outPoint = COutPoint(int(utxo['txid'], 16), utxo['vout'])
            outValue = int(utxo['amount']) * COIN
            prevtx_json = rpc_conn.getrawtransaction(utxo['txid'], 1)
            prevTx = CTransaction()
            prevTx.deserialize(BytesIO(hex_str_to_bytes(prevtx_json['hex'])))
            if (prevTx.is_coinbase() or prevTx.is_coinstake()) and utxo['confirmations'] < 100:
                # skip immature coins
                continue
            prevScript = prevtx_json['vout'][utxo['vout']]['scriptPubKey']['hex']
            prevTime = prevtx_json['blocktime']
            prevouts[outPoint.serialize_uniqueness()] = (outValue, prevScript, prevTime)

        return prevouts


    def make_txes(self, node_id, spendingPrevOuts, to_pubKey):
        """ makes a list of CTransactions each spending an input from spending PrevOuts to an output to_pubKey
        :param   node_id:            (int) index of the CTestNode used as rpc connection. Must own spendingPrevOuts.
                 spendingPrevouts:   ({bytes --> (int, bytes, int)} dictionary)
                                     maps CStake "uniqueness" (i.e. serialized COutPoint)
                                     to (amount, prevScript, timeBlockFrom).
                 to_pubKey           (bytes) recipient public key
        :return: block_txes:         ([CTransaction] list)
        """
        assert_greater_than(len(self.nodes), node_id)
        rpc_conn = self.nodes[node_id]
        block_txes = []
        for uniqueness in spendingPrevOuts:
            value_out = int(spendingPrevOuts[uniqueness][0] - DEFAULT_FEE * COIN)
            scriptPubKey = CScript([to_pubKey, OP_CHECKSIG])
            prevout = COutPoint()
            prevout.deserialize_uniqueness(BytesIO(uniqueness))
            tx = create_transaction_from_outpoint(prevout, b"", value_out, scriptPubKey)
            # sign tx
            raw_spend = rpc_conn.signrawtransaction(bytes_to_hex_str(tx.serialize()))['hex']
            # add signed tx to the list
            signed_tx = CTransaction()
            signed_tx.from_hex(raw_spend)
            block_txes.append(signed_tx)

        return block_txes

    def stake_block(self,
            node_id,
            nVersion,
            nHeight,
            prevHash,
            prevModifier,
            finalsaplingroot,
            stakeableUtxos,
            startTime,
            privKeyWIF,
            vtx,
            fDoubleSpend):
        """ manually stakes a block selecting the coinstake input from a list of candidates
        :param   node_id:           (int) index of the CTestNode used as rpc connection. Must own stakeableUtxos.
                 nVersion:          (int) version of the block being produced (7 or 8)
                 nHeight:           (int) height of the block being produced
                 prevHash:          (string) hex string of the previous block hash
                 prevModifier       (string) hex string of the previous block stake modifier
                 finalsaplingroot   (string) hex string of the previous block sapling root (blocks V8)
                 stakeableUtxos:    ({bytes --> (int, bytes, int)} dictionary)
                                    maps CStake "uniqueness" (i.e. serialized COutPoint)
                                    to (amount, prevScript, timeBlockFrom).
                 startTime:         (int) epoch time to be used as blocktime (iterated in solve_stake)
                 privKeyWIF:        (string) private key to be used for staking/signing
                                    If empty string, it will be used the pk from the stake input
                                    (dumping the sk from rpc_conn). If None, then the DUMMY_KEY will be used.
                 vtx:               ([CTransaction] list) transactions to add to block.vtx
                 fDoubleSpend:      (bool) wether any tx in vtx is allowed to spend the coinstake input
        :return: block:             (CBlock) block produced, must be manually relayed
        """
        assert_greater_than(len(self.nodes), node_id)
        rpc_conn = self.nodes[node_id]
        if not len(stakeableUtxos) > 0:
            raise Exception("Need at least one stakeable utxo to stake a block!")
        # Get start time to stake
        if startTime is None:
            startTime = time.time()
        # Create empty block with coinbase
        nTime = int(startTime) & 0xfffffff0
        coinbaseTx = create_coinbase_pos(nHeight)
        block = create_block(int(prevHash, 16), coinbaseTx, nTime, nVersion, int(finalsaplingroot, 16))
        block.nVersion = nVersion

        # Find valid kernel hash - iterates stakeableUtxos, then block.nTime
        block.solve_stake(stakeableUtxos, int(prevModifier, 16))

        block_sig_key = CECKey()

        coinstakeTx_unsigned = CTransaction()
        prevout = COutPoint()
        prevout.deserialize_uniqueness(BytesIO(block.prevoutStake))
        coinstakeTx_unsigned.vin.append(CTxIn(prevout, b"", 0xffffffff))
        coinstakeTx_unsigned.vout.append(CTxOut())
        amount, prevScript, _ = stakeableUtxos[block.prevoutStake]
        outNValue = int(amount + 250 * COIN)
        coinstakeTx_unsigned.vout.append(CTxOut(outNValue, hex_str_to_bytes(prevScript)))
        if privKeyWIF == "":
            # Use dummy key
            if not hasattr(self, 'DUMMY_KEY'):
                self.init_dummy_key()
            block_sig_key = self.DUMMY_KEY
            # replace coinstake output script
            coinstakeTx_unsigned.vout[1].scriptPubKey = CScript([block_sig_key.get_pubkey(), OP_CHECKSIG])
        else:
            if privKeyWIF == None:
                # Use pk of the input. Ask sk from rpc_conn
                rawtx = rpc_conn.getrawtransaction('{:064x}'.format(prevout.hash), True)
                privKeyWIF = rpc_conn.dumpprivkey(rawtx["vout"][prevout.n]["scriptPubKey"]["addresses"][0])
            # Use the provided privKeyWIF (cold staking).
            # export the corresponding private key to sign block
            privKey, compressed = wif_to_privkey(privKeyWIF)
            block_sig_key.set_compressed(compressed)
            block_sig_key.set_secretbytes(bytes.fromhex(privKey))

        # Sign coinstake TX and add it to the block
        stake_tx_signed_raw_hex = rpc_conn.signrawtransaction(
            bytes_to_hex_str(coinstakeTx_unsigned.serialize()))['hex']

        # Add coinstake to the block
        coinstakeTx = CTransaction()
        coinstakeTx.from_hex(stake_tx_signed_raw_hex)
        block.vtx.append(coinstakeTx)

        # Add provided transactions to the block.
        # Don't add tx doublespending the coinstake input, unless fDoubleSpend=True
        for tx in vtx:
            if not fDoubleSpend and tx.spends(prevout):
                continue
            block.vtx.append(tx)

        # Get correct MerkleRoot and rehash block
        block.hashMerkleRoot = block.calc_merkle_root()
        block.rehash()

        # sign block with block signing key and return it
        block.sign_block(block_sig_key)
        return block


    def stake_next_block(self, node_id,
            stakeableUtxos,
            btime=None,
            privKeyWIF=None,
            vtx=[],
            fDoubleSpend=False):
        """ Calls stake_block appending to the current tip"""
        assert_greater_than(len(self.nodes), node_id)
        saplingActive = self.nodes[node_id].getblockchaininfo()['upgrades']['v5 shield']['status'] == 'active'
        blockVersion = 8 if saplingActive else 7
        nHeight = self.nodes[node_id].getblockcount()
        prevHhash = self.nodes[node_id].getblockhash(nHeight)
        prevBlock = self.nodes[node_id].getblock(prevHhash, True)
        prevModifier = prevBlock['stakeModifier']
        saplingRoot = prevBlock['finalsaplingroot'] # !TODO: update this if the block contains sapling txes
        return self.stake_block(node_id,
                                blockVersion,
                                nHeight+1,
                                prevHhash,
                                prevModifier,
                                saplingRoot,
                                stakeableUtxos,
                                btime,
                                privKeyWIF,
                                vtx,
                                fDoubleSpend)


    def check_tx_in_chain(self, node_id, txid):
        assert_greater_than(len(self.nodes), node_id)
        rawTx = self.nodes[node_id].getrawtransaction(txid, 1)
        assert_greater_than(rawTx["confirmations"], 0)


    def spend_inputs(self, node_id, inputs, outputs):
        """ auxiliary function used by spend_utxo / spend_utxos """
        assert_greater_than(len(self.nodes), node_id)
        rpc_conn = self.nodes[node_id]
        spendingTx = rpc_conn.createrawtransaction(inputs, outputs)
        spendingTx_signed = rpc_conn.signrawtransaction(spendingTx)
        if spendingTx_signed["complete"]:
            txhash = rpc_conn.sendrawtransaction(spendingTx_signed["hex"])
            return txhash
        else:
            return ""


    def spend_utxo(self, node_id, utxo, recipient=''):
        """ spend amount from previously unspent output to a provided address
        :param    node_id:    (int) index of the CTestNode used as rpc connection. Must own the utxo.
                  utxo:       (JSON) returned from listunspent used as input
                  recipient:  (string) destination address (new one if not provided)
        :return:  txhash:     (string) tx hash if successful, empty string otherwise
        """
        assert_greater_than(len(self.nodes), node_id)
        rpc_conn = self.nodes[node_id]
        inputs = [{"txid": utxo["txid"], "vout": utxo["vout"]}]
        out_amount = float(utxo["amount"]) - DEFAULT_FEE
        outputs = {}
        if recipient == '':
            recipient = rpc_conn.getnewaddress()
        outputs[recipient] = out_amount
        return self.spend_inputs(node_id, inputs, outputs)


    def spend_utxos(self, node_id, utxo_list, recipient='', fMultiple=False):
        """ spend utxos to provided list of addresses or 10 new generate ones.
        :param    node_id:     (int) index of the CTestNode used as rpc connection. Must own the utxo.
                  utxo_list:   (JSON list) returned from listunspent used as input
                  recipient:   (string, optional) destination address (new one if not provided)
                  fMultiple:   (boolean, optional, default=false) spend each utxo on a different tx
        :return:  txHashes:    (string list) list of hashes of completed txs
        """
        assert_greater_than(len(self.nodes), node_id)
        rpc_conn = self.nodes[node_id]
        txHashes = []

        # If no recipient is given, create a new one
        if recipient == '':
            recipient = rpc_conn.getnewaddress()

        # If fMultiple=True send one tx for each utxo
        if fMultiple:
            for utxo in utxo_list:
                txHash = self.spend_utxo(node_id, utxo, recipient)
                if txHash != "":
                    txHashes.append(txHash)

        # Otherwise make a single tx with all the inputs
        else:
            inputs = [{"txid": x["txid"], "vout": x["vout"]} for x in utxo_list]
            out_amount = sum([float(x["amount"]) for x in utxo_list]) - DEFAULT_FEE
            outputs = {}
            if recipient == '':
                recipient = rpc_conn.getnewaddress()
            outputs[recipient] = out_amount
            txHash = self.spend_inputs(node_id, inputs, outputs)
            if txHash != "":
                txHashes.append(txHash)

        return txHashes


    def generate_pos(self, node_id, btime=None):
        """ stakes a block using generate on nodes[node_id]"""
        assert_greater_than(len(self.nodes), node_id)
        rpc_conn = self.nodes[node_id]
        ss = rpc_conn.getstakingstatus()
        assert ss["walletunlocked"]
        assert ss["stakeablecoins"] > 0
        assert ss["stakingbalance"] > 0.0
        if btime is not None:
            next_btime = btime + 60
        fStaked = False
        failures = 0
        while not fStaked:
            try:
                rpc_conn.generate(1)
                fStaked = True
            except JSONRPCException as e:
                if ("Couldn't create new block" in str(e)):
                    failures += 1
                    # couldn't generate block. check that this node can still stake (after 60 failures)
                    if failures > 60:
                        ss = rpc_conn.getstakingstatus()
                        if not (ss["walletunlocked"] and ss["stakeablecoins"] > 0 and ss["stakingbalance"] > 0.0):
                            raise AssertionError("Node %d unable to stake!" % node_id)
                    # try to stake one sec in the future
                    if btime is not None:
                        btime += 1
                        set_node_times(self.nodes, btime)
                    else:
                        time.sleep(1)
                else:
                    raise e
        # block generated. adjust block time
        if btime is not None:
            btime = max(btime + 1, next_btime)
            set_node_times(self.nodes, btime)
            return btime
        else:
            return None


    def generate_pow(self, node_id, btime=None):
        """ stakes a block using generate on nodes[node_id]"""
        assert_greater_than(len(self.nodes), node_id)
        self.nodes[node_id].generate(1)
        if btime is not None:
            btime += 60
            set_node_times(self.nodes, btime)
        return btime


    def set_spork(self, node_id, sporkName, value):
        assert_greater_than(len(self.nodes), node_id)
        return self.nodes[node_id].spork(sporkName, value)


    def get_spork(self, node_id, sporkName):
        assert_greater_than(len(self.nodes), node_id)
        return self.nodes[node_id].spork("show")[sporkName]


    def activate_spork(self, node_id, sporkName):
        return self.set_spork(node_id, sporkName, SPORK_ACTIVATION_TIME)


    def deactivate_spork(self, node_id, sporkName):
        return self.set_spork(node_id, sporkName, SPORK_DEACTIVATION_TIME)


    def is_spork_active(self, node_id, sporkName):
        assert_greater_than(len(self.nodes), node_id)
        return self.nodes[node_id].spork("active")[sporkName]


    def get_mn_lastseen(self, node, mnTxHash):
        mnData = node.listmasternodes(mnTxHash)
        if len(mnData) == 0:
            return -1
        return mnData[0]["lastseen"]


    def get_mn_status(self, node, mnTxHash):
        mnData = node.listmasternodes(mnTxHash)
        if len(mnData) == 0:
            return ""
        assert_equal(len(mnData), 1)
        return mnData[0]["status"]


    def advance_mocktime(self, secs):
        self.mocktime += secs
        set_node_times(self.nodes, self.mocktime)
        time.sleep(1)


    def wait_until_mnsync_finished(self):
        SYNC_FINISHED = [999] * self.num_nodes
        synced = [-1] * self.num_nodes
        time.sleep(2)
        timeout = time.time() + 45
        while synced != SYNC_FINISHED and time.time() < timeout:
            for i in range(self.num_nodes):
                if synced[i] != SYNC_FINISHED[i]:
                    synced[i] = self.nodes[i].mnsync("status")["RequestedMasternodeAssets"]
            if synced != SYNC_FINISHED:
                self.advance_mocktime(2)
                time.sleep(5)
        if synced != SYNC_FINISHED:
            raise AssertionError("Unable to complete mnsync: %s" % str(synced))


    def wait_until_mn_status(self, status, mnTxHash, _timeout, orEmpty=False, with_ping_mns=[]):
        nodes_status = [None] * self.num_nodes

        def node_synced(i):
            return nodes_status[i] == status or (orEmpty and nodes_status[i] == "")

        def all_synced():
            for i in range(self.num_nodes):
                if not node_synced(i):
                    return False
            return True

        time.sleep(2)
        timeout = time.time() + _timeout
        while not all_synced() and time.time() < timeout:
            for i in range(self.num_nodes):
                if not node_synced(i):
                    nodes_status[i] = self.get_mn_status(self.nodes[i], mnTxHash)
            if not all_synced():
                time.sleep(2)
                self.send_pings(with_ping_mns)
        if not all_synced():
            strErr = "Unable to get get status \"%s\" on all nodes for mnode %s. Current: %s" % (
                    status, mnTxHash, str(nodes_status))
            raise AssertionError(strErr)


    def wait_until_mn_enabled(self, mnTxHash, _timeout, _with_ping_mns=[]):
        self.wait_until_mn_status("ENABLED", mnTxHash, _timeout, with_ping_mns=_with_ping_mns)


    def wait_until_mn_preenabled(self, mnTxHash, _timeout, _with_ping_mns=[]):
        self.wait_until_mn_status("PRE_ENABLED", mnTxHash, _timeout, with_ping_mns=_with_ping_mns)


    def wait_until_mn_vinspent(self, mnTxHash, _timeout, _with_ping_mns=[]):
        self.wait_until_mn_status("VIN_SPENT", mnTxHash, _timeout, orEmpty=True, with_ping_mns=_with_ping_mns)


    def controller_start_masternode(self, mnOwner, masternodeAlias):
        ret = mnOwner.startmasternode("alias", "false", masternodeAlias, True)
        assert_equal(ret["result"], "success")
        time.sleep(1)


    def send_pings(self, mnodes):
        for node in mnodes:
            try:
                node.mnping()["sent"]
            except:
                pass
            time.sleep(1)


    def stake_and_sync(self, node_id, num_blocks):
        for i in range(num_blocks):
            self.mocktime = self.generate_pos(node_id, self.mocktime)
        self.sync_blocks()
        time.sleep(1)


    def stake_and_ping(self, node_id, num_blocks, with_ping_mns=[]):
        # stake blocks and send mn pings in between
        for i in range(num_blocks):
            self.stake_and_sync(node_id, 1)
            if len(with_ping_mns) > 0:
                self.send_pings(with_ping_mns)

    # !TODO: remove after obsoleting legacy system
    def setupDMN(self,
                 mnOwner,
                 miner,
                 mnRemotePos,
                 strType,           # "fund"|"internal"|"external"
                 outpoint=None):    # COutPoint, only for "external"
        self.log.info("Creating%s proRegTx for deterministic masternode..." % (
                      " and funding" if strType == "fFund" else ""))
        collateralAdd = mnOwner.getnewaddress("dmn")
        ipport = "127.0.0.1:" + str(p2p_port(mnRemotePos))
        ownerAdd = mnOwner.getnewaddress("dmn_owner")
        operatorAdd = mnOwner.getnewaddress("dmn_operator")
        operatorKey = mnOwner.dumpprivkey(operatorAdd)
        votingAdd = mnOwner.getnewaddress("dmn_voting")
        if strType == "fund":
            # send to the owner the collateral tx cost + some dust for the ProReg and fee
            fundingTxId = miner.sendtoaddress(collateralAdd, Decimal('101'))
            # confirm and verify reception
            self.stake_and_sync(self.nodes.index(miner), 1)
            assert_greater_than(mnOwner.getrawtransaction(fundingTxId, 1)["confirmations"], 0)
            # create and send the ProRegTx funding the collateral
            proTxId = mnOwner.protx_register_fund(collateralAdd, ipport, ownerAdd,
                                                  operatorAdd, votingAdd, collateralAdd)
        elif strType == "internal":
            mnOwner.getnewaddress("dust")
            # send to the owner the collateral tx cost + some dust for the ProReg and fee
            collateralTxId = miner.sendtoaddress(collateralAdd, Decimal('100'))
            miner.sendtoaddress(collateralAdd, Decimal('1'))
            # confirm and verify reception
            self.stake_and_sync(self.nodes.index(miner), 1)
            json_tx = mnOwner.getrawtransaction(collateralTxId, True)
            collateralTxId_n = -1
            for o in json_tx["vout"]:
                if o["value"] == Decimal('100'):
                    collateralTxId_n = o["n"]
                    break
            assert_greater_than(collateralTxId_n, -1)
            assert_greater_than(json_tx["confirmations"], 0)
            proTxId = mnOwner.protx_register(collateralTxId, collateralTxId_n, ipport, ownerAdd,
                                             operatorAdd, votingAdd, collateralAdd)
        elif strType == "external":
            self.log.info("Setting up ProRegTx with collateral externally-signed...")
            # send the tx from the miner
            payoutAdd = mnOwner.getnewaddress("payout")
            register_res = miner.protx_register_prepare(outpoint.hash, outpoint.n, ipport, ownerAdd,
                                                        operatorAdd, votingAdd, payoutAdd)
            self.log.info("ProTx prepared")
            message_to_sign = register_res["signMessage"]
            collateralAdd = register_res["collateralAddress"]
            signature = mnOwner.signmessage(collateralAdd, message_to_sign)
            self.log.info("ProTx signed")
            proTxId = miner.protx_register_submit(register_res["tx"], signature)
        else:
            raise Exception("Type %s not available" % strType)

        self.sync_mempools([mnOwner, miner])
        # confirm and verify inclusion in list
        self.stake_and_sync(self.nodes.index(miner), 1)
        assert_greater_than(self.nodes[mnRemotePos].getrawtransaction(proTxId, 1)["confirmations"], 0)
        assert proTxId in self.nodes[mnRemotePos].protx_list(False)
        return proTxId, operatorKey

    def setupMasternode(self,
                        mnOwner,
                        miner,
                        masternodeAlias,
                        mnOwnerDirPath,
                        mnRemotePos,
                        masternodePrivKey):
        self.log.info("adding balance to the mn owner for " + masternodeAlias + "..")
        mnAddress = mnOwner.getnewaddress(masternodeAlias)
        # send to the owner the collateral tx cost
        collateralTxId = miner.sendtoaddress(mnAddress, Decimal('100'))
        # confirm and verify reception
        self.stake_and_sync(self.nodes.index(miner), 1)
        json_tx = mnOwner.getrawtransaction(collateralTxId, True)
        collateralTxId_n = -1
        for o in json_tx["vout"]:
            if o["value"] == Decimal('100'):
                collateralTxId_n = o["n"]
                break
        assert_greater_than(collateralTxId_n, -1)
        assert_greater_than(json_tx["confirmations"], 0)
        # update masternode file
        self.log.info("collateral accepted for " + masternodeAlias + ". Updating masternode.conf...")
        confData = "%s 127.0.0.1:%d %s %s %d" % (masternodeAlias,
                                                 p2p_port(mnRemotePos),
                                                 masternodePrivKey,
                                                 collateralTxId,
                                                 collateralTxId_n)
        destPath = os.path.join(mnOwnerDirPath, "masternode.conf")
        with open(destPath, "a+") as file_object:
            file_object.write("\n")
            file_object.write(confData)

        # lock collateral
        mnOwner.lockunspent(False, [{"txid": collateralTxId, "vout": collateralTxId_n}])

        # return the collateral outpoint
        return COutPoint(collateralTxId, collateralTxId_n)


### ----------------------
### ----- DMN setup ------
### ----------------------

    def connect_to_all(self, nodePos):
        for i in range(self.num_nodes):
            if i != nodePos and self.nodes[i] is not None:
                connect_nodes(self.nodes[i], nodePos)

    def assert_equal_for_all(self, expected, func_name, *args):
        def not_found():
            raise Exception("function %s not found!" % func_name)

        assert_equal([getattr(x, func_name, not_found)(*args) for x in self.nodes],
                     [expected] * self.num_nodes)

    """
    Create a ProReg tx, which has the collateral as one of its outputs
    """
    def protx_register_fund(self, miner, controller, dmn, collateral_addr, op_rew=None):
        # send to the owner the collateral tx + some dust for the ProReg and fee
        funding_txid = miner.sendtoaddress(collateral_addr, Decimal('101'))
        # confirm and verify reception
        miner.generate(1)
        self.sync_blocks([miner, controller])
        assert_greater_than(controller.getrawtransaction(funding_txid, True)["confirmations"], 0)
        # create and send the ProRegTx funding the collateral
        if op_rew is None:
            dmn.proTx = controller.protx_register_fund(collateral_addr, dmn.ipport, dmn.owner,
                                                       dmn.operator, dmn.voting, dmn.payee)
        else:
            dmn.proTx = controller.protx_register_fund(collateral_addr, dmn.ipport, dmn.owner,
                                                       dmn.operator, dmn.voting, dmn.payee,
                                                       op_rew["reward"], op_rew["address"])
        dmn.collateral = COutPoint(int(dmn.proTx, 16),
                                   get_collateral_vout(controller.getrawtransaction(dmn.proTx, True)))

    """
    Create a ProReg tx, which references an 100 PIV UTXO as collateral.
    The controller node owns the collateral and creates the ProReg tx.
    """
    def protx_register(self, miner, controller, dmn, collateral_addr):
        # send to the owner the exact collateral tx amount
        funding_txid = miner.sendtoaddress(collateral_addr, Decimal('100'))
        # send another output to be used for the fee of the proReg tx
        miner.sendtoaddress(collateral_addr, Decimal('1'))
        # confirm and verify reception
        miner.generate(1)
        self.sync_blocks([miner, controller])
        json_tx = controller.getrawtransaction(funding_txid, True)
        assert_greater_than(json_tx["confirmations"], 0)
        # create and send the ProRegTx
        dmn.collateral = COutPoint(int(funding_txid, 16), get_collateral_vout(json_tx))
        dmn.proTx = controller.protx_register(funding_txid, dmn.collateral.n, dmn.ipport, dmn.owner,
                                              dmn.operator, dmn.voting, dmn.payee)

    """
    Create a ProReg tx, referencing a collateral signed externally (eg. HW wallets).
    Here the controller node owns the collateral (and signs), but the miner creates the ProReg tx.
    """
    def protx_register_ext(self, miner, controller, dmn, outpoint, fSubmit):
        # send to the owner the collateral tx if the outpoint is not specified
        if outpoint is None:
            funding_txid = miner.sendtoaddress(controller.getnewaddress("collateral"), Decimal('100'))
            # confirm and verify reception
            miner.generate(1)
            self.sync_blocks([miner, controller])
            json_tx = controller.getrawtransaction(funding_txid, True)
            assert_greater_than(json_tx["confirmations"], 0)
            outpoint = COutPoint(int(funding_txid, 16), get_collateral_vout(json_tx))
        dmn.collateral = outpoint
        # Prepare the message to be signed externally by the owner of the collateral (the controller)
        reg_tx = miner.protx_register_prepare("%064x" % outpoint.hash, outpoint.n, dmn.ipport, dmn.owner,
                                              dmn.operator, dmn.voting, dmn.payee)
        sig = controller.signmessage(reg_tx["collateralAddress"], reg_tx["signMessage"])
        if fSubmit:
            dmn.proTx = miner.protx_register_submit(reg_tx["tx"], sig)
        else:
            return reg_tx["tx"], sig

    """ Create and register new deterministic masternode
    :param   idx:              (int) index of the (remote) node in self.nodes
             miner_idx:        (int) index of the miner in self.nodes
             controller_idx:   (int) index of the controller in self.nodes
             strType:          (string) "fund"|"internal"|"external"
             payout_addr:      (string) payee address. If not specified, reuse the collateral address.
             outpoint:         (COutPoint) collateral outpoint to be used with "external".
                                 It must be owned by the controller (proTx is sent from the miner).
                                 If not provided, a new utxo is created, sending it from the miner.
             op_addr_and_key:  (list of strings) List with two entries, operator address (0) and private key (1).
                                 If not provided, a new address-key pair is generated.
    :return: dmn:              (Masternode) the deterministic masternode object
    """
    def register_new_dmn(self, idx, miner_idx, controller_idx, strType,
                         payout_addr=None, outpoint=None, op_addr_and_key=None):
        # Prepare remote node
        assert idx != miner_idx
        assert idx != controller_idx
        miner_node = self.nodes[miner_idx]
        controller_node = self.nodes[controller_idx]
        mn_node = self.nodes[idx]

        # Generate ip and addresses/keys
        collateral_addr = controller_node.getnewaddress("mncollateral-%d" % idx)
        if payout_addr is None:
            payout_addr = collateral_addr
        dmn = create_new_dmn(idx, controller_node, payout_addr, op_addr_and_key)

        # Create ProRegTx
        self.log.info("Creating%s proRegTx for deterministic masternode idx=%d..." % (
            " and funding" if strType == "fund" else "", idx))
        if strType == "fund":
            self.protx_register_fund(miner_node, controller_node, dmn, collateral_addr)
        elif strType == "internal":
            self.protx_register(miner_node, controller_node, dmn, collateral_addr)
        elif strType == "external":
            self.protx_register_ext(miner_node, controller_node, dmn, outpoint, True)
        else:
            raise Exception("Type %s not available" % strType)
        time.sleep(1)
        self.sync_mempools([miner_node, controller_node])

        # confirm and verify inclusion in list
        miner_node.generate(1)
        self.sync_blocks()
        json_tx = mn_node.getrawtransaction(dmn.proTx, 1)
        assert_greater_than(json_tx["confirmations"], 0)
        assert dmn.proTx in mn_node.protx_list(False)

        # check coin locking
        assert is_coin_locked_by(controller_node, dmn.collateral)

        # check json payload against local dmn object
        self.check_proreg_payload(dmn, json_tx)

        return dmn

    def check_mn_list_on_node(self, idx, mns):
        self.nodes[idx].syncwithvalidationinterfacequeue()
        mnlist = self.nodes[idx].listmasternodes()
        if len(mnlist) != len(mns):
            raise Exception("Invalid mn list on node %d:\n%s\nExpected:%s" % (idx, str(mnlist), str(mns)))
        protxs = [x["proTxHash"] for x in mnlist]
        for mn in mns:
            if mn.proTx not in protxs:
                raise Exception("ProTx for mn %d (%s) not found in the list of node %d" % (mn.idx, mn.proTx, idx))

    def check_proreg_payload(self, dmn, json_tx):
        assert "payload" in json_tx
        # null hash if funding collateral
        collateral_hash = 0 if int(json_tx["txid"], 16) == dmn.collateral.hash \
                            else dmn.collateral.hash
        pl = json_tx["payload"]
        assert_equal(pl["version"], 1)
        assert_equal(pl["collateralHash"], "%064x" % collateral_hash)
        assert_equal(pl["collateralIndex"], dmn.collateral.n)
        assert_equal(pl["service"], dmn.ipport)
        assert_equal(pl["ownerAddress"], dmn.owner)
        assert_equal(pl["votingAddress"], dmn.voting)
        assert_equal(pl["operatorAddress"], dmn.operator)
        assert_equal(pl["payoutAddress"], dmn.payee)


### ------------------------------------------------------

class SkipTest(Exception):
    """This exception is raised to skip a test"""
    def __init__(self, message):
        self.message = message


'''
PivxTestFramework extensions
'''
# !TODO: remove after obsoleting legacy system
class PivxTier2TestFramework(PivxTestFramework):

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 6
        self.enable_mocktime()

        self.ownerOnePos = 0
        self.remoteOnePos = 1
        self.ownerTwoPos = 2
        self.remoteTwoPos = 3
        self.minerPos = 4
        self.remoteDMN1Pos = 5

        self.extra_args = [["-nuparams=v5_shield:249", "-nuparams=v6_evo:250"]] * self.num_nodes
        for i in [self.remoteOnePos, self.remoteTwoPos, self.remoteDMN1Pos]:
            self.extra_args[i] += ["-listen", "-externalip=127.0.0.1"]
        self.extra_args[self.minerPos].append("-sporkkey=932HEevBSujW2ud7RfB1YF91AFygbBRQj3de3LyaCRqNzKKgWXi")

        self.masternodeOneAlias = "mnOne"
        self.masternodeTwoAlias = "mntwo"

        self.mnOnePrivkey = "9247iC59poZmqBYt9iDh9wDam6v9S1rW5XekjLGyPnDhrDkP4AK"
        self.mnTwoPrivkey = "92Hkebp3RHdDidGZ7ARgS4orxJAGyFUPDXNqtsYsiwho1HGVRbF"

        # Updated in setup_3_masternodes_network() to be called at the start of run_test
        self.ownerOne = None        # self.nodes[self.ownerOnePos]
        self.remoteOne = None       # self.nodes[self.remoteOnePos]
        self.ownerTwo = None        # self.nodes[self.ownerTwoPos]
        self.remoteTwo = None       # self.nodes[self.remoteTwoPos]
        self.miner = None           # self.nodes[self.minerPos]
        self.remoteDMN1 = None       # self.nodes[self.remoteDMN1Pos]
        self.mnOneCollateral = COutPoint()
        self.mnTwoCollateral = COutPoint()
        self.proRegTx1 = None       # hash of provider-register-tx


    def send_3_pings(self):
        mns = [self.remoteOne, self.remoteTwo]
        self.advance_mocktime(30)
        self.send_pings(mns)
        self.stake(1, mns)
        self.advance_mocktime(30)
        self.send_pings(mns)
        time.sleep(2)

    def stake(self, num_blocks, with_ping_mns=[]):
        self.stake_and_ping(self.minerPos, num_blocks, with_ping_mns)

    def controller_start_all_masternodes(self):
        self.controller_start_masternode(self.ownerOne, self.masternodeOneAlias)
        self.controller_start_masternode(self.ownerTwo, self.masternodeTwoAlias)
        self.wait_until_mn_preenabled(self.mnOneCollateral.hash, 40)
        self.wait_until_mn_preenabled(self.mnTwoCollateral.hash, 40)
        self.log.info("masternodes started, waiting until both get enabled..")
        self.send_3_pings()
        self.wait_until_mn_enabled(self.mnOneCollateral.hash, 120, [self.remoteOne, self.remoteTwo])
        self.wait_until_mn_enabled(self.mnTwoCollateral.hash, 120, [self.remoteOne, self.remoteTwo])
        self.log.info("masternodes enabled and running properly!")

    def advance_mocktime_and_stake(self, secs_to_add):
        self.advance_mocktime(secs_to_add - 60 + 1)
        self.mocktime = self.generate_pos(self.minerPos, self.mocktime)
        time.sleep(2)

    def setup_3_masternodes_network(self):
        self.ownerOne = self.nodes[self.ownerOnePos]
        self.remoteOne = self.nodes[self.remoteOnePos]
        self.ownerTwo = self.nodes[self.ownerTwoPos]
        self.remoteTwo = self.nodes[self.remoteTwoPos]
        self.miner = self.nodes[self.minerPos]
        self.remoteDMN1 = self.nodes[self.remoteDMN1Pos]
        ownerOneDir = os.path.join(self.options.tmpdir, "node%d" % self.ownerOnePos)
        ownerTwoDir = os.path.join(self.options.tmpdir, "node%d" % self.ownerTwoPos)

        self.log.info("generating 256 blocks..")
        # First mine 250 PoW blocks
        for i in range(250):
            self.mocktime = self.generate_pow(self.minerPos, self.mocktime)
        self.sync_blocks()
        # Then start staking
        self.stake(6)

        self.log.info("masternodes setup..")
        # setup first masternode node, corresponding to nodeOne
        self.mnOneCollateral = self.setupMasternode(
            self.ownerOne,
            self.miner,
            self.masternodeOneAlias,
            os.path.join(ownerOneDir, "regtest"),
            self.remoteOnePos,
            self.mnOnePrivkey)
        # setup second masternode node, corresponding to nodeTwo
        self.mnTwoCollateral = self.setupMasternode(
            self.ownerTwo,
            self.miner,
            self.masternodeTwoAlias,
            os.path.join(ownerTwoDir, "regtest"),
            self.remoteTwoPos,
            self.mnTwoPrivkey)
        # setup deterministic masternode
        self.proRegTx1, self.dmn1Privkey = self.setupDMN(
            self.ownerOne,
            self.miner,
            self.remoteDMN1Pos,
            "fund"
        )

        self.log.info("masternodes setup completed, initializing them..")

        # now both are configured, let's activate the masternodes
        self.stake(1)
        time.sleep(3)
        self.advance_mocktime(10)
        remoteOnePort = p2p_port(self.remoteOnePos)
        remoteTwoPort = p2p_port(self.remoteTwoPos)
        self.remoteOne.initmasternode(self.mnOnePrivkey, "127.0.0.1:"+str(remoteOnePort))
        self.remoteTwo.initmasternode(self.mnTwoPrivkey, "127.0.0.1:"+str(remoteTwoPort))
        self.remoteDMN1.initmasternode(self.dmn1Privkey, "", True)

        # wait until mnsync complete on all nodes
        self.stake(1)
        self.wait_until_mnsync_finished()
        self.log.info("tier two synced! starting masternodes..")

        # Now everything is set, can start both masternodes
        self.controller_start_all_masternodes()

    def spend_collateral(self, mnOwner, collateralOutpoint, miner):
        send_value = satoshi_round(100 - 0.001)
        inputs = [{'txid': collateralOutpoint.hash, 'vout': collateralOutpoint.n}]
        outputs = {}
        outputs[mnOwner.getnewaddress()] = float(send_value)
        rawtx = mnOwner.createrawtransaction(inputs, outputs)
        signedtx = mnOwner.signrawtransaction(rawtx)
        txid = miner.sendrawtransaction(signedtx['hex'])
        self.sync_mempools()
        self.log.info("Collateral spent in %s" % txid)
        self.send_pings([self.remoteTwo])
        self.stake(1, [self.remoteTwo])
