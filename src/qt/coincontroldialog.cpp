// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "coincontroldialog.h"
#include "ui_coincontroldialog.h"

#include "addresstablemodel.h"
#include "bitcoinunits.h"
#include "coincontrol.h"
#include "guiutil.h"
#include "init.h"
#include "optionsmodel.h"
#include "policy/policy.h"
#include "txmempool.h"
#include "wallet/fees.h"
#include "wallet/wallet.h"
#include "walletmodel.h"

#include "qt/pivx/qtutils.h"

#include <QApplication>
#include <QCheckBox>
#include <QCursor>
#include <QDialogButtonBox>
#include <QFlags>
#include <QIcon>
#include <QSettings>
#include <QString>
#include <QTreeWidget>


bool CCoinControlWidgetItem::operator<(const QTreeWidgetItem &other) const {
    int column = treeWidget()->sortColumn();
    if (column == CoinControlDialog::COLUMN_AMOUNT || column == CoinControlDialog::COLUMN_DATE || column == CoinControlDialog::COLUMN_CONFIRMATIONS)
        return data(column, Qt::UserRole).toLongLong() < other.data(column, Qt::UserRole).toLongLong();
    return QTreeWidgetItem::operator<(other);
}


CoinControlDialog::CoinControlDialog(QWidget* parent, bool _forDelegation) : QDialog(parent, Qt::WindowSystemMenuHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint),
                                                        ui(new Ui::CoinControlDialog),
                                                        model(0),
                                                        forDelegation(_forDelegation)
{
    coinControl = new CCoinControl();
    ui->setupUi(this);

    /* Open CSS when configured */
    this->setStyleSheet(GUIUtil::loadStyleSheet());
    setCssProperty({ui->frameContainer,
                    ui->layoutAmount,
                    ui->layoutAfter,
                    ui->layoutBytes,
                    ui->layoutChange,
                    ui->layoutDust,
                    ui->layoutFee,
                    ui->layoutQuantity
                    }, "container-border-purple");

    // Title
    ui->labelTitle->setProperty("cssClass", "text-title-dialog");

    // Label Style
    setCssProperty({ui->labelCoinControlAfterFeeText,
                    ui->labelCoinControlAmountText,
                    ui->labelCoinControlBytesText,
                    ui->labelCoinControlChangeText,
                    ui->labelCoinControlLowOutputText,
                    ui->labelCoinControlFeeText,
                    ui->labelCoinControlQuantityText
                    }, "text-main-purple");

    // Value Style
    setCssProperty({ui->labelCoinControlAfterFee,
                    ui->labelCoinControlAmount,
                    ui->labelCoinControlBytes,
                    ui->labelCoinControlChange,
                    ui->labelCoinControlLowOutput,
                    ui->labelCoinControlFee,
                    ui->labelCoinControlQuantity
                    }, "text-main-purple");

    ui->groupBox_2->setProperty("cssClass", "group-box");
    ui->treeWidget->setProperty("cssClass", "table-tree");
    ui->labelLocked->setProperty("cssClass", "text-main-purple");

    // Buttons
    setCssProperty({ui->pushButtonSelectAll, ui->pushButtonToggleLock}, "btn-check");
    ui->pushButtonOk->setProperty("cssClass", "btn-primary");

    // context menu actions
    QAction* copyAddressAction = new QAction(tr("Copy address"), this);
    QAction* copyLabelAction = new QAction(tr("Copy label"), this);
    QAction* copyAmountAction = new QAction(tr("Copy amount"), this);
    copyTransactionHashAction = new QAction(tr("Copy transaction ID"), this); // we need to enable/disable this
    lockAction = new QAction(tr("Lock unspent"), this);                       // we need to enable/disable this
    unlockAction = new QAction(tr("Unlock unspent"), this);                   // we need to enable/disable this

    // context menu
    contextMenu = new QMenu();
    contextMenu->addAction(copyAddressAction);
    contextMenu->addAction(copyLabelAction);
    contextMenu->addAction(copyAmountAction);
    contextMenu->addAction(copyTransactionHashAction);
    contextMenu->addSeparator();
    contextMenu->addAction(lockAction);
    contextMenu->addAction(unlockAction);

    // context menu signals
    connect(ui->treeWidget, &QWidget::customContextMenuRequested, this, &CoinControlDialog::showMenu);
    connect(copyAddressAction, &QAction::triggered, this, &CoinControlDialog::copyAddress);
    connect(copyLabelAction, &QAction::triggered, this, &CoinControlDialog::copyLabel);
    connect(copyAmountAction, &QAction::triggered, this, &CoinControlDialog::copyAmount);
    connect(copyTransactionHashAction, &QAction::triggered, this, &CoinControlDialog::copyTransactionHash);
    connect(lockAction, &QAction::triggered, this, &CoinControlDialog::lockCoin);
    connect(unlockAction, &QAction::triggered, this, &CoinControlDialog::unlockCoin);

    // clipboard actions
    setCssProperty({
        ui->pushButtonAmount,
        ui->pushButtonQuantity,
        ui->pushButtonFee,
        ui->pushButtonAlterFee,
        ui->pushButtonBytes,
        ui->pushButtonChange,
        ui->pushButtonDust
        }, "ic-copy-big"
    );

    connect(ui->pushButtonQuantity, &QPushButton::clicked, this, &CoinControlDialog::clipboardQuantity);
    connect(ui->pushButtonAmount, &QPushButton::clicked, this, &CoinControlDialog::clipboardAmount);
    connect(ui->pushButtonFee, &QPushButton::clicked, this, &CoinControlDialog::clipboardFee);
    connect(ui->pushButtonAlterFee, &QPushButton::clicked, this, &CoinControlDialog::clipboardAfterFee);
    connect(ui->pushButtonBytes, &QPushButton::clicked, this, &CoinControlDialog::clipboardBytes);
    connect(ui->pushButtonDust, &QPushButton::clicked, this, &CoinControlDialog::clipboardLowOutput);
    connect(ui->pushButtonChange, &QPushButton::clicked, this, &CoinControlDialog::clipboardChange);

    if (ui->pushButtonSelectAll->isChecked()) {
        ui->pushButtonSelectAll->setText(tr("Unselect all"));
    } else {
        ui->pushButtonSelectAll->setText(tr("Select all"));
    }

    // toggle tree/list mode
    connect(ui->radioTreeMode, &QRadioButton::toggled, this, &CoinControlDialog::radioTreeMode);
    connect(ui->radioListMode, &QRadioButton::toggled, this, &CoinControlDialog::radioListMode);

    // click on checkbox
    connect(ui->treeWidget, &QTreeWidget::itemChanged, this, &CoinControlDialog::viewItemChanged);

    // click on header
    ui->treeWidget->header()->setSectionsClickable(true);
    connect(ui->treeWidget->header(), &QHeaderView::sectionClicked, this, &CoinControlDialog::headerSectionClicked);

    // ok button
    connect(ui->pushButtonOk, &QPushButton::clicked, this, &CoinControlDialog::accept);

    // (un)select all
    connect(ui->pushButtonSelectAll, &QPushButton::clicked, this, &CoinControlDialog::buttonSelectAllClicked);

    // Toggle lock state
    connect(ui->pushButtonToggleLock, &QPushButton::clicked, this, &CoinControlDialog::buttonToggleLockClicked);

    ui->treeWidget->setColumnWidth(COLUMN_CHECKBOX, colCheckBoxWidth_treeMode);
    ui->treeWidget->setColumnWidth(COLUMN_AMOUNT, 110);
    ui->treeWidget->setColumnWidth(COLUMN_LABEL, 160);
    ui->treeWidget->setColumnWidth(COLUMN_ADDRESS, 310);
    ui->treeWidget->setColumnWidth(COLUMN_DATE, 145);
    ui->treeWidget->setColumnWidth(COLUMN_CONFIRMATIONS, 65);
    ui->treeWidget->setColumnHidden(COLUMN_TXHASH, true);         // store transacton hash in this column, but dont show it
    ui->treeWidget->setColumnHidden(COLUMN_VOUT_INDEX, true);     // store vout index in this column, but dont show it

    ui->treeWidget->header()->setStretchLastSection(true);
    // default view is sorted by amount desc
    sortView(COLUMN_AMOUNT, Qt::DescendingOrder);

    // restore list mode and sortorder as a convenience feature
    QSettings settings;
    if (settings.contains("nCoinControlMode") && !settings.value("nCoinControlMode").toBool()) {
        ui->radioTreeMode->setChecked(true);
        ui->treeWidget->setRootIsDecorated(true);
        ui->radioTreeMode->click();
    }else{
        ui->radioListMode->setChecked(true);
        ui->treeWidget->setRootIsDecorated(false);
    }
    if (settings.contains("nCoinControlSortColumn") && settings.contains("nCoinControlSortOrder"))
        sortView(settings.value("nCoinControlSortColumn").toInt(), (static_cast<Qt::SortOrder>(settings.value("nCoinControlSortOrder").toInt())));
}

