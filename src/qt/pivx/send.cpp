// Copyright (c) 2019-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/send.h"
#include "qt/pivx/forms/ui_send.h"
#include "qt/pivx/addnewcontactdialog.h"
#include "qt/pivx/qtutils.h"
#include "qt/pivx/sendchangeaddressdialog.h"
#include "qt/pivx/optionbutton.h"
#include "qt/pivx/sendconfirmdialog.h"
#include "qt/pivx/guitransactionsutils.h"
#include "qt/pivx/loadingdialog.h"
#include "clientmodel.h"
#include "optionsmodel.h"
#include "operationresult.h"
#include "addresstablemodel.h"
#include "coincontrol.h"
#include "script/standard.h"
#include "openuridialog.h"

#define REQUEST_PREPARE_TX 1
#define REQUEST_REFRESH_BALANCE 2

SendWidget::SendWidget(PIVXGUI* parent) :
    PWidget(parent),
    ui(new Ui::send),
    coinIcon(new QPushButton())
{
    ui->setupUi(this);

    this->setStyleSheet(parent->styleSheet());

    /* Containers */
    setCssProperty(ui->left, "container");
    ui->left->setContentsMargins(0,20,0,20);
    setCssProperty(ui->right, "container-right");
    ui->right->setContentsMargins(20,10,20,20);

    /* Light Font */
    QFont fontLight;
    fontLight.setWeight(QFont::Light);

    /* Title */
    setCssProperty(ui->labelTitle, "text-title-screen");
    ui->labelTitle->setFont(fontLight);

    /* Button Group */
    setCssProperty(ui->pushLeft, "btn-check-left");
    ui->pushLeft->setChecked(true);
    setCssProperty(ui->pushRight, "btn-check-right");

    /* Subtitle */
    setCssProperty({ui->labelSubtitle1, ui->labelSubtitle2}, "text-subtitle");

    /* Address - Amount*/
    setCssProperty({ui->labelSubtitleAddress, ui->labelSubtitleAmount}, "text-title");

    /* Buttons */
    setCssBtnSecondary(ui->pushButtonFee);
    setCssProperty(ui->pushButtonClear, "btn-secundary-clear");
    setCssProperty(ui->pushButtonAddRecipient, "btn-secundary-add");
    setCssBtnPrimary(ui->pushButtonSave);
    setCssBtnSecondary(ui->pushButtonReset);

    // Coin control
    ui->btnCoinControl->setTitleClassAndText("btn-title-grey", tr("Coin Control"));
    ui->btnCoinControl->setSubTitleClassAndText("text-subtitle", tr("Select the source of the coins"));

    // Change address option
    ui->btnChangeAddress->setTitleClassAndText("btn-title-grey", tr("Change Address"));
    ui->btnChangeAddress->setSubTitleClassAndText("text-subtitle", tr("Customize the change address"));

    // Uri
    ui->btnUri->setTitleClassAndText("btn-title-grey", tr("Open URI"));
    ui->btnUri->setSubTitleClassAndText("text-subtitle", tr("Parse a PIVX URI"));

    // Shield coins
    ui->btnShieldCoins->setTitleClassAndText("btn-title-grey", tr("Shield Coins"));
    ui->btnShieldCoins->setSubTitleClassAndText("text-subtitle", tr("Convert all transparent coins into shielded coins"));
    ui->btnShieldCoins->setVisible(false);

    connect(ui->pushButtonFee, &QPushButton::clicked, this, &SendWidget::onChangeCustomFeeClicked);
    connect(ui->btnCoinControl, &OptionButton::clicked, this, &SendWidget::onCoinControlClicked);
    connect(ui->btnChangeAddress, &OptionButton::clicked, this, &SendWidget::onChangeAddressClicked);
    connect(ui->btnUri, &OptionButton::clicked, this, &SendWidget::onOpenUriClicked);
    connect(ui->btnShieldCoins, &OptionButton::clicked, this, &SendWidget::onShieldCoinsClicked);
    connect(ui->pushButtonReset, &QPushButton::clicked, [this](){ onResetCustomOptions(true); });
    connect(ui->checkBoxDelegations, &QCheckBox::stateChanged, this, &SendWidget::onCheckBoxChanged);

    setCssProperty(ui->coinWidget, "container-coin-type");
    setCssProperty(ui->labelLine, "container-divider");


    // Total Send
    setCssProperty(ui->labelTitleTotalSend, "text-title");
    setCssProperty(ui->labelAmountSend, "text-body1");

    // Total Remaining
    setCssProperty(ui->labelTitleTotalRemaining, "text-title");
    setCssProperty(ui->labelAmountRemaining, "text-body1");

    // Icon Send
    ui->stackedWidget->addWidget(coinIcon);
    coinIcon->show();
    coinIcon->raise();

    setCssProperty(coinIcon, "coin-icon-piv");

    QSize BUTTON_SIZE = QSize(24, 24);
    coinIcon->setMinimumSize(BUTTON_SIZE);
    coinIcon->setMaximumSize(BUTTON_SIZE);

    int posX = 0;
    int posY = 20;
    coinIcon->move(posX, posY);

    // Entry
    addEntry();

    // Init custom fee false (updated in loadWalletModel)
    setCustomFeeSelected(false);

    // Connect
    connect(ui->pushLeft, &QPushButton::clicked, [this](){onPIVSelected(true);});
    connect(ui->pushRight,  &QPushButton::clicked, [this](){onPIVSelected(false);});
    connect(ui->pushButtonSave, &QPushButton::clicked, this, &SendWidget::onSendClicked);
    connect(ui->pushButtonAddRecipient, &QPushButton::clicked, this, &SendWidget::onAddEntryClicked);
    connect(ui->pushButtonClear, &QPushButton::clicked, [this](){clearAll(true);});

    coinControlDialog = new CoinControlDialog();
}

