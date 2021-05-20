// Copyright (c) 2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef PROPOSALCARD_H
#define PROPOSALCARD_H

#include "qt/pivx/governancemodel.h"

#include <QGridLayout>
#include <QProgressBar>
#include <QWidget>

namespace Ui {
class ProposalCard;
}

class ProposalCard : public QWidget
{
    Q_OBJECT

public:
    explicit ProposalCard(QWidget *parent = nullptr);
    ~ProposalCard();

    void setProposal(const ProposalInfo& _proposalInfo);
    ProposalInfo getProposal() { return proposalInfo; }

    // Update-only functions
    void setNeedsUpdate(bool _update) { needsUpdate = _update; }
    bool isUpdateNeeded() { return needsUpdate; }

public Q_SLOTS:
    void onCopyUrlClicked();

Q_SIGNALS:
    void voteClicked();

private:
    Ui::ProposalCard *ui;
    ProposalInfo proposalInfo;
    bool needsUpdate{false};
};

#endif // PROPOSALCARD_H
