#!/usr/bin/env python3
# Copyright (c) 2015-2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test node responses to invalid network messages."""
import struct
import time

from test_framework import messages
from test_framework.mininode import (
    P2PDataStore,
    P2PInterface,
)
from test_framework.test_framework import PivxTestFramework
from test_framework.util import (
    hex_str_to_bytes,
)

MSG_LIMIT = 2 * 1024 * 1024  # 2MB, per MAX_PROTOCOL_MESSAGE_LENGTH
VALID_DATA_LIMIT = MSG_LIMIT - 5  # Account for the 5-byte length prefix

class msg_unrecognized:
    """Nonsensical message. Modeled after similar types in test_framework.messages."""

    command = b'badmsg'

    def __init__(self, *, str_data):
        self.str_data = str_data.encode() if not isinstance(str_data, bytes) else str_data

    def serialize(self):
        return messages.ser_string(self.str_data)

    def __repr__(self):
        return "{}(data={})".format(self.command, self.str_data)


class SenderOfAddrV2(P2PInterface):
    def wait_for_sendaddrv2(self):
        self.wait_until(lambda: 'sendaddrv2' in self.last_message)

class InvalidMessagesTest(PivxTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def run_test(self):
        self.test_magic_bytes()
        self.test_checksum()
        self.test_size()
        self.test_command()
        self.test_addrv2_empty()
        self.test_addrv2_no_addresses()
        self.test_addrv2_too_long_address()
        self.test_addrv2_unrecognized_network()
        self.test_large_inv()
        self.test_resource_exhaustion()

    def test_magic_bytes(self):
        conn = self.nodes[0].add_p2p_connection(P2PDataStore())
        with self.nodes[0].assert_debug_log(['PROCESSMESSAGE: INVALID MESSAGESTART badmsg']):
            msg = conn.build_message(msg_unrecognized(str_data="d"))
            # modify magic bytes
            msg = b'\xff' * 4 + msg[4:]
            conn.send_raw_message(msg)
            conn.wait_for_disconnect(timeout=1)
            self.nodes[0].disconnect_p2ps()

    def test_checksum(self):
        conn = self.nodes[0].add_p2p_connection(P2PDataStore())
        with self.nodes[0].assert_debug_log(['ProcessMessages(badmsg, 2 bytes): CHECKSUM ERROR expected 78df0a04 was ffffffff']):
            msg = conn.build_message(msg_unrecognized(str_data="d"))
            # Checksum is after start bytes (4B), message type (12B), len (4B)
            cut_len = 4 + 12 + 4
            # modify checksum
            msg = msg[:cut_len] + b'\xff' * 4 + msg[cut_len + 4:]
            self.nodes[0].p2p.send_raw_message(msg)
            conn.sync_with_ping(timeout=1)
            self.nodes[0].disconnect_p2ps()

    def test_size(self):
        conn = self.nodes[0].add_p2p_connection(P2PDataStore())
        with self.nodes[0].assert_debug_log(['']):
            # Create a message with oversized payload
            msg = msg_unrecognized(str_data="d" * (VALID_DATA_LIMIT + 1))
            msg = conn.build_message(msg)
            self.nodes[0].p2p.send_raw_message(msg)
            conn.wait_for_disconnect(timeout=1)
            self.nodes[0].disconnect_p2ps()

    def test_command(self):
        conn = self.nodes[0].add_p2p_connection(P2PDataStore())
        with self.nodes[0].assert_debug_log(['PROCESSMESSAGE: ERRORS IN HEADER']):
            msg = msg_unrecognized(str_data="d")
            msg = conn.build_message(msg)
            # Modify command
            msg = msg[:7] + b'\x00' + msg[7 + 1:]
            self.nodes[0].p2p.send_raw_message(msg)
            conn.sync_with_ping(timeout=1)
            self.nodes[0].disconnect_p2ps()

    def test_addrv2(self, label, required_log_messages, raw_addrv2):
        node = self.nodes[0]
        conn = node.add_p2p_connection(SenderOfAddrV2())

        # Make sure pivxd signals support for ADDRv2, otherwise this test
        # will bombard an old node with messages it does not recognize which
        # will produce unexpected results.
        conn.wait_for_sendaddrv2()

        self.log.info('Test addrv2: ' + label)

        msg = msg_unrecognized(str_data=b'')
        msg.command = b'addrv2'
        with node.assert_debug_log(required_log_messages):
            # override serialize() which would include the length of the data
            msg.serialize = lambda: raw_addrv2
            conn.send_raw_message(conn.build_message(msg))
            conn.sync_with_ping()

        node.disconnect_p2ps()

    def test_addrv2_empty(self):
        self.test_addrv2('empty',
            [
                'received: addrv2 (0 bytes)',
                'ProcessMessages(addrv2, 0 bytes): Exception',
                'end of data',
            ],
            b'')

    def test_addrv2_no_addresses(self):
        self.test_addrv2('no addresses',
            [
                'received: addrv2 (1 bytes)',
            ],
            hex_str_to_bytes('00'))

    def test_addrv2_too_long_address(self):
        self.test_addrv2('too long address',
            [
                'received: addrv2 (525 bytes)',
                'ProcessMessage(addrv2, 525 bytes) FAILED',
                'Address too long: 513 > 512',
            ],
            hex_str_to_bytes(
                '01' +       # number of entries
                '61bc6649' + # time, Fri Jan  9 02:54:25 UTC 2009
                '00' +       # service flags, COMPACTSIZE(NODE_NONE)
                '01' +       # network type (IPv4)
                'fd0102' +   # address length (COMPACTSIZE(513))
                'ab' * 513 + # address
                '208d'))     # port

    def test_addrv2_unrecognized_network(self):
        now_hex = struct.pack('<I', int(time.time())).hex()
        self.test_addrv2('unrecognized network',
            [
                'received: addrv2 (25 bytes)',
                'IP 9.9.9.9 mapped',
                'Added 1 addresses',
            ],
            hex_str_to_bytes(
                '02' +     # number of entries
                # this should be ignored without impeding acceptance of subsequent ones
                now_hex +  # time
                '01' +     # service flags, COMPACTSIZE(NODE_NETWORK)
                '99' +     # network type (unrecognized)
                '02' +     # address length (COMPACTSIZE(2))
                'ab' * 2 + # address
                '208d' +   # port
                # this should be added:
                now_hex +  # time
                '01' +     # service flags, COMPACTSIZE(NODE_NETWORK)
                '01' +     # network type (IPv4)
                '04' +     # address length (COMPACTSIZE(4))
                '09' * 4 + # address
                '208d'))   # port

    def test_large_inv(self):
        conn = self.nodes[0].add_p2p_connection(P2PInterface())
        with self.nodes[0].assert_debug_log(['Misbehaving', 'peer=9 (0 -> 20): message inv size() = 50001']):
            msg = messages.msg_inv([messages.CInv(1, 1)] * 50001)
            conn.send_and_ping(msg)
        with self.nodes[0].assert_debug_log(['Misbehaving', 'peer=9 (20 -> 40): message getdata size() = 50001']):
            msg = messages.msg_getdata([messages.CInv(1, 1)] * 50001)
            conn.send_and_ping(msg)
        self.nodes[0].disconnect_p2ps()

    def test_resource_exhaustion(self):
        conn = self.nodes[0].add_p2p_connection(P2PDataStore())
        conn2 = self.nodes[0].add_p2p_connection(P2PDataStore())
        msg_at_size = msg_unrecognized(str_data="b" * VALID_DATA_LIMIT)
        assert len(msg_at_size.serialize()) == MSG_LIMIT

        self.log.info("Sending a bunch of large, junk messages to test memory exhaustion. May take a bit...")

        # Run a bunch of times to test for memory exhaustion.
        for _ in range(80):
            conn.send_message(msg_at_size)

        # Check that, even though the node is being hammered by nonsense from one
        # connection, it can still service other peers in a timely way.
        for _ in range(20):
            conn2.sync_with_ping(timeout=2)

        # Peer 1, despite being served up a bunch of nonsense, should still be connected.
        self.log.info("Waiting for node to drop junk messages.")
        conn.sync_with_ping(timeout=400)
        assert conn.is_connected
        self.nodes[0].disconnect_p2ps()


if __name__ == '__main__':
    InvalidMessagesTest().main()