void SendWidget::refreshAmounts()
{
    CAmount total = 0;
    QMutableListIterator<SendMultiRow*> it(entries);
    while (it.hasNext()) {
        SendMultiRow* entry = it.next();
        CAmount amount = entry->getAmountValue();
        if (amount > 0)
            total += amount;
    }

    nDisplayUnit = walletModel->getOptionsModel()->getDisplayUnit();

    CAmount totalAmount = 0;
    CAmount delegatedBalance = 0;
    QString titleTotalRemaining;
    if (coinControlDialog->coinControl->HasSelected()) {
        // Set remaining balance to the sum of the coinControl selected inputs
        std::vector<OutPointWrapper> coins;
        coinControlDialog->coinControl->ListSelected(coins);
        CAmount selectedBalance = 0;
        for (const auto& coin : coins) {
            selectedBalance += coin.value;
        }
        totalAmount = selectedBalance - total;
        titleTotalRemaining = tr("Total remaining from the selected UTXO");
    } else {
        interfaces::WalletBalances balances = walletModel->GetWalletBalances();
        if (isTransparent) {
            totalAmount = balances.balance - balances.shielded_balance - walletModel->getLockedBalance() - total;
            if (!fDelegationsChecked) {
                totalAmount -= balances.delegate_balance;
            }
            // show delegated balance if exist
            delegatedBalance = balances.delegate_balance;
        } else {
            totalAmount = balances.shielded_balance - total;
        }
        titleTotalRemaining = tr("Unlocked remaining");
    }

    QString type = isTransparent ? "transparent" : "shielded";
    QString labelAmountRemaining = GUIUtil::formatBalance( totalAmount, nDisplayUnit, false) + " " + type;
    QMetaObject::invokeMethod(this, "updateAmounts", Qt::QueuedConnection,
                              Q_ARG(QString, titleTotalRemaining),
                              Q_ARG(QString, GUIUtil::formatBalance(total, nDisplayUnit, false)),
                              Q_ARG(QString, labelAmountRemaining),
                              Q_ARG(CAmount, delegatedBalance));
}

void SendWidget::updateAmounts(const QString& _titleTotalRemaining,
                               const QString& _labelAmountSend,
                               const QString& _labelAmountRemaining,
                               CAmount _delegationBalance)
{
    ui->labelTitleTotalRemaining->setText(_titleTotalRemaining);
    ui->labelAmountSend->setText(_labelAmountSend);
    ui->labelAmountRemaining->setText(_labelAmountRemaining);
    // show or hide delegations checkbox if need be
    showHideCheckBoxDelegations(_delegationBalance);
}

void SendWidget::loadClientModel()
{
    if (clientModel) {
        connect(clientModel, &ClientModel::numBlocksChanged, [this](){
            if (customFeeDialog) customFeeDialog->updateFee();
        });
    }
}

void SendWidget::loadWalletModel()
{
    if (walletModel) {
        if (walletModel->getOptionsModel()) {
            // display unit
            nDisplayUnit = walletModel->getOptionsModel()->getDisplayUnit();
        }

        // set walletModel for entries
        for (SendMultiRow *entry : entries) {
            if (entry) {
                entry->setWalletModel(walletModel);
            }
        }

        // Restore custom fee from wallet Settings
        CAmount nCustomFee;
        if (walletModel->getWalletCustomFee(nCustomFee)) {
            setCustomFeeSelected(true, nCustomFee);
        }

        // TODO: This only happen when the coin control features are modified in other screen, check before do this if the wallet has another screen modifying it.
        // Coin Control
        //connect(walletModel->getOptionsModel(), &OptionsModel::coinControlFeaturesChanged, [this](){});
        //ui->frameCoinControl->setVisible(model->getOptionsModel()->getCoinControlFeatures());
        //coinControlUpdateLabels();
    }
}

