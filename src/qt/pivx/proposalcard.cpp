// Copyright (c) 2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/proposalcard.h"
#include "qt/pivx/forms/ui_proposalcard.h"

#include "qt/pivx/qtutils.h"

ProposalCard::ProposalCard(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::ProposalCard)
{
    ui->setupUi(this);
    setStyleSheet(parent->styleSheet());
    setCssProperty(ui->btnVote, "btn-primary");
    setCssProperty(ui->card, "card-governance");
    setCssProperty(ui->labelPropName, "card-title");
    setCssProperty(ui->labelPropAmount, "card-amount");
    setCssProperty(ui->labelPropMonths, "card-time");
    setCssProperty(ui->labelStatus, "card-status-passing");
    setCssProperty(ui->btnVote, "card-btn-vote");
    setCssProperty(ui->btnLink, "btn-link");
    setCssProperty(ui->containerVotes, "card-progress-box");
    ui->containerVotes->setContentsMargins(1,1,1,1);
    ui->containerVotes->layout()->setMargin(0);
    ui->votesBar->setMaximum(100);
    ui->votesBar->setMinimum(0);
    ui->votesBar->setTextVisible(false);
    setCssProperty(ui->votesBar, "vote-progress");
    ui->votesBar->setContentsMargins(0,0,0,0);

    connect(ui->btnVote, &QPushButton::clicked, [this](){ Q_EMIT voteClicked(); });
    connect(ui->btnLink, &QPushButton::clicked, this, &ProposalCard::onCopyUrlClicked);
}


void ProposalCard::setProposal(const ProposalInfo& _proposalInfo)
{
    proposalInfo = _proposalInfo;
    ui->labelPropName->setText(QString::fromStdString(proposalInfo.name));
    ui->labelPropAmount->setText(GUIUtil::formatBalance(proposalInfo.amount));
    ui->labelPropMonths->setText(tr("%1 months passed of %2")
        .arg(proposalInfo.totalPayments - proposalInfo.remainingPayments).arg(proposalInfo.totalPayments));
    double totalVotes = _proposalInfo.votesYes + _proposalInfo.votesNo;
    double percentageNo = (totalVotes == 0) ? 0 :  (_proposalInfo.votesNo / totalVotes) * 100;
    double percentageYes = (totalVotes == 0) ? 0 : (_proposalInfo.votesYes / totalVotes) * 100;
    ui->votesBar->setValue((int)percentageNo);
    ui->labelNo->setText(QString::fromStdString(std::to_string((int)percentageNo) + "% No"));
    ui->labelYes->setText(QString::fromStdString("Yes "+ std::to_string((int)percentageYes) + "%"));

    QString cssClassStatus;
    if (percentageYes < percentageNo) {
        cssClassStatus = "card-status-not-passing";
        ui->labelStatus->setText(tr("Not Passing"));
    } else if (percentageYes > percentageNo) {
        cssClassStatus = "card-status-passing";
        ui->labelStatus->setText(tr("Passing"));
    } else {
        cssClassStatus = "card-status-no-votes";
        ui->labelStatus->setText(tr("No Votes"));
    }
    setCssProperty(ui->labelStatus, cssClassStatus, true);
}

void ProposalCard::onCopyUrlClicked()
{
    // todo: add copy
}

ProposalCard::~ProposalCard()
{
    delete ui;
}
