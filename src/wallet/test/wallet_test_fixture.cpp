// Copyright (c) 2016-2021 The Bitcoin Core developers
// Copyright (c) 2020-2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "wallet/test/wallet_test_fixture.h"

#include "rpc/server.h"
#include "wallet/db.h"
#include "wallet/rpcwallet.h"
#include "wallet/wallet.h"

WalletTestingSetup::WalletTestingSetup(const std::string& chainName):
        SaplingTestingSetup(chainName), m_wallet("mock", CWalletDBWrapper::CreateMock())
{
    bool fFirstRun;
    m_wallet.LoadWallet(fFirstRun);
    RegisterValidationInterface(&m_wallet);

    RegisterWalletRPCCommands(tableRPC);
}

WalletTestingSetup::~WalletTestingSetup()
{
    UnregisterValidationInterface(&m_wallet);
}