CoinControlDialog::~CoinControlDialog()
{
    QSettings settings;
    settings.setValue("nCoinControlMode", ui->radioListMode->isChecked());
    settings.setValue("nCoinControlSortColumn", sortColumn);
    settings.setValue("nCoinControlSortOrder", (int)sortOrder);

    delete ui;
    delete coinControl;
}

void CoinControlDialog::setModel(WalletModel* _model)
{
    this->model = _model;

    if (model && model->getOptionsModel() && model->getAddressTableModel()) {
        updateView();
        updateLabelLocked();
        updateLabels();
    }
}

// (un)select all
void CoinControlDialog::buttonSelectAllClicked()
{
    // "Select all": if some entry is unchecked, then check it
    // "Unselect all": if some entry is checked, then uncheck it
    const bool fSelectAll = ui->pushButtonSelectAll->isChecked();
    Qt::CheckState wantedState = fSelectAll ? Qt::Checked : Qt::Unchecked;
    ui->treeWidget->setEnabled(false);
    for (int i = 0; i < ui->treeWidget->topLevelItemCount(); i++)
        if (ui->treeWidget->topLevelItem(i)->checkState(COLUMN_CHECKBOX) != wantedState)
            ui->treeWidget->topLevelItem(i)->setCheckState(COLUMN_CHECKBOX, wantedState);
    ui->treeWidget->setEnabled(true);
    if (!fSelectAll)
        coinControl->UnSelectAll(); // just to be sure
    updateLabels();
}

