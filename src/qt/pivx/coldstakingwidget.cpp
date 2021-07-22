// Copyright (c) 2019-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/coldstakingwidget.h"
#include "qt/pivx/forms/ui_coldstakingwidget.h"
#include "qt/pivx/qtutils.h"
#include "amount.h"
#include "guiutil.h"
#include "qt/pivx/requestdialog.h"
#include "qt/pivx/tooltipmenu.h"
#include "qt/pivx/furlistrow.h"
#include "qt/pivx/sendconfirmdialog.h"
#include "qt/pivx/addnewcontactdialog.h"
#include "qt/pivx/guitransactionsutils.h"
#include "walletmodel.h"
#include "optionsmodel.h"
#include "coincontroldialog.h"
#include "coincontrol.h"
#include "qt/pivx/csrow.h"

#define DECORATION_SIZE 70
#define NUM_ITEMS 3
#define LOAD_MIN_TIME_INTERVAL 15
#define REQUEST_LOAD_TASK 1


class CSDelegationHolder : public FurListRow<QWidget*>
{
public:
    CSDelegationHolder();

    explicit CSDelegationHolder(bool _isLightTheme) : FurListRow(), isLightTheme(_isLightTheme) {}

    CSRow* createHolder(int pos) override
    {
        if (!cachedRow) cachedRow = new CSRow();
        return cachedRow;
    }

    void init(QWidget* holder,const QModelIndex &index, bool isHovered, bool isSelected) const override
    {
        CSRow *row = static_cast<CSRow*>(holder);
        row->updateState(isLightTheme, isHovered, isSelected);

        QString address = index.data(Qt::DisplayRole).toString();
        QString label = index.sibling(index.row(), ColdStakingModel::OWNER_ADDRESS_LABEL).data(Qt::DisplayRole).toString();
        if (label.isEmpty()) {
            label = QObject::tr("Address with no label");
        }
        bool isWhitelisted = index.sibling(index.row(), ColdStakingModel::IS_WHITELISTED).data(Qt::DisplayRole).toBool();
        QString amountStr = index.sibling(index.row(), ColdStakingModel::TOTAL_STACKEABLE_AMOUNT_STR).data(Qt::DisplayRole).toString();
        bool isReceivedDelegation = index.sibling(index.row(), ColdStakingModel::IS_RECEIVED_DELEGATION).data(Qt::DisplayRole).toBool();
        row->updateView(address, label, isWhitelisted, isReceivedDelegation, amountStr);
        row->showMenuButton(isReceivedDelegation);
    }

    QColor rectColor(bool isHovered, bool isSelected) override
    {
        return getRowColor(isLightTheme, isHovered, isSelected);
    }

    ~CSDelegationHolder() override
    {
        if (cachedRow)
            delete cachedRow;
    }

    bool isLightTheme;
private:
    CSRow *cachedRow = nullptr;
};

