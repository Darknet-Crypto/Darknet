// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "rpcconsole.h"
#include "ui_rpcconsole.h"

#include "bantablemodel.h"
#include "clientmodel.h"
#include "peertablemodel.h"
#include "qt/rpcexecutor.h"

#include "chainparams.h"
#include "netbase.h"
#include "util/system.h"
#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif // ENABLE_WALLET

#ifdef ENABLE_WALLET
#include <db_cxx.h>
#endif

#include <QDir>
#include <QKeyEvent>
#include <QMenu>
#include <QScrollBar>
#include <QThread>
#include <QTime>
#include <QStringList>

// TODO: add a scrollback limit, as there is currently none
// TODO: make it possible to filter out categories (esp debug messages when implemented)
// TODO: receive errors and debug messages through ClientModel

const int CONSOLE_HISTORY = 50;
const QSize ICON_SIZE(24, 24);

const int INITIAL_TRAFFIC_GRAPH_MINS = 30;

// Repair parameters
const QString SALVAGEWALLET("-salvagewallet");
const QString RESCAN("-rescan");
const QString ZAPTXES1("-zapwallettxes=1");
const QString ZAPTXES2("-zapwallettxes=2");
const QString UPGRADEWALLET("-upgradewallet");
const QString REINDEX("-reindex");
const QString RESYNC("-resync");

const struct {
    const char* url;
    const char* source;
} ICON_MAPPING[] = {
    {"cmd-request", ":/icons/tx_input"},
    {"cmd-reply", ":/icons/tx_output"},
    {"cmd-error", ":/icons/tx_output"},
    {"misc", ":/icons/tx_inout"},
    {NULL, NULL}};

RPCConsole::RPCConsole(QWidget* parent) : QDialog(parent, Qt::WindowSystemMenuHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint),
                                          ui(new Ui::RPCConsole),
                                          clientModel(0),
                                          historyPtr(0),
                                          cachedNodeid(-1),
                                          peersTableContextMenu(0),
                                          banTableContextMenu(0)
{
    ui->setupUi(this);
    GUIUtil::restoreWindowGeometry("nRPCConsoleWindow", this->size(), this);

#ifndef Q_OS_MAC
    ui->openDebugLogfileButton->setIcon(QIcon(":/icons/export"));
#endif

    // Install event filter for up and down arrow
    ui->lineEdit->installEventFilter(this);
    ui->messagesWidget->installEventFilter(this);

    connect(ui->clearButton, &QPushButton::clicked, this, &RPCConsole::clear);
    connect(ui->btnClearTrafficGraph, &QPushButton::clicked, ui->trafficGraph, &TrafficGraphWidget::clear);

    // Wallet Repair Buttons
    connect(ui->btn_salvagewallet, &QPushButton::clicked, this, &RPCConsole::walletSalvage);
    connect(ui->btn_rescan, &QPushButton::clicked, this, &RPCConsole::walletRescan);
    connect(ui->btn_zapwallettxes1, &QPushButton::clicked, this, &RPCConsole::walletZaptxes1);
    connect(ui->btn_zapwallettxes2, &QPushButton::clicked, this, &RPCConsole::walletZaptxes2);
    connect(ui->btn_upgradewallet, &QPushButton::clicked, this, &RPCConsole::walletUpgrade);
    connect(ui->btn_reindex, &QPushButton::clicked, this, &RPCConsole::walletReindex);
    connect(ui->btn_resync, &QPushButton::clicked, this, &RPCConsole::walletResync);

    // set library version labels
#ifdef ENABLE_WALLET
    std::string strPathCustom = gArgs.GetArg("-backuppath", "");
    int nCustomBackupThreshold = gArgs.GetArg("-custombackupthreshold", DEFAULT_CUSTOMBACKUPTHRESHOLD);

    if(!strPathCustom.empty()) {
        ui->wallet_custombackuppath->setText(QString::fromStdString(strPathCustom));
        ui->wallet_custombackuppath_label->show();
        ui->wallet_custombackuppath->show();
        if (nCustomBackupThreshold > 0) {
            ui->wallet_custombackupthreshold->setText(QString::fromStdString(std::to_string(nCustomBackupThreshold)));
            ui->wallet_custombackupthreshold_label->setVisible(true);
            ui->wallet_custombackupthreshold->setVisible(true);
        }
    }

    ui->berkeleyDBVersion->setText(DbEnv::version(0, 0, 0));
    ui->wallet_path->setText(QString::fromStdString(GetDataDir().string() + QDir::separator().toLatin1() + gArgs.GetArg("-wallet", DEFAULT_WALLET_DAT)));
#else

    ui->label_berkeleyDBVersion->hide();
    ui->berkeleyDBVersion->hide();
#endif
    // Register RPC timer interface
    rpcTimerInterface = new QtRPCTimerInterface();
    // avoid accidentally overwriting an existing, non QTThread
    // based timer interface
    RPCSetTimerInterfaceIfUnset(rpcTimerInterface);

    startExecutor();
    setTrafficGraphRange(INITIAL_TRAFFIC_GRAPH_MINS);

    ui->peerHeading->setText(tr("Select a peer to view detailed information."));

    clear();
}