void SendWidget::hideContactsMenu()
{
    if (menuContacts && menuContacts->isVisible()) {
        menuContacts->hide();
    }
}

void SendWidget::clearAll(bool fClearSettings)
{
    onResetCustomOptions(false);
    if (fClearSettings) onResetSettings();
    hideContactsMenu();
    clearEntries();
    tryRefreshAmounts();
}

void SendWidget::onResetSettings()
{
    if (customFeeDialog) customFeeDialog->clear();
    setCustomFeeSelected(false);
    if (walletModel) walletModel->setWalletCustomFee(false, DEFAULT_TRANSACTION_FEE);
}

void SendWidget::onResetCustomOptions(bool fRefreshAmounts)
{
    ui->btnChangeAddress->setActive(false);
    if (ui->checkBoxDelegations->isChecked()) ui->checkBoxDelegations->setChecked(false);
    resetCoinControl();
    if (fRefreshAmounts) {
        tryRefreshAmounts();
    }
}

void SendWidget::resetCoinControl()
{
    if (coinControlDialog) coinControlDialog->coinControl->SetNull();
    ui->btnCoinControl->setActive(false);
}

void SendWidget::resetChangeAddress()
{
    if (coinControlDialog) coinControlDialog->coinControl->destChange = CNoDestination();
    ui->btnChangeAddress->setActive(false);
    ui->btnChangeAddress->setVisible(isTransparent);
}

void SendWidget::clearEntries()
{
    int num = entries.length();
    for (int i = 0; i < num; ++i) {
        ui->scrollAreaWidgetContents->layout()->takeAt(0)->widget()->deleteLater();
    }
    entries.clear();

    addEntry();
}

void SendWidget::addEntry()
{
    if (entries.isEmpty()) {
        createEntry();
    } else {
        if (entries.length() == 1) {
            SendMultiRow *entry = entries.at(0);
            entry->hideLabels();
            entry->setNumber(1);
        } else if (entries.length() == MAX_SEND_POPUP_ENTRIES) {
            inform(tr("Maximum amount of outputs reached"));
            return;
        }

        SendMultiRow *sendMultiRow = createEntry();
        sendMultiRow->setNumber(entries.length());
        sendMultiRow->hideLabels();
    }
    setFocusOnLastEntry();
}

SendMultiRow* SendWidget::createEntry()
{
    SendMultiRow *sendMultiRow = new SendMultiRow(window, this);
    if (this->walletModel) sendMultiRow->setWalletModel(this->walletModel);
    entries.append(sendMultiRow);
    ui->scrollAreaWidgetContents->layout()->addWidget(sendMultiRow);
    connect(sendMultiRow, &SendMultiRow::onContactsClicked, this, &SendWidget::onContactsClicked);
    connect(sendMultiRow, &SendMultiRow::onMenuClicked, this, &SendWidget::onMenuClicked);
    connect(sendMultiRow, &SendMultiRow::onValueChanged, this, &SendWidget::onValueChanged);
    return sendMultiRow;
}

void SendWidget::onAddEntryClicked()
{
    // Check prev valid entries before add a new one.
    for (SendMultiRow* entry : entries) {
        if (!entry || !entry->validate()) {
            inform(tr("Invalid entry, previous entries must be valid before add a new one"));
            return;
        }
    }
    addEntry();
}

void SendWidget::resizeEvent(QResizeEvent *event)
{
    resizeMenu();
    QWidget::resizeEvent(event);
}

void SendWidget::showEvent(QShowEvent *event)
{
    // Set focus on last recipient address when Send-window is displayed
    setFocusOnLastEntry();
    tryRefreshAmounts();
}

void SendWidget::setFocusOnLastEntry()
{
    if (!entries.isEmpty()) entries.last()->setFocus();
}

void SendWidget::showHideCheckBoxDelegations(CAmount delegationBalance)
{
    // Show checkbox only when there is any available owned delegation and
    // coincontrol is not selected, and we are trying to spend transparent PIVs.
    const bool isCControl = coinControlDialog ? coinControlDialog->coinControl->HasSelected() : false;
    const bool hasDel = delegationBalance > 0;

    const bool showCheckBox = isTransparent && !isCControl && hasDel;
    ui->checkBoxDelegations->setVisible(showCheckBox);
    if (showCheckBox)
        ui->checkBoxDelegations->setToolTip(
                tr("Possibly spend coins delegated for cold-staking (currently available: %1").arg(
                        GUIUtil::formatBalance(delegationBalance, nDisplayUnit, false))
        );
}

