// Copyright (c) 2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "test/test_pivx.h"

#include "txdb.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

BOOST_AUTO_TEST_SUITE(zerocoindb_tests)

BOOST_FIXTURE_TEST_CASE(accumulator_checksums_db, TestingSetup)
{
    // create random checksums map and write it in the DB
    std::map<std::pair<uint32_t, libzerocoin::CoinDenomination>, int> rand_map;
    for (const auto& d : libzerocoin::zerocoinDenomList) {
        for (int i = 0; i < 20; i++) {
            uint32_t rand_checksum = InsecureRand32();
            int rand_height = InsecureRandRange(2000000);
            rand_map[std::make_pair(rand_checksum, d)] = rand_height;
            zerocoinDB->WriteAccChecksum(rand_checksum, d, rand_height);
        }
    }

    // test ReadAll
    std::map<std::pair<uint32_t, libzerocoin::CoinDenomination>, int> new_map;
    BOOST_CHECK(zerocoinDB->ReadAll(new_map));
    BOOST_CHECK_EQUAL(new_map.size(), rand_map.size());
    for (const auto& it : rand_map) {
        const auto& h = new_map.find(it.first);
        BOOST_CHECK(h != new_map.end());
        BOOST_CHECK(h->second == it.second);
    }

    // test WipeAccChecksums
    zerocoinDB->WipeAccChecksums();
    std::map<std::pair<uint32_t, libzerocoin::CoinDenomination>, int> empty_map;
    BOOST_CHECK(zerocoinDB->ReadAll(empty_map));
    BOOST_CHECK(empty_map.empty());
}

BOOST_AUTO_TEST_SUITE_END()
