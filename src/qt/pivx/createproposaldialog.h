// Copyright (c) 2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#ifndef CREATEPROPOSALDIALOG_H
#define CREATEPROPOSALDIALOG_H

#include <QDialog>

namespace Ui {
class CreateProposalDialog;
class QPushButton;
}

class GovernanceModel;
class SnackBar;
class WalletModel;

class CreateProposalDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CreateProposalDialog(QWidget *parent, GovernanceModel* _govModel, WalletModel* _walletModel);
    ~CreateProposalDialog() override;

public Q_SLOTS:
    void onNextClicked();
    void onBackClicked();
    void propNameChanged(const QString& newText);
    void propUrlChanged(const QString& newText);
    void propAmountChanged(const QString& newText);
    void propMonthsChanged(const QString& newText);
    bool propaddressChanged(const QString& newText);

private:
    Ui::CreateProposalDialog *ui;
    GovernanceModel* govModel{nullptr};
    WalletModel* walletModel{nullptr};
    SnackBar* snackBar{nullptr};
    QPushButton* icConfirm1{nullptr};
    QPushButton* icConfirm2{nullptr};
    QPushButton* icConfirm3{nullptr};
    int pos = 0;

    void setupPageOne();
    void setupPageTwo();
    void setupPageThree();

    bool validatePageOne();
    bool validatePageTwo();

    void inform(const QString& text);
};

#endif // CREATEPROPOSALDIALOG_H
