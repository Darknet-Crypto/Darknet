// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2017 The Bitcoin Core developers
// Copyright (c) 2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "wallet/init.h"

#include "guiinterfaceutil.h"
#include "net.h"
#include "util/system.h"
#include "utilmoneystr.h"
#include "validation.h"
#include "wallet/wallet.h"
#include "wallet/walletutil.h"

std::string GetWalletHelpString(bool showDebug)
{
    std::string strUsage = HelpMessageGroup(_("Wallet options:"));
    strUsage += HelpMessageOpt("-backuppath=<dir|file>", _("Specify custom backup path to add a copy of any wallet backup. If set as dir, every backup generates a timestamped file. If set as file, will rewrite to that file every backup."));
    strUsage += HelpMessageOpt("-createwalletbackups=<n>", strprintf(_("Number of automatic wallet backups (default: %d)"), DEFAULT_CREATEWALLETBACKUPS));
    strUsage += HelpMessageOpt("-disablewallet", strprintf(_("Do not load the wallet and disable wallet RPC calls (default: %u)"), DEFAULT_DISABLE_WALLET));
    strUsage += HelpMessageOpt("-keypool=<n>", strprintf(_("Set key pool size to <n> (default: %u)"), DEFAULT_KEYPOOL_SIZE));
    strUsage += HelpMessageOpt("-legacywallet", _("On first run, create a legacy wallet instead of a HD wallet"));
    strUsage += HelpMessageOpt("-maxtxfee=<amt>", strprintf(_("Maximum total fees to use in a single wallet transaction, setting too low may abort large transactions (default: %s)"), FormatMoney(maxTxFee)));
    strUsage += HelpMessageOpt("-mintxfee=<amt>", strprintf(_("Fees (in %s/Kb) smaller than this are considered zero fee for transaction creation (default: %s)"), CURRENCY_UNIT, FormatMoney(CWallet::minTxFee.GetFeePerK())));
    strUsage += HelpMessageOpt("-paytxfee=<amt>", strprintf(_("Fee (in %s/kB) to add to transactions you send (default: %s)"), CURRENCY_UNIT, FormatMoney(payTxFee.GetFeePerK())));
    strUsage += HelpMessageOpt("-rescan", _("Rescan the block chain for missing wallet transactions") + " " + _("on startup"));
    strUsage += HelpMessageOpt("-salvagewallet", _("Attempt to recover private keys from a corrupt wallet file") + " " + _("on startup"));
    strUsage += HelpMessageOpt("-spendzeroconfchange", strprintf(_("Spend unconfirmed change when sending transactions (default: %u)"), DEFAULT_SPEND_ZEROCONF_CHANGE));
    strUsage += HelpMessageOpt("-txconfirmtarget=<n>", strprintf(_("If paytxfee is not set, include enough fee so transactions begin confirmation on average within n blocks (default: %u)"), 1));
    strUsage += HelpMessageOpt("-upgradewallet", _("Upgrade wallet to latest format") + " " + _("on startup"));
    strUsage += HelpMessageOpt("-wallet=<path>", _("Specify wallet database path. Can be specified multiple times to load multiple wallets. Path is interpreted relative to <walletdir> if it is not absolute, and will be created if it does not exist (as a directory containing a wallet.dat file and log files). For backwards compatibility this will also accept names of existing data files in <walletdir>.)"));
    strUsage += HelpMessageOpt("-walletdir=<dir>", _("Specify directory to hold wallets (default: <datadir>/wallets if it exists, otherwise <datadir>)"));
    strUsage += HelpMessageOpt("-walletnotify=<cmd>", _("Execute command when a wallet transaction changes (%s in cmd is replaced by TxID)"));
    strUsage += HelpMessageOpt("-zapwallettxes=<mode>", _("Delete all wallet transactions and only recover those parts of the blockchain through -rescan on startup") +
        " " + _("(1 = keep tx meta data e.g. payment request information, 2 = drop tx meta data)"));
    strUsage += HelpMessageGroup(_("Mining/Staking options:"));
    strUsage += HelpMessageOpt("-coldstaking=<n>", strprintf(_("Enable cold staking functionality (0-1, default: %u). Disabled if staking=0"), DEFAULT_COLDSTAKING));
    strUsage += HelpMessageOpt("-gen", strprintf(_("Generate coins (default: %u)"), DEFAULT_GENERATE));
    strUsage += HelpMessageOpt("-genproclimit=<n>", strprintf(_("Set the number of threads for coin generation if enabled (-1 = all cores, default: %d)"), DEFAULT_GENERATE_PROCLIMIT));
    strUsage += HelpMessageOpt("-minstakesplit=<amt>", strprintf(_("Minimum positive amount (in PIV) allowed by GUI and RPC for the stake split threshold (default: %s)"), FormatMoney(DEFAULT_MIN_STAKE_SPLIT_THRESHOLD)));
    strUsage += HelpMessageOpt("-staking=<n>", strprintf(_("Enable staking functionality (0-1, default: %u)"), DEFAULT_STAKING));
    if (showDebug) {
        strUsage += HelpMessageGroup(_("Wallet debugging/testing options:"));
        strUsage += HelpMessageOpt("-dblogsize=<n>", strprintf(_("Flush database activity from memory pool to disk log every <n> megabytes (default: %u)"), DEFAULT_WALLET_DBLOGSIZE));
        strUsage += HelpMessageOpt("-flushwallet", strprintf(_("Run a thread to flush wallet periodically (default: %u)"), DEFAULT_FLUSHWALLET));
        strUsage += HelpMessageOpt("-privdb", strprintf(_("Sets the DB_PRIVATE flag in the wallet db environment (default: %u)"), DEFAULT_WALLET_PRIVDB));
    }

    return strUsage;
}

bool WalletParameterInteraction()
{
    if (gArgs.GetBoolArg("-disablewallet", DEFAULT_DISABLE_WALLET)) {
        return true;
    }

    if (gArgs.GetBoolArg("-sysperms", false)) {
        return UIError(strprintf(_("%s is not allowed in combination with enabled wallet functionality"), "-sysperms"));
    }

    gArgs.SoftSetArg("-wallet", "");
    const bool is_multiwallet = gArgs.GetArgs("-wallet").size() > 1;

    if (gArgs.GetBoolArg("-salvagewallet", false)) {
        if (is_multiwallet) {
            return UIError(strprintf(_("%s is only allowed with a single wallet file"), "-salvagewallet"));
        }
        // Rewrite just private keys: rescan to find transactions
        if (gArgs.SoftSetBoolArg("-rescan", true)) {
            LogPrintf("%s: parameter interaction: -salvagewallet=1 -> setting -rescan=1\n", __func__);
        }
    }

    bool zapwallettxes = gArgs.GetBoolArg("-zapwallettxes", false);
    // -zapwallettxes implies dropping the mempool on startup
    if (zapwallettxes && gArgs.SoftSetBoolArg("-persistmempool", false)) {
        LogPrintf("%s: parameter interaction: -zapwallettxes=%s -> setting -persistmempool=0\n", __func__, zapwallettxes);
    }

    // -zapwallettxes implies a rescan
    if (zapwallettxes) {
        if (is_multiwallet) {
            return UIError(strprintf(_("%s is only allowed with a single wallet file"), "-zapwallettxes"));
        }
        if (gArgs.SoftSetBoolArg("-rescan", true)) {
            LogPrintf("%s: parameter interaction: -zapwallettxes=<mode> -> setting -rescan=1\n", __func__);
        }
    }

    if (is_multiwallet) {
        if (gArgs.GetBoolArg("-upgradewallet", false)) {
            return UIError(strprintf(_("%s is only allowed with a single wallet file"), "-upgradewallet"));
        }
    }

    if (gArgs.IsArgSet("-mintxfee")) {
        CAmount n = 0;
        if (ParseMoney(gArgs.GetArg("-mintxfee", ""), n) && n > 0)
            CWallet::minTxFee = CFeeRate(n);
        else
            return UIError(AmountErrMsg("mintxfee", gArgs.GetArg("-mintxfee", "")));
    }
    if (gArgs.IsArgSet("-paytxfee")) {
        CAmount nFeePerK = 0;
        if (!ParseMoney(gArgs.GetArg("-paytxfee", ""), nFeePerK))
            return UIError(AmountErrMsg("paytxfee", gArgs.GetArg("-paytxfee", "")));
        if (nFeePerK > nHighTransactionFeeWarning)
            UIWarning(strprintf(_("Warning: %s is set very high! This is the transaction fee you will pay if you send a transaction."), "-paytxfee"));
        payTxFee = CFeeRate(nFeePerK, 1000);
        if (payTxFee < ::minRelayTxFee) {
            return UIError(strprintf(_("Invalid amount for %s: '%s' (must be at least %s)"), "-paytxfee",
                                       gArgs.GetArg("-paytxfee", ""), ::minRelayTxFee.ToString()));
        }
    }
    if (gArgs.IsArgSet("-maxtxfee")) {
        CAmount nMaxFee = 0;
        if (!ParseMoney(gArgs.GetArg("-maxtxfee", ""), nMaxFee))
            return UIError(AmountErrMsg("maxtxfee", gArgs.GetArg("-maxtxfee", "")));
        if (nMaxFee > nHighTransactionMaxFeeWarning)
            UIWarning(strprintf(_("Warning: %s is set very high! Fees this large could be paid on a single transaction."), "-maxtxfee"));
        maxTxFee = nMaxFee;
        if (CFeeRate(maxTxFee, 1000) < ::minRelayTxFee) {
            return UIError(strprintf(_("Invalid amount for %s: '%s' (must be at least the minimum relay fee of %s to prevent stuck transactions)"),
                                       "-maxtxfee", gArgs.GetArg("-maxtxfee", ""), ::minRelayTxFee.ToString()));
        }
    }
    if (gArgs.IsArgSet("-minstakesplit")) {
        CAmount n = 0;
        if (ParseMoney(gArgs.GetArg("-minstakesplit", ""), n) && n > 0)
            CWallet::minStakeSplitThreshold = n;
        else
            return UIError(AmountErrMsg("minstakesplit", gArgs.GetArg("-minstakesplit", "")));
    }
    nTxConfirmTarget = gArgs.GetArg("-txconfirmtarget", 1);
    bSpendZeroConfChange = gArgs.GetBoolArg("-spendzeroconfchange", DEFAULT_SPEND_ZEROCONF_CHANGE);
    bdisableSystemnotifications = gArgs.GetBoolArg("-disablesystemnotifications", false);

    return true;
}

bool WalletVerify()
{
    if (gArgs.GetBoolArg("-disablewallet", DEFAULT_DISABLE_WALLET)) {
        return true;
    }

    if (gArgs.IsArgSet("-walletdir")) {
        fs::path wallet_dir = gArgs.GetArg("-walletdir", "");
        if (!fs::exists(wallet_dir)) {
            return UIError(strprintf(_("Specified -walletdir \"%s\" does not exist"), wallet_dir.string()));
        } else if (!fs::is_directory(wallet_dir)) {
            return UIError(strprintf(_("Specified -walletdir \"%s\" is not a directory"), wallet_dir.string()));
        } else if (!wallet_dir.is_absolute()) {
            return UIError(strprintf(_("Specified -walletdir \"%s\" is a relative path"), wallet_dir.string()));
        }
    }

    LogPrintf("Using wallet directory %s\n", GetWalletDir().string());

    uiInterface.InitMessage(_("Verifying wallet(s)..."));

    // Keep track of each wallet absolute path to detect duplicates.
    std::set<fs::path> wallet_paths;

    for (const std::string& walletFile : gArgs.GetArgs("-wallet")) {
        auto opRes = VerifyWalletPath(walletFile);
        if (!opRes) return UIError(opRes.getError());

        fs::path wallet_path = fs::absolute(walletFile, GetWalletDir());
        if (!wallet_paths.insert(wallet_path).second) {
            return UIError(strprintf(_("Error loading wallet %s. Duplicate %s filename specified."), walletFile, "-wallet"));
        }

        std::string strError;
        if (!CWalletDB::VerifyEnvironment(wallet_path, strError)) {
            return UIError(strError);
        }

        if (gArgs.GetBoolArg("-salvagewallet", false)) {
            // Recover readable keypairs:
            CWallet dummyWallet("dummy", CWalletDBWrapper::CreateDummy());
            std::string backup_filename;
            // Even if we don't use this lock in this function, we want to preserve
            // lock order in LoadToWallet if query of chain state is needed to know
            // tx status. If lock can't be taken, tx confirmation status may be not
            // reliable.
            LOCK(cs_main);
            if (!CWalletDB::Recover(wallet_path, (void *)&dummyWallet, CWalletDB::RecoverKeysOnlyFilter, backup_filename)) {
                return false;
            }
        }

        std::string strWarning;
        bool dbV = CWalletDB::VerifyDatabaseFile(wallet_path, strWarning, strError);
        if (!strWarning.empty()) {
            UIWarning(strWarning);
        }
        if (!dbV) {
            return UIError(strError);
        }
    }

    return true;
}

bool InitLoadWallet()
{
    if (gArgs.GetBoolArg("-disablewallet", DEFAULT_DISABLE_WALLET)) {
        LogPrintf("Wallet disabled!\n");
        return true;
    }

    for (const std::string& walletFile : gArgs.GetArgs("-wallet")) {
        // create/load wallet
        CWallet * const pwallet = CWallet::CreateWalletFromFile(walletFile, fs::absolute(walletFile, GetWalletDir()));
        if (!pwallet) {
            return false;
        }

        // automatic backup
        std::string strWarning, strError;
        if (!AutoBackupWallet(*pwallet, strWarning, strError)) {
            if (!strWarning.empty()) {
                UIWarning(strprintf("%s: %s", walletFile, strWarning));
            }
            if (!strError.empty()) {
                return UIError(strprintf("%s: %s", walletFile, strError));
            }
        }

        // add to wallets in use
        vpwallets.emplace_back(pwallet);
    }

    return true;
}