// Toggle lock state
void CoinControlDialog::buttonToggleLockClicked()
{
    if (!fSelectTransparent) return; // todo: implement locked notes
    QTreeWidgetItem* item;
    // Works in list-mode only
    if (ui->radioListMode->isChecked()) {
        ui->treeWidget->setEnabled(false);
        for (int i = 0; i < ui->treeWidget->topLevelItemCount(); i++) {
            item = ui->treeWidget->topLevelItem(i);

            COutPoint outpt(uint256S(item->text(COLUMN_TXHASH).toStdString()), item->text(COLUMN_VOUT_INDEX).toUInt());
            if (model->isLockedCoin(uint256S(item->text(COLUMN_TXHASH).toStdString()), item->text(COLUMN_VOUT_INDEX).toUInt())) {
                model->unlockCoin(outpt);
                item->setDisabled(false);
                // restore cold-stake snowflake icon for P2CS which were previously locked
                if (item->data(COLUMN_CHECKBOX, Qt::UserRole) == QString("Delegated"))
                    item->setIcon(COLUMN_CHECKBOX, QIcon("://ic-check-cold-staking-off"));
                else
                    item->setIcon(COLUMN_CHECKBOX, QIcon());
            } else {
                model->lockCoin(outpt);
                item->setDisabled(true);
                item->setIcon(COLUMN_CHECKBOX, QIcon(":/icons/lock_closed"));
            }
            updateLabelLocked();
        }
        ui->treeWidget->setEnabled(true);
        updateLabels();
    } else {
        QMessageBox msgBox;
        msgBox.setObjectName("lockMessageBox");
        msgBox.setStyleSheet(GUIUtil::loadStyleSheet());
        msgBox.setText(tr("Please switch to \"List mode\" to use this function."));
        msgBox.exec();
    }
}

// context menu
void CoinControlDialog::showMenu(const QPoint& point)
{
    QTreeWidgetItem* item = ui->treeWidget->itemAt(point);
    if (item) {
        contextMenuItem = item;

        // disable some items (like Copy Transaction ID, lock, unlock) for tree roots in context menu
        if (item->text(COLUMN_TXHASH).length() == 64) { // transaction hash is 64 characters (this means its a child node, so its not a parent node in tree mode)
            copyTransactionHashAction->setEnabled(true);
            if (model->isLockedCoin(uint256S(item->text(COLUMN_TXHASH).toStdString()), item->text(COLUMN_VOUT_INDEX).toUInt())) {
                lockAction->setEnabled(false);
                unlockAction->setEnabled(true);
            } else {
                lockAction->setEnabled(true);
                unlockAction->setEnabled(false);
            }
        } else { // this means click on parent node in tree mode -> disable all
            copyTransactionHashAction->setEnabled(false);
            lockAction->setEnabled(false);
            unlockAction->setEnabled(false);
        }

        // show context menu
        contextMenu->exec(QCursor::pos());
    }
}

// context menu action: copy amount
void CoinControlDialog::copyAmount()
{
    GUIUtil::setClipboard(BitcoinUnits::removeSpaces(contextMenuItem->text(COLUMN_AMOUNT)));
}

// context menu action: copy label
void CoinControlDialog::copyLabel()
{
    if (ui->radioTreeMode->isChecked() && contextMenuItem->text(COLUMN_LABEL).length() == 0 && contextMenuItem->parent())
        GUIUtil::setClipboard(contextMenuItem->parent()->text(COLUMN_LABEL));
    else
        GUIUtil::setClipboard(contextMenuItem->text(COLUMN_LABEL));
}

// context menu action: copy address
void CoinControlDialog::copyAddress()
{
    if (ui->radioTreeMode->isChecked() && contextMenuItem->text(COLUMN_ADDRESS).length() == 0 && contextMenuItem->parent())
        GUIUtil::setClipboard(contextMenuItem->parent()->text(COLUMN_ADDRESS));
    else
        GUIUtil::setClipboard(contextMenuItem->text(COLUMN_ADDRESS));
}

// context menu action: copy transaction id
void CoinControlDialog::copyTransactionHash()
{
    GUIUtil::setClipboard(contextMenuItem->text(COLUMN_TXHASH));
}

