// Copyright (c) 2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/createproposaldialog.h"
#include "qt/pivx/forms/ui_createproposaldialog.h"

#include "qt/pivx/governancemodel.h"
#include "qt/pivx/qtutils.h"
#include "qt/pivx/snackbar.h"

void initPageIndexBtn(QPushButton* btn)
{
    QSize BUTTON_SIZE = QSize(22, 22);
    setCssProperty(btn, "ic-step-confirm");
    btn->setMinimumSize(BUTTON_SIZE);
    btn->setMaximumSize(BUTTON_SIZE);
    btn->move(0, 0);
    btn->show();
    btn->raise();
    btn->setVisible(false);
}

CreateProposalDialog::CreateProposalDialog(QWidget *parent, GovernanceModel* _govModel, WalletModel* _walletModel) :
    QDialog(parent),
    ui(new Ui::CreateProposalDialog),
    govModel(_govModel),
    walletModel(_walletModel),
    icConfirm1(new QPushButton()),
    icConfirm2(new QPushButton()),
    icConfirm3(new QPushButton())
{
    ui->setupUi(this);
    this->setStyleSheet(parent->styleSheet());

    setCssProperty(ui->frame, "container-dialog");
    ui->frame->setContentsMargins(10,10,10,10);
    setCssProperty({ui->labelLine1, ui->labelLine2}, "line-purple");
    setCssProperty({ui->groupBoxName, ui->groupContainer}, "container-border");
    setCssProperty({ui->pushNumber1, ui->pushNumber2, ui->pushNumber3}, "btn-number-check");
    setCssProperty({ui->pushName1, ui->pushName2, ui->pushName3}, "btn-name-check");

    // Pages setup
    setupPageOne();
    setupPageTwo();
    setupPageThree();

    // Confirm icons
    ui->stackedIcon1->addWidget(icConfirm1);
    ui->stackedIcon2->addWidget(icConfirm2);
    ui->stackedIcon3->addWidget(icConfirm3);
    initPageIndexBtn(icConfirm1);
    initPageIndexBtn(icConfirm2);
    initPageIndexBtn(icConfirm3);

    // Connect btns
    setCssProperty(ui->btnNext, "btn-primary");
    ui->btnNext->setText(tr("NEXT"));
    setCssProperty(ui->btnBack, "btn-dialog-cancel");
    ui->btnBack->setVisible(false);
    ui->btnBack->setText(tr("BACK"));
    setCssProperty(ui->pushButtonSkip, "ic-close");

    connect(ui->pushButtonSkip, &QPushButton::clicked, this, &CreateProposalDialog::close);
    connect(ui->btnNext, &QPushButton::clicked, this, &CreateProposalDialog::onNextClicked);
    connect(ui->btnBack, &QPushButton::clicked, this, &CreateProposalDialog::onBackClicked);
}

void setEditBoxStyle(QLabel* label, QLineEdit* lineEdit, const QString& placeholderText)
{
    setCssProperty(label, "text-title");
    lineEdit->setPlaceholderText(placeholderText);
    setCssProperty(lineEdit, "edit-primary");
    lineEdit->setAttribute(Qt::WA_MacShowFocusRect, false);
    setShadow(lineEdit);
}

void CreateProposalDialog::setupPageOne()
{
    setCssProperty(ui->labelTitle1, "text-title-dialog");
    setCssProperty(ui->labelMessage1b, "dialog-proposal-message");
    setEditBoxStyle(ui->labelName, ui->lineEditPropName, "e.g Best proposal ever!");
    setEditBoxStyle(ui->labelURL, ui->lineEditURL, "e.g https://forum.pivx/proposals/best_proposal_ever");

    connect(ui->lineEditPropName, &QLineEdit::textChanged, this, &CreateProposalDialog::propNameChanged);
    connect(ui->lineEditURL, &QLineEdit::textChanged, this, &CreateProposalDialog::propUrlChanged);
}

void CreateProposalDialog::setupPageTwo()
{
    setCssProperty(ui->labelTitleDest, "text-title-dialog");
    setCssProperty(ui->labelMessageDest, "dialog-proposal-message");
    setEditBoxStyle(ui->labelAmount, ui->lineEditAmount, "e.g 500 PIV");
    setEditBoxStyle(ui->labelMonths, ui->lineEditMonths, "e.g 2");
    setEditBoxStyle(ui->labelAddress, ui->lineEditAddress, "e.g D...something..");

    ui->lineEditAmount->setValidator(new QIntValidator(1,43200, this));
    ui->lineEditMonths->setValidator(new QIntValidator(1, govModel->getPropMaxPaymentsCount(), this));

    connect(ui->lineEditAmount, &QLineEdit::textChanged, this, &CreateProposalDialog::propAmountChanged);
    connect(ui->lineEditMonths, &QLineEdit::textChanged, this, &CreateProposalDialog::propMonthsChanged);
    connect(ui->lineEditAddress, &QLineEdit::textChanged, this, &CreateProposalDialog::propaddressChanged);
}