void SendWidget::onSendClicked()
{
    if (!walletModel || !walletModel->getOptionsModel())
        return;

    QList<SendCoinsRecipient> recipients;
    bool hasShieldedOutput = false;

    for (SendMultiRow* entry : entries) {
        // TODO: Check UTXO splitter here..
        // Validate send..
        if (entry && entry->validate()) {
            auto recipient = entry->getValue();
            bool isShielded = recipient.isShieldedAddr;
            if (!hasShieldedOutput) hasShieldedOutput = isShielded;
            if (!recipient.message.isEmpty() && !isShielded) {
                // memo set for transparent address
                if (!ask(tr("Warning!"),
                         tr("Cannot send memo to address\n%1\n\n"
                            "Encrypted memo messages are available only for shielded recipients.\n\n"
                            "Do you wish to proceed without memo?\n").arg(recipient.address))) {
                    return;
                } else {
                    // remove memo, so it doesn't show on the confirmation dialog.
                    recipient.message.clear();
                }
            }
            recipients.append(recipient);
        } else {
            inform(tr("Invalid entry"));
            return;
        }
    }

    if (recipients.isEmpty()) {
        inform(tr("No set recipients"));
        return;
    }

    ProcessSend(recipients, hasShieldedOutput);
}

void SendWidget::ProcessSend(QList<SendCoinsRecipient>& recipients, bool hasShieldedOutput,
                             const std::function<bool(QList<SendCoinsRecipient>&)>& func)
{
    // First check SPORK_20 (before unlock)
    bool isShieldedTx = hasShieldedOutput || !isTransparent;
    if (isShieldedTx) {
        if (walletModel->isSaplingInMaintenance()) {
            inform(tr("Sapling Protocol temporarily in maintenance. Shielded transactions disabled (SPORK 20)"));
            return;
        }
    }

    auto ptrUnlockedContext = MakeUnique<WalletModel::UnlockContext>(walletModel->requestUnlock());
    if (!ptrUnlockedContext->isValid()) {
        // Unlock wallet was cancelled
        inform(tr("Cannot send, wallet locked"));
        return;
    }

    // Perform needed operation that requires the wallet unlocked
    if (func && !func(recipients)) return;

    // If tx exists then there is an on-going process being executed, return.
    if (isProcessing || ptrModelTx) {
        inform(tr("On going process being executed, please wait until it's finished to create a new transaction"));
        return;
    }
    ptrModelTx = new WalletModelTransaction(recipients);
    ptrModelTx->useV2 = isShieldedTx;

    // Prepare tx
    window->showHide(true);
    LoadingDialog *dialog = new LoadingDialog(window, tr("Preparing transaction"));
    dialog->execute(this, REQUEST_PREPARE_TX, std::move(ptrUnlockedContext));
    openDialogWithOpaqueBackgroundFullScreen(dialog, window);

    // If all went well, ask if want to broadcast it
    if (processingResult) {
        if (sendFinalStep()) {
            updateEntryLabels(ptrModelTx->getRecipients());
        }
        setFocusOnLastEntry();
    } else if (!processingResultError->isEmpty()){
        inform(*processingResultError);
    }

    // Process finished, can reset the tx model now. todo: this can get wrapped on a cached struct.
    delete ptrModelTx;
    ptrModelTx = nullptr;
    if (processingResultError) {
        processingResultError->clear();
        processingResultError = nullopt;
    }
    processingResult = false;
}

OperationResult SendWidget::prepareShielded(WalletModelTransaction* currentTransaction, bool fromTransparent)
{
    bool hasCoinsOrNotesSelected = coinControlDialog && coinControlDialog->coinControl && coinControlDialog->coinControl->HasSelected();
    return walletModel->PrepareShieldedTransaction(currentTransaction,
                                                   fromTransparent,
                                                   hasCoinsOrNotesSelected ? coinControlDialog->coinControl : nullptr);
}

OperationResult SendWidget::prepareTransparent(WalletModelTransaction* currentTransaction)
{
    if (!walletModel) return errorOut("Error, no wallet model loaded");
    // prepare transaction for getting txFee earlier
    WalletModel::SendCoinsReturn prepareStatus;
    prepareStatus = walletModel->prepareTransaction(currentTransaction,
                                                    coinControlDialog ? coinControlDialog->coinControl : nullptr,
                                                    fDelegationsChecked);

    // process prepareStatus and on error generate message shown to user
    CClientUIInterface::MessageBoxFlags informType;
    QString informMsg = GuiTransactionsUtils::ProcessSendCoinsReturn(
            this,
            prepareStatus,
            walletModel,
            informType,
            BitcoinUnits::formatWithUnit(walletModel->getOptionsModel()->getDisplayUnit(),
                                         currentTransaction->getTransactionFee()),
            true
    );

    if (!informMsg.isEmpty()) {
        return errorOut(informMsg.toStdString());
    }

    if (prepareStatus.status != WalletModel::OK) {
        return errorOut("Cannot create transaction.");
    }
    return OperationResult(true);
}

