// Copyright (c) 2019-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/masternodeswidget.h"
#include "qt/pivx/forms/ui_masternodeswidget.h"

#include "qt/pivx/qtutils.h"
#include "qt/pivx/mnrow.h"
#include "qt/pivx/mninfodialog.h"
#include "qt/pivx/masternodewizarddialog.h"

#include "activemasternode.h"
#include "clientmodel.h"
#include "fs.h"
#include "guiutil.h"
#include "init.h"
#include "masternode-sync.h"
#include "masternodeconfig.h"
#include "masternodeman.h"
#include "wallet/wallet.h"
#include "util/system.h"
#include "qt/pivx/mnmodel.h"
#include "qt/pivx/optionbutton.h"
#include <fstream>

#define DECORATION_SIZE 65
#define NUM_ITEMS 3
#define REQUEST_START_ALL 1
#define REQUEST_START_MISSING 2

class MNHolder : public FurListRow<QWidget*>
{
public:
    MNHolder();

    explicit MNHolder(bool _isLightTheme) : FurListRow(), isLightTheme(_isLightTheme) {}

    MNRow* createHolder(int pos) override
    {
        if (!cachedRow) cachedRow = new MNRow();
        return cachedRow;
    }

    void init(QWidget* holder,const QModelIndex &index, bool isHovered, bool isSelected) const override
    {
        MNRow* row = static_cast<MNRow*>(holder);
        QString label = index.data(Qt::DisplayRole).toString();
        QString address = index.sibling(index.row(), MNModel::ADDRESS).data(Qt::DisplayRole).toString();
        QString status = index.sibling(index.row(), MNModel::STATUS).data(Qt::DisplayRole).toString();
        bool wasCollateralAccepted = index.sibling(index.row(), MNModel::WAS_COLLATERAL_ACCEPTED).data(Qt::DisplayRole).toBool();
        row->updateView("Address: " + address, label, status, wasCollateralAccepted);
    }

    QColor rectColor(bool isHovered, bool isSelected) override
    {
        return getRowColor(isLightTheme, isHovered, isSelected);
    }

    ~MNHolder() override{}

    bool isLightTheme;
    MNRow* cachedRow = nullptr;
};

MasterNodesWidget::MasterNodesWidget(PIVXGUI *parent) :
    PWidget(parent),
    ui(new Ui::MasterNodesWidget),
    isLoading(false)
{
    ui->setupUi(this);

    delegate = new FurAbstractListItemDelegate(
            DECORATION_SIZE,
            new MNHolder(isLightTheme()),
            this
    );

    this->setStyleSheet(parent->styleSheet());

    /* Containers */
    setCssProperty(ui->left, "container");
    ui->left->setContentsMargins(0,20,0,20);
    setCssProperty(ui->right, "container-right");
    ui->right->setContentsMargins(20,20,20,20);

    /* Light Font */
    QFont fontLight;
    fontLight.setWeight(QFont::Light);

    /* Title */
    setCssTitleScreen(ui->labelTitle);
    ui->labelTitle->setFont(fontLight);
    setCssSubtitleScreen(ui->labelSubtitle1);

    /* Buttons */
    setCssBtnPrimary(ui->pushButtonSave);
    setCssBtnPrimary(ui->pushButtonStartAll);
    setCssBtnPrimary(ui->pushButtonStartMissing);

    /* Options */
    ui->btnAbout->setTitleClassAndText("btn-title-grey", tr("What is a Masternode?"));
    ui->btnAbout->setSubTitleClassAndText("text-subtitle", tr("FAQ explaining what Masternodes are"));
    ui->btnAboutController->setTitleClassAndText("btn-title-grey", tr("What is a Controller?"));
    ui->btnAboutController->setSubTitleClassAndText("text-subtitle", tr("FAQ explaining what is a Masternode Controller"));

    setCssProperty(ui->listMn, "container");
    ui->listMn->setItemDelegate(delegate);
    ui->listMn->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listMn->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listMn->setAttribute(Qt::WA_MacShowFocusRect, false);
    ui->listMn->setSelectionBehavior(QAbstractItemView::SelectRows);

    ui->emptyContainer->setVisible(false);
    setCssProperty(ui->pushImgEmpty, "img-empty-master");
    setCssProperty(ui->labelEmpty, "text-empty");

    connect(ui->pushButtonSave, &QPushButton::clicked, this, &MasterNodesWidget::onCreateMNClicked);
    connect(ui->pushButtonStartAll, &QPushButton::clicked, [this]() {
        onStartAllClicked(REQUEST_START_ALL);
    });
    connect(ui->pushButtonStartMissing, &QPushButton::clicked, [this]() {
        onStartAllClicked(REQUEST_START_MISSING);
    });
    connect(ui->listMn, &QListView::clicked, this, &MasterNodesWidget::onMNClicked);
    connect(ui->btnAbout, &OptionButton::clicked, [this](){window->openFAQ(SettingsFaqWidget::Section::MASTERNODE);});
    connect(ui->btnAboutController, &OptionButton::clicked, [this](){window->openFAQ(SettingsFaqWidget::Section::MNCONTROLLER);});
}

