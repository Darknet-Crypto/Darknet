// Copyright (c) 2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/votedialog.h"
#include "qt/pivx/forms/ui_votedialog.h"

#include "qt/pivx/mnmodel.h"
#include "qt/pivx/mnselectiondialog.h"
#include "qt/pivx/qtutils.h"

VoteDialog::VoteDialog(QWidget *parent, GovernanceModel* _govModel, MNModel* _mnModel) :
    QDialog(parent),
    ui(new Ui::VoteDialog),
    govModel(_govModel),
    mnModel(_mnModel)
{
    ui->setupUi(this);
    this->setStyleSheet(parent->styleSheet());
    setCssProperty(ui->frame, "container-dialog");
    setCssProperty(ui->labelTitle, "text-title-dialog");
    setCssProperty(ui->labelSubtitle, "text-subtitle");

    // Vote Info
    setCssProperty(ui->labelTitleVote, "vote-title");
    setCssProperty(ui->labelAmount, "vote-amount");
    ui->labelAmount->setAlignment(Qt::AlignCenter);
    setCssProperty(ui->labelTime, "vote-time");
    ui->labelTime->setAlignment(Qt::AlignCenter);
    setCssProperty(ui->labelMessage, "vote-message");
    ui->labelMessage->setAlignment(Qt::AlignCenter);

    setCssProperty(ui->btnEsc, "ic-close");
    setCssProperty(ui->btnCancel, "btn-dialog-cancel");
    setCssProperty(ui->btnSave, "btn-primary");
    setCssProperty(ui->btnLink, "btn-link");
    setCssProperty(ui->btnSelectMasternodes, "btn-vote-select");
    setCssProperty(ui->containerNo, "card-progress-box");
    setCssProperty(ui->containerYes, "card-progress-box");

    progressBarNo = new QProgressBar(ui->containerNo);
    checkBoxNo = new QCheckBox(ui->containerNo);
    initVoteCheck(ui->containerNo, checkBoxNo, progressBarNo, "No", Qt::LayoutDirection::RightToLeft, false);

    progressBarYes = new QProgressBar(ui->containerYes);
    checkBoxYes = new QCheckBox(ui->containerYes);
    initVoteCheck(ui->containerYes, checkBoxYes, progressBarYes, "Yes", Qt::LayoutDirection::LeftToRight, true);

    connect(ui->btnSelectMasternodes, &QPushButton::clicked, this, &VoteDialog::onMnSelectionClicked);
    connect(ui->btnEsc, &QPushButton::clicked, this, &VoteDialog::close);
    connect(ui->btnCancel, &QPushButton::clicked, this, &VoteDialog::close);
    connect(ui->btnSave, &QPushButton::clicked, this, &VoteDialog::onAcceptClicked);
}

void VoteDialog::onAcceptClicked()
{
    close();
}

void VoteDialog::showEvent(QShowEvent *event)
{
    // Qt hack to solve the macOS-only extra margin issue.
    progressBarYes->setFixedWidth(progressBarYes->width() + 5);
    progressBarNo->setFixedWidth(progressBarNo->width() + 5);
}

void VoteDialog::onMnSelectionClicked()
{
    PIVXGUI* window = dynamic_cast<PIVXGUI*>(parent());
    MnSelectionDialog* dialog = new MnSelectionDialog(window);
    dialog->resize(size());
    dialog->setModel(mnModel);
    openDialogWithOpaqueBackgroundY(dialog, window, 4.5, 5, false);
    dialog->deleteLater();
}

void VoteDialog::onCheckBoxClicked(QCheckBox* checkBox, QProgressBar* progressBar, bool isVoteYes)
{
    if (isVoteYes) {
        checkBoxNo->setCheckState(Qt::Unchecked);
    } else {
        checkBoxYes->setCheckState(Qt::Unchecked);
    }
}

void VoteDialog::initVoteCheck(QWidget* container, QCheckBox* checkBox, QProgressBar* progressBar, QString text, Qt::LayoutDirection direction, bool isVoteYes)
{
    QGridLayout* gridLayout = dynamic_cast<QGridLayout*>(container->layout());
    progressBar->setMaximum(100);
    progressBar->setMinimum(0);
    progressBar->setLayoutDirection(direction);
    progressBar->setTextVisible(false);
    progressBar->setAlignment(Qt::AlignCenter);
    progressBar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    progressBar->setOrientation(Qt::Horizontal);
    progressBar->setContentsMargins(0,0,0,0);
    setCssProperty(progressBar, "vote-progress-yes");
    gridLayout->addWidget(progressBar, 0, 0, 1, 1);
    progressBar->setAttribute(Qt::WA_LayoutUsesWidgetRect);

    checkBox->setText(text);
    checkBox->setLayoutDirection(direction);
    setCssProperty(checkBox, "check-vote");
    gridLayout->addWidget(checkBox, 0, 0, 1, 1);
    setCssProperty(container, "vote-grid");
    gridLayout->setMargin(0);
    container->setContentsMargins(0,0,0,0);
    connect(checkBox, &QCheckBox::clicked, [this, checkBox, progressBar, isVoteYes](){ onCheckBoxClicked(checkBox, progressBar, isVoteYes); });
    checkBox->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    checkBox->show();

    // Progress bar
    progressBar->setValue(35);
}

VoteDialog::~VoteDialog()
{
    delete ui;
}