bool SendWidget::sendFinalStep()
{
    showHideOp(true);
    const bool fStakeDelegationVoided = ptrModelTx->fIsStakeDelegationVoided;
    QString warningStr = QString();
    if (fStakeDelegationVoided)
        warningStr = tr("WARNING:\nTransaction spends a cold-stake delegation, voiding it.\n"
                        "These coins will no longer be cold-staked.");
    TxDetailDialog* dialog = new TxDetailDialog(window, true, warningStr);
    dialog->setDisplayUnit(walletModel->getOptionsModel()->getDisplayUnit());
    dialog->setData(walletModel, ptrModelTx);
    dialog->adjustSize();
    openDialogWithOpaqueBackgroundY(dialog, window, 3, 15);

    if (dialog->isConfirm()) {
        // now send the prepared transaction
        WalletModel::SendCoinsReturn sendStatus = dialog->getStatus();
        // process sendStatus and on error generate message shown to user
        GuiTransactionsUtils::ProcessSendCoinsReturnAndInform(
                this,
                sendStatus,
                walletModel
        );

        if (sendStatus.status == WalletModel::OK) {
            clearAll(false);
            inform(tr("Transaction sent"));
            dialog->deleteLater();
            return true;
        }
    }

    dialog->deleteLater();
    return false;
}

void SendWidget::run(int type)
{
    if (type == REQUEST_PREPARE_TX) {
        assert(!processingResult);
        if (!isProcessing) {
            isProcessing = true;
            OperationResult result(false);
            if ((result = ptrModelTx->useV2 ?
                        prepareShielded(ptrModelTx, isTransparent) :
                        prepareTransparent(ptrModelTx)
                        )) {
                processingResult = true;
            } else {
                processingResult = false;
                processingResultError = tr(result.getError().c_str());
            }
            isProcessing = false;
        }
    } else if (type == REQUEST_REFRESH_BALANCE) {
        if (!isUpdatingBalance) {
            isUpdatingBalance = true;
            refreshAmounts();
            isUpdatingBalance = false;
        }
    }
}

void SendWidget::onError(QString error, int type)
{
    isProcessing = false;
    processingResultError = error;
}

void SendWidget::tryRefreshAmounts()
{
    if (!execute(REQUEST_REFRESH_BALANCE)) {
        inform(tr("Processing full, refreshing amounts later"));
    }
}

void SendWidget::updateEntryLabels(const QList<SendCoinsRecipient>& recipients)
{
    for (const SendCoinsRecipient& rec : recipients) {
        QString label = rec.label;
        if (!label.isNull()) {
            QString labelOld = walletModel->getAddressTableModel()->labelForAddress(rec.address);
            if (label.compare(labelOld) != 0) {
                CTxDestination dest = DecodeDestination(rec.address.toStdString());
                if (!walletModel->updateAddressBookLabels(dest, label.toStdString(),
                                                          this->walletModel->isMine(dest) ?
                                                                  AddressBook::AddressBookPurpose::RECEIVE :
                                                                  AddressBook::AddressBookPurpose::SEND)) {
                    // Label update failed
                    Q_EMIT message("", tr("Address label update failed for address: %1").arg(rec.address), CClientUIInterface::MSG_ERROR);
                    return;
                }
            }
        }

    }
}

void SendWidget::onChangeAddressClicked()
{
    showHideOp(true);
    SendChangeAddressDialog* dialog = new SendChangeAddressDialog(window, walletModel);
    if (IsValidDestination(coinControlDialog->coinControl->destChange)) {
        dialog->setAddress(QString::fromStdString(EncodeDestination(coinControlDialog->coinControl->destChange)));
    }

    CTxDestination destChange = (openDialogWithOpaqueBackgroundY(dialog, window, 3, 5) ?
                                 dialog->getDestination() : CNoDestination());

    if (!IsValidDestination(destChange)) {
        // no change address set
        ui->btnChangeAddress->setActive(false);
    } else {
        // Ask confirmation if external address
        if (!walletModel->isMine(destChange) && !ask(tr("Warning!"),
                tr("The change address doesn't belong to this wallet.\n\nDo you want to continue?"))) {
            dialog->deleteLater();
            return;
        }
        ui->btnChangeAddress->setActive(true);
    }

    // save change address in coin control
    coinControlDialog->coinControl->destChange = destChange;
    dialog->deleteLater();
}