// context menu action: lock coin
void CoinControlDialog::lockCoin()
{
    if (!fSelectTransparent) return; // todo: implement locked notes
    if (contextMenuItem->checkState(COLUMN_CHECKBOX) == Qt::Checked)
        contextMenuItem->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);

    COutPoint outpt(uint256S(contextMenuItem->text(COLUMN_TXHASH).toStdString()), contextMenuItem->text(COLUMN_VOUT_INDEX).toUInt());
    model->lockCoin(outpt);
    contextMenuItem->setDisabled(true);
    contextMenuItem->setIcon(COLUMN_CHECKBOX, QIcon(":/icons/lock_closed"));
    updateLabelLocked();
}

// context menu action: unlock coin
void CoinControlDialog::unlockCoin()
{
    if (!fSelectTransparent) return; // todo: implement locked notes
    COutPoint outpt(uint256S(contextMenuItem->text(COLUMN_TXHASH).toStdString()), contextMenuItem->text(COLUMN_VOUT_INDEX).toUInt());
    model->unlockCoin(outpt);
    contextMenuItem->setDisabled(false);
    // restore cold-stake snowflake icon for P2CS which were previously locked
    if (contextMenuItem->data(COLUMN_CHECKBOX, Qt::UserRole) == QString("Delegated"))
        contextMenuItem->setIcon(COLUMN_CHECKBOX, QIcon("://ic-check-cold-staking-off"));
    else
        contextMenuItem->setIcon(COLUMN_CHECKBOX, QIcon());
    updateLabelLocked();
}

// copy label "Quantity" to clipboard
void CoinControlDialog::clipboardQuantity()
{
    GUIUtil::setClipboard(ui->labelCoinControlQuantity->text());
    inform(tr("Quantity Copied"));
}

// copy label "Amount" to clipboard
void CoinControlDialog::clipboardAmount()
{
    GUIUtil::setClipboard(BitcoinUnits::removeSpaces(ui->labelCoinControlAmount->text().left(ui->labelCoinControlAmount->text().indexOf(" "))));
    inform(tr("Amount Copied"));
}

// copy label "Fee" to clipboard
void CoinControlDialog::clipboardFee()
{
    GUIUtil::setClipboard(BitcoinUnits::removeSpaces(ui->labelCoinControlFee->text().left(ui->labelCoinControlFee->text().indexOf(" ")).replace("~", "")));
    inform(tr("Fee Copied"));
}

// copy label "After fee" to clipboard
void CoinControlDialog::clipboardAfterFee()
{
    GUIUtil::setClipboard(BitcoinUnits::removeSpaces(ui->labelCoinControlAfterFee->text().left(ui->labelCoinControlAfterFee->text().indexOf(" ")).replace("~", "")));
    inform(tr("After Fee Copied"));
}

// copy label "Bytes" to clipboard
void CoinControlDialog::clipboardBytes()
{
    GUIUtil::setClipboard(ui->labelCoinControlBytes->text().replace("~", ""));
    inform(tr("Bytes Copied"));
}

// copy label "Dust" to clipboard
void CoinControlDialog::clipboardLowOutput()
{
    GUIUtil::setClipboard(ui->labelCoinControlLowOutput->text());
    inform(tr("Dust Copied"));
}

// copy label "Change" to clipboard
void CoinControlDialog::clipboardChange()
{
    GUIUtil::setClipboard(BitcoinUnits::removeSpaces(ui->labelCoinControlChange->text().left(ui->labelCoinControlChange->text().indexOf(" ")).replace("~", "")));
    inform(tr("Change Copied"));
}

// treeview: sort
void CoinControlDialog::sortView(int column, Qt::SortOrder order)
{
    sortColumn = column;
    sortOrder = order;
    ui->treeWidget->sortItems(column, order);
    ui->treeWidget->header()->setSortIndicator(sortColumn, sortOrder);
}

// treeview: clicked on header
void CoinControlDialog::headerSectionClicked(int logicalIndex)
{
    if (logicalIndex == COLUMN_CHECKBOX) { // click on most left column -> do nothing
        ui->treeWidget->header()->setSortIndicator(sortColumn, sortOrder);
    } else {
        if (sortColumn == logicalIndex)
            sortOrder = ((sortOrder == Qt::AscendingOrder) ? Qt::DescendingOrder : Qt::AscendingOrder);
        else {
            sortColumn = logicalIndex;
            sortOrder = ((sortColumn == COLUMN_LABEL || sortColumn == COLUMN_ADDRESS) ? Qt::AscendingOrder : Qt::DescendingOrder); // if label or address then default => asc, else default => desc
        }

        sortView(sortColumn, sortOrder);
    }
}

// toggle tree mode
void CoinControlDialog::radioTreeMode(bool checked)
{
    if (checked && model)
        updateView();
}

// toggle list mode
void CoinControlDialog::radioListMode(bool checked)
{
    if (checked && model)
        updateView();
}

