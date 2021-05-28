// Copyright (c) 2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef VOTEDIALOG_H
#define VOTEDIALOG_H

#include <QDialog>
#include <QCheckBox>
#include <QProgressBar>

namespace Ui {
class VoteDialog;
}

struct ProposalInfo;
struct VoteInfo;
class MNModel;
class MnSelectionDialog;
class GovernanceModel;
class SnackBar;

class VoteDialog : public QDialog
{
    Q_OBJECT

public:
    explicit VoteDialog(QWidget *parent, GovernanceModel* _govModel, MNModel* _mnModel);
    ~VoteDialog();

    void showEvent(QShowEvent *event) override;
    void setProposal(const ProposalInfo& prop);

public Q_SLOTS:
    void onAcceptClicked();
    void onCheckBoxClicked(QCheckBox* checkBox, QProgressBar* progressBar, bool isVoteYes);
    void onMnSelectionClicked();

private:
    Ui::VoteDialog *ui;
    GovernanceModel* govModel{nullptr};
    MNModel* mnModel{nullptr};
    SnackBar* snackBar{nullptr};

    QCheckBox* checkBoxNo{nullptr};
    QCheckBox* checkBoxYes{nullptr};
    QProgressBar* progressBarNo{nullptr};
    QProgressBar* progressBarYes{nullptr};

    std::unique_ptr<ProposalInfo> proposal;
    std::vector<VoteInfo> votes;
    MnSelectionDialog* mnSelectionDialog{nullptr};
    std::vector<std::string> vecSelectedMn;

    void initVoteCheck(QWidget* container, QCheckBox* checkBox, QProgressBar* progressBar,
                       const QString& text, Qt::LayoutDirection direction, bool isVoteYes);

    void inform(const QString& text);
    void updateMnSelectionNum();
};

#endif // VOTEDIALOG_H