void SendWidget::onOpenUriClicked()
{
    showHideOp(true);
    OpenURIDialog *dlg = new OpenURIDialog(window);
    if (openDialogWithOpaqueBackgroundY(dlg, window, 3, 5)) {

        SendCoinsRecipient rcp;
        if (!GUIUtil::parseBitcoinURI(dlg->getURI(), &rcp)) {
            inform(tr("Invalid URI"));
            return;
        }
        if (!walletModel->validateAddress(rcp.address)) {
            inform(tr("Invalid address in URI"));
            return;
        }

        int listSize = entries.size();
        if (listSize == 1) {
            SendMultiRow *entry = entries[0];
            entry->setAddressAndLabelOrDescription(rcp.address, rcp.message);
            entry->setAmount(BitcoinUnits::format(nDisplayUnit, rcp.amount, false));
        } else {
            // Use the last one if it's invalid or add a new one
            SendMultiRow *entry = entries[listSize - 1];
            if (!entry->validate()) {
                addEntry();
                entry = entries[listSize];
            }
            entry->setAddressAndLabelOrDescription(rcp.address, rcp.message);
            entry->setAmount(BitcoinUnits::format(nDisplayUnit, rcp.amount, false));
        }
        Q_EMIT receivedURI(dlg->getURI());
    }
    dlg->deleteLater();
}

void SendWidget::onChangeCustomFeeClicked()
{
    showHideOp(true);
    if (!customFeeDialog) {
        customFeeDialog = new SendCustomFeeDialog(window, walletModel);
    }
    if (openDialogWithOpaqueBackgroundY(customFeeDialog, window, 3, 5)) {
        const CAmount& nFeePerKb = customFeeDialog->getFeeRate().GetFeePerK();
        setCustomFeeSelected(customFeeDialog->isCustomFeeChecked(), nFeePerKb);
    }
}

void SendWidget::onCoinControlClicked()
{
    if (walletModel->getBalance() > 0) {
        // future: move coin control initialization and refresh to a worker thread.
        if (!coinControlDialog->hasModel()) coinControlDialog->setModel(walletModel);
        coinControlDialog->setSelectionType(isTransparent);
        coinControlDialog->refreshDialog();
        setCoinControlPayAmounts();
        coinControlDialog->exec();
        ui->btnCoinControl->setActive(coinControlDialog->coinControl->HasSelected());
        tryRefreshAmounts();
    } else {
        inform(tr("You don't have any %1 to select.").arg(CURRENCY_UNIT.c_str()));
    }
}

void SendWidget::onShieldCoinsClicked()
{
    if (walletModel->isSaplingInMaintenance()) {
        inform(tr("Sapling Protocol temporarily in maintenance. Shielded transactions disabled (SPORK 20)"));
        return;
    }

    auto balances = walletModel->GetWalletBalances();
    CAmount availableBalance = balances.balance - balances.shielded_balance - walletModel->getLockedBalance();
    if (availableBalance > 0) {

        // Calculate the required fee first. TODO future: Unify this code with the code in coincontroldialog into the model.
        std::map<WalletModel::ListCoinsKey, std::vector<WalletModel::ListCoinsValue>> mapCoins;
        walletModel->listCoins(mapCoins);
        unsigned int nBytesInputs = 0;
        for (const auto& out : mapCoins) {
            bool isP2CS = out.first.stakerAddress != nullopt;
            nBytesInputs += (CTXIN_SPEND_DUST_SIZE + (isP2CS ? 1 : 0)) * out.second.size();
        }
        nBytesInputs += OUTPUTDESCRIPTION_SIZE;
        nBytesInputs += (BINDINGSIG_SIZE + 8);
        // (plus at least 2 bytes for shielded in/outs len sizes)
        nBytesInputs += 2;
        // ExtraPayload size for special txes. For now 1 byte for nullopt.
        nBytesInputs += 1;
        // nVersion, nType, nLockTime and vin/vout len sizes
        nBytesInputs += 10;
        CAmount nPayFee = GetMinRelayFee(nBytesInputs) * DEFAULT_SHIELDEDTXFEE_K;

        // load recipient
        QList<SendCoinsRecipient> recipients;
        SendCoinsRecipient recipient;
        recipient.amount = availableBalance - nPayFee;
        recipient.isShieldedAddr = true;
        recipients.append(recipient); // address is added later on, when the wallet is unlocked

        // Ask if the user want to do it
        if (!ask(tr("Shield Coins"),
                 tr("You are just about to anonymize all of your balance!\nAvailable %1\nWith fee %2\n\n"
                    "Meaning that you will be able to perform completely\nanonymous transactions"
                    "\n\nDo you want to continue?\n").arg(GUIUtil::formatBalanceWithoutHtml(recipient.amount, nDisplayUnit, false))
                                                     .arg(GUIUtil::formatBalanceWithoutHtml(nPayFee, nDisplayUnit, false))
                    )) {
            return;
        }

        // Process spending
        ProcessSend(recipients, true, [this](QList<SendCoinsRecipient>& recipients) {
            QString strAddress;
            auto res = walletModel->getNewShieldedAddress(strAddress, "");
            // Check for generation errors
            if (!res) {
                inform(tr("Error generating address to shield PIVs"));
                return false;
            }
            recipients.back().address = strAddress;
            resetCoinControl();
            return true;
        });
    } else {
        inform(tr("You don't have any transparent PIVs to shield."));
    }
}