RPCConsole::~RPCConsole()
{
    GUIUtil::saveWindowGeometry("nRPCConsoleWindow", this);
    Q_EMIT stopExecutor();
    RPCUnsetTimerInterface(rpcTimerInterface);
    delete rpcTimerInterface;
    delete ui;
}

bool RPCConsole::eventFilter(QObject* obj, QEvent* event)
{
    if (event->type() == QEvent::KeyPress) // Special key handling
    {
        QKeyEvent* keyevt = static_cast<QKeyEvent*>(event);
        int key = keyevt->key();
        Qt::KeyboardModifiers mod = keyevt->modifiers();
        switch (key) {
        case Qt::Key_Up:
            if (obj == ui->lineEdit) {
                browseHistory(-1);
                return true;
            }
            break;
        case Qt::Key_Down:
            if (obj == ui->lineEdit) {
                browseHistory(1);
                return true;
            }
            break;
        case Qt::Key_PageUp: /* pass paging keys to messages widget */
        case Qt::Key_PageDown:
            if (obj == ui->lineEdit) {
                QApplication::postEvent(ui->messagesWidget, new QKeyEvent(*keyevt));
                return true;
            }
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            // forward these events to lineEdit
            if(obj == autoCompleter->popup()) {
                QApplication::postEvent(ui->lineEdit, new QKeyEvent(*keyevt));
                return true;
            }
            break;
        default:
            // Typing in messages widget brings focus to line edit, and redirects key there
            // Exclude most combinations and keys that emit no text, except paste shortcuts
            if (obj == ui->messagesWidget && ((!mod && !keyevt->text().isEmpty() && key != Qt::Key_Tab) ||
                                                 ((mod & Qt::ControlModifier) && key == Qt::Key_V) ||
                                                 ((mod & Qt::ShiftModifier) && key == Qt::Key_Insert))) {
                ui->lineEdit->setFocus();
                QApplication::postEvent(ui->lineEdit, new QKeyEvent(*keyevt));
                return true;
            }
        }
    }
    return QDialog::eventFilter(obj, event);
}

