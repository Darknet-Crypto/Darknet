// Copyright (c) 2019 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/requestdialog.h"
#include "qt/pivx/forms/ui_requestdialog.h"
#include <QListView>

#include "qt/pivx/qtutils.h"
#include "guiutil.h"
#include "amount.h"
#include "optionsmodel.h"

RequestDialog::RequestDialog(QWidget *parent) :
    FocusedDialog(parent),
    ui(new Ui::RequestDialog)
{
    ui->setupUi(this);

    this->setStyleSheet(parent->styleSheet());
    setCssProperty(ui->frame, "container-dialog");

    // Text
    setCssProperty(ui->labelTitle, "text-title-dialog");
    setCssProperty(ui->labelMessage, "text-main-grey");

    // Combo Coins
    setCssProperty(ui->comboBoxCoin, "btn-combo-coins");
    setCssProperty(ui->comboContainer, "container-purple");

    // Label
    setCssProperty(ui->labelSubtitleLabel, "text-title2-dialog");
    setCssEditLineDialog(ui->lineEditLabel, true);

    // Amount
    setCssProperty(ui->labelSubtitleAmount, "text-title2-dialog");
    setCssEditLineDialog(ui->lineEditAmount, true);
    GUIUtil::setupAmountWidget(ui->lineEditAmount, this);

    // Description
    setCssProperty(ui->labelSubtitleDescription, "text-title2-dialog");
    setCssEditLineDialog(ui->lineEditDescription, true);

    // Stack
    ui->stack->setCurrentIndex(pos);
    // Request QR Page
    // Address
    ui->labelAddress->setText(tr("Error"));
    setCssProperty(ui->labelAddress, "text-main-grey-big");

    // Buttons
    setCssProperty(ui->btnEsc, "ic-close");
    setCssProperty(ui->btnCancel, "btn-dialog-cancel");
    setCssBtnPrimary(ui->btnSave);
    setCssBtnPrimary(ui->btnCopyAddress);
    setCssBtnPrimary(ui->btnCopyUrl);

    connect(ui->btnCancel, &QPushButton::clicked, this, &RequestDialog::close);
    connect(ui->btnEsc, &QPushButton::clicked, this, &RequestDialog::close);
    connect(ui->btnSave, &QPushButton::clicked, this, &RequestDialog::accept);
    // TODO: Change copy address for save image (the method is already implemented in other class called exportQr or something like that)
    connect(ui->btnCopyAddress, &QPushButton::clicked, this, &RequestDialog::onCopyClicked);
    connect(ui->btnCopyUrl, &QPushButton::clicked, this, &RequestDialog::onCopyUriClicked);
}

void RequestDialog::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
}

void RequestDialog::setPaymentRequest(bool isPaymentRequest)
{
    this->isPaymentRequest = isPaymentRequest;
    if (!this->isPaymentRequest) {
        ui->labelMessage->setText(tr("Creates an address to receive coin delegations and be able to stake them."));
        ui->labelTitle->setText(tr("New Cold Staking Address"));
        ui->labelSubtitleAmount->setText(tr("Amount (optional)"));
    }
}

void RequestDialog::accept()
{
    if (walletModel) {
        QString labelStr = ui->lineEditLabel->text();

        //Amount
        int displayUnit = walletModel->getOptionsModel()->getDisplayUnit();
        bool isValueValid = true;
        CAmount value = (ui->lineEditAmount->text().isEmpty() ?
                            0 :
                            GUIUtil::parseValue(ui->lineEditAmount->text(), displayUnit, &isValueValid)
                        );

        if (!this->isPaymentRequest) {
            // Add specific checks for cold staking address creation
            if (labelStr.isEmpty()) {
                inform(tr("Address label cannot be empty"));
                return;
            }
        }

        if (value < 0 || !isValueValid) {
            inform(tr("Invalid amount"));
            return;
        }

        info = new SendCoinsRecipient();
        info->label = labelStr;
        info->amount = value;
        info->message = ui->lineEditDescription->text();

        // address
        std::string label = info->label.isEmpty() ? "" : info->label.toStdString();
        QString title;

        CallResult<Destination> r(false, "");
        if (this->isPaymentRequest) {
            r = walletModel->getNewAddress(label);
            title = tr("Request for ") + BitcoinUnits::format(displayUnit, value, false, BitcoinUnits::separatorAlways) + " " + QString(CURRENCY_UNIT.c_str());
        } else {
            r = walletModel->getNewStakingAddress(label);
            title = tr("Cold Staking Address Generated");
        }

        if (!r) {
            // TODO: notify user about this error
            close();
            return;
        }

        info->address = QString::fromStdString(r.getObjResult()->ToString());
        ui->labelTitle->setText(title);

        updateQr(info->address);
        ui->labelAddress->setText(info->address);
        ui->buttonsStack->setVisible(false);
        pos = 1;
        ui->stack->setCurrentIndex(pos);
    }
}

void RequestDialog::onCopyClicked()
{
    if (info) {
        GUIUtil::setClipboard(info->address);
        res = 2;
        QDialog::accept();
    }
}

void RequestDialog::onCopyUriClicked()
{
    if (info) {
        GUIUtil::setClipboard(GUIUtil::formatBitcoinURI(*info));
        res = 1;
        QDialog::accept();
    }
}

void RequestDialog::showEvent(QShowEvent *event)
{
    if (ui->lineEditAmount) ui->lineEditAmount->setFocus();
}

void RequestDialog::updateQr(QString str)
{
    QString uri = GUIUtil::formatBitcoinURI(*info);
    ui->labelQrImg->setText("");
    QString error;
    QPixmap pixmap = encodeToQr(uri, error);
    if (!pixmap.isNull()) {
        qrImage = &pixmap;
        ui->labelQrImg->setPixmap(qrImage->scaled(ui->labelQrImg->width(), ui->labelQrImg->height()));
    } else {
        ui->labelQrImg->setText(!error.isEmpty() ? error : "Error encoding address");
    }
}

void RequestDialog::inform(QString text)
{
    if (!snackBar)
        snackBar = new SnackBar(nullptr, this);
    snackBar->setText(text);
    snackBar->resize(this->width(), snackBar->height());
    openDialog(snackBar, this);
}

RequestDialog::~RequestDialog()
{
    delete ui;
}