void MasterNodesWidget::showEvent(QShowEvent *event)
{
    if (mnModel) mnModel->updateMNList();
    if (!timer) {
        timer = new QTimer(this);
        connect(timer, &QTimer::timeout, [this]() {mnModel->updateMNList();});
    }
    timer->start(30000);
}

void MasterNodesWidget::hideEvent(QHideEvent *event)
{
    if (timer) timer->stop();
}

void MasterNodesWidget::setMNModel(MNModel* _mnModel)
{
    mnModel = _mnModel;
    ui->listMn->setModel(mnModel);
    ui->listMn->setModelColumn(AddressTableModel::Label);
    updateListState();
}

void MasterNodesWidget::updateListState()
{
    bool show = mnModel->rowCount() > 0;
    ui->listMn->setVisible(show);
    ui->emptyContainer->setVisible(!show);
    ui->pushButtonStartAll->setVisible(show);
}

void MasterNodesWidget::onMNClicked(const QModelIndex &index)
{
    ui->listMn->setCurrentIndex(index);
    QRect rect = ui->listMn->visualRect(index);
    QPoint pos = rect.topRight();
    pos.setX(pos.x() - (DECORATION_SIZE * 2));
    pos.setY(pos.y() + (DECORATION_SIZE * 1.5));
    if (!this->menu) {
        this->menu = new TooltipMenu(window, this);
        this->menu->setEditBtnText(tr("Start"));
        this->menu->setDeleteBtnText(tr("Delete"));
        this->menu->setCopyBtnText(tr("Info"));
        connect(this->menu, &TooltipMenu::message, this, &AddressesWidget::message);
        connect(this->menu, &TooltipMenu::onEditClicked, this, &MasterNodesWidget::onEditMNClicked);
        connect(this->menu, &TooltipMenu::onDeleteClicked, this, &MasterNodesWidget::onDeleteMNClicked);
        connect(this->menu, &TooltipMenu::onCopyClicked, this, &MasterNodesWidget::onInfoMNClicked);
        this->menu->adjustSize();
    } else {
        this->menu->hide();
    }
    this->index = index;
    menu->move(pos);
    menu->show();

    // Back to regular status
    ui->listMn->scrollTo(index);
    ui->listMn->clearSelection();
    ui->listMn->setFocus();
}

bool MasterNodesWidget::checkMNsNetwork()
{
    bool isTierTwoSync = mnModel->isMNsNetworkSynced();
    if (!isTierTwoSync) inform(tr("Please wait until the node is fully synced"));
    return isTierTwoSync;
}