void RPCConsole::setClientModel(ClientModel* model)
{
    clientModel = model;
    ui->trafficGraph->setClientModel(model);
    if (model && clientModel->getPeerTableModel() && clientModel->getBanTableModel()) {
        // Keep up to date with client
        setNumConnections(model->getNumConnections());
        connect(model, &ClientModel::numConnectionsChanged, this, &RPCConsole::setNumConnections);

        setNumBlocks(model->getNumBlocks());
        connect(model, &ClientModel::numBlocksChanged, this, &RPCConsole::setNumBlocks);

        connect(model, &ClientModel::strMasternodesChanged, this, &RPCConsole::setMasternodeCount);

        updateTrafficStats(model->getTotalBytesRecv(), model->getTotalBytesSent());
        connect(model, &ClientModel::bytesChanged, this, &RPCConsole::updateTrafficStats);

        // set up peer table
        ui->peerWidget->setModel(model->getPeerTableModel());
        ui->peerWidget->verticalHeader()->hide();
        ui->peerWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
        ui->peerWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
        ui->peerWidget->setSelectionMode(QAbstractItemView::SingleSelection);
        ui->peerWidget->setContextMenuPolicy(Qt::CustomContextMenu);
        ui->peerWidget->setColumnWidth(PeerTableModel::Address, ADDRESS_COLUMN_WIDTH);
        ui->peerWidget->setColumnWidth(PeerTableModel::Subversion, SUBVERSION_COLUMN_WIDTH);
        ui->peerWidget->setColumnWidth(PeerTableModel::Ping, PING_COLUMN_WIDTH);
        ui->peerWidget->horizontalHeader()->setStretchLastSection(true);

        // create peer table context menu actions
        QAction* disconnectAction = new QAction(tr("&Disconnect Node"), this);
        QAction* banAction1h      = new QAction(tr("Ban Node for") + " " + tr("1 &hour"), this);
        QAction* banAction24h     = new QAction(tr("Ban Node for") + " " + tr("1 &day"), this);
        QAction* banAction7d      = new QAction(tr("Ban Node for") + " " + tr("1 &week"), this);
        QAction* banAction365d    = new QAction(tr("Ban Node for") + " " + tr("1 &year"), this);

        // create peer table context menu
        peersTableContextMenu = new QMenu();
        peersTableContextMenu->addAction(disconnectAction);
        peersTableContextMenu->addAction(banAction1h);
        peersTableContextMenu->addAction(banAction24h);
        peersTableContextMenu->addAction(banAction7d);
        peersTableContextMenu->addAction(banAction365d);

        connect(banAction1h, &QAction::triggered, [this] { banSelectedNode(60 * 60); });
        connect(banAction24h, &QAction::triggered, [this] { banSelectedNode(60 * 60 * 24); });
        connect(banAction7d, &QAction::triggered, [this] { banSelectedNode(60 * 60 * 24 * 7); });
        connect(banAction365d, &QAction::triggered, [this] { banSelectedNode(60 * 60 * 24 * 365); });

        // peer table context menu signals
        connect(ui->peerWidget, &QTableView::customContextMenuRequested, this, &RPCConsole::showPeersTableContextMenu);
        connect(disconnectAction, &QAction::triggered, this, &RPCConsole::disconnectSelectedNode);

        // peer table signal handling - update peer details when selecting new node
        connect(ui->peerWidget->selectionModel(), &QItemSelectionModel::selectionChanged, this, &RPCConsole::peerSelected);
        // peer table signal handling - update peer details when new nodes are added to the model
        connect(model->getPeerTableModel(), &PeerTableModel::layoutChanged, this, &RPCConsole::peerLayoutChanged);

        // set up ban table
        ui->banlistWidget->setModel(model->getBanTableModel());
        ui->banlistWidget->verticalHeader()->hide();
        ui->banlistWidget->setEditTriggers(QAbstractItemView::NoEditTriggers);
        ui->banlistWidget->setSelectionBehavior(QAbstractItemView::SelectRows);
        ui->banlistWidget->setSelectionMode(QAbstractItemView::SingleSelection);
        ui->banlistWidget->setContextMenuPolicy(Qt::CustomContextMenu);
        ui->banlistWidget->setColumnWidth(BanTableModel::Address, BANSUBNET_COLUMN_WIDTH);
        ui->banlistWidget->setColumnWidth(BanTableModel::Bantime, BANTIME_COLUMN_WIDTH);
        ui->banlistWidget->horizontalHeader()->setStretchLastSection(true);

        // create ban table context menu action
        QAction* unbanAction = new QAction(tr("&Unban Node"), this);

        // create ban table context menu
        banTableContextMenu = new QMenu();
        banTableContextMenu->addAction(unbanAction);

        // ban table context menu signals
        connect(ui->banlistWidget, &QTableView::customContextMenuRequested, this, &RPCConsole::showBanTableContextMenu);
        connect(unbanAction, &QAction::triggered, this, &RPCConsole::unbanSelectedNode);

        // ban table signal handling - clear peer details when clicking a peer in the ban table
        connect(ui->banlistWidget, &QTableView::clicked, this, &RPCConsole::clearSelectedNode);
        // ban table signal handling - ensure ban table is shown or hidden (if empty)
        connect(model->getBanTableModel(), &BanTableModel::layoutChanged, this, &RPCConsole::showOrHideBanTableIfRequired);
        showOrHideBanTableIfRequired();

        // Provide initial values
        ui->clientVersion->setText(model->formatFullVersion());
        ui->clientName->setText(model->clientName());
        ui->buildDate->setText(model->formatBuildDate());
        ui->dataDir->setText(model->dataDir());
        ui->startupTime->setText(model->formatClientStartupTime());
        ui->networkName->setText(QString::fromStdString(Params().NetworkIDString()));

        //Setup autocomplete and attach it
        QStringList wordList;
        std::vector<std::string> commandList = tableRPC.listCommands();
        for (size_t i = 0; i < commandList.size(); ++i)
        {
            wordList << commandList[i].c_str();
            wordList << ("help " + commandList[i]).c_str();
        }

        wordList << "help-console";
        wordList.sort();
        autoCompleter = new QCompleter(wordList, this);
        autoCompleter->setModelSorting(QCompleter::CaseSensitivelySortedModel);
        ui->lineEdit->setCompleter(autoCompleter);

        // clear the lineEdit after activating from QCompleter
        autoCompleter->popup()->installEventFilter(this);
    }
}