void CreateProposalDialog::setupPageThree()
{
    setCssProperty(ui->labelTitle3, "text-title-dialog");
    ui->stackedWidget->setCurrentIndex(pos);
    setCssProperty({ui->labelResultNameTitle,
                    ui->labelResultAmountTitle,
                    ui->labelResultAddressTitle,
                    ui->labelResultMonthsTitle,
                    ui->labelResultUrlTitle},
                   "text-title");
    setCssProperty({ui->labelResultName,
                    ui->labelResultName,
                    ui->labelResultAmount,
                    ui->labelResultAddress,
                    ui->labelResultMonths,
                    ui->labelResultUrl}, "text-body1-dialog");
}

void CreateProposalDialog::propNameChanged(const QString& newText)
{
    setCssEditLine(ui->lineEditPropName, !newText.isEmpty(), true);
}

void CreateProposalDialog::propUrlChanged(const QString& newText)
{
    setCssEditLine(ui->lineEditURL, govModel->validatePropURL(newText).getRes(), true);
}

void CreateProposalDialog::propAmountChanged(const QString& newText)
{
    setCssEditLine(ui->lineEditAmount, govModel->validatePropAmount(newText.toInt()).getRes(), true);
}

void CreateProposalDialog::propMonthsChanged(const QString& newText)
{
    setCssEditLine(ui->lineEditMonths, govModel->validatePropPaymentCount(newText.toInt()).getRes(), true);
}

bool CreateProposalDialog::propaddressChanged(const QString& str)
{
    if (!str.isEmpty()) {
        QString trimmedStr = str.trimmed();
        bool isShielded = false;
        const bool valid = walletModel->validateAddress(trimmedStr, false, isShielded) && !isShielded;
        setCssEditLine(ui->lineEditAddress,  valid, true);
        return valid;
    }
    setCssEditLine(ui->lineEditAddress, true, true);
    return false;
}

bool CreateProposalDialog::validatePageOne()
{
    if (ui->lineEditPropName->text().isEmpty()) {
        inform(tr("Proposal name field cannot be empty"));
        return false;
    }
    auto res = govModel->validatePropURL(ui->lineEditURL->text());
    if (!res) inform(QString::fromStdString(res.getError()));
    return res.getRes();
}

bool CreateProposalDialog::validatePageTwo()
{
    QString sPaymentCount = ui->lineEditAmount->text();
    if (sPaymentCount.isEmpty()) {
        inform(tr("Proposal amount field cannot be empty"));
        return false;
    }

    // Amount validation
    auto opRes = govModel->validatePropAmount(ui->lineEditAmount->text().toInt());
    if (!opRes) {
        inform(QString::fromStdString(opRes.getError()));
        return false;
    }

    // Payments count validation
    opRes = govModel->validatePropPaymentCount(sPaymentCount.toInt());
    if (!opRes) {
        inform(QString::fromStdString(opRes.getError()));
        return false;
    }

    if (!propaddressChanged(ui->lineEditAddress->text())) {
        inform(tr("Invalid payment address"));
        return false;
    }

    return true;
}

void CreateProposalDialog::onNextClicked()
{
    int nextPos = pos + 1;
    switch (pos){
        case 0:{
            if (!validatePageOne()) return;
            ui->stackedWidget->setCurrentIndex(nextPos);
            ui->pushNumber2->setChecked(true);
            ui->pushName3->setChecked(false);
            ui->pushName2->setChecked(true);
            ui->pushName1->setChecked(true);
            icConfirm1->setVisible(true);
            ui->btnBack->setVisible(true);
            break;
        }
        case 1:{
            if (!validatePageTwo()) return;
            ui->stackedWidget->setCurrentIndex(nextPos);
            ui->pushNumber3->setChecked(true);
            ui->pushName3->setChecked(true);
            ui->pushName2->setChecked(true);
            ui->pushName1->setChecked(true);
            icConfirm2->setVisible(true);
            ui->btnNext->setText("Send");
            break;
        }
        case 2:{
            accept();
        }
    }
    pos = nextPos;
}

void CreateProposalDialog::onBackClicked()
{
    if (pos == 0) return;
    pos--;
    switch(pos){
        case 0:{
            ui->stackedWidget->setCurrentIndex(pos);
            ui->pushNumber1->setChecked(true);
            ui->pushNumber3->setChecked(false);
            ui->pushNumber2->setChecked(false);
            ui->pushName3->setChecked(false);
            ui->pushName2->setChecked(false);
            ui->pushName1->setChecked(true);
            icConfirm1->setVisible(false);
            ui->btnBack->setVisible(false);
            break;
        }
        case 1:{
            ui->stackedWidget->setCurrentIndex(pos);
            ui->pushNumber2->setChecked(true);
            ui->pushNumber3->setChecked(false);
            ui->pushName3->setChecked(false);
            ui->pushName2->setChecked(true);
            ui->pushName1->setChecked(true);
            icConfirm2->setVisible(false);
            ui->btnNext->setText("Next");
            break;
        }
    }
}

void CreateProposalDialog::inform(const QString& text)
{
    if (!snackBar) snackBar = new SnackBar(nullptr, this);
    snackBar->setText(text);
    snackBar->resize(this->width(), snackBar->height());
    openDialog(snackBar, this);
}

CreateProposalDialog::~CreateProposalDialog()
{
    delete ui;
}