void MasterNodesWidget::onEditMNClicked()
{
    if (walletModel) {
        if (!walletModel->isRegTestNetwork() && !checkMNsNetwork()) return;
        if (index.sibling(index.row(), MNModel::WAS_COLLATERAL_ACCEPTED).data(Qt::DisplayRole).toBool()) {
            // Start MN
            QString strAlias = this->index.data(Qt::DisplayRole).toString();
            if (ask(tr("Start Masternode"), tr("Are you sure you want to start masternode %1?\n").arg(strAlias))) {
                WalletModel::UnlockContext ctx(walletModel->requestUnlock());
                if (!ctx.isValid()) {
                    // Unlock wallet was cancelled
                    inform(tr("Cannot edit masternode, wallet locked"));
                    return;
                }
                startAlias(strAlias);
            }
        } else {
            inform(tr("Cannot start masternode, the collateral transaction has not been confirmed by the network yet.\n"
                    "Please wait few more minutes (masternode collaterals require %1 confirmations).").arg(MasternodeCollateralMinConf()));
        }
    }
}

static bool startMN(const CMasternodeConfig::CMasternodeEntry& mne, std::string& strError)
{
    CMasternodeBroadcast mnb;
    if (!CMasternodeBroadcast::Create(mne.getIp(), mne.getPrivKey(), mne.getTxHash(), mne.getOutputIndex(), strError, mnb))
        return false;

    mnodeman.UpdateMasternodeList(mnb);
    mnb.Relay();
    return true;
}

void MasterNodesWidget::startAlias(QString strAlias)
{
    QString strStatusHtml;
    strStatusHtml += "Alias: " + strAlias + " ";

    for (CMasternodeConfig::CMasternodeEntry mne : masternodeConfig.getEntries()) {
        if (mne.getAlias() == strAlias.toStdString()) {
            std::string strError;
            strStatusHtml += (!startMN(mne, strError)) ? ("failed to start.\nError: " + QString::fromStdString(strError)) : "successfully started.";
            break;
        }
    }
    // update UI and notify
    updateModelAndInform(strStatusHtml);
}

void MasterNodesWidget::updateModelAndInform(QString informText)
{
    mnModel->updateMNList();
    inform(informText);
}

void MasterNodesWidget::onStartAllClicked(int type)
{
    if (!Params().IsRegTestNet() && !checkMNsNetwork()) return;     // skip on RegNet: so we can test even if tier two not synced

    if (isLoading) {
        inform(tr("Background task is being executed, please wait"));
    } else {
        std::unique_ptr<WalletModel::UnlockContext> pctx = MakeUnique<WalletModel::UnlockContext>(walletModel->requestUnlock());
        if (!pctx->isValid()) {
            warn(tr("Start ALL masternodes failed"), tr("Wallet unlock cancelled"));
            return;
        }
        isLoading = true;
        if (!execute(type, std::move(pctx))) {
            isLoading = false;
            inform(tr("Cannot perform Masternodes start"));
        }
    }
}

bool MasterNodesWidget::startAll(QString& failText, bool onlyMissing)
{
    int amountOfMnFailed = 0;
    int amountOfMnStarted = 0;
    for (CMasternodeConfig::CMasternodeEntry mne : masternodeConfig.getEntries()) {
        // Check for missing only
        QString mnAlias = QString::fromStdString(mne.getAlias());
        if (onlyMissing && !mnModel->isMNInactive(mnAlias)) {
            if (!mnModel->isMNActive(mnAlias))
                amountOfMnFailed++;
            continue;
        }

        if (!mnModel->isMNCollateralMature(mnAlias)) {
            amountOfMnFailed++;
            continue;
        }

        std::string strError;
        if (!startMN(mne, strError)) {
            amountOfMnFailed++;
        } else {
            amountOfMnStarted++;
        }
    }
    if (amountOfMnFailed > 0) {
        failText = tr("%1 Masternodes failed to start, %2 started").arg(amountOfMnFailed).arg(amountOfMnStarted);
        return false;
    }
    return true;
}

void MasterNodesWidget::run(int type)
{
    bool isStartMissing = type == REQUEST_START_MISSING;
    if (type == REQUEST_START_ALL || isStartMissing) {
        QString failText;
        QString inform = startAll(failText, isStartMissing) ? tr("All Masternodes started!") : failText;
        QMetaObject::invokeMethod(this, "updateModelAndInform", Qt::QueuedConnection,
                                  Q_ARG(QString, inform));
    }

    isLoading = false;
}

