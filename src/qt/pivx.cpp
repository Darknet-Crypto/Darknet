// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include "config/pivx-config.h"
#endif

#include "qt/pivx/pivxgui.h"

#include "clientmodel.h"
#include "guiconstants.h"
#include "guiutil.h"
#include "intro.h"
#include "net.h"
#include "networkstyle.h"
#include "optionsmodel.h"
#include "qt/pivx/splash.h"
#include "qt/pivx/welcomecontentwidget.h"
#include "utilitydialog.h"
#include "winshutdownmonitor.h"

#ifdef ENABLE_WALLET
#include "qt/pivx/governancemodel.h"
#include "qt/pivx/mnmodel.h"
#include "paymentserver.h"
#include "walletmodel.h"
#include "interfaces/wallet.h"
#include "wallet/walletutil.h"
#endif
#include "masternodeconfig.h"

#include "fs.h"
#include "init.h"
#include "rpc/server.h"
#include "guiinterface.h"
#include "util/system.h"
#include "warnings.h"

#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif

#include <QApplication>
#include <QDebug>
#include <QLibraryInfo>
#include <QLocale>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QTranslator>

#if defined(QT_STATICPLUGIN)
#include <QtPlugin>
#if defined(QT_QPA_PLATFORM_XCB)
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_WINDOWS)
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
#elif defined(QT_QPA_PLATFORM_COCOA)
Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin);
#endif
Q_IMPORT_PLUGIN(QSvgPlugin);
Q_IMPORT_PLUGIN(QSvgIconPlugin);
Q_IMPORT_PLUGIN(QGifPlugin);
#endif

// Declare meta types used for QMetaObject::invokeMethod
Q_DECLARE_METATYPE(bool*)
Q_DECLARE_METATYPE(CAmount)
Q_DECLARE_METATYPE(interfaces::WalletBalances);
Q_DECLARE_METATYPE(uint256)

static void InitMessage(const std::string& message)
{
    LogPrintf("init message: %s\n", message);
}

/*
   Translate string to current locale using Qt.
 */
static std::string Translate(const char* psz)
{
    return QCoreApplication::translate("pivx-core", psz).toStdString();
}

static QString GetLangTerritory(bool forceLangFromSetting = false)
{
    QSettings settings;
    // Get desired locale (e.g. "de_DE")
    // 1) System default language
    QString lang_territory = QLocale::system().name();
    // 2) Language from QSettings
    QString lang_territory_qsettings = settings.value("language", "").toString();
    if (!lang_territory_qsettings.isEmpty())
        lang_territory = lang_territory_qsettings;
    // 3) -lang command line argument
    lang_territory = QString::fromStdString(gArgs.GetArg("-lang", lang_territory.toStdString()));
    return (forceLangFromSetting) ? lang_territory_qsettings : lang_territory;
}