/** Restart wallet with "-salvagewallet" */
void RPCConsole::walletSalvage()
{
    buildParameterlist(SALVAGEWALLET);
}

/** Restart wallet with "-rescan" */
void RPCConsole::walletRescan()
{
    buildParameterlist(RESCAN);
}

/** Restart wallet with "-zapwallettxes=1" */
void RPCConsole::walletZaptxes1()
{
    buildParameterlist(ZAPTXES1);
}

/** Restart wallet with "-zapwallettxes=2" */
void RPCConsole::walletZaptxes2()
{
    buildParameterlist(ZAPTXES2);
}

/** Restart wallet with "-upgradewallet" */
void RPCConsole::walletUpgrade()
{
    buildParameterlist(UPGRADEWALLET);
}

/** Restart wallet with "-reindex" */
void RPCConsole::walletReindex()
{
    buildParameterlist(REINDEX);
}

/** Restart wallet with "-resync" */
void RPCConsole::walletResync()
{
    QString resyncWarning = tr("This will delete your local blockchain folders and the wallet will synchronize the complete Blockchain from scratch.<br /><br />");
        resyncWarning +=   tr("This needs quite some time and downloads a lot of data.<br /><br />");
        resyncWarning +=   tr("Your transactions and funds will be visible again after the download has completed.<br /><br />");
        resyncWarning +=   tr("Do you want to continue?.<br />");
    QMessageBox::StandardButton retval = QMessageBox::question(this, tr("Confirm resync Blockchain"),
        resyncWarning,
        QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);

    if (retval != QMessageBox::Yes) {
        // Resync canceled
        return;
    }

    // Restart and resync
    buildParameterlist(RESYNC);
}

/** Build command-line parameter list for restart */
void RPCConsole::buildParameterlist(QString arg)
{
    // Get command-line arguments and remove the application name
    QStringList args = QApplication::arguments();
    args.removeFirst();

    // Remove existing repair-options
    args.removeAll(SALVAGEWALLET);
    args.removeAll(RESCAN);
    args.removeAll(ZAPTXES1);
    args.removeAll(ZAPTXES2);
    args.removeAll(UPGRADEWALLET);
    args.removeAll(REINDEX);

    // Append repair parameter to command line.
    args.append(arg);

    // Send command-line arguments to BitcoinGUI::handleRestart()
    Q_EMIT handleRestart(args);
}