void MasterNodesWidget::onError(QString error, int type)
{
    if (type == REQUEST_START_ALL) {
        QMetaObject::invokeMethod(this, "inform", Qt::QueuedConnection,
                                  Q_ARG(QString, "Error starting all Masternodes"));
    }
}

void MasterNodesWidget::onInfoMNClicked()
{
    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid()) {
        // Unlock wallet was cancelled
        inform(tr("Cannot show Masternode information, wallet locked"));
        return;
    }
    showHideOp(true);
    MnInfoDialog* dialog = new MnInfoDialog(window);
    QString label = index.data(Qt::DisplayRole).toString();
    QString address = index.sibling(index.row(), MNModel::ADDRESS).data(Qt::DisplayRole).toString();
    QString status = index.sibling(index.row(), MNModel::STATUS).data(Qt::DisplayRole).toString();
    QString txId = index.sibling(index.row(), MNModel::COLLATERAL_ID).data(Qt::DisplayRole).toString();
    QString outIndex = index.sibling(index.row(), MNModel::COLLATERAL_OUT_INDEX).data(Qt::DisplayRole).toString();
    QString pubKey = index.sibling(index.row(), MNModel::PUB_KEY).data(Qt::DisplayRole).toString();
    dialog->setData(pubKey, label, address, txId, outIndex, status);
    dialog->adjustSize();
    showDialog(dialog, 3, 17);
    if (dialog->exportMN) {
        if (ask(tr("Remote Masternode Data"),
                tr("You are just about to export the required data to run a Masternode\non a remote server to your clipboard.\n\n\n"
                   "You will only have to paste the data in the pivx.conf file\nof your remote server and start it, "
                   "then start the Masternode using\nthis controller wallet (select the Masternode in the list and press \"start\").\n"
                ))) {
            // export data
            QString exportedMN = "masternode=1\n"
                                 "externalip=" + address.left(address.lastIndexOf(":")) + "\n" +
                                 "masternodeaddr=" + address + + "\n" +
                                 "masternodeprivkey=" + index.sibling(index.row(), MNModel::PRIV_KEY).data(Qt::DisplayRole).toString() + "\n";
            GUIUtil::setClipboard(exportedMN);
            inform(tr("Masternode data copied to the clipboard."));
        }
    }

    dialog->deleteLater();
}