ColdStakingWidget::ColdStakingWidget(PIVXGUI* parent) :
    PWidget(parent),
    ui(new Ui::ColdStakingWidget),
    isLoading(false)
{
    ui->setupUi(this);
    this->setStyleSheet(parent->styleSheet());

    /* Containers */
    setCssProperty(ui->left, "container");
    ui->left->setContentsMargins(0,20,0,0);
    setCssProperty(ui->right, "container-right");
    ui->right->setContentsMargins(0,10,0,20);

    /* Light Font */
    QFont fontLight;
    fontLight.setWeight(QFont::Light);

    /* Title */
    setCssTitleScreen(ui->labelTitle);
    ui->labelTitle->setFont(fontLight);

    /* Button Group */
    setCssProperty(ui->pushLeft, "btn-check-left");
    setCssProperty(ui->pushRight, "btn-check-right");

    /* Subtitle */
    setCssSubtitleScreen(ui->labelSubtitle1);
    spacerDiv = new QSpacerItem(40, 20, QSizePolicy::Maximum, QSizePolicy::Expanding);

    setCssProperty(ui->labelSubtitleDescription, "text-title");
    btnOwnerContact = ui->lineEditOwnerAddress->addAction(QIcon("://ic-contact-arrow-down"), QLineEdit::TrailingPosition);
    setCssProperty(ui->lineEditOwnerAddress, "edit-primary-multi-book");
    ui->lineEditOwnerAddress->setAttribute(Qt::WA_MacShowFocusRect, 0);
    setShadow(ui->lineEditOwnerAddress);
    connect(ui->lineEditOwnerAddress, &QLineEdit::textChanged, this, &ColdStakingWidget::onOwnerAddressChanged);

    setCssSubtitleScreen(ui->labelSubtitle2);
    ui->labelSubtitle2->setContentsMargins(0,2,0,0);

    setCssBtnPrimary(ui->pushButtonSend);
    setCssBtnSecondary(ui->pushButtonClear);

    connect(ui->pushButtonClear, &QPushButton::clicked, this, &ColdStakingWidget::clearAll);

    setCssProperty(ui->labelEditTitle, "text-title");
    sendMultiRow = new SendMultiRow(window, this);
    sendMultiRow->setOnlyStakingAddressAccepted(true);
    ((QVBoxLayout*)ui->containerSend->layout())->insertWidget(1, sendMultiRow);
    connect(sendMultiRow, &SendMultiRow::onContactsClicked, [this](){ onContactsClicked(false); });

    // List
    setCssProperty(ui->labelStakingTotal, "text-title-right");
    setCssProperty(ui->labelListHistory, "text-title");
    setCssProperty(ui->pushImgEmpty, "img-empty-transactions");
    setCssProperty(ui->labelEmpty, "text-empty");

    ui->btnCoinControl->setTitleClassAndText("btn-title-grey", tr("Coin Control"));
    ui->btnCoinControl->setSubTitleClassAndText("text-subtitle", tr("Select %1 outputs to delegate.").arg(CURRENCY_UNIT.c_str()));

    ui->btnColdStaking->setTitleClassAndText("btn-title-grey", tr("Create Cold Staking Address"));
    ui->btnColdStaking->setSubTitleClassAndText("text-subtitle", tr("Creates an address to receive delegated coins\nand stake them on their owner's behalf."));
    ui->btnColdStaking->layout()->setMargin(0);

    connect(ui->btnCoinControl, &OptionButton::clicked, this, &ColdStakingWidget::onCoinControlClicked);
    connect(ui->btnColdStaking, &OptionButton::clicked, this, &ColdStakingWidget::onColdStakeClicked);

    onDelegateSelected(true);
    ui->pushRight->setChecked(true);
    connect(ui->pushLeft, &QPushButton::clicked, [this](){onDelegateSelected(false);});
    connect(ui->pushRight,  &QPushButton::clicked, [this](){onDelegateSelected(true);});

    // List
    setCssProperty(ui->listView, "container");
    txHolder = new CSDelegationHolder(isLightTheme());
    delegate = new FurAbstractListItemDelegate(
                DECORATION_SIZE,
                txHolder,
                this
    );

    addressHolder = new AddressHolder(isLightTheme());
    addressDelegate = new FurAbstractListItemDelegate(
            DECORATION_SIZE,
            addressHolder,
            this
    );

    ui->listView->setItemDelegate(delegate);
    ui->listView->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listView->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listView->setAttribute(Qt::WA_MacShowFocusRect, false);
    ui->listView->setSelectionBehavior(QAbstractItemView::SelectRows);

    ui->btnMyStakingAddresses->setChecked(true);
    ui->listViewStakingAddress->setVisible(false);

    ui->btnMyStakingAddresses->setTitleClassAndText("btn-title-grey", tr("My Cold Staking Addresses"));
    ui->btnMyStakingAddresses->setSubTitleClassAndText("text-subtitle", tr("List your own cold staking addresses."));
    ui->btnMyStakingAddresses->layout()->setMargin(0);
    ui->btnMyStakingAddresses->setRightIconClass("ic-arrow");

    // List Addresses
    setCssProperty(ui->listViewStakingAddress, "container");
    ui->listViewStakingAddress->setItemDelegate(addressDelegate);
    ui->listViewStakingAddress->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listViewStakingAddress->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listViewStakingAddress->setAttribute(Qt::WA_MacShowFocusRect, false);
    ui->listViewStakingAddress->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->listViewStakingAddress->setUniformItemSizes(true);

    // Sort Controls
    SortEdit* lineEdit = new SortEdit(ui->comboBoxSort);
    connect(lineEdit, &SortEdit::Mouse_Pressed, [this](){ui->comboBoxSort->showPopup();});
    connect(ui->comboBoxSort, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, &ColdStakingWidget::onSortChanged);
    SortEdit* lineEditOrder = new SortEdit(ui->comboBoxSortOrder);
    connect(lineEditOrder, &SortEdit::Mouse_Pressed, [this](){ui->comboBoxSortOrder->showPopup();});
    connect(ui->comboBoxSortOrder, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, &ColdStakingWidget::onSortOrderChanged);
    fillAddressSortControls(lineEdit, lineEditOrder, ui->comboBoxSort, ui->comboBoxSortOrder);
    ui->sortWidget->setVisible(false);

    connect(ui->pushButtonSend, &QPushButton::clicked, this, &ColdStakingWidget::onSendClicked);
    connect(btnOwnerContact, &QAction::triggered, [this](){ onContactsClicked(true); });
    connect(ui->listView, &QListView::clicked, this, &ColdStakingWidget::handleAddressClicked);
    connect(ui->listViewStakingAddress, &QListView::clicked, this, &ColdStakingWidget::handleMyColdAddressClicked);
    connect(ui->btnMyStakingAddresses, &OptionButton::clicked, this, &ColdStakingWidget::onMyStakingAddressesClicked);

    coinControlDialog = new CoinControlDialog(nullptr, true);
}

