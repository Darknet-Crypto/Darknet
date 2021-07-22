// Copyright (c) 2016-2021 The Bitcoin Core developers
// Copyright (c) 2020-2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "wallet/test/wallet_test_fixture.h"

#include "rpc/server.h"
#include "wallet/db.h"
#include "wallet/rpcwallet.h"

WalletTestingSetup::WalletTestingSetup(const std::string& chainName):
        SaplingTestingSetup(chainName)
{
    bitdb.MakeMock();

    bool fFirstRun;
    std::unique_ptr<CWalletDBWrapper> dbw(new CWalletDBWrapper(&bitdb, "wallet_test.dat"));
    pwalletMain = std::make_unique<CWallet>(std::move(dbw));
    pwalletMain->LoadWallet(fFirstRun);
    RegisterValidationInterface(pwalletMain.get());

    RegisterWalletRPCCommands(tableRPC);
}

WalletTestingSetup::~WalletTestingSetup()
{
    UnregisterValidationInterface(pwalletMain.get());

    bitdb.Flush(true);
    bitdb.Reset();
}
