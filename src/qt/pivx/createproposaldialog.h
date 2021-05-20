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

class CreateProposalDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CreateProposalDialog(QWidget *parent = nullptr);
    ~CreateProposalDialog() override;

public Q_SLOTS:
    void onNextClicked();
    void onBackClicked();
private:
    Ui::CreateProposalDialog *ui;
    QPushButton *icConfirm1;
    QPushButton *icConfirm2;
    QPushButton *icConfirm3;
    int pos = 0;

    void setupPageOne();
    void setupPageTwo();
    void setupPageThree();
};

#endif // CREATEPROPOSALDIALOG_H