void ColdStakingWidget::loadWalletModel()
{
    if (walletModel) {
        sendMultiRow->setWalletModel(walletModel);
        txModel = walletModel->getTransactionTableModel();
        csModel = new ColdStakingModel(walletModel, txModel, walletModel->getAddressTableModel(), this);
        ui->listView->setModel(csModel);
        ui->listView->setModelColumn(ColdStakingModel::OWNER_ADDRESS);

        addressTableModel = walletModel->getAddressTableModel();
        addressesFilter = new AddressFilterProxyModel(AddressTableModel::ColdStaking, this);
        addressesFilter->setSourceModel(addressTableModel);
        addressesFilter->sort(sortType, sortOrder);
        ui->listViewStakingAddress->setModel(addressesFilter);
        ui->listViewStakingAddress->setModelColumn(AddressTableModel::Address);

        connect(txModel, &TransactionTableModel::txArrived, this, &ColdStakingWidget::onTxArrived);

        updateDisplayUnit();

        ui->containerHistoryLabel->setVisible(false);
        ui->emptyContainer->setVisible(false);
        ui->listView->setVisible(false);
    }

}

void ColdStakingWidget::onTxArrived(const QString& hash, const bool isCoinStake, const bool isCSAnyType)
{
    if (isCSAnyType) {
        tryRefreshDelegations();
    }
}

void ColdStakingWidget::walletSynced(bool sync)
{
    if (this->isChainSync != sync) {
        this->isChainSync = sync;
        tryRefreshDelegations();
    }
}

void ColdStakingWidget::showEvent(QShowEvent *event)
{
    tryRefreshDelegations();
}

void ColdStakingWidget::tryRefreshDelegations()
{
    if (!isVisible()) return;
    // Check for min update time to not reload the UI so often if the node is syncing.
    int64_t now = GetTime();
    if (lastRefreshTime + LOAD_MIN_TIME_INTERVAL < now) {
        lastRefreshTime = now;
        refreshDelegations();
    }
}

bool ColdStakingWidget::refreshDelegations()
{
    if (isLoading) return false;
    isLoading = true;
    return execute(REQUEST_LOAD_TASK);
}

void ColdStakingWidget::onDelegationsRefreshed()
{
    isLoading = false;
    bool hasDel = csModel->rowCount() > 0;

    updateStakingTotalLabel();

    // Update list if we are showing that section.
    if (!isInDelegation) {
        showList(hasDel);
        ui->labelStakingTotal->setVisible(hasDel);
    }
}

