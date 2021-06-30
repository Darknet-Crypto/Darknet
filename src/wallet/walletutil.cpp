// Copyright (c) 2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "wallet/walletutil.h"
#include "util/system.h"

fs::path GetWalletDir()
{
    fs::path path;

    if (gArgs.IsArgSet("-walletdir")) {
        path = gArgs.GetArg("-walletdir", "");
        if (!fs::is_directory(path)) {
            // If the path specified doesn't exist, we return the deliberately
            // invalid empty string.
            path = "";
        }
    } else {
        path = GetDataDir();
        // If a wallets directory exists, use that, otherwise default to GetDataDir
        if (fs::is_directory(path / "wallets")) {
            path /= "wallets";
        }
    }

    return path;
}

OperationResult VerifyWalletPath(const std::string& walletFile)
{
    // Do some checking on wallet path. It should be either a:
    //
    // 1. Path where a directory can be created.
    // 2. Path to an existing directory.
    // 3. Path to a symlink to a directory.
    // 4. For backwards compatibility, the name of a data file in -walletdir.
    fs::path wallet_path = fs::absolute(walletFile, GetWalletDir());
    fs::file_type path_type = fs::symlink_status(wallet_path).type();
    if (!(path_type == fs::file_not_found || path_type == fs::directory_file ||
          (path_type == fs::symlink_file && fs::is_directory(wallet_path)) ||
          (path_type == fs::regular_file && fs::path(walletFile).filename() == walletFile))) {
        return {false, (strprintf(
                _("Invalid -wallet path '%s'. -wallet path should point to a directory where wallet.dat and "
                  "database/log.?????????? files can be stored, a location where such a directory could be created "
                  "or (for backwards compatibility) the name of an existing data file in -walletdir (%s)"),
                walletFile, GetWalletDir()))};
    }
    return {true};
}
