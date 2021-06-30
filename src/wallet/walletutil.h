// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_UTIL_H
#define BITCOIN_WALLET_UTIL_H

#include "fs.h"
#include "operationresult.h"

//! Get the path of the wallet directory.
fs::path GetWalletDir();
//! Verify the wallet db's path
OperationResult VerifyWalletPath(const std::string& walletFile);

#endif // BITCOIN_WALLET_UTIL_H