void RPCConsole::clear()
{
    ui->messagesWidget->clear();
    history.clear();
    historyPtr = 0;
    ui->lineEdit->clear();
    ui->lineEdit->setFocus();

    // Add smoothly scaled icon images.
    // (when using width/height on an img, Qt uses nearest instead of linear interpolation)
    for (int i = 0; ICON_MAPPING[i].url; ++i) {
        ui->messagesWidget->document()->addResource(
            QTextDocument::ImageResource,
            QUrl(ICON_MAPPING[i].url),
            QImage(ICON_MAPPING[i].source).scaled(ICON_SIZE, Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    }

    // Set default style sheet
    ui->messagesWidget->document()->setDefaultStyleSheet(
        "table { }"
        "td.time { color: #808080; padding-top: 3px; } "
        "td.message { font-family: Courier, Courier New, Lucida Console, monospace; font-size: 12px; } " // Todo: Remove fixed font-size
        "td.cmd-request { color: #006060; } "
        "td.cmd-error { color: red; } "
        ".secwarning { color: red; }"
        "b { color: #006060; } ");

#ifdef Q_OS_MAC
    QString clsKey = "(⌘)-L";
#else
    QString clsKey = "Ctrl-L";
#endif

    message(RPCExecutor::CMD_REPLY, (tr("Welcome to the PIVX RPC console.") + "<br>" +
                        tr("Use up and down arrows to navigate history, and %1 to clear screen.").arg("<b>"+clsKey+"</b>") + "<br>" +
                        tr("Type %1 for an overview of available commands.").arg("<b>help</b>") + "<br>" +
                        tr("For more information on using this console type %1.").arg("<b>help-console</b>") +
                        "<br><span class=\"secwarning\"><br>" +
                        tr("WARNING: Scammers have been active, telling users to type commands here, stealing their wallet contents. Do not use this console without fully understanding the ramifications of a command.") +
                        "</span>"),
                        true);
}

void RPCConsole::reject()
{
    // Ignore escape keypress if this is not a seperate window
    if (windowType() != Qt::Widget)
        QDialog::reject();
}

void RPCConsole::message(int category, const QString& message, bool html)
{
    QTime time = QTime::currentTime();
    QString timeString = time.toString();
    QString out;
    out += "<table><tr><td class=\"time\" width=\"65\">" + timeString + "</td>";
    out += "<td class=\"icon\" width=\"32\"><img src=\"" + RPCExecutor::categoryClass(category) + "\"></td>";
    out += "<td class=\"message " + RPCExecutor::categoryClass(category) + "\" valign=\"middle\">";
    if (html)
        out += message;
    else
        out += GUIUtil::HtmlEscape(message, true);
    out += "</td></tr></table>";
    ui->messagesWidget->append(out);
}

void RPCConsole::setNumConnections(int count)
{
    if (!clientModel)
        return;

    QString connections = QString::number(count) + " (";
    connections += tr("In:") + " " + QString::number(clientModel->getNumConnections(CONNECTIONS_IN)) + " / ";
    connections += tr("Out:") + " " + QString::number(clientModel->getNumConnections(CONNECTIONS_OUT)) + ")";

    ui->numberOfConnections->setText(connections);
}

void RPCConsole::setNumBlocks(int count)
{
    ui->numberOfBlocks->setText(QString::number(count));
    if (clientModel) {
        ui->lastBlockTime->setText(clientModel->getLastBlockDate().toString());
        ui->lastBlockHash->setText(clientModel->getLastBlockHash());
    }
}

void RPCConsole::setMasternodeCount(const QString& strMasternodes)
{
    ui->masternodeCount->setText(strMasternodes);
}

void RPCConsole::on_lineEdit_returnPressed()
{
    QString cmd = ui->lineEdit->text();
    ui->lineEdit->clear();

    if (!cmd.isEmpty()) {
        message(RPCExecutor::CMD_REQUEST, cmd);
        Q_EMIT cmdRequest(cmd);
        // Remove command, if already in history
        history.removeOne(cmd);
        // Append command to history
        history.append(cmd);
        // Enforce maximum history size
        while (history.size() > CONSOLE_HISTORY)
            history.removeFirst();
        // Set pointer to end of history
        historyPtr = history.size();
        // Scroll console view to end
        scrollToEnd();
    }
}

void RPCConsole::browseHistory(int offset)
{
    historyPtr += offset;
    if (historyPtr < 0)
        historyPtr = 0;
    if (historyPtr > history.size())
        historyPtr = history.size();
    QString cmd;
    if (historyPtr < history.size())
        cmd = history.at(historyPtr);
    ui->lineEdit->setText(cmd);
}

void RPCConsole::startExecutor()
{
    QThread* thread = new QThread;
    RPCExecutor* executor = new RPCExecutor();
    executor->moveToThread(thread);

    // Replies from executor object must go to this object
    connect(executor, &RPCExecutor::reply, this, static_cast<void (RPCConsole::*)(int, const QString&)>(&RPCConsole::message));
    // Requests from this object must go to executor
    connect(this, &RPCConsole::cmdRequest, executor, &RPCExecutor::request);

    // On stopExecutor signal
    // - queue executor for deletion (in execution thread)
    // - quit the Qt event loop in the execution thread
    connect(this, &RPCConsole::stopExecutor, executor, &RPCExecutor::deleteLater);
    connect(this, &RPCConsole::stopExecutor, thread, &QThread::quit);
    // Queue the thread for deletion (in this thread) when it is finished
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);

    // Default implementation of QThread::run() simply spins up an event loop in the thread,
    // which is what we want.
    thread->start();
}

void RPCConsole::on_tabWidget_currentChanged(int index)
{
    if (ui->tabWidget->widget(index) == ui->tab_console) {
        ui->lineEdit->setFocus();
    } else if (ui->tabWidget->widget(index) != ui->tab_peers) {
        clearSelectedNode();
    }
}

void RPCConsole::on_openDebugLogfileButton_clicked()
{
    GUIUtil::openDebugLogfile();
}

void RPCConsole::scrollToEnd()
{
    QScrollBar* scrollbar = ui->messagesWidget->verticalScrollBar();
    scrollbar->setValue(scrollbar->maximum());
}

void RPCConsole::on_sldGraphRange_valueChanged(int value)
{
    const int multiplier = 5; // each position on the slider represents 5 min
    int mins = value * multiplier;
    setTrafficGraphRange(mins);
}

QString RPCConsole::FormatBytes(quint64 bytes)
{
    if (bytes < 1024)
        return QString(tr("%1 B")).arg(bytes);
    if (bytes < 1024 * 1024)
        return QString(tr("%1 KB")).arg(bytes / 1024);
    if (bytes < 1024 * 1024 * 1024)
        return QString(tr("%1 MB")).arg(bytes / 1024 / 1024);

    return QString(tr("%1 GB")).arg(bytes / 1024 / 1024 / 1024);
}

void RPCConsole::setTrafficGraphRange(int mins)
{
    ui->trafficGraph->setGraphRangeMins(mins);
    ui->lblGraphRange->setText(GUIUtil::formatDurationStr(mins * 60));
}

void RPCConsole::updateTrafficStats(quint64 totalBytesIn, quint64 totalBytesOut)
{
    ui->lblBytesIn->setText(FormatBytes(totalBytesIn));
    ui->lblBytesOut->setText(FormatBytes(totalBytesOut));
}

void RPCConsole::showInfo()
{
    ui->tabWidget->setCurrentIndex(0);
    show();
}

void RPCConsole::showConsole()
{
    ui->tabWidget->setCurrentIndex(1);
    show();
}

void RPCConsole::showNetwork()
{
    ui->tabWidget->setCurrentIndex(2);
    show();
}

void RPCConsole::showPeers()
{
    ui->tabWidget->setCurrentIndex(3);
    show();
}

void RPCConsole::showRepair()
{
    ui->tabWidget->setCurrentIndex(4);
    show();
}

void RPCConsole::showConfEditor()
{
    GUIUtil::openConfigfile();
}

void RPCConsole::showMNConfEditor()
{
    GUIUtil::openMNConfigfile();
}

void RPCConsole::peerSelected(const QItemSelection& selected, const QItemSelection& deselected)
{
    Q_UNUSED(deselected);

    if (!clientModel || !clientModel->getPeerTableModel() || selected.indexes().isEmpty())
        return;

    const CNodeCombinedStats* stats = clientModel->getPeerTableModel()->getNodeStats(selected.indexes().first().row());
    if (stats)
        updateNodeDetail(stats);
}

void RPCConsole::peerLayoutChanged()
{
    if (!clientModel || !clientModel->getPeerTableModel())
        return;

    const CNodeCombinedStats* stats = NULL;
    bool fUnselect = false;
    bool fReselect = false;

    if (cachedNodeid == -1) // no node selected yet
        return;

    // find the currently selected row
    int selectedRow = -1;
    QModelIndexList selectedModelIndex = ui->peerWidget->selectionModel()->selectedIndexes();
    if (!selectedModelIndex.isEmpty()) {
        selectedRow = selectedModelIndex.first().row();
    }

    // check if our detail node has a row in the table (it may not necessarily
    // be at selectedRow since its position can change after a layout change)
    int detailNodeRow = clientModel->getPeerTableModel()->getRowByNodeId(cachedNodeid);

    if (detailNodeRow < 0) {
        // detail node dissapeared from table (node disconnected)
        fUnselect = true;
    } else {
        if (detailNodeRow != selectedRow) {
            // detail node moved position
            fUnselect = true;
            fReselect = true;
        }

        // get fresh stats on the detail node.
        stats = clientModel->getPeerTableModel()->getNodeStats(detailNodeRow);
    }

    if (fUnselect && selectedRow >= 0) {
        clearSelectedNode();
    }

    if (fReselect) {
        ui->peerWidget->selectRow(detailNodeRow);
    }

    if (stats)
        updateNodeDetail(stats);
}

void RPCConsole::updateNodeDetail(const CNodeCombinedStats* stats)
{
    // Update cached nodeid
    cachedNodeid = stats->nodeStats.nodeid;

    // update the detail ui with latest node information
    QString peerAddrDetails(QString::fromStdString(stats->nodeStats.addrName) + " ");
    peerAddrDetails += tr("(node id: %1)").arg(QString::number(stats->nodeStats.nodeid));
    if (!stats->nodeStats.addrLocal.empty())
        peerAddrDetails += "<br />" + tr("via %1").arg(QString::fromStdString(stats->nodeStats.addrLocal));
    ui->peerHeading->setText(peerAddrDetails);
    ui->peerServices->setText(GUIUtil::formatServicesStr(stats->nodeStats.nServices));
    ui->peerLastSend->setText(stats->nodeStats.nLastSend ? GUIUtil::formatDurationStr(GetSystemTimeInSeconds() - stats->nodeStats.nLastSend) : tr("never"));
    ui->peerLastRecv->setText(stats->nodeStats.nLastRecv ? GUIUtil::formatDurationStr(GetSystemTimeInSeconds() - stats->nodeStats.nLastRecv) : tr("never"));
    ui->peerBytesSent->setText(FormatBytes(stats->nodeStats.nSendBytes));
    ui->peerBytesRecv->setText(FormatBytes(stats->nodeStats.nRecvBytes));
    ui->peerConnTime->setText(GUIUtil::formatDurationStr(GetSystemTimeInSeconds() - stats->nodeStats.nTimeConnected));
    ui->peerPingTime->setText(GUIUtil::formatPingTime(stats->nodeStats.dPingTime));
    ui->peerPingWait->setText(GUIUtil::formatPingTime(stats->nodeStats.dPingWait));
    ui->timeoffset->setText(GUIUtil::formatTimeOffset(stats->nodeStats.nTimeOffset));
    ui->peerVersion->setText(QString("%1").arg(QString::number(stats->nodeStats.nVersion)));
    ui->peerSubversion->setText(QString::fromStdString(stats->nodeStats.cleanSubVer));
    ui->peerDirection->setText(stats->nodeStats.fInbound ? tr("Inbound") : tr("Outbound"));
    ui->peerHeight->setText(QString("%1").arg(QString::number(stats->nodeStats.nStartingHeight)));
    ui->peerWhitelisted->setText(stats->nodeStats.fWhitelisted ? tr("Yes") : tr("No"));

    // This check fails for example if the lock was busy and
    // nodeStateStats couldn't be fetched.
    if (stats->fNodeStateStatsAvailable) {
        // Ban score is init to 0
        ui->peerBanScore->setText(QString("%1").arg(stats->nodeStateStats.nMisbehavior));

        // Sync height is init to -1
        if (stats->nodeStateStats.nSyncHeight > -1)
            ui->peerSyncHeight->setText(QString("%1").arg(stats->nodeStateStats.nSyncHeight));
        else
            ui->peerSyncHeight->setText(tr("Unknown"));

        // Common height is init to -1
        if (stats->nodeStateStats.nCommonHeight > -1)
            ui->peerCommonHeight->setText(QString("%1").arg(stats->nodeStateStats.nCommonHeight));
        else
            ui->peerCommonHeight->setText(tr("Unknown"));
    }

    ui->detailWidget->show();
}

void RPCConsole::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
}

