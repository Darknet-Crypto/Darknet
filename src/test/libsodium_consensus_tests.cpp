// Copyright (c) 2021 The PIVX Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php .

#include "test/test_pivx.h"
#include "sapling/sodium_sanity.h"
#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(libsodium_consensus_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(LibsodiumPubkeyValidation)
{
    libsodium_sanity_test();
}

BOOST_AUTO_TEST_SUITE_END()