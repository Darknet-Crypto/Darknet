// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/pivx-config.h"
#endif

#include "optionsmodel.h"

#include "bitcoinunits.h"
#include "guiutil.h"

#include "init.h"
#include "mapport.h"
#include "net.h"
#include "netbase.h"
#include "txdb.h" // for -dbcache defaults
#include "util/system.h"

#ifdef ENABLE_WALLET
#include "masternodeconfig.h"
#include "wallet/wallet.h"
#include "wallet/walletdb.h"
#endif

#include <QDebug>
#include <QStringList>

OptionsModel::OptionsModel(QObject* parent) : QAbstractListModel(parent)
{
    Init();
}

void OptionsModel::addOverriddenOption(const std::string& option)
{
    strOverriddenByCommandLine += QString::fromStdString(option) + "=" + QString::fromStdString(gArgs.GetArg(option, "")) + " ";
}

// Writes all missing QSettings with their default values
void OptionsModel::Init()
{
    resetSettings = false;
    QSettings settings;

    // Ensure restart flag is unset on client startup
    setRestartRequired(false);

    // These are Qt-only settings:

    // Window
    setWindowDefaultOptions(settings);

    // Display
    if (!settings.contains("fHideZeroBalances"))
        settings.setValue("fHideZeroBalances", true);
    fHideZeroBalances = settings.value("fHideZeroBalances").toBool();

    if (!settings.contains("fHideOrphans"))
        settings.setValue("fHideOrphans", true);
    fHideOrphans = settings.value("fHideOrphans").toBool();

    if (!settings.contains("fCoinControlFeatures"))
        settings.setValue("fCoinControlFeatures", false);
    fCoinControlFeatures = settings.value("fCoinControlFeatures", false).toBool();

    if (!settings.contains("fShowColdStakingScreen"))
        settings.setValue("fShowColdStakingScreen", false);
    showColdStakingScreen = settings.value("fShowColdStakingScreen", false).toBool();

    if (!settings.contains("fShowMasternodesTab"))
        settings.setValue("fShowMasternodesTab", masternodeConfig.getCount());

    // Main
    setMainDefaultOptions(settings);

    // Wallet
#ifdef ENABLE_WALLET
    setWalletDefaultOptions(settings);
#endif

    // Network
    setNetworkDefaultOptions(settings);
    // Display
    setDisplayDefaultOptions(settings);

    language = settings.value("language").toString();
}

void OptionsModel::refreshDataView()
{
    Q_EMIT dataChanged(index(0), index(rowCount(QModelIndex()) - 1));
}

void OptionsModel::setMainDefaultOptions(QSettings& settings, bool reset)
{
    // These are shared with the core or have a command-line parameter
    // and we want command-line parameters to overwrite the GUI settings.
    //
    // If setting doesn't exist create it with defaults.
    //
    // If gArgs.SoftSetArg() or gArgs.SoftSetBoolArg() return false we were overridden
    // by command-line and show this in the UI.
    // Main

    // Default database cache is bumped from the original 100 MiB value.
    // If we still have the old setting "nDatabaseCache" then:
    // - if the value is equal to the old default (100 MiB), update the new setting "nDatabaseCache2"
    //   to the new default value (300 MiB)
    // - if the value is different, then copy it to "nDatabaseCache2"
    // - remove the old setting
    if (settings.contains("nDatabaseCache")) {
        qint64 saved_dbcache = settings.value("nDatabaseCache").toLongLong();
        settings.setValue("nDatabaseCache2", saved_dbcache != 100 ? saved_dbcache : (qint64)nDefaultDbCache);
        settings.remove("nDatabaseCache");
    }

    if (!settings.contains("nDatabaseCache2") || reset)
        settings.setValue("nDatabaseCache2", (qint64)nDefaultDbCache);
    if (!gArgs.SoftSetArg("-dbcache", settings.value("nDatabaseCache2").toString().toStdString()))
        addOverriddenOption("-dbcache");

    if (!settings.contains("nThreadsScriptVerif") || reset)
        settings.setValue("nThreadsScriptVerif", DEFAULT_SCRIPTCHECK_THREADS);
    if (!gArgs.SoftSetArg("-par", settings.value("nThreadsScriptVerif").toString().toStdString()))
        addOverriddenOption("-par");

    if (reset) {
        refreshDataView();
    }
}

void OptionsModel::setWalletDefaultOptions(QSettings& settings, bool reset)
{
    if (!settings.contains("bSpendZeroConfChange") || reset)
        settings.setValue("bSpendZeroConfChange", false);
    if (!gArgs.SoftSetBoolArg("-spendzeroconfchange", settings.value("bSpendZeroConfChange").toBool()))
        addOverriddenOption("-spendzeroconfchange");
    if (reset) {
        refreshDataView();
    }
}

void OptionsModel::setNetworkDefaultOptions(QSettings& settings, bool reset)
{
    if (!settings.contains("fUseUPnP") || reset)
        settings.setValue("fUseUPnP", DEFAULT_UPNP);
    if (!gArgs.SoftSetBoolArg("-upnp", settings.value("fUseUPnP").toBool()))
        addOverriddenOption("-upnp");

    if (!settings.contains("fUseNatpmp") || reset )
        settings.setValue("fUseNatpmp", DEFAULT_NATPMP);
    if (!gArgs.SoftSetBoolArg("-natpmp", settings.value("fUseNatpmp").toBool()))
        addOverriddenOption("-natpmp");

    if (!settings.contains("fListen") || reset)
        settings.setValue("fListen", DEFAULT_LISTEN);
    if (!gArgs.SoftSetBoolArg("-listen", settings.value("fListen").toBool()))
        addOverriddenOption("-listen");

    if (!settings.contains("fUseProxy") || reset)
        settings.setValue("fUseProxy", false);
    if (!settings.contains("addrProxy") || reset)
        settings.setValue("addrProxy", "127.0.0.1:9050");
    // Only try to set -proxy, if user has enabled fUseProxy
    if (settings.value("fUseProxy").toBool() && !gArgs.SoftSetArg("-proxy", settings.value("addrProxy").toString().toStdString()))
        addOverriddenOption("-proxy");
    else if (!settings.value("fUseProxy").toBool() && !gArgs.GetArg("-proxy", "").empty())
        addOverriddenOption("-proxy");

    if (reset) {
        refreshDataView();
    }
}

void OptionsModel::setWindowDefaultOptions(QSettings& settings, bool reset)
{
    if (!settings.contains("fMinimizeToTray") || reset)
        settings.setValue("fMinimizeToTray", false);
    fMinimizeToTray = settings.value("fMinimizeToTray").toBool();

    if (!settings.contains("fMinimizeOnClose") || reset)
        settings.setValue("fMinimizeOnClose", false);
    fMinimizeOnClose = settings.value("fMinimizeOnClose").toBool();

    if (reset) {
        refreshDataView();
    }
}

void OptionsModel::setDisplayDefaultOptions(QSettings& settings, bool reset)
{
    if (!settings.contains("nDisplayUnit") || reset)
        settings.setValue("nDisplayUnit", BitcoinUnits::PIV);
    nDisplayUnit = settings.value("nDisplayUnit").toInt();
    if (!settings.contains("digits") || reset)
        settings.setValue("digits", "2");
    if (!settings.contains("theme") || reset)
        settings.setValue("theme", "");
    if (!settings.contains("fCSSexternal") || reset)
        settings.setValue("fCSSexternal", false);
    if (!settings.contains("language") || reset)
        settings.setValue("language", "");
    if (!gArgs.SoftSetArg("-lang", settings.value("language").toString().toStdString()))
        addOverriddenOption("-lang");

    if (settings.contains("nAnonymizePivxAmount") || reset)
        gArgs.SoftSetArg("-anonymizepivxamount", settings.value("nAnonymizePivxAmount").toString().toStdString());

    if (!settings.contains("strThirdPartyTxUrls") || reset)
        settings.setValue("strThirdPartyTxUrls", "");
    strThirdPartyTxUrls = settings.value("strThirdPartyTxUrls", "").toString();

    fHideCharts = gArgs.GetBoolArg("-hidecharts", false);

    if (reset) {
        refreshDataView();
    }
}

void OptionsModel::Reset()
{
    QSettings settings;

    // Remove all entries from our QSettings object
    settings.clear();
    resetSettings = true; // Needed in pivx.cpp during shotdown to also remove the window positions

    // default setting for OptionsModel::StartAtStartup - disabled
    if (GUIUtil::GetStartOnSystemStartup())
        GUIUtil::SetStartOnSystemStartup(false);
}

int OptionsModel::rowCount(const QModelIndex& parent) const
{
    return OptionIDRowCount;
}

// read QSettings values and return them
QVariant OptionsModel::data(const QModelIndex& index, int role) const
{
    if (role == Qt::EditRole) {
        QSettings settings;
        switch (index.row()) {
        case StartAtStartup:
            return GUIUtil::GetStartOnSystemStartup();
        case MinimizeToTray:
            return fMinimizeToTray;
        case MapPortUPnP:
#ifdef USE_UPNP
            return settings.value("fUseUPnP");
#else
            return false;
#endif // USE_UPNP
        case MapPortNatpmp:
#ifdef USE_NATPMP
            return settings.value("fUseNatpmp");
#else
            return false;
#endif // USE_NATPMP
        case MinimizeOnClose:
            return fMinimizeOnClose;

        // default proxy
        case ProxyUse:
            return settings.value("fUseProxy", false);
        case ProxyIP: {
            // contains IP at index 0 and port at index 1
            QStringList strlIpPort = GUIUtil::SplitSkipEmptyParts(settings.value("addrProxy").toString(), ":");
            return strlIpPort.at(0);
        }
        case ProxyPort: {
            // contains IP at index 0 and port at index 1
            QStringList strlIpPort = GUIUtil::SplitSkipEmptyParts(settings.value("addrProxy").toString(), ":");
            return strlIpPort.at(1);
        }

#ifdef ENABLE_WALLET
        case SpendZeroConfChange:
            return settings.value("bSpendZeroConfChange");
        case ShowMasternodesTab:
            return settings.value("fShowMasternodesTab");
#endif
        case DisplayUnit:
            return nDisplayUnit;
        case ThirdPartyTxUrls:
            return strThirdPartyTxUrls;
        case Digits:
            return settings.value("digits");
        case Theme:
            return settings.value("theme");
        case Language:
            return settings.value("language");
        case CoinControlFeatures:
            return fCoinControlFeatures;
        case ShowColdStakingScreen:
            return showColdStakingScreen;
        case DatabaseCache:
            return settings.value("nDatabaseCache2");
        case ThreadsScriptVerif:
            return settings.value("nThreadsScriptVerif");
        case HideCharts:
            return fHideCharts;
        case HideZeroBalances:
            return settings.value("fHideZeroBalances");
        case HideOrphans:
            return settings.value("fHideOrphans");
        case Listen:
            return settings.value("fListen");
        default:
            return QVariant();
        }
    }
    return QVariant();
}

// write QSettings values
bool OptionsModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    bool successful = true; /* set to false on parse error */
    if (role == Qt::EditRole) {
        QSettings settings;
        switch (index.row()) {
        case StartAtStartup:
            successful = GUIUtil::SetStartOnSystemStartup(value.toBool());
            break;
        case MinimizeToTray:
            fMinimizeToTray = value.toBool();
            settings.setValue("fMinimizeToTray", fMinimizeToTray);
            break;
        case MapPortUPnP: // core option - can be changed on-the-fly
            settings.setValue("fUseUPnP", value.toBool());
            break;
        case MapPortNatpmp: // core option - can be changed on-the-fly
            settings.setValue("fUseNatpmp", value.toBool());
            break;
        case MinimizeOnClose:
            fMinimizeOnClose = value.toBool();
            settings.setValue("fMinimizeOnClose", fMinimizeOnClose);
            break;

        // default proxy
        case ProxyUse:
            if (settings.value("fUseProxy") != value) {
                settings.setValue("fUseProxy", value.toBool());
                setRestartRequired(true);
            }
            break;
        case ProxyIP: {
            // contains current IP at index 0 and current port at index 1
            QStringList strlIpPort = GUIUtil::SplitSkipEmptyParts(settings.value("addrProxy").toString(), ":");
            // if that key doesn't exist or has a changed IP
            if (!settings.contains("addrProxy") || strlIpPort.at(0) != value.toString()) {
                // construct new value from new IP and current port
                QString strNewValue = value.toString() + ":" + strlIpPort.at(1);
                settings.setValue("addrProxy", strNewValue);
                setRestartRequired(true);
            }
        } break;
        case ProxyPort: {
            // contains current IP at index 0 and current port at index 1
            QStringList strlIpPort = GUIUtil::SplitSkipEmptyParts(settings.value("addrProxy").toString(), ":");
            // if that key doesn't exist or has a changed port
            if (!settings.contains("addrProxy") || strlIpPort.at(1) != value.toString()) {
                // construct new value from current IP and new port
                QString strNewValue = strlIpPort.at(0) + ":" + value.toString();
                settings.setValue("addrProxy", strNewValue);
                setRestartRequired(true);
            }
        } break;
#ifdef ENABLE_WALLET
        case SpendZeroConfChange:
            if (settings.value("bSpendZeroConfChange") != value) {
                settings.setValue("bSpendZeroConfChange", value);
                setRestartRequired(true);
            }
            break;
        case ShowMasternodesTab:
            if (settings.value("fShowMasternodesTab") != value) {
                settings.setValue("fShowMasternodesTab", value);
                setRestartRequired(true);
            }
            break;
#endif
        case DisplayUnit:
            setDisplayUnit(value);
            break;
        case ThirdPartyTxUrls:
            if (strThirdPartyTxUrls != value.toString()) {
                strThirdPartyTxUrls = value.toString();
                settings.setValue("strThirdPartyTxUrls", strThirdPartyTxUrls);
                setRestartRequired(true);
            }
            break;
        case Digits:
            if (settings.value("digits") != value) {
                settings.setValue("digits", value);
                setRestartRequired(true);
            }
            break;
        case Theme:
            if (settings.value("theme") != value) {
                settings.setValue("theme", value);
                setRestartRequired(true);
            }
            break;
        case Language:
            if (settings.value("language") != value) {
                settings.setValue("language", value);
                setRestartRequired(true);
            }
            break;
        case HideCharts:
            fHideCharts = value.toBool();   // memory only
            Q_EMIT hideChartsChanged(fHideCharts);
            break;
        case HideZeroBalances:
            fHideZeroBalances = value.toBool();
            settings.setValue("fHideZeroBalances", fHideZeroBalances);
            Q_EMIT hideZeroBalancesChanged(fHideZeroBalances);
            break;
        case HideOrphans:
            fHideOrphans = value.toBool();
            settings.setValue("fHideOrphans", fHideOrphans);
            Q_EMIT hideOrphansChanged(fHideOrphans);
            break;
        case CoinControlFeatures:
            fCoinControlFeatures = value.toBool();
            settings.setValue("fCoinControlFeatures", fCoinControlFeatures);
            Q_EMIT coinControlFeaturesChanged(fCoinControlFeatures);
            break;
        case ShowColdStakingScreen:
            this->showColdStakingScreen = value.toBool();
            settings.setValue("fShowColdStakingScreen", this->showColdStakingScreen);
            Q_EMIT showHideColdStakingScreen(this->showColdStakingScreen);
            break;
        case DatabaseCache:
            if (settings.value("nDatabaseCache2") != value) {
                settings.setValue("nDatabaseCache2", value);
                setRestartRequired(true);
            }
            break;
        case ThreadsScriptVerif:
            if (settings.value("nThreadsScriptVerif") != value) {
                settings.setValue("nThreadsScriptVerif", value);
                setRestartRequired(true);
            }
            break;
        case Listen:
            if (settings.value("fListen") != value) {
                settings.setValue("fListen", value);
                setRestartRequired(true);
            }
            break;
        default:
            break;
        }
    }

    Q_EMIT dataChanged(index, index);

    return successful;
}

/** Updates current unit in memory, settings and emits displayUnitChanged(newUnit) signal */
void OptionsModel::setDisplayUnit(const QVariant& value)
{
    if (!value.isNull()) {
        QSettings settings;
        nDisplayUnit = value.toInt();
        settings.setValue("nDisplayUnit", nDisplayUnit);
        Q_EMIT displayUnitChanged(nDisplayUnit);
    }
}

void OptionsModel::setRestartRequired(bool fRequired)
{
    QSettings settings;
    return settings.setValue("fRestartRequired", fRequired);
}

bool OptionsModel::isRestartRequired()
{
    QSettings settings;
    return settings.value("fRestartRequired", false).toBool();
}
