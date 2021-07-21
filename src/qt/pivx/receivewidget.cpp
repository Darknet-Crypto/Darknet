// Copyright (c) 2019-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/receivewidget.h"
#include "qt/pivx/forms/ui_receivewidget.h"
#include "qt/pivx/requestdialog.h"
#include "qt/pivx/addnewcontactdialog.h"
#include "qt/pivx/qtutils.h"
#include "qt/pivx/myaddressrow.h"
#include "qt/pivx/furlistrow.h"
#include "qt/pivx/addressholder.h"
#include "walletmodel.h"
#include "guiutil.h"

#include <QModelIndex>
#include <QColor>
#include <QDateTime>

#define DECORATION_SIZE 70
#define NUM_ITEMS 3

ReceiveWidget::ReceiveWidget(PIVXGUI* parent) :
    PWidget(parent),
    ui(new Ui::ReceiveWidget)
{
    ui->setupUi(this);
    this->setStyleSheet(parent->styleSheet());

    delegate = new FurAbstractListItemDelegate(
                DECORATION_SIZE,
                new AddressHolder(isLightTheme()),
                this
                );

    // Containers
    setCssProperty(ui->left, "container");
    ui->left->setContentsMargins(20,20,20,20);
    setCssProperty(ui->right, "container-right");
    ui->right->setContentsMargins(0,9,0,0);

    // Title
    setCssTitleScreen(ui->labelTitle);
    setCssSubtitleScreen(ui->labelSubtitle1);

    // Address
    setCssProperty(ui->labelAddress, "label-address-box");

    /* Button Group */
    setCssProperty(ui->pushLeft, "btn-check-left");
    setCssProperty(ui->pushRight, "btn-check-right");
    setCssSubtitleScreen(ui->labelSubtitle2);
    ui->labelSubtitle2->setContentsMargins(0,2,4,0);

    setCssSubtitleScreen(ui->labelDate);
    setCssSubtitleScreen(ui->labelLabel);

    // Options
    ui->btnMyAddresses->setTitleClassAndText("btn-title-grey", tr("My Addresses"));
    ui->btnMyAddresses->setSubTitleClassAndText("text-subtitle", tr("List your own addresses"));
    ui->btnMyAddresses->layout()->setMargin(0);
    ui->btnMyAddresses->setRightIconClass("ic-arrow");

    ui->btnRequest->setTitleClassAndText("btn-title-grey", tr("Create Request"));
    ui->btnRequest->setSubTitleClassAndText("text-subtitle", tr("Request payment with a fixed amount"));
    ui->btnRequest->layout()->setMargin(0);

    ui->pushButtonLabel->setLayoutDirection(Qt::RightToLeft);
    setCssProperty(ui->pushButtonLabel, "btn-secundary-label");

    ui->pushButtonNewAddress->setLayoutDirection(Qt::RightToLeft);
    setCssProperty(ui->pushButtonNewAddress, "btn-secundary-new-address");

    ui->pushButtonCopy->setLayoutDirection(Qt::RightToLeft);
    setCssProperty(ui->pushButtonCopy, "btn-secundary-copy");

    setCssProperty(ui->labelQrImg, "text-subtitle");

    // List Addresses
    setCssProperty(ui->listViewAddress, "container");
    ui->listViewAddress->setItemDelegate(delegate);
    ui->listViewAddress->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listViewAddress->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listViewAddress->setAttribute(Qt::WA_MacShowFocusRect, false);
    ui->listViewAddress->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->listViewAddress->setUniformItemSizes(true);

    spacer = new QSpacerItem(40, 20, QSizePolicy::Maximum, QSizePolicy::Expanding);
    ui->btnMyAddresses->setChecked(true);
    ui->container_right->addItem(spacer);
    ui->listViewAddress->setVisible(false);

    // My Address search filter
    initCssEditLine(ui->lineEditFilter, true);
    ui->lineEditFilter->setStyleSheet("font: 14px;");

    // Sort Controls
    SortEdit* lineEdit = new SortEdit(ui->comboBoxSort);
    connect(lineEdit, &SortEdit::Mouse_Pressed, [this](){ui->comboBoxSort->showPopup();});
    connect(ui->comboBoxSort, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, &ReceiveWidget::onSortChanged);
    SortEdit* lineEditOrder = new SortEdit(ui->comboBoxSortOrder);
    connect(lineEditOrder, &SortEdit::Mouse_Pressed, [this](){ui->comboBoxSortOrder->showPopup();});
    connect(ui->comboBoxSortOrder, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, &ReceiveWidget::onSortOrderChanged);
    fillAddressSortControls(lineEdit, lineEditOrder, ui->comboBoxSort, ui->comboBoxSortOrder);
    ui->sortWidget->setVisible(false);

    // Connect
    connect(ui->pushButtonLabel, &QPushButton::clicked, this, &ReceiveWidget::onLabelClicked);
    connect(ui->pushButtonCopy, &QPushButton::clicked, this, &ReceiveWidget::onCopyClicked);
    connect(ui->pushButtonNewAddress, &QPushButton::clicked, this, &ReceiveWidget::onNewAddressClicked);
    connect(ui->listViewAddress, &QListView::clicked, this, &ReceiveWidget::handleAddressClicked);
    connect(ui->btnRequest, &OptionButton::clicked, this, &ReceiveWidget::onRequestClicked);
    connect(ui->btnMyAddresses, &OptionButton::clicked, this, &ReceiveWidget::onMyAddressesClicked);
    connect(ui->lineEditFilter, &QLineEdit::textChanged, [this](){filterChanged(ui->lineEditFilter->text());});

    ui->pushLeft->setChecked(true);
    connect(ui->pushLeft, &QPushButton::clicked, [this](){onTransparentSelected(true);});
    connect(ui->pushRight,  &QPushButton::clicked, [this](){onTransparentSelected(false);});
}