void SendWidget::setCoinControlPayAmounts()
{
    if (!coinControlDialog) return;
    coinControlDialog->clearPayAmounts();
    QMutableListIterator<SendMultiRow*> it(entries);
    while (it.hasNext()) {
        const auto& entry = it.next();
        coinControlDialog->addPayAmount(entry->getAmountValue(), entry->getValue().isShieldedAddr);
    }
}

void SendWidget::onValueChanged()
{
    tryRefreshAmounts();
}

void SendWidget::onCheckBoxChanged()
{
    const bool checked = ui->checkBoxDelegations->isChecked();
    if (checked != fDelegationsChecked) {
        fDelegationsChecked = checked;
        tryRefreshAmounts();
    }
}

void SendWidget::onPIVSelected(bool _isTransparent)
{
    isTransparent = _isTransparent;
    resetChangeAddress();
    resetCoinControl();
    tryRefreshAmounts();
    updateStyle(coinIcon);
}

void SendWidget::onContactsClicked(SendMultiRow* entry)
{
    focusedEntry = entry;
    if (menu && menu->isVisible()) {
        menu->hide();
    }

    int contactsSize = walletModel->getAddressTableModel()->sizeSend() +
                        walletModel->getAddressTableModel()->sizeShieldedSend();
    if (contactsSize == 0) {
        inform(tr("No contacts available, you can go to the contacts screen and add some there!"));
        return;
    }

    int height = (contactsSize <= 2) ? entry->getEditHeight() * ( 2 * (contactsSize + 1 )) : entry->getEditHeight() * 6;
    int width = entry->getEditWidth();

    if (!menuContacts) {
        menuContacts = new ContactsDropdown(
                    width,
                    height,
                    this
        );
        menuContacts->setWalletModel(walletModel, {AddressTableModel::Send, AddressTableModel::ShieldedSend});
        connect(menuContacts, &ContactsDropdown::contactSelected, [this](QString address, QString label) {
            if (focusedEntry) {
                if (label != "(no label)")
                    focusedEntry->setLabel(label);
                focusedEntry->setAddress(address);
            }
        });

    }

    if (menuContacts->isVisible()) {
        menuContacts->hide();
        return;
    }

    menuContacts->resizeList(width, height);
    menuContacts->setStyleSheet(this->styleSheet());
    menuContacts->adjustSize();

    QPoint pos;
    if (entries.size() > 1) {
        pos = entry->pos();
        pos.setY((pos.y() + (focusedEntry->getEditHeight() - 12) * 4));
    } else {
        pos = focusedEntry->getEditLineRect().bottomLeft();
        pos.setY((pos.y() + (focusedEntry->getEditHeight() - 12) * 3));
    }
    pos.setX(pos.x() + 20);
    menuContacts->move(pos);
    menuContacts->show();
}

void SendWidget::onMenuClicked(SendMultiRow* entry)
{
    focusedEntry = entry;
    if (menuContacts && menuContacts->isVisible()) {
        menuContacts->hide();
    }
    QPoint pos = entry->pos();
    pos.setX(pos.x() + (entry->width() - entry->getMenuBtnWidth()));
    pos.setY(pos.y() + entry->height() + (entry->getMenuBtnWidth()));

    if (!this->menu) {
        this->menu = new TooltipMenu(window, this);
        this->menu->setCopyBtnText(tr("Add Memo"));
        this->menu->setEditBtnText(tr("Save contact"));
        this->menu->setLastBtnVisible(true);
        this->menu->setLastBtnText(tr("Subtract fee"));
        this->menu->setMinimumHeight(157);
        this->menu->setMinimumSize(this->menu->width() + 30, this->menu->height());
        connect(this->menu, &TooltipMenu::message, this, &AddressesWidget::message);
        connect(this->menu, &TooltipMenu::onEditClicked, this, &SendWidget::onContactMultiClicked);
        connect(this->menu, &TooltipMenu::onDeleteClicked, this, &SendWidget::onDeleteClicked);
        connect(this->menu, &TooltipMenu::onCopyClicked, this, &SendWidget::onEntryMemoClicked);
        connect(this->menu, &TooltipMenu::onLastClicked, this, &SendWidget::onSubtractFeeFromAmountChecked);
    } else {
        this->menu->hide();
    }
    this->menu->setLastBtnCheckable(true, entry->getSubtractFeeFromAmount());
    menu->move(pos);
    menu->show();
}