void ColdStakingWidget::run(int type)
{
    if (type == REQUEST_LOAD_TASK) {
        csModel->updateCSList();
        QMetaObject::invokeMethod(this, "onDelegationsRefreshed", Qt::QueuedConnection);
    }
}
void ColdStakingWidget::onError(QString error, int type)
{
    isLoading = false;
    inform(tr("Error loading delegations: %1").arg(error));
}

void ColdStakingWidget::onContactsClicked(bool ownerAdd)
{
    isContactOwnerSelected = ownerAdd;
    onContactsClicked();
}

void ColdStakingWidget::onContactsClicked()
{
    if (menu && menu->isVisible()) {
        menu->hide();
    }

    int contactsSize = isContactOwnerSelected ? walletModel->getAddressTableModel()->sizeRecv() : walletModel->getAddressTableModel()->sizeColdSend();
    if (contactsSize == 0) {
        inform(isContactOwnerSelected ?
                 tr( "No receive addresses available, you can go to the receive screen and create some there!") :
                 tr("No contacts available, you can go to the contacts screen and add some there!")
        );
        return;
    }

    int height;
    int width;
    QPoint pos;

    if (isContactOwnerSelected) {
        height = ui->lineEditOwnerAddress->height();
        width = ui->lineEditOwnerAddress->width();
        pos = ui->containerSend->rect().bottomLeft();
        pos.setY((pos.y() + (height - 12) * 3));
    } else {
        height = sendMultiRow->getEditHeight();
        width = sendMultiRow->getEditWidth();
        pos = sendMultiRow->getEditLineRect().bottomLeft();
        pos.setY((pos.y() + (height - 14) * 4));
    }

    int margin1, margin2;
    ui->verticalLayoutTop->getContentsMargins(&margin1, nullptr, nullptr, nullptr);
    ui->vContainerOwner->getContentsMargins(&margin2, nullptr, nullptr, nullptr);
    pos.setX(pos.x() + margin1 + margin2);

    height = (contactsSize <= 2) ? height * ( 2 * (contactsSize + 1 )) : height * 6;

    if (!menuContacts) {
        menuContacts = new ContactsDropdown(
                width,
                height,
                this
        );
        connect(menuContacts, &ContactsDropdown::contactSelected, [this](QString address, QString label) {
            if (isContactOwnerSelected) {
                ui->lineEditOwnerAddress->setText(address);
            } else {
                sendMultiRow->setLabel(label);
                sendMultiRow->setAddress(address);
            }
        });
    }

    if (menuContacts->isVisible()) {
        menuContacts->hide();
        return;
    }

    menuContacts->setWalletModel(walletModel, {(isContactOwnerSelected ? AddressTableModel::Receive : AddressTableModel::ColdStakingSend)});
    menuContacts->resizeList(width, height);
    menuContacts->setStyleSheet(styleSheet());
    menuContacts->adjustSize();
    menuContacts->move(pos);
    menuContacts->show();
}

void ColdStakingWidget::onDelegateSelected(bool delegate)
{
    isInDelegation = delegate;
    if (menu && menu->isVisible()) {
        menu->hide();
    }

    if (menuAddresses && menuAddresses->isVisible()) {
        menuAddresses->hide();
    }

    if (isInDelegation) {
        ui->btnCoinControl->setVisible(true);
        ui->containerSend->setVisible(true);
        ui->containerBtn->setVisible(true);
        ui->emptyContainer->setVisible(false);
        ui->listView->setVisible(false);
        ui->containerHistoryLabel->setVisible(false);
        ui->btnColdStaking->setVisible(false);
        ui->btnMyStakingAddresses->setVisible(false);
        ui->listViewStakingAddress->setVisible(false);
        ui->sortWidget->setVisible(false);
        ui->rightContainer->addItem(spacerDiv);
    } else {
        ui->btnCoinControl->setVisible(false);
        ui->containerSend->setVisible(false);
        ui->containerBtn->setVisible(false);
        ui->btnColdStaking->setVisible(true);
        showList(csModel->rowCount() > 0);
        ui->btnMyStakingAddresses->setVisible(true);
        // Show address list, if it was previously open
        ui->listViewStakingAddress->setVisible(isStakingAddressListVisible);
        ui->sortWidget->setVisible(isStakingAddressListVisible);
    }
}