void ReceiveWidget::loadWalletModel()
{
    if (walletModel) {
        this->addressTableModel = walletModel->getAddressTableModel();
        this->filter = new AddressFilterProxyModel(AddressTableModel::Receive, this);
        this->filter->setSourceModel(addressTableModel);
        this->filter->sort(sortType, sortOrder);
        ui->listViewAddress->setModel(this->filter);
        ui->listViewAddress->setModelColumn(AddressTableModel::Address);

        if (!info) info = new SendCoinsRecipient();
        refreshView();

        // data change
        connect(this->addressTableModel, &AddressTableModel::dataChanged, [this](const QModelIndex& tl, const QModelIndex& br){ refreshView(tl, br); });
    }
}

void ReceiveWidget::refreshView(const QModelIndex& tl, const QModelIndex& br)
{
    const QModelIndex& index = tl.sibling(tl.row(), AddressTableModel::Address);
    const QString& typeRole = index.data(AddressTableModel::TypeRole).toString();
    if (shieldedMode && typeRole != AddressTableModel::ShieldedReceive) return;
    if (!shieldedMode && typeRole != AddressTableModel::Receive) return;
    return refreshView(index.data(Qt::DisplayRole).toString());
}

void ReceiveWidget::refreshView(QString refreshAddress)
{
    try {
        QString latestAddress = (refreshAddress.isEmpty()) ? this->addressTableModel->getAddressToShow(shieldedMode) : refreshAddress;

        if (latestAddress.isEmpty()) {
            // Check for generation errors
            ui->labelQrImg->setText(tr("No available address\ntry unlocking the wallet"));
            inform(tr("Error generating address"));
            return;
        }

        QString addressToShow = latestAddress;
        int64_t time = walletModel->getKeyCreationTime(latestAddress.toStdString());
        if (shieldedMode) {
            addressToShow = addressToShow.left(20) + "..." + addressToShow.right(19);
        }

        ui->labelAddress->setText(addressToShow);
        ui->labelDate->setText(GUIUtil::dateTimeStr(QDateTime::fromTime_t(static_cast<uint>(time))));
        updateQr(latestAddress);
        updateLabel();
    } catch (const std::runtime_error& error) {
        ui->labelQrImg->setText(tr("No available address\ntry unlocking the wallet"));
        inform(tr("Error generating address"));
    }
}

void ReceiveWidget::updateLabel()
{
    if (!info->address.isEmpty()) {
        // Check if address label exists
        QString label = addressTableModel->labelForAddress(info->address);
        if (!label.isEmpty()) {
            ui->labelLabel->setVisible(true);
            ui->labelLabel->setText(label);
            ui->pushButtonLabel->setText(tr("Edit Label"));
        } else {
            ui->labelLabel->setVisible(false);
            ui->pushButtonLabel->setText(tr("Add Label"));
        }
    }
}

void ReceiveWidget::updateQr(QString& address)
{
    info->address = address;
    QString uri = GUIUtil::formatBitcoinURI(*info);
    ui->labelQrImg->setText("");

    QString error;
    QColor qrColor("#382d4d");
    QPixmap pixmap = encodeToQr(uri, error, qrColor);
    if (!pixmap.isNull()) {
        qrImage = &pixmap;
        ui->labelQrImg->setPixmap(qrImage->scaled(ui->labelQrImg->width(), ui->labelQrImg->height()));
    } else {
        ui->labelQrImg->setText(!error.isEmpty() ? error : "Error encoding address");
    }
}

