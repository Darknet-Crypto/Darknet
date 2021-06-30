// Copyright (c) 2016-2021 The Bitcoin Core developers
// Copyright (c) 2020-2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_WALLET_TEST_FIXTURE_H
#define PIVX_WALLET_TEST_FIXTURE_H

#include "test/librust/sapling_test_fixture.h"
#include "wallet/wallet.h"


/** Testing setup and teardown for wallet.
 */
struct WalletTestingSetup : public SaplingTestingSetup
{
    WalletTestingSetup(const std::string& chainName = CBaseChainParams::MAIN);
    ~WalletTestingSetup();

    CWallet m_wallet;
};

struct WalletRegTestingSetup : public WalletTestingSetup
{
    WalletRegTestingSetup() : WalletTestingSetup(CBaseChainParams::REGTEST) {}
};

#endif // PIVX_WALLET_TEST_FIXTURE_H