void ColdStakingWidget::updateDisplayUnit() {
    if (walletModel && walletModel->getOptionsModel()) {
        nDisplayUnit = walletModel->getOptionsModel()->getDisplayUnit();
    }
}

void ColdStakingWidget::showList(bool show)
{
    ui->emptyContainer->setVisible(!show);
    ui->listView->setVisible(show);
    ui->containerHistoryLabel->setVisible(show);
}

void ColdStakingWidget::onSendClicked()
{
    if (!walletModel || !walletModel->getOptionsModel())
        return;

    if (!walletModel->isColdStakingNetworkelyEnabled()) {
        inform(tr("Cold staking is networkely disabled"));
        return;
    }

    if (!sendMultiRow->validate()) {
        inform(tr("Invalid entry"));
        return;
    }

    SendCoinsRecipient dest = sendMultiRow->getValue();
    dest.isP2CS = true;

    // Amount must be >= minColdStakingAmount
    const CAmount& minColdStakingAmount = walletModel->getMinColdStakingAmount();
    if (dest.amount < minColdStakingAmount) {
        inform(tr("Invalid entry, minimum delegable amount is ") +
               BitcoinUnits::formatWithUnit(nDisplayUnit, minColdStakingAmount));
        return;
    }

    QString inputOwner = ui->lineEditOwnerAddress->text();
    bool isOwnerEmpty = inputOwner.isEmpty();
    if (!isOwnerEmpty && !walletModel->validateAddress(inputOwner)) {
        inform(tr("Owner address invalid"));
        return;
    }


    bool isStakingAddressFromThisWallet = walletModel->isMine(dest.address);
    bool isOwnerAddressFromThisWallet = isOwnerEmpty || walletModel->isMine(inputOwner);

    // Warn the user if the owner address is not from this wallet
    if (!isOwnerAddressFromThisWallet && !ask(tr("ALERT!"),
                tr("Delegating to an external owner address!\n\n"
                   "The delegated coins will NOT be spendable by this wallet.\nSpending these coins will need to be done from the wallet or\ndevice containing the owner address.\n\n"
                   "Do you wish to proceed?"))) {
            return;
    }

    // Don't try to delegate the balance if both addresses are from this wallet
    if (isStakingAddressFromThisWallet && isOwnerAddressFromThisWallet) {
        inform(tr("Staking address corresponds to this wallet, change it to an external node"));
        return;
    }

    // Unlock wallet
    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid()) {
        // Unlock wallet was cancelled
        inform(tr("Cannot send delegation, wallet locked"));
        return;
    }

    dest.ownerAddress = inputOwner;
    QList<SendCoinsRecipient> recipients;
    recipients.append(dest);

    // Prepare transaction for getting txFee earlier (exlude delegated coins)
    WalletModelTransaction currentTransaction(recipients);
    WalletModel::SendCoinsReturn prepareStatus = walletModel->prepareTransaction(&currentTransaction, coinControlDialog->coinControl, false);

    // process prepareStatus and on error generate message shown to user
    GuiTransactionsUtils::ProcessSendCoinsReturnAndInform(
            this,
            prepareStatus,
            walletModel,
            BitcoinUnits::formatWithUnit(nDisplayUnit, currentTransaction.getTransactionFee()),
            true
    );

    if (prepareStatus.status != WalletModel::OK) {
        inform(tr("Cannot create transaction."));
        return;
    }

    showHideOp(true);
    TxDetailDialog* dialog = new TxDetailDialog(window);
    dialog->setDisplayUnit(nDisplayUnit);
    dialog->setData(walletModel, &currentTransaction);
    dialog->adjustSize();
    openDialogWithOpaqueBackgroundY(dialog, window, 3, 5);

    if (dialog->isConfirm()) {
        // now send the prepared transaction
        WalletModel::SendCoinsReturn sendStatus = dialog->getStatus();
        // process sendStatus and on error generate message shown to user
        GuiTransactionsUtils::ProcessSendCoinsReturnAndInform(
                this,
                sendStatus,
                walletModel
        );

        if (sendStatus.status == WalletModel::OK) {
            clearAll();
            inform(tr("Coins delegated"));
        }
    }

    dialog->deleteLater();
}