void ReceiveWidget::handleAddressClicked(const QModelIndex &index)
{
    QModelIndex rIndex = filter->mapToSource(index);
    refreshView(rIndex.data(Qt::DisplayRole).toString());
}

void ReceiveWidget::onLabelClicked()
{
    if (walletModel && !isShowingDialog) {
        isShowingDialog = true;
        showHideOp(true);
        AddNewContactDialog *dialog = new AddNewContactDialog(window);
        dialog->setTexts(tr("Edit Address Label"));
        dialog->setData(info->address, addressTableModel->labelForAddress(info->address));
        if (openDialogWithOpaqueBackgroundY(dialog, window, 3.5, 6)) {
            QString label = dialog->getLabel();
            const CWDestination address = Standard::DecodeDestination(info->address.toUtf8().constData());
            if (!label.isEmpty() && walletModel->updateAddressBookLabels(
                    address,
                    label.toUtf8().constData(),
                    AddressBook::AddressBookPurpose::RECEIVE
            )
                    ) {
                // update label status (icon color)
                updateLabel();
                inform(tr("Address label saved"));
            } else {
                inform(tr("Error storing address label"));
            }
        }
        isShowingDialog = false;
    }
}

void ReceiveWidget::onNewAddressClicked()
{
    try {
        WalletModel::UnlockContext ctx(walletModel->requestUnlock());
        if (!ctx.isValid()) {
            // Unlock wallet was cancelled
            inform(tr("Cannot create new address, wallet locked"));
            return;
        }

        QString strAddress;
        CallResult<Destination> r(false, "");
        if (!shieldedMode) {
            r = walletModel->getNewAddress("");
            if (r) strAddress = QString::fromStdString(r.getObjResult()->ToString());
        } else {
            r = walletModel->getNewShieldedAddress(strAddress, "");
        }

        // Check validity
        if (!r) {
            inform(r.getError().c_str());
            return;
        }

        refreshView(strAddress);
        inform(tr("New address created"));
    } catch (const std::runtime_error& error) {
        // Error generating address
        inform("Error generating address");
    }
}

void ReceiveWidget::onCopyClicked()
{
    GUIUtil::setClipboard(info->address);
    inform(tr("Address copied"));
}


void ReceiveWidget::onRequestClicked()
{
    showAddressGenerationDialog(true);
}

void ReceiveWidget::showAddressGenerationDialog(bool isPaymentRequest)
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

void ReceiveWidget::onMyAddressesClicked()
{
    bool isVisible = ui->listViewAddress->isVisible();
    if (!isVisible) {
        ui->btnMyAddresses->setRightIconClass("btn-dropdown", true);
        ui->listViewAddress->setVisible(true);
        ui->sortWidget->setVisible(true);
        ui->container_right->removeItem(spacer);
        ui->listViewAddress->update();
    } else {
        ui->btnMyAddresses->setRightIconClass("ic-arrow", true);
        ui->container_right->addItem(spacer);
        ui->listViewAddress->setVisible(false);
        ui->sortWidget->setVisible(false);
    }
}

void ReceiveWidget::onSortChanged(int idx)
{
    sortType = (AddressTableModel::ColumnIndex) ui->comboBoxSort->itemData(idx).toInt();
    sortAddresses();
}

void ReceiveWidget::onSortOrderChanged(int idx)
{
    sortOrder = (Qt::SortOrder) ui->comboBoxSortOrder->itemData(idx).toInt();
    sortAddresses();
}

void ReceiveWidget::filterChanged(const QString& str)
{
    this->filter->setFilterRegExp(str);
}

void ReceiveWidget::sortAddresses()
{
    if (this->filter)
        this->filter->sort(sortType, sortOrder);
}

void ReceiveWidget::onTransparentSelected(bool transparentSelected)
{
    shieldedMode = !transparentSelected;
    refreshView();
    this->filter->setType(shieldedMode ? AddressTableModel::ShieldedReceive : AddressTableModel::Receive);
};

void ReceiveWidget::changeTheme(bool isLightTheme, QString& theme)
{
    static_cast<AddressHolder*>(this->delegate->getRowFactory())->isLightTheme = isLightTheme;
}

ReceiveWidget::~ReceiveWidget()
{
    delete ui;
}