void RPCConsole::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    if (!clientModel || !clientModel->getPeerTableModel())
        return;

    // start PeerTableModel auto refresh
    clientModel->getPeerTableModel()->startAutoRefresh();
    clientModel->startMasternodesTimer();
}

void RPCConsole::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);

    if (!clientModel || !clientModel->getPeerTableModel())
        return;

    // stop PeerTableModel auto refresh
    clientModel->getPeerTableModel()->stopAutoRefresh();
    clientModel->stopMasternodesTimer();
}

void RPCConsole::showBackups()
{
    GUIUtil::showBackups();
}

void RPCConsole::showPeersTableContextMenu(const QPoint& point)
{
    QModelIndex index = ui->peerWidget->indexAt(point);
    if (index.isValid())
    peersTableContextMenu->exec(QCursor::pos());
}

void RPCConsole::showBanTableContextMenu(const QPoint& point)
{
    QModelIndex index = ui->banlistWidget->indexAt(point);
    if (index.isValid())
        banTableContextMenu->exec(QCursor::pos());
}

void RPCConsole::disconnectSelectedNode()
{
    if(!g_connman)
        return;
    // Get currently selected peer address
    NodeId id = GUIUtil::getEntryData(ui->peerWidget, 0, PeerTableModel::NetNodeId).toInt();
    // Find the node, disconnect it and clear the selected node
    if(g_connman->DisconnectNode(id))
        clearSelectedNode();
}