void ColdStakingWidget::clearAll()
{
    if (sendMultiRow) sendMultiRow->clear();
    ui->lineEditOwnerAddress->clear();
    if (coinControlDialog->coinControl) {
        coinControlDialog->coinControl->SetNull();
        ui->btnCoinControl->setActive(false);
    }
}

void ColdStakingWidget::onCoinControlClicked()
{
    if (isInDelegation) {
        if (walletModel->getBalance() > 0) {
            if (!coinControlDialog->hasModel()) coinControlDialog->setModel(walletModel);
            coinControlDialog->refreshDialog();
            setCoinControlPayAmounts();
            coinControlDialog->exec();
            ui->btnCoinControl->setActive(coinControlDialog->coinControl->HasSelected());
        } else {
            inform(tr("You don't have any %1 to select.").arg(CURRENCY_UNIT.c_str()));
        }
    }
}

void ColdStakingWidget::setCoinControlPayAmounts()
{
    if (!coinControlDialog) return;
    coinControlDialog->clearPayAmounts();
    coinControlDialog->addPayAmount(sendMultiRow->getAmountValue(), false);
}

void ColdStakingWidget::onColdStakeClicked()
{
    showAddressGenerationDialog(false);
}

void ColdStakingWidget::showAddressGenerationDialog(bool isPaymentRequest)
{
    if (walletModel && !isShowingDialog) {
        WalletModel::UnlockContext ctx(walletModel->requestUnlock());
        if (!ctx.isValid()) {
            // Unlock wallet was cancelled
            inform(tr("Cannot perform operation, wallet locked"));
            return;
        }
        isShowingDialog = true;
        showHideOp(true);
        RequestDialog *dialog = new RequestDialog(window);
        dialog->setWalletModel(walletModel);
        dialog->setPaymentRequest(isPaymentRequest);
        openDialogWithOpaqueBackgroundY(dialog, window, 3.5, 12);
        if (dialog->res == 1) {
            inform(tr("URI copied to clipboard"));
        } else if (dialog->res == 2) {
            inform(tr("Address copied to clipboard"));
        }
        dialog->deleteLater();
        isShowingDialog = false;
    }
}

void ColdStakingWidget::handleMyColdAddressClicked(const QModelIndex &_index)
{
    ui->listViewStakingAddress->setCurrentIndex(_index);

    QRect rect = ui->listViewStakingAddress->visualRect(_index);
    QPoint pos = rect.topRight();
    pos.setX( parentWidget()->rect().right() - (DECORATION_SIZE * 1.5) );
    pos.setY(pos.y() + (DECORATION_SIZE * 2.5));

    QModelIndex rIndex = addressesFilter->mapToSource(_index);

    if (!menuAddresses) {
        menuAddresses = new TooltipMenu(window, this);
        menuAddresses->setEditBtnText(tr("Copy"));
        menuAddresses->setDeleteBtnText(tr("Edit"));
        menuAddresses->setCopyBtnVisible(false);
        menuAddresses->adjustSize();
        connect(menuAddresses, &TooltipMenu::message, this, &AddressesWidget::message);
        connect(menuAddresses, &TooltipMenu::onEditClicked, this, &ColdStakingWidget::onAddressCopyClicked);
        connect(menuAddresses, &TooltipMenu::onDeleteClicked, this, &ColdStakingWidget::onAddressEditClicked);
    } else {
        menuAddresses->hide();
    }

    this->addressIndex = rIndex;

    menuAddresses->move(pos);
    menuAddresses->show();
}