// checkbox clicked by user
void CoinControlDialog::viewItemChanged(QTreeWidgetItem* item, int column)
{
    if (column == COLUMN_CHECKBOX && item->text(COLUMN_TXHASH).length() == 64) { // transaction hash is 64 characters (this means its a child node, so its not a parent node in tree mode)
        BaseOutPoint outpt(uint256S(item->text(COLUMN_TXHASH).toStdString()),
                           item->text(COLUMN_VOUT_INDEX).toUInt(),
                           fSelectTransparent);
        if (item->checkState(COLUMN_CHECKBOX) == Qt::Unchecked)
            coinControl->UnSelect(outpt);
        else if (item->isDisabled()) // locked (this happens if "check all" through parent node)
            item->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);
        else {
            const CAmount value = static_cast<CAmount>(item->data(CoinControlDialog::COLUMN_AMOUNT, Qt::UserRole).toLongLong());
            bool isP2CS = item->data(COLUMN_CHECKBOX, Qt::UserRole) == QString("Delegated");
            coinControl->Select(outpt, value, isP2CS);
        }

        // selection changed -> update labels
        if (ui->treeWidget->isEnabled()) { // do not update on every click for (un)select all
            updateLabels();
        }
    }
}

// shows count of locked unspent outputs
void CoinControlDialog::updateLabelLocked()
{
    if (fSelectTransparent) {
        std::set<COutPoint> vOutpts = model->listLockedCoins();
        if (!vOutpts.empty()) {
            ui->labelLocked->setText(tr("(%1 locked)").arg(vOutpts.size()));
            ui->labelLocked->setVisible(true);
        } else
            ui->labelLocked->setVisible(false);
    } else {
        // TODO: implement locked notes functionality inside the wallet..
    }
}

// serialized int size
static int GetCompactSize(uint64_t nSize)
{
    if (nSize < 253) {
        return 1;
    } else if (nSize <= std::numeric_limits<unsigned short>::max()) {
        return 3;
    } else if (nSize <= std::numeric_limits<unsigned int>::max()) {
        return 5;
    }
    return 9;
}

TotalAmounts CoinControlDialog::getTotals() const
{
    TotalAmounts t;

    std::vector<OutPointWrapper> vCoinControl;
    coinControl->ListSelected(vCoinControl);

    for (const OutPointWrapper& out : vCoinControl) {
        // Quantity
        t.nQuantity++;
        // Amount
        t.nAmount += out.value;
        // Bytes
        t.nBytes += (fSelectTransparent ? (CTXIN_SPEND_DUST_SIZE + (out.isP2CS ? 1 : 0))
                                      : SPENDDESCRIPTION_SIZE);
    }

    // selected inputs
    int nTransIns, nShieldIns;
    if (fSelectTransparent) {
        nTransIns = t.nQuantity;
        nShieldIns = 0;
    } else {
        nTransIns = 0;
        nShieldIns = t.nQuantity;
    }

    // calculation
    const int P2CS_OUT_SIZE = 61;
    int nTransOuts = 0, nShieldOuts = 0;
    if (t.nQuantity > 0) {
        // Bytes: nBytesInputs + (sum of nBytesOutputs)
        // always assume +1 (p2pkh) output for change here
        t.nBytes += (fSelectTransparent ? CTXOUT_REGULAR_SIZE : OUTPUTDESCRIPTION_SIZE);
        for (const auto& a : payAmounts) {
            t.nPayAmount += a.first;
            bool shieldedOut = a.second;
            if (shieldedOut) nShieldOuts++;
            else nTransOuts++;
            if (a.first > 0 && !t.fDust) {
                if (a.first < (shieldedOut ? GetShieldedDustThreshold(dustRelayFee) : GetDustThreshold(dustRelayFee)))
                    t.fDust = true;
            }
            t.nBytes += (shieldedOut ? OUTPUTDESCRIPTION_SIZE
                                   : (forDelegation ? P2CS_OUT_SIZE : CTXOUT_REGULAR_SIZE));
        }

        // Shielded txes must include binding sig and valueBalance
        bool isShieldedTx = (nShieldIns + nShieldOuts > 0);
        if (isShieldedTx) {
            t.nBytes += (BINDINGSIG_SIZE + 8);
            // shielded in/outs len sizes
            t.nBytes += (GetCompactSize(nShieldIns) + GetCompactSize(nShieldOuts));
        }

        // !TODO: ExtraPayload size for special txes. For now 1 byte for nullopt.
        t.nBytes += 1;

        // nVersion, nType, nLockTime
        t.nBytes += 8;

        // vin/vout len sizes
        t.nBytes += (GetCompactSize(nTransIns) +  GetCompactSize(nTransOuts));

        // Fee (default K fixed for shielded fee for now)
        t.nPayFee = GetMinRelayFee(t.nBytes) * (isShieldedTx ? DEFAULT_SHIELDEDTXFEE_K : 1);

        if (t.nPayAmount > 0) {
            t.nChange = t.nAmount - t.nPayFee - t.nPayAmount;

            // Never create dust outputs; if we would, just add the dust to the fee.
            CAmount dustThreshold = fSelectTransparent ? GetDustThreshold(dustRelayFee)
                                                       : GetShieldedDustThreshold(dustRelayFee);
            if (t.nChange > 0 && t.nChange < dustThreshold) {
                t.nPayFee += t.nChange;
                t.nChange = 0;
            }

            if (t.nChange == 0)
                t.nBytes -= (fSelectTransparent ? CTXOUT_REGULAR_SIZE : SPENDDESCRIPTION_SIZE);
        }

        // after fee
        t.nAfterFee = std::max<CAmount>(t.nAmount - t.nPayFee, 0);
    }
    return t;
}

