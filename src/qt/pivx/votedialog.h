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

class MNModel;
class GovernanceModel;

class VoteDialog : public QDialog
{
    Q_OBJECT

public:
    explicit VoteDialog(QWidget *parent, GovernanceModel* _govModel, MNModel* _mnModel);
    ~VoteDialog();

    void showEvent(QShowEvent *event) override;

public Q_SLOTS:
    void onAcceptClicked();
    void onCheckBoxClicked(QCheckBox* checkBox, QProgressBar* progressBar, bool isVoteYes);
    void onMnSelectionClicked();

private:
    Ui::VoteDialog *ui;
    GovernanceModel* govModel{nullptr};
    MNModel* mnModel{nullptr};

    QCheckBox* checkBoxNo{nullptr};
    QCheckBox* checkBoxYes{nullptr};
    QProgressBar* progressBarNo{nullptr};
    QProgressBar* progressBarYes{nullptr};

    void initVoteCheck(QWidget* container, QCheckBox* checkBox, QProgressBar* progressBar,
                       QString text, Qt::LayoutDirection direction, bool isVoteYes);
};

#endif // VOTEDIALOG_H