void RPCConsole::banSelectedNode(int bantime)
{
    if (!clientModel || !g_connman)
        return;

    // Get currently selected peer address
    QString strNode = GUIUtil::getEntryData(ui->peerWidget, 0, PeerTableModel::Address).toString();
    // Find possible nodes, ban it and clear the selected node
    std::string nStr = strNode.toStdString();
    std::string addr;
    int port = 0;
    SplitHostPort(nStr, port, addr);

    CNetAddr resolved;
    if (!LookupHost(addr.c_str(), resolved, false))
        return;
    g_connman->Ban(resolved, BanReasonManuallyAdded, bantime);
    clearSelectedNode();
    clientModel->getBanTableModel()->refresh();
}

void RPCConsole::unbanSelectedNode()
{
    if (!clientModel)
        return;

    // Get currently selected ban address
    QString strNode = GUIUtil::getEntryData(ui->banlistWidget, 0, BanTableModel::Address).toString();
    CSubNet possibleSubnet;

    LookupSubNet(strNode.toStdString().c_str(), possibleSubnet);
    if (possibleSubnet.IsValid() && g_connman)
    {
        g_connman->Unban(possibleSubnet);
        clientModel->getBanTableModel()->refresh();
    }
}

void RPCConsole::clearSelectedNode()
{
    ui->peerWidget->selectionModel()->clearSelection();
    cachedNodeid = -1;
    ui->detailWidget->hide();
    ui->peerHeading->setText(tr("Select a peer to view detailed information."));
}

void RPCConsole::showOrHideBanTableIfRequired()
{
    if (!clientModel)
        return;

    bool visible = clientModel->getBanTableModel()->shouldShow();
    ui->banlistWidget->setVisible(visible);
    ui->banHeading->setVisible(visible);
}