void CoinControlDialog::updateLabels()
{
    if (!model)
        return;

    ui->labelTitle->setText(fSelectTransparent ?
            "Select PIV Outputs to Spend" :
            "Select Shielded PIV to Spend");

    const TotalAmounts& t = getTotals();

    // update SelectAll button state
    // if inputs selected > inputs unselected, set checked (label "Unselect All")
    // if inputs selected <= inputs unselected, set unchecked (label "Select All")
    updatePushButtonSelectAll(coinControl->QuantitySelected() * 2 > nSelectableInputs);

    // actually update labels
    int nDisplayUnit = BitcoinUnits::PIV;
    if (model && model->getOptionsModel())
        nDisplayUnit = model->getOptionsModel()->getDisplayUnit();

    // enable/disable "dust" and "change"
    const bool hasPayAmount = t.nPayAmount > 0;
    ui->labelCoinControlLowOutputText->setEnabled(hasPayAmount);
    ui->labelCoinControlLowOutput->setEnabled(hasPayAmount);
    ui->labelCoinControlChangeText->setEnabled(hasPayAmount);
    ui->labelCoinControlChange->setEnabled(hasPayAmount);

    // stats
    ui->labelCoinControlQuantity->setText(QString::number(t.nQuantity));
    ui->labelCoinControlAmount->setText(BitcoinUnits::formatWithUnit(nDisplayUnit, t.nAmount));
    ui->labelCoinControlFee->setText(BitcoinUnits::formatWithUnit(nDisplayUnit, t.nPayFee));
    ui->labelCoinControlAfterFee->setText(BitcoinUnits::formatWithUnit(nDisplayUnit, t.nAfterFee));
    ui->labelCoinControlBytes->setText(((t.nBytes > 0) ? "~" : "") + QString::number(t.nBytes));
    ui->labelCoinControlLowOutput->setText(t.fDust ? tr("yes") : tr("no"));
    ui->labelCoinControlChange->setText(BitcoinUnits::formatWithUnit(nDisplayUnit, t.nChange));
    if (t.nPayFee > 0 && !(payTxFee.GetFeePerK() > 0 && fPayAtLeastCustomFee && t.nBytes < 1000)) {
        ui->labelCoinControlFee->setText("~" + ui->labelCoinControlFee->text());
        ui->labelCoinControlAfterFee->setText("~" + ui->labelCoinControlAfterFee->text());
        if (t.nChange > 0)
            ui->labelCoinControlChange->setText("~" + ui->labelCoinControlChange->text());
    }

    // turn labels "red"
    ui->labelCoinControlLowOutput->setStyleSheet((t.fDust) ? "color:red;" : "");                                      // Dust = "yes"

    // tool tips
    QString toolTip3 = tr("This label turns red, if recipient receives an amount smaller than %1 (transparent) / %2 (shield)."
            ).arg(BitcoinUnits::formatWithUnit(nDisplayUnit, GetDustThreshold(dustRelayFee))).arg(BitcoinUnits::formatWithUnit(nDisplayUnit, GetShieldedDustThreshold(dustRelayFee)));

    // how many satoshis the estimated fee can vary per byte we guess wrong
    double dFeeVary;
    if (payTxFee.GetFeePerK() > 0)
        dFeeVary = (double)std::max(GetRequiredFee(1000), payTxFee.GetFeePerK()) / 1000;
    else
        dFeeVary = (double)std::max(GetRequiredFee(1000), mempool.estimateSmartFee(nTxConfirmTarget).GetFeePerK()) / 1000;
    QString toolTip4 = tr("Can vary +/- %1 u%2 per input.").arg(dFeeVary).arg(CURRENCY_UNIT.c_str());

    ui->labelCoinControlFee->setToolTip(toolTip4);
    ui->labelCoinControlAfterFee->setToolTip(toolTip4);
    ui->labelCoinControlLowOutput->setToolTip(toolTip3);
    ui->labelCoinControlChange->setToolTip(toolTip4);
    ui->labelCoinControlFeeText->setToolTip(ui->labelCoinControlFee->toolTip());
    ui->labelCoinControlAfterFeeText->setToolTip(ui->labelCoinControlAfterFee->toolTip());
    ui->labelCoinControlBytesText->setToolTip(ui->labelCoinControlBytes->toolTip());
    ui->labelCoinControlLowOutputText->setToolTip(ui->labelCoinControlLowOutput->toolTip());
    ui->labelCoinControlChangeText->setToolTip(ui->labelCoinControlChange->toolTip());

    // Insufficient funds
    QLabel* label = findChild<QLabel*>("labelCoinControlInsuffFunds");
    if (label)
        label->setVisible(t.nChange < 0);
}