void SendWidget::onContactMultiClicked()
{
    if (focusedEntry) {
        QString address = focusedEntry->getAddress();
        if (address.isEmpty()) {
            inform(tr("Address field is empty"));
            return;
        }

        bool isStakingAddr = false;
        auto pivAdd = Standard::DecodeDestination(address.toStdString(), isStakingAddr);

        if (!Standard::IsValidDestination(pivAdd) || isStakingAddr) {
            inform(tr("Invalid address"));
            return;
        }

        if (walletModel->isMine(pivAdd)) {
            inform(tr("Cannot store your own address as contact"));
            return;
        }

        showHideOp(true);
        AddNewContactDialog *dialog = new AddNewContactDialog(window);
        QString label = walletModel->getAddressTableModel()->labelForAddress(address);
        if (!label.isNull()) {
            dialog->setTexts(tr("Update Contact"), "Edit label for the selected address:\n%1");
            dialog->setData(address, label);
        } else {
            dialog->setTexts(tr("Create New Contact"), "Save label for the selected address:\n%1");
            dialog->setData(address, "");
        }
        openDialogWithOpaqueBackgroundY(dialog, window, 3, 5);
        if (dialog->res) {
            if (label == dialog->getLabel()) {
                return;
            }
            if (walletModel->updateAddressBookLabels(pivAdd, dialog->getLabel().toStdString(),
                    AddressBook::AddressBookPurpose::SEND)) {
                inform(tr("New Contact Stored"));
            } else {
                inform(tr("Error Storing Contact"));
            }
        }
        dialog->deleteLater();
    }

}

void SendWidget::onEntryMemoClicked()
{
    if (focusedEntry) {
        focusedEntry->launchMemoDialog();
        menu->setCopyBtnText(tr("Memo"));
    }
}

void SendWidget::onSubtractFeeFromAmountChecked()
{
    if (focusedEntry) {
        focusedEntry->toggleSubtractFeeFromAmount();
    }
}

void SendWidget::onDeleteClicked()
{
    if (focusedEntry) {
        focusedEntry->hide();
        focusedEntry->deleteLater();
        int entryNumber = focusedEntry->getNumber();

        // remove selected entry and update row number for the others
        QMutableListIterator<SendMultiRow*> it(entries);
        while (it.hasNext()) {
            SendMultiRow* entry = it.next();
            if (focusedEntry == entry) {
                it.remove();
            } else if (focusedEntry && entry->getNumber() > entryNumber) {
                entry->setNumber(entry->getNumber() - 1);
            }
        }

        if (entries.size() == 1) {
            SendMultiRow* sendMultiRow = QMutableListIterator<SendMultiRow*>(entries).next();
            sendMultiRow->setNumber(entries.length());
            sendMultiRow->showLabels();
        }

        focusedEntry = nullptr;

        // Update total amounts
        tryRefreshAmounts();
        setFocusOnLastEntry();
    }
}

void SendWidget::resizeMenu()
{
    if (menuContacts && menuContacts->isVisible() && focusedEntry) {
        int width = focusedEntry->getEditWidth();
        menuContacts->resizeList(width, menuContacts->height());
        menuContacts->resize(width, menuContacts->height());
        QPoint pos = focusedEntry->getEditLineRect().bottomLeft();
        pos.setX(pos.x() + 20);
        pos.setY(pos.y() + ((focusedEntry->getEditHeight() - 12)  * 3));
        menuContacts->move(pos);
    }
}

void SendWidget::setCustomFeeSelected(bool isSelected, const CAmount& customFee)
{
    isCustomFeeSelected = isSelected;
    ui->pushButtonFee->setText(isCustomFeeSelected ?
                    tr("Custom Fee %1").arg(BitcoinUnits::formatWithUnit(nDisplayUnit, customFee) + "/kB") :
                    tr("Customize Fee"));
    if (walletModel)
        walletModel->setWalletDefaultFee(customFee);
}

void SendWidget::changeTheme(bool isLightTheme, QString& theme)
{
    coinControlDialog->setStyleSheet(theme);
}

SendWidget::~SendWidget()
{
    delete ui;
    delete coinControlDialog;
}