void MasterNodesWidget::onDeleteMNClicked()
{
    QString txId = index.sibling(index.row(), MNModel::COLLATERAL_ID).data(Qt::DisplayRole).toString();
    QString outIndex = index.sibling(index.row(), MNModel::COLLATERAL_OUT_INDEX).data(Qt::DisplayRole).toString();
    QString qAliasString = index.data(Qt::DisplayRole).toString();
    std::string aliasToRemove = qAliasString.toStdString();

    if (!ask(tr("Delete Masternode"), tr("You are just about to delete Masternode:\n%1\n\nAre you sure?").arg(qAliasString)))
        return;

    std::string strConfFile = "masternode.conf";
    std::string strDataDir = GetDataDir().string();
    fs::path conf_file_path(strConfFile);
    if (strConfFile != conf_file_path.filename().string()) {
        throw std::runtime_error(strprintf(_("masternode.conf %s resides outside data directory %s"), strConfFile, strDataDir));
    }

    fs::path pathBootstrap = GetDataDir() / strConfFile;
    if (fs::exists(pathBootstrap)) {
        fs::path pathMasternodeConfigFile = GetMasternodeConfigFile();
        fs::ifstream streamConfig(pathMasternodeConfigFile);

        if (!streamConfig.good()) {
            inform(tr("Invalid masternode.conf file"));
            return;
        }

        int lineNumToRemove = -1;
        int linenumber = 1;
        std::string lineCopy = "";
        for (std::string line; std::getline(streamConfig, line); linenumber++) {
            if (line.empty()) continue;

            std::istringstream iss(line);
            std::string comment, alias, ip, privKey, txHash, outputIndex;

            if (iss >> comment) {
                if (comment.at(0) == '#') continue;
                iss.str(line);
                iss.clear();
            }

            if (!(iss >> alias >> ip >> privKey >> txHash >> outputIndex)) {
                iss.str(line);
                iss.clear();
                if (!(iss >> alias >> ip >> privKey >> txHash >> outputIndex)) {
                    streamConfig.close();
                    inform(tr("Error parsing masternode.conf file"));
                    return;
                }
            }

            if (aliasToRemove == alias) {
                lineNumToRemove = linenumber;
            } else
                lineCopy += line + "\n";

        }

        if (lineCopy.size() == 0) {
            lineCopy = "# Masternode config file\n"
                                    "# Format: alias IP:port masternodeprivkey collateral_output_txid collateral_output_index\n"
                                    "# Example: mn1 127.0.0.2:51472 93HaYBVUCYjEMeeH1Y4sBGLALQZE1Yc1K64xiqgX37tGBDQL8Xg 2bcd3c84c84f87eaa86e4e56834c92927a07f9e18718810b92e0d0324456a67c 0\n";
        }

        streamConfig.close();

        if (lineNumToRemove != -1) {
            fs::path pathConfigFile = AbsPathForConfigVal(fs::path("masternode_temp.conf"));
            FILE* configFile = fsbridge::fopen(pathConfigFile, "w");
            fwrite(lineCopy.c_str(), std::strlen(lineCopy.c_str()), 1, configFile);
            fclose(configFile);

            fs::path pathOldConfFile = AbsPathForConfigVal(fs::path("old_masternode.conf"));
            if (fs::exists(pathOldConfFile)) {
                fs::remove(pathOldConfFile);
            }
            rename(pathMasternodeConfigFile, pathOldConfFile);

            fs::path pathNewConfFile = AbsPathForConfigVal(fs::path("masternode.conf"));
            rename(pathConfigFile, pathNewConfFile);

            // Unlock collateral
            bool convertOK = false;
            unsigned int indexOut = outIndex.toUInt(&convertOK);
            if (convertOK) {
                COutPoint collateralOut(uint256S(txId.toStdString()), indexOut);
                walletModel->unlockCoin(collateralOut);
            }

            // Remove alias
            masternodeConfig.remove(aliasToRemove);
            // Update list
            mnModel->removeMn(index);
            updateListState();
        }
    } else {
        inform(tr("masternode.conf file doesn't exists"));
    }
}

void MasterNodesWidget::onCreateMNClicked()
{
    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid()) {
        // Unlock wallet was cancelled
        inform(tr("Cannot create Masternode controller, wallet locked"));
        return;
    }

    CAmount mnCollateralAmount = clientModel->getMNCollateralRequiredAmount();
    if (walletModel->getBalance() <= mnCollateralAmount) {
        inform(tr("Not enough balance to create a masternode, %1 required.")
            .arg(GUIUtil::formatBalance(mnCollateralAmount, BitcoinUnits::PIV)));
        return;
    }
    showHideOp(true);
    MasterNodeWizardDialog *dialog = new MasterNodeWizardDialog(walletModel, clientModel, window);
    if (openDialogWithOpaqueBackgroundY(dialog, window, 5, 7)) {
        if (dialog->isOk) {
            // Update list
            mnModel->addMn(dialog->mnEntry);
            updateListState();
            // add mn
            inform(dialog->returnStr);
        } else {
            warn(tr("Error creating masternode"), dialog->returnStr);
        }
    }
    dialog->deleteLater();
}

void MasterNodesWidget::changeTheme(bool isLightTheme, QString& theme)
{
    static_cast<MNHolder*>(this->delegate->getRowFactory())->isLightTheme = isLightTheme;
}

MasterNodesWidget::~MasterNodesWidget()
{
    delete ui;
}