void CoinControlDialog::loadAvailableCoin(bool treeMode,
                                          CCoinControlWidgetItem* itemWalletAddress,
                                          QFlags<Qt::ItemFlag> flgCheckbox,
                                          QFlags<Qt::ItemFlag> flgTristate,
                                          int nDisplayUnit,
                                          const QString& sWalletAddress,
                                          const Optional<QString>& stakerAddress,
                                          const QString& sWalletLabel,
                                          const uint256& txhash,
                                          const uint32_t outIndex,
                                          const CAmount nValue,
                                          const int64_t nTime,
                                          const int nDepth,
                                          const bool isChange)
{
    CCoinControlWidgetItem* itemOutput;
    if (treeMode)
        itemOutput = new CCoinControlWidgetItem(itemWalletAddress);
    else
        itemOutput = new CCoinControlWidgetItem(ui->treeWidget);
    itemOutput->setFlags(flgCheckbox);
    itemOutput->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);

    // if listMode or change => show PIVX address. In tree mode, address is not shown again for direct wallet address outputs
    if (!treeMode) {
        itemOutput->setText(COLUMN_ADDRESS, sWalletAddress);
    }else {
        itemOutput->setToolTip(COLUMN_ADDRESS, sWalletAddress);
    }

    // label
    if (isChange) {
        // tooltip stating where the change is being stored.
        itemOutput->setToolTip(COLUMN_LABEL, tr("change in %1").arg(sWalletAddress));
        itemOutput->setText(COLUMN_LABEL, tr("(change)"));
    } else if (!treeMode) {
        itemOutput->setText(COLUMN_LABEL, sWalletLabel);
    }

    // amount
    itemOutput->setText(COLUMN_AMOUNT, BitcoinUnits::format(nDisplayUnit, nValue));
    itemOutput->setToolTip(COLUMN_AMOUNT, BitcoinUnits::format(nDisplayUnit, nValue));
    itemOutput->setData(COLUMN_AMOUNT, Qt::UserRole, QVariant((qlonglong) nValue));

    // date
    itemOutput->setText(COLUMN_DATE, GUIUtil::dateTimeStr(nTime));
    itemOutput->setToolTip(COLUMN_DATE, GUIUtil::dateTimeStr(nTime));
    itemOutput->setData(COLUMN_DATE, Qt::UserRole, QVariant((qlonglong) nTime));

    // confirmations
    itemOutput->setText(COLUMN_CONFIRMATIONS, QString::number(nDepth));
    itemOutput->setData(COLUMN_CONFIRMATIONS, Qt::UserRole, QVariant((qlonglong) nDepth));

    // transaction hash
    itemOutput->setText(COLUMN_TXHASH, QString::fromStdString(txhash.GetHex()));

    // vout index
    itemOutput->setText(COLUMN_VOUT_INDEX, QString::number(outIndex));

    // disable locked coins
    const bool isLockedCoin = model->isLockedCoin(txhash, outIndex);
    if (isLockedCoin) {
        --nSelectableInputs;
        coinControl->UnSelect({txhash, outIndex}); // just to be sure
        itemOutput->setDisabled(true);
        itemOutput->setIcon(COLUMN_CHECKBOX, QIcon(":/icons/lock_closed"));
    }

    // set checkbox
    if (coinControl->IsSelected(COutPoint(txhash, outIndex)))
        itemOutput->setCheckState(COLUMN_CHECKBOX, Qt::Checked);

    // outputs delegated (for cold staking)
    if (stakerAddress) {
        itemOutput->setData(COLUMN_CHECKBOX, Qt::UserRole, QString("Delegated"));
        if (!isLockedCoin)
            itemOutput->setIcon(COLUMN_CHECKBOX, QIcon("://ic-check-cold-staking-off"));
        itemOutput->setToolTip(COLUMN_CHECKBOX, tr("delegated to %1 for cold staking").arg(*stakerAddress));
    }
}

