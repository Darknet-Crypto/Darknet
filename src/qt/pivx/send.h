// Copyright (c) 2019-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef SEND_H
#define SEND_H

#include <QWidget>
#include <QPushButton>

#include "qt/pivx/pwidget.h"
#include "qt/pivx/contactsdropdown.h"
#include "qt/pivx/sendmultirow.h"
#include "qt/pivx/sendcustomfeedialog.h"
#include "walletmodel.h"
#include "coincontroldialog.h"
#include "qt/pivx/tooltipmenu.h"

#include <atomic>

static const int MAX_SEND_POPUP_ENTRIES = 8;

class PIVXGUI;
class ClientModel;
class OperationResult;
class WalletModel;
class WalletModelTransaction;

namespace Ui {
class send;
class QPushButton;
}

class SendWidget : public PWidget
{
    Q_OBJECT

public:
    explicit SendWidget(PIVXGUI* parent);
    ~SendWidget();

    void addEntry();

    void loadClientModel() override;
    void loadWalletModel() override;

Q_SIGNALS:
    /** Signal raised when a URI was entered or dragged to the GUI */
    void receivedURI(const QString& uri);

public Q_SLOTS:
    void onChangeAddressClicked();
    void onChangeCustomFeeClicked();
    void onCoinControlClicked();
    void onOpenUriClicked();
    void onValueChanged();
    void refreshAmounts();
    void changeTheme(bool isLightTheme, QString &theme) override;

protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;

    void run(int type) override;
    void onError(QString error, int type) override;

private Q_SLOTS:
    void onPIVSelected(bool _isTransparent);
    void onSendClicked();
    void onContactsClicked(SendMultiRow* entry);
    void onMenuClicked(SendMultiRow* entry);
    void onAddEntryClicked();
    void clearEntries();
    void clearAll(bool fClearSettings = true);
    void onCheckBoxChanged();
    void onContactMultiClicked();
    void onDeleteClicked();
    void onResetCustomOptions(bool fRefreshAmounts);
    void onResetSettings();
    void txBroadcasted();
    void resetSendProcess(QString informError);

private:
    Ui::send *ui;
    QPushButton *coinIcon;
    QPushButton *btnContacts;

    SendCustomFeeDialog* customFeeDialog = nullptr;
    bool isCustomFeeSelected = false;
    bool fDelegationsChecked = false;
    CAmount cachedDelegatedBalance{0};

    int nDisplayUnit;
    QList<SendMultiRow*> entries;
    CoinControlDialog *coinControlDialog = nullptr;

    // Cached tx
    std::shared_ptr<WalletModelTransaction> ptrModelTx;
    std::atomic<bool> isProcessing{false};

    ContactsDropdown *menuContacts = nullptr;
    TooltipMenu *menu = nullptr;
    // Current focus entry
    SendMultiRow* focusedEntry = nullptr;

    bool isTransparent = true;
    void resizeMenu();
    QString recipientsToString(QList<SendCoinsRecipient> recipients);
    SendMultiRow* createEntry();
    OperationResult prepareShielded(WalletModelTransaction* tx, bool fromTransparent);
    OperationResult prepareTransparent(WalletModelTransaction* tx);
    bool sendFinalStep(WalletModelTransaction& currentTransaction);
    void setFocusOnLastEntry();
    void showHideCheckBoxDelegations();
    void updateEntryLabels(QList<SendCoinsRecipient> recipients);
    void setCustomFeeSelected(bool isSelected, const CAmount& customFee = DEFAULT_TRANSACTION_FEE);
    void setCoinControlPayAmounts();
};

#endif // SEND_H