void ColdStakingWidget::handleAddressClicked(const QModelIndex &rIndex)
{
    bool isReceivedDelegation = rIndex.sibling(rIndex.row(), ColdStakingModel::IS_RECEIVED_DELEGATION).data(Qt::DisplayRole).toBool();

    ui->listView->setCurrentIndex(rIndex);
    QRect rect = ui->listView->visualRect(rIndex);
    QPoint pos = rect.topRight();
    pos.setX(pos.x() - (DECORATION_SIZE * 2));
    pos.setY(pos.y() + (DECORATION_SIZE * 2));

    if (!this->menu) {
        this->menu = new TooltipMenu(window, this);
        this->menu->setEditBtnText(tr("Stake"));
        this->menu->setDeleteBtnText(tr("Blacklist"));
        this->menu->setCopyBtnText(tr("Edit Label"));
        this->menu->setLastBtnText(tr("Copy owner\naddress"), 40);
        this->menu->setLastBtnVisible(true);
        this->menu->setMinimumHeight(157);
        this->menu->setFixedHeight(157);
        this->menu->setMinimumWidth(125);
        connect(this->menu, &TooltipMenu::message, this, &AddressesWidget::message);
        connect(this->menu, &TooltipMenu::onEditClicked, this, &ColdStakingWidget::onEditClicked);
        connect(this->menu, &TooltipMenu::onDeleteClicked, this, &ColdStakingWidget::onDeleteClicked);
        connect(this->menu, &TooltipMenu::onCopyClicked, [this](){onLabelClicked();});
        connect(this->menu, &TooltipMenu::onLastClicked, this, &ColdStakingWidget::onCopyOwnerClicked);
    } else {
        this->menu->hide();
    }

    this->index = rIndex;

    if (isReceivedDelegation) {
        bool isWhitelisted = rIndex.sibling(rIndex.row(), ColdStakingModel::IS_WHITELISTED).data(
                Qt::DisplayRole).toBool();
        this->menu->setDeleteBtnVisible(isWhitelisted);
        this->menu->setEditBtnVisible(!isWhitelisted);
        this->menu->setCopyBtnVisible(true);
        this->menu->setMinimumHeight(157);
    } else {
        // owner side
        this->menu->setDeleteBtnVisible(false);
        this->menu->setEditBtnVisible(false);
        this->menu->setCopyBtnVisible(false);
        this->menu->setMinimumHeight(60);
    }

    this->menu->adjustSize();

    menu->move(pos);
    menu->show();
}

void ColdStakingWidget::onAddressCopyClicked()
{
    GUIUtil::setClipboard(addressIndex.data(Qt::DisplayRole).toString());
    inform(tr("Address copied"));
}

void ColdStakingWidget::onAddressEditClicked()
{
    onLabelClicked(
            tr("Edit Cold Address Label"),
            addressIndex,
            false
    );
}

void ColdStakingWidget::onEditClicked()
{
    // whitelist address
    if (!csModel->whitelist(index)) {
        inform(tr("Whitelist failed, please check the logs"));
        return;
    }
    QString label = index.sibling(index.row(), ColdStakingModel::OWNER_ADDRESS_LABEL).data(Qt::DisplayRole).toString();
    if (label.isEmpty()) {
        label = index.data(Qt::DisplayRole).toString();
    }
    updateStakingTotalLabel();
    inform(label + tr(" staking!"));
}

void ColdStakingWidget::onDeleteClicked()
{
    // blacklist address
    if (!csModel->blacklist(index)) {
        inform(tr("Blacklist failed, please check the logs"));
        return;
    }
    QString label = index.sibling(index.row(), ColdStakingModel::OWNER_ADDRESS_LABEL).data(Qt::DisplayRole).toString();
    if (label.isEmpty()) {
        label = index.data(Qt::DisplayRole).toString();
    }
    updateStakingTotalLabel();
    inform(label + tr(" blacklisted from staking"));
}

void ColdStakingWidget::onCopyClicked()
{
    // show address info
}

void ColdStakingWidget::onCopyOwnerClicked()
{
    QString owner = index.data(Qt::DisplayRole).toString();
    GUIUtil::setClipboard(owner);
    inform(tr("Owner address copied"));
}

void ColdStakingWidget::onLabelClicked()
{
    onLabelClicked(
            tr("Edit Owner Address Label"),
            index,
            false
    );
}