void CoinControlDialog::updateView()
{
    if (!model || !model->getOptionsModel() || !model->getAddressTableModel())
        return;

    bool treeMode = ui->radioTreeMode->isChecked();
    ui->treeWidget->setRootIsDecorated(treeMode);

    ui->treeWidget->clear();
    ui->treeWidget->setEnabled(false); // performance, otherwise updateLabels would be called for every checked checkbox
    QFlags<Qt::ItemFlag> flgCheckbox = Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable;
    QFlags<Qt::ItemFlag> flgTristate = Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsTristate;

    int nDisplayUnit = model->getOptionsModel()->getDisplayUnit();
    nSelectableInputs = 0;
    std::map<WalletModel::ListCoinsKey, std::vector<WalletModel::ListCoinsValue>> mapCoins;
    model->listCoins(mapCoins, fSelectTransparent);

    for (const auto& coins : mapCoins) {
        CCoinControlWidgetItem* itemWalletAddress = new CCoinControlWidgetItem();
        itemWalletAddress->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);
        const WalletModel::ListCoinsKey& keys = coins.first;
        const QString& sWalletAddress = keys.address;
        const Optional<QString>& stakerAddress = keys.stakerAddress;
        QString sWalletLabel = model->getAddressTableModel()->labelForAddress(sWalletAddress);
        if (sWalletLabel.isEmpty())
            sWalletLabel = tr("(no label)");

        if (treeMode) {
            // wallet address
            ui->treeWidget->addTopLevelItem(itemWalletAddress);

            itemWalletAddress->setFlags(flgTristate);
            itemWalletAddress->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);

            // label
            itemWalletAddress->setText(COLUMN_LABEL, sWalletLabel);
            itemWalletAddress->setToolTip(COLUMN_LABEL, sWalletLabel);

            // address
            itemWalletAddress->setText(COLUMN_ADDRESS, sWalletAddress);
            if (stakerAddress != nullopt) {
                itemWalletAddress->setIcon(COLUMN_CONFIRMATIONS, QIcon("://ic-check-cold-staking-off"));
                QString label = tr("Delegated to %1").arg(*stakerAddress);
                itemWalletAddress->setToolTip(COLUMN_ADDRESS, label);
                itemWalletAddress->setToolTip(COLUMN_CONFIRMATIONS, label);
            } else {
                itemWalletAddress->setToolTip(COLUMN_ADDRESS, sWalletAddress);
            }
        }

        CAmount nSum = 0;
        int nChildren = 0;
        for (const WalletModel::ListCoinsValue& out : coins.second) {
            ++nSelectableInputs;
            nSum += out.nValue;
            nChildren++;

            loadAvailableCoin(treeMode, itemWalletAddress, flgCheckbox, flgTristate,
                              nDisplayUnit, sWalletAddress, stakerAddress, sWalletLabel,
                              out.txhash, out.outIndex, out.nValue, out.nTime, out.nDepth,
                              keys.isChange);
        }

        // amount
        if (treeMode) {
            itemWalletAddress->setText(COLUMN_CHECKBOX, "(" + QString::number(nChildren) + ")");
            itemWalletAddress->setText(COLUMN_AMOUNT, BitcoinUnits::format(nDisplayUnit, nSum));
            itemWalletAddress->setToolTip(COLUMN_AMOUNT, BitcoinUnits::format(nDisplayUnit, nSum));
            itemWalletAddress->setData(COLUMN_AMOUNT, Qt::UserRole, QVariant((qlonglong) nSum));
        }
    }

    // expand all partially selected
    if (treeMode) {
        for (int i = 0; i < ui->treeWidget->topLevelItemCount(); i++)
            if (ui->treeWidget->topLevelItem(i)->checkState(COLUMN_CHECKBOX) == Qt::PartiallyChecked)
                ui->treeWidget->topLevelItem(i)->setExpanded(true);
        // restore saved width for COLUMN_CHECKBOX
        ui->treeWidget->setColumnWidth(COLUMN_CHECKBOX, colCheckBoxWidth_treeMode);
    } else {
        // save COLUMN_CHECKBOX width for tree-mode
        colCheckBoxWidth_treeMode = std::max(110, ui->treeWidget->columnWidth(COLUMN_CHECKBOX));
        // minimize COLUMN_CHECKBOX width in list-mode (need to display only the check box)
        ui->treeWidget->setColumnWidth(COLUMN_CHECKBOX, 70);
    }

    // sort view
    sortView(sortColumn, sortOrder);
    ui->treeWidget->setEnabled(true);
}

void CoinControlDialog::refreshDialog()
{
    updateView();
    updateLabelLocked();
    updateLabels();
}

void CoinControlDialog::inform(const QString& text)
{
    if (!snackBar) snackBar = new SnackBar(nullptr, this);
    snackBar->setText(text);
    snackBar->resize(this->width(), snackBar->height());
    openDialog(snackBar, this);
}

void CoinControlDialog::clearPayAmounts()
{
    payAmounts.clear();
}

void CoinControlDialog::addPayAmount(const CAmount& amount, bool isShieldedRecipient)
{
    payAmounts.emplace_back(amount, isShieldedRecipient);
}

void CoinControlDialog::updatePushButtonSelectAll(bool checked)
{
    ui->pushButtonSelectAll->setChecked(checked);
    ui->pushButtonSelectAll->setText(checked ? tr("Unselect all") : tr("Select All"));
}