/** Set up translations */
static void initTranslations(QTranslator& qtTranslatorBase, QTranslator& qtTranslator, QTranslator& translatorBase, QTranslator& translator, bool forceLangFromSettings = false)
{
    // Remove old translators
    QApplication::removeTranslator(&qtTranslatorBase);
    QApplication::removeTranslator(&qtTranslator);
    QApplication::removeTranslator(&translatorBase);
    QApplication::removeTranslator(&translator);

    // Get desired locale (e.g. "de_DE")
    // 1) System default language
    QString lang_territory = GetLangTerritory(forceLangFromSettings);

    // Convert to "de" only by truncating "_DE"
    QString lang = lang_territory;
    lang.truncate(lang_territory.lastIndexOf('_'));

    // Load language files for configured locale:
    // - First load the translator for the base language, without territory
    // - Then load the more specific locale translator

    // Load e.g. qt_de.qm
    if (qtTranslatorBase.load("qt_" + lang, QLibraryInfo::location(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qtTranslatorBase);

    // Load e.g. qt_de_DE.qm
    if (qtTranslator.load("qt_" + lang_territory, QLibraryInfo::location(QLibraryInfo::TranslationsPath)))
        QApplication::installTranslator(&qtTranslator);

    // Load e.g. bitcoin_de.qm (shortcut "de" needs to be defined in pivx.qrc)
    if (translatorBase.load(lang, ":/translations/"))
        QApplication::installTranslator(&translatorBase);

    // Load e.g. bitcoin_de_DE.qm (shortcut "de_DE" needs to be defined in pivx.qrc)
    if (translator.load(lang_territory, ":/translations/"))
        QApplication::installTranslator(&translator);
}

/* qDebug() message handler --> debug.log */
void DebugMessageHandler(QtMsgType type, const QMessageLogContext& context, const QString& msg)
{
    Q_UNUSED(context);
    if (type == QtDebugMsg) {
        LogPrint(BCLog::QT, "GUI: %s\n", msg.toStdString());
    } else {
        LogPrintf("GUI: %s\n", msg.toStdString());
    }
}

/** Class encapsulating PIVX Core startup and shutdown.
 * Allows running startup and shutdown in a different thread from the UI thread.
 */
class BitcoinCore : public QObject
{
    Q_OBJECT
public:
    explicit BitcoinCore();

public Q_SLOTS:
    void initialize();
    void shutdown();
    void restart(QStringList args);

Q_SIGNALS:
    void initializeResult(int retval);
    void shutdownResult(int retval);
    void runawayException(const QString& message);

private:
    /// Flag indicating a restart
    bool execute_restart{false};

    /// Pass fatal exception message to UI thread
    void handleRunawayException(const std::exception* e);
};

/** Main PIVX application object */
class BitcoinApplication : public QApplication
{
    Q_OBJECT
public:
    explicit BitcoinApplication(int& argc, char** argv);
    ~BitcoinApplication();

#ifdef ENABLE_WALLET
    /// Create payment server
    void createPaymentServer();
#endif
    /// parameter interaction/setup based on rules
    void parameterSetup();
    /// Create options model
    void createOptionsModel();
    /// Create main window
    void createWindow(const NetworkStyle* networkStyle);
    /// Create splash screen
    void createSplashScreen(const NetworkStyle* networkStyle);

    /// Create tutorial screen
    bool createTutorialScreen();

    /// Request core initialization
    void requestInitialize();
    /// Request core shutdown
    void requestShutdown();

    /// Get process return value
    int getReturnValue() { return returnValue; }

    /// Get window identifier of QMainWindow (PIVXGUI)
    WId getMainWinId() const;

public Q_SLOTS:
    void initializeResult(int retval);
    void shutdownResult(int retval);
    /// Handle runaway exceptions. Shows a message box with the problem and quits the program.
    void handleRunawayException(const QString& message);
    void updateTranslation(bool forceLangFromSettings = false);

Q_SIGNALS:
    void requestedInitialize();
    void requestedRestart(QStringList args);
    void requestedShutdown();
    void stopThread();
    void splashFinished(QWidget* window);

private:
    QThread* coreThread{nullptr};
    OptionsModel* optionsModel{nullptr};
    ClientModel* clientModel{nullptr};
    PIVXGUI* window{nullptr};
    QTimer* pollShutdownTimer{nullptr};
#ifdef ENABLE_WALLET
    PaymentServer* paymentServer{nullptr};
    WalletModel* walletModel{nullptr};
    GovernanceModel* govModel{nullptr};
    MNModel* mnModel{nullptr};
#endif
    int returnValue{0};
    QTranslator qtTranslatorBase, qtTranslator, translatorBase, translator;

    void startThread();
};

#include "pivx.moc"

BitcoinCore::BitcoinCore() : QObject()
{
}

void BitcoinCore::handleRunawayException(const std::exception* e)
{
    PrintExceptionContinue(e, "Runaway exception");
    Q_EMIT runawayException(QString::fromStdString(GetWarnings("gui")));
}

void BitcoinCore::initialize()
{
    execute_restart = true;

    try {
        qDebug() << __func__ << ": Running AppInit2 in thread";
        if (!AppInitBasicSetup()) {
            Q_EMIT initializeResult(false);
            return;
        }
        if (!AppInitParameterInteraction()) {
            Q_EMIT initializeResult(false);
            return;
        }
        if (!AppInitSanityChecks()) {
            Q_EMIT initializeResult(false);
            return;
        }
        int rv = AppInitMain();
        Q_EMIT initializeResult(rv);
    } catch (const std::exception& e) {
        handleRunawayException(&e);
    } catch (...) {
        handleRunawayException(NULL);
    }
}

void BitcoinCore::restart(QStringList args)
{
    if (execute_restart) { // Only restart 1x, no matter how often a user clicks on a restart-button
        execute_restart = false;
        try {
            qDebug() << __func__ << ": Running Restart in thread";
            Interrupt();
            PrepareShutdown();
            qDebug() << __func__ << ": Shutdown finished";
            Q_EMIT shutdownResult(1);
            CExplicitNetCleanup::callCleanup();
            QProcess::startDetached(QApplication::applicationFilePath(), args);
            qDebug() << __func__ << ": Restart initiated...";
            QApplication::quit();
        } catch (const std::exception& e) {
            handleRunawayException(&e);
        } catch (...) {
            handleRunawayException(NULL);
        }
    }
}

void BitcoinCore::shutdown()
{
    try {
        qDebug() << __func__ << ": Running Shutdown in thread";
        Interrupt();
        Shutdown();
        qDebug() << __func__ << ": Shutdown finished";
        Q_EMIT shutdownResult(1);
    } catch (const std::exception& e) {
        handleRunawayException(&e);
    } catch (...) {
        handleRunawayException(NULL);
    }
}

BitcoinApplication::BitcoinApplication(int& argc, char** argv) : QApplication(argc, argv),
                                                                 coreThread(0),
                                                                 optionsModel(0),
                                                                 clientModel(0),
                                                                 window(0),
                                                                 pollShutdownTimer(0),
#ifdef ENABLE_WALLET
                                                                 paymentServer(0),
                                                                 walletModel(0),
#endif
                                                                 returnValue(0)
{
    setQuitOnLastWindowClosed(false);
}

BitcoinApplication::~BitcoinApplication()
{
    if (coreThread) {
        qDebug() << __func__ << ": Stopping thread";
        Q_EMIT stopThread();
        coreThread->wait();
        qDebug() << __func__ << ": Stopped thread";
    }

    delete window;
    window = 0;
#ifdef ENABLE_WALLET
    delete paymentServer;
    paymentServer = 0;
#endif
    // Delete Qt-settings if user clicked on "Reset Options"
    QSettings settings;
    if (optionsModel && optionsModel->resetSettings) {
        settings.clear();
        settings.sync();
    }
    delete optionsModel;
    optionsModel = 0;
}

#ifdef ENABLE_WALLET
void BitcoinApplication::createPaymentServer()
{
    paymentServer = new PaymentServer(this);
}
#endif

void BitcoinApplication::createOptionsModel()
{
    optionsModel = new OptionsModel();
}

void BitcoinApplication::createWindow(const NetworkStyle* networkStyle)
{
    window = new PIVXGUI(networkStyle, 0);

    pollShutdownTimer = new QTimer(window);
    connect(pollShutdownTimer, &QTimer::timeout, window, &PIVXGUI::detectShutdown);
}

void BitcoinApplication::createSplashScreen(const NetworkStyle* networkStyle)
{
    Splash* splash = new Splash(networkStyle);
    // We don't hold a direct pointer to the splash screen after creation, so use
    // Qt::WA_DeleteOnClose to make sure that the window will be deleted eventually.
    splash->setAttribute(Qt::WA_DeleteOnClose);
    splash->show();
    connect(this, &BitcoinApplication::splashFinished, splash, &Splash::slotFinish);
    connect(this, &BitcoinApplication::requestedShutdown, splash, &QWidget::close);
}

bool BitcoinApplication::createTutorialScreen()
{
    WelcomeContentWidget* widget = new WelcomeContentWidget();

    connect(widget, &WelcomeContentWidget::onLanguageSelected, [this](){
        updateTranslation(true);
    });

    widget->exec();
    bool ret = widget->isOk;
    widget->deleteLater();
    return ret;
}

void BitcoinApplication::updateTranslation(bool forceLangFromSettings){
    // Re-initialize translations after change them
    initTranslations(this->qtTranslatorBase, this->qtTranslator, this->translatorBase, this->translator, forceLangFromSettings);
}

void BitcoinApplication::startThread()
{
    if (coreThread)
        return;
    coreThread = new QThread(this);
    BitcoinCore* executor = new BitcoinCore();
    executor->moveToThread(coreThread);

    /*  communication to and from thread */
    connect(executor, &BitcoinCore::initializeResult, this, &BitcoinApplication::initializeResult);
    connect(executor, &BitcoinCore::shutdownResult, this, &BitcoinApplication::shutdownResult);
    connect(executor, &BitcoinCore::runawayException, this, &BitcoinApplication::handleRunawayException);
    connect(this, &BitcoinApplication::requestedInitialize, executor, &BitcoinCore::initialize);
    connect(this, &BitcoinApplication::requestedShutdown, executor, &BitcoinCore::shutdown);
    connect(window, &PIVXGUI::requestedRestart, executor, &BitcoinCore::restart);
    /*  make sure executor object is deleted in its own thread */
    connect(this, &BitcoinApplication::stopThread, executor, &QObject::deleteLater);
    connect(this, &BitcoinApplication::stopThread, coreThread, &QThread::quit);

    coreThread->start();
}

void BitcoinApplication::parameterSetup()
{
    // Default printtoconsole to false for the GUI. GUI programs should not
    // print to the console unnecessarily.
    gArgs.SoftSetBoolArg("-printtoconsole", false);

    InitLogging();
    InitParameterInteraction();
}

void BitcoinApplication::requestInitialize()
{
    qDebug() << __func__ << ": Requesting initialize";
    startThread();
    Q_EMIT requestedInitialize();
}

void BitcoinApplication::requestShutdown()
{
    qDebug() << __func__ << ": Requesting shutdown";
    startThread();
    window->hide();
    if (walletModel) walletModel->stop();
    window->setClientModel(nullptr);
    pollShutdownTimer->stop();

#ifdef ENABLE_WALLET
    window->removeAllWallets();
    delete walletModel;
    walletModel = nullptr;
#endif
    delete clientModel;
    clientModel = nullptr;

    // Show a simple window indicating shutdown status
    ShutdownWindow::showShutdownWindow(window);

    StartShutdown();

    // Request shutdown from core thread
    Q_EMIT requestedShutdown();
}

void BitcoinApplication::initializeResult(int retval)
{
    qDebug() << __func__ << ": Initialization result: " << retval;
    // Set exit result: 0 if successful, 1 if failure
    returnValue = retval ? 0 : 1;
    if (retval) {
#ifdef ENABLE_WALLET
        paymentServer->setOptionsModel(optionsModel);
#endif

        clientModel = new ClientModel(optionsModel);
        window->setClientModel(clientModel);

#ifdef ENABLE_WALLET
        mnModel = new MNModel(this);
        govModel = new GovernanceModel(clientModel, mnModel);
        // TODO: Expose secondary wallets
        if (!vpwallets.empty()) {
            walletModel = new WalletModel(vpwallets[0], optionsModel);
            walletModel->setClientModel(clientModel);
            mnModel->setWalletModel(walletModel);
            govModel->setWalletModel(walletModel);
            walletModel->init();
            mnModel->init();

            window->setGovModel(govModel);
            window->addWallet(PIVXGUI::DEFAULT_WALLET, walletModel);
            window->setCurrentWallet(PIVXGUI::DEFAULT_WALLET);
            window->setMNModel(mnModel);
        }
#endif

        // If -min option passed, start window minimized.
        if (gArgs.GetBoolArg("-min", false)) {
            window->showMinimized();
        } else {
            window->show();
        }
        Q_EMIT splashFinished(window);

#ifdef ENABLE_WALLET
        // Now that initialization/startup is done, process any command-line
        // PIVX: URIs or payment requests:
        //connect(paymentServer, &PaymentServer::receivedPaymentRequest, window, &PIVXGUI::handlePaymentRequest);
        connect(window, &PIVXGUI::receivedURI, paymentServer, &PaymentServer::handleURIOrFile);
        connect(paymentServer, &PaymentServer::message, [this](const QString& title, const QString& message, unsigned int style) {
          window->message(title, message, style);
        });
        QTimer::singleShot(100, paymentServer, &PaymentServer::uiReady);
#endif
        pollShutdownTimer->start(200);
    } else {
        quit(); // Exit main loop
    }
}

void BitcoinApplication::shutdownResult(int retval)
{
    qDebug() << __func__ << ": Shutdown result: " << retval;
    quit(); // Exit main loop after shutdown finished
}

void BitcoinApplication::handleRunawayException(const QString& message)
{
    QMessageBox::critical(0, "Runaway exception", QObject::tr("A fatal error occurred. PIVX can no longer continue safely and will quit.") + QString("\n\n") + message);
    ::exit(1);
}

WId BitcoinApplication::getMainWinId() const
{
    if (!window)
        return 0;

    return window->winId();
}

#ifndef BITCOIN_QT_TEST
int main(int argc, char* argv[])
{
    SetupEnvironment();

    /// 1. Parse command-line options. These take precedence over anything else.
    // Command-line options take precedence:
    gArgs.ParseParameters(argc, argv);

// Do not refer to data directory yet, this can be overridden by Intro::pickDataDirectory

/// 2. Basic Qt initialization (not dependent on parameters or configuration)
    Q_INIT_RESOURCE(pivx_locale);
    Q_INIT_RESOURCE(pivx);

    // Generate high-dpi pixmaps
    QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
#if QT_VERSION >= 0x050600
    QGuiApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
#ifdef Q_OS_MAC
    QApplication::setAttribute(Qt::AA_DontShowIconsInMenus);
#endif
    BitcoinApplication app(argc, argv);

    // Register meta types used for QMetaObject::invokeMethod
    qRegisterMetaType<bool*>();
    //   Need to pass name here as CAmount is a typedef (see http://qt-project.org/doc/qt-5/qmetatype.html#qRegisterMetaType)
    //   IMPORTANT if it is no longer a typedef use the normal variant above
    qRegisterMetaType<CAmount>("CAmount");
    qRegisterMetaType<CAmount>("interfaces::WalletBalances");
    qRegisterMetaType<size_t>("size_t");

    /// 3. Application identification
    // must be set before OptionsModel is initialized or translations are loaded,
    // as it is used to locate QSettings
    QApplication::setOrganizationName(QAPP_ORG_NAME);
    QApplication::setOrganizationDomain(QAPP_ORG_DOMAIN);
    QApplication::setApplicationName(QAPP_APP_NAME_DEFAULT);

    /// 4. Initialization of translations, so that intro dialog is in user's language
    // Now that QSettings are accessible, initialize translations
    //initTranslations(qtTranslatorBase, qtTranslator, translatorBase, translator);
    app.updateTranslation();
    translationInterface.Translate.connect(Translate);

    // Show help message immediately after parsing command-line options (for "-lang") and setting locale,
    // but before showing splash screen.
    if (gArgs.IsArgSet("-?") || gArgs.IsArgSet("-h") || gArgs.IsArgSet("-help") || gArgs.IsArgSet("-version")) {
        HelpMessageDialog help(nullptr, gArgs.IsArgSet("-version"));
        help.showOrPrint();
        return 1;
    }

    /// 5. Now that settings and translations are available, ask user for data directory
    // User language is set up: pick a data directory
    if (!Intro::pickDataDirectory())
        return 0;

    /// 6. Determine availability of data and blocks directory and parse pivx.conf
    /// - Do not call GetDataDir(true) before this step finishes
    if (!fs::is_directory(GetDataDir(false))) {
        QMessageBox::critical(0, QObject::tr("PIVX Core"),
            QObject::tr("Error: Specified data directory \"%1\" does not exist.").arg(QString::fromStdString(gArgs.GetArg("-datadir", ""))));
        return 1;
    }
    try {
        gArgs.ReadConfigFile(gArgs.GetArg("-conf", PIVX_CONF_FILENAME));
    } catch (const std::exception& e) {
        QMessageBox::critical(0, QObject::tr("PIVX Core"),
            QObject::tr("Error: Cannot parse configuration file: %1. Only use key=value syntax.").arg(e.what()));
        return 0;
    }

    /// 7. Determine network (and switch to network specific options)
    // - Do not call Params() before this step
    // - Do this after parsing the configuration file, as the network can be switched there
    // - QSettings() will use the new application name after this, resulting in network-specific settings
    // - Needs to be done before createOptionsModel

    // Check for -testnet or -regtest parameter (Params() calls are only valid after this clause)
    try {
        SelectParams(gArgs.GetChainName());
    } catch(const std::exception& e) {
        QMessageBox::critical(0, QObject::tr("PIVX Core"), QObject::tr("Error: %1").arg(e.what()));
        return 1;
    }
#ifdef ENABLE_WALLET
    // Parse URIs on command line -- this can affect Params()
    PaymentServer::ipcParseCommandLine(argc, argv);
#endif

    QScopedPointer<const NetworkStyle> networkStyle(NetworkStyle::instantiate(QString::fromStdString(Params().NetworkIDString())));
    assert(!networkStyle.isNull());
    // Allow for separate UI settings for testnets
    QApplication::setApplicationName(networkStyle->getAppName());
    // Re-initialize translations after changing application name (language in network-specific settings can be different)
    app.updateTranslation();

#ifdef ENABLE_WALLET
    /// 7a. parse masternode.conf
    std::string strErr;
    if (!masternodeConfig.read(strErr)) {
        QMessageBox::critical(0, QObject::tr("PIVX Core"),
            QObject::tr("Error reading masternode configuration file: %1").arg(strErr.c_str()));
        return 0;
    }

    /// 8. URI IPC sending
    // - Do this early as we don't want to bother initializing if we are just calling IPC
    // - Do this *after* setting up the data directory, as the data directory hash is used in the name
    // of the server.
    // - Do this after creating app and setting up translations, so errors are
    // translated properly.
    if (PaymentServer::ipcSendCommandLine())
        exit(0);

    // Start up the payment server early, too, so impatient users that click on
    // pivx: links repeatedly have their payment requests routed to this process:
    app.createPaymentServer();
#endif

    /// 9. Main GUI initialization
    // Install global event filter that makes sure that long tooltips can be word-wrapped
    app.installEventFilter(new GUIUtil::ToolTipToRichTextFilter(TOOLTIP_WRAP_THRESHOLD, &app));
#if defined(Q_OS_WIN)
    // Install global event filter for processing Windows session related Windows messages (WM_QUERYENDSESSION and WM_ENDSESSION)
    qApp->installNativeEventFilter(new WinShutdownMonitor());
#endif
    // Install qDebug() message handler to route to debug.log
    qInstallMessageHandler(DebugMessageHandler);
    // Allow parameter interaction before we create the options model
    app.parameterSetup();
    // Load GUI settings from QSettings
    app.createOptionsModel();

    // Subscribe to global signals from core
    uiInterface.InitMessage.connect(InitMessage);

    bool ret = true;
#ifdef ENABLE_WALLET
    // Check if at least one wallet exists or needs to be created
    const std::string& strWalletFile = gArgs.GetArg("-wallet", "");
    auto opRes = VerifyWalletPath(strWalletFile);
    if (!opRes) throw std::runtime_error(opRes.getError());

    if (!fs::exists(fs::absolute(strWalletFile, GetWalletDir()))) {
        // wallet doesn't exist, popup tutorial screen.
        ret = app.createTutorialScreen();
    }
#endif
    if(!ret){
        // wallet not loaded.
        return 0;
    }

    if (gArgs.GetBoolArg("-splash", true) && !gArgs.GetBoolArg("-min", false))
        app.createSplashScreen(networkStyle.data());

    try {
        app.createWindow(networkStyle.data());
        app.requestInitialize();
#if defined(Q_OS_WIN)
        WinShutdownMonitor::registerShutdownBlockReason(QObject::tr("%1 didn't yet exit safely...").arg(QObject::tr(PACKAGE_NAME)), (HWND)app.getMainWinId());
#endif
        app.exec();
        app.requestShutdown();
        app.exec();
    } catch (const std::exception& e) {
        PrintExceptionContinue(&e, "Runaway exception");
        app.handleRunawayException(QString::fromStdString(GetWarnings("gui")));
    } catch (...) {
        PrintExceptionContinue(NULL, "Runaway exception");
        app.handleRunawayException(QString::fromStdString(GetWarnings("gui")));
    }
    return app.getReturnValue();
}
#endif // BITCOIN_QT_TEST