void ColdStakingWidget::onLabelClicked(QString dialogTitle, const QModelIndex &index, const bool isMyColdStakingAddresses)
{
    if (walletModel && !isShowingDialog) {
        isShowingDialog = true;
        showHideOp(true);
        AddNewContactDialog *dialog = new AddNewContactDialog(window);
        dialog->setTexts(dialogTitle);
        QString qAddress = index.data(Qt::DisplayRole).toString();
        dialog->setData(qAddress, walletModel->getAddressTableModel()->labelForAddress(qAddress));
        if (openDialogWithOpaqueBackgroundY(dialog, window, 3.5, 6)) {
            QString label = dialog->getLabel();
            std::string stdString = qAddress.toStdString();
            std::string purpose = walletModel->getAddressTableModel()->purposeForAddress(stdString);
            const CTxDestination address = DecodeDestination(stdString.data());
            if (!label.isEmpty() && walletModel->updateAddressBookLabels(
                    address,
                    label.toUtf8().constData(),
                    purpose
            )) {
                if (isMyColdStakingAddresses) {
                    addressTableModel->notifyChange(index);
                } else
                    csModel->updateCSList();
                inform(tr("Address label saved"));
            } else {
                inform(tr("Error storing address label"));
            }
        }
        isShowingDialog = false;
    }
}

void ColdStakingWidget::onMyStakingAddressesClicked()
{
    isStakingAddressListVisible = !ui->listViewStakingAddress->isVisible();
    ui->btnMyStakingAddresses->setRightIconClass((isStakingAddressListVisible ?
                                                  "btn-dropdown" : "ic-arrow"), true);
    ui->listViewStakingAddress->setVisible(isStakingAddressListVisible);
    if (isStakingAddressListVisible) {
        ui->sortWidget->setVisible(true);
        ui->rightContainer->removeItem(spacerDiv);
        ui->listViewStakingAddress->update();
    } else {
        ui->sortWidget->setVisible(false);
        ui->rightContainer->addItem(spacerDiv);
    }
}

void ColdStakingWidget::onOwnerAddressChanged()
{
    const bool isValid = ui->lineEditOwnerAddress->text().isEmpty() || (
            walletModel && walletModel->validateAddress(ui->lineEditOwnerAddress->text()));

    setCssProperty(ui->lineEditOwnerAddress, isValid ? "edit-primary-multi-book" : "edit-primary-multi-book-error", true);
}

void ColdStakingWidget::changeTheme(bool isLightTheme, QString& theme)
{
    static_cast<CSDelegationHolder*>(delegate->getRowFactory())->isLightTheme = isLightTheme;
    static_cast<AddressHolder*>(addressDelegate->getRowFactory())->isLightTheme = isLightTheme;
    ui->listView->update();
}

void ColdStakingWidget::updateStakingTotalLabel()
{
    const CAmount& total = csModel->getTotalAmount();
    ui->labelStakingTotal->setText(tr("Total Staking: %1").arg(
            (total == 0) ? "0.00 " + QString(CURRENCY_UNIT.c_str()) : GUIUtil::formatBalance(total, nDisplayUnit))
    );
}

void ColdStakingWidget::onSortChanged(int idx)
{
    sortType = (AddressTableModel::ColumnIndex) ui->comboBoxSort->itemData(idx).toInt();
    sortAddresses();
}

void ColdStakingWidget::onSortOrderChanged(int idx)
{
    sortOrder = (Qt::SortOrder) ui->comboBoxSortOrder->itemData(idx).toInt();
    sortAddresses();
}

void ColdStakingWidget::sortAddresses()
{
    if (this->addressesFilter)
        this->addressesFilter->sort(sortType, sortOrder);
}


ColdStakingWidget::~ColdStakingWidget()
{
    if (sendMultiRow)
        delete sendMultiRow;
    if (txHolder)
        delete txHolder;
    if (csModel)
        delete csModel;
    if (addressHolder)
        delete addressHolder;

    ui->rightContainer->removeItem(spacerDiv);
    delete spacerDiv;
    delete ui;
    delete coinControlDialog;
}
