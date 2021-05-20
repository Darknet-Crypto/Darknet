// Copyright (c) 2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/createproposaldialog.h"
#include "qt/pivx/forms/ui_createproposaldialog.h"

#include "qt/pivx/qtutils.h"

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

CreateProposalDialog::CreateProposalDialog(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::CreateProposalDialog),
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
}

void CreateProposalDialog::setupPageTwo()
{
    setCssProperty(ui->labelTitleDest, "text-title-dialog");
    setCssProperty(ui->labelMessageDest, "dialog-proposal-message");
    setEditBoxStyle(ui->labelAmount, ui->lineEditAmount, "e.g 500 PIV");
    setEditBoxStyle(ui->labelMonths, ui->lineEditMonths, "e.g 2");
    setEditBoxStyle(ui->labelAddress, ui->lineEditAddress, "e.g D...something..");
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

void CreateProposalDialog::onNextClicked()
{
    int nextPos = pos + 1;
    switch (pos){
        case 0:{
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

CreateProposalDialog::~CreateProposalDialog()
{
    delete ui;
}
