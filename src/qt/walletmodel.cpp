// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "walletmodel.h"

#include "addresstablemodel.h"
#include "guiconstants.h"
#include "optionsmodel.h"
#include "recentrequeststablemodel.h"
#include "transactiontablemodel.h"
#include "init.h" // for ShutdownRequested(). Future: move to an interface wrapper

#include "base58.h"
#include "coincontrol.h"
#include "db.h"
#include "keystore.h"
#include "interfaces/handler.h"
#include "sapling/key_io_sapling.h"
#include "sapling/sapling_operation.h"
#include "spork.h"
#include "sync.h"
#include "guiinterface.h"
#include "wallet/wallet.h"
#include "wallet/walletdb.h" // for BackupWallet
#include <stdint.h>
#include <iostream>

#include <QDebug>
#include <QSet>
#include <QTimer>


WalletModel::WalletModel(CWallet* wallet, OptionsModel* optionsModel, QObject* parent) : QObject(parent), wallet(wallet), walletWrapper(*wallet),
                                                                                         optionsModel(optionsModel),
                                                                                         cachedEncryptionStatus(Unencrypted),
                                                                                         cachedNumBlocks(0)
{
    fHaveWatchOnly = wallet->HaveWatchOnly();
    fForceCheckBalanceChanged = false;

    addressTableModel = new AddressTableModel(wallet, this);
    transactionTableModel = new TransactionTableModel(wallet, this);
    recentRequestsTableModel = new RecentRequestsTableModel(wallet, this);

    // This timer will be fired repeatedly to update the balance
    pollTimer = new QTimer(this);
    connect(pollTimer, &QTimer::timeout, this, &WalletModel::pollBalanceChanged);
    pollTimer->start(MODEL_UPDATE_DELAY);

    subscribeToCoreSignals();
}

WalletModel::~WalletModel()
{
    unsubscribeFromCoreSignals();
}

bool WalletModel::isTestNetwork() const
{
    return Params().IsTestnet();
}

bool WalletModel::isRegTestNetwork() const
{
    return Params().IsRegTestNet();
}

bool WalletModel::isShutdownRequested()
{
    return ShutdownRequested();
}

bool WalletModel::isColdStakingNetworkelyEnabled() const
{
    return !sporkManager.IsSporkActive(SPORK_19_COLDSTAKING_MAINTENANCE);
}

bool WalletModel::isSaplingInMaintenance() const
{
    return sporkManager.IsSporkActive(SPORK_20_SAPLING_MAINTENANCE);
}

bool WalletModel::isSaplingEnforced() const
{
    return Params().GetConsensus().NetworkUpgradeActive(cachedNumBlocks, Consensus::UPGRADE_V5_0);
}

bool WalletModel::isStakingStatusActive() const
{
    return wallet && wallet->IsStakingActive();
}

bool WalletModel::isHDEnabled() const
{
    return wallet->IsHDEnabled();
}

bool WalletModel::isSaplingWalletEnabled() const
{
    return wallet->IsSaplingUpgradeEnabled();
}

bool WalletModel::upgradeWallet(std::string& upgradeError)
{
    // This action must be performed in a separate thread and not the main one.
    LOCK2(cs_main, wallet->cs_wallet);

    // Get version
    int prev_version = wallet->GetVersion();
    // Upgrade wallet's version
    wallet->SetMinVersion(FEATURE_LATEST);
    wallet->SetMaxVersion(FEATURE_LATEST);

    // Upgrade to HD
    return wallet->Upgrade(upgradeError, prev_version);
}

CAmount WalletModel::getBalance(const CCoinControl* coinControl, bool fIncludeDelegated, bool fUnlockedOnly, bool fIncludeShielded) const
{
    if (coinControl) {
        CAmount nBalance = 0;
        CWallet::AvailableCoinsFilter coinsFilter;
        coinsFilter.fIncludeDelegated = fIncludeDelegated;
        std::vector<COutput> vCoins;
        wallet->AvailableCoins(&vCoins, coinControl, coinsFilter);
        for (const COutput& out : vCoins) {
            bool fSkip = fUnlockedOnly && isLockedCoin(out.tx->GetHash(), out.i);
            if (out.fSpendable && !fSkip)
                nBalance += out.tx->tx->vout[out.i].nValue;
        }

        return nBalance;
    }

    return wallet->GetAvailableBalance(fIncludeDelegated, fIncludeShielded) - (fUnlockedOnly ? wallet->GetLockedCoins() : CAmount(0));
}

CAmount WalletModel::getUnlockedBalance(const CCoinControl* coinControl, bool fIncludeDelegated, bool fIncludeShielded) const
{
    return getBalance(coinControl, fIncludeDelegated, true, fIncludeShielded);
}

CAmount WalletModel::getMinColdStakingAmount() const
{
    return MIN_COLDSTAKING_AMOUNT;
}

CAmount WalletModel::getLockedBalance() const
{
    return wallet->GetLockedCoins();
}

bool WalletModel::haveWatchOnly() const
{
    return fHaveWatchOnly;
}

CAmount WalletModel::getDelegatedBalance() const
{
    return wallet->GetDelegatedBalance();
}

bool WalletModel::isColdStaking() const
{
    // TODO: Complete me..
    return false;
}

void WalletModel::updateStatus()
{
    if (!wallet) return;
    EncryptionStatus newEncryptionStatus = getEncryptionStatus();

    if (cachedEncryptionStatus != newEncryptionStatus)
        Q_EMIT encryptionStatusChanged(newEncryptionStatus);
}

bool WalletModel::isWalletUnlocked() const
{
    if (!wallet) return false;
    EncryptionStatus status = getEncryptionStatus();
    return (status == Unencrypted || status == Unlocked);
}

bool WalletModel::isWalletLocked(bool fFullUnlocked) const
{
    if (!wallet) return false;
    EncryptionStatus status = getEncryptionStatus();
    return (status == Locked || (!fFullUnlocked && status == UnlockedForStaking));
}

bool IsImportingOrReindexing()
{
    return fImporting || fReindex;
}

void WalletModel::pollBalanceChanged()
{
    // Wait a little bit more when the wallet is reindexing and/or importing, no need to lock cs_main so often.
    if (IsImportingOrReindexing()) {
        static uint8_t waitLonger = 0;
        waitLonger++;
        if (waitLonger < 10) // 10 seconds
            return;
        waitLonger = 0;
    }

    // Get required locks upfront. This avoids the GUI from getting stuck on
    // periodical polls if the core is holding the locks for a longer time -
    // for example, during a wallet rescan.
    TRY_LOCK(cs_main, lockMain);
    if (!lockMain)
        return;
    TRY_LOCK(wallet->cs_wallet, lockWallet);
    if (!lockWallet)
        return;

    // Don't continue processing if the chain tip time is less than the first
    // key creation time as there is no need to iterate over the transaction
    // table model in this case.
    auto tip = chainActive.Tip();
    if (tip->GetBlockTime() < getCreationTime())
        return;

    int chainHeight = tip->nHeight;
    if (fForceCheckBalanceChanged || chainHeight != cachedNumBlocks) {
        fForceCheckBalanceChanged = false;

        // Balance and number of transactions might have changed
        cachedNumBlocks = chainHeight;

        checkBalanceChanged(walletWrapper.getBalances());
        if (transactionTableModel) {
            transactionTableModel->updateConfirmations();
        }

        // Address in receive tab may have been used
        Q_EMIT notifyReceiveAddressChanged();
    }
}

void WalletModel::emitBalanceChanged()
{
    // Force update of UI elements even when no values have changed
   Q_EMIT balanceChanged(walletWrapper.getBalances());
}

void WalletModel::checkBalanceChanged(const interfaces::WalletBalances& newBalance)
{
    if (newBalance.balanceChanged(m_cached_balances)) {
        m_cached_balances = newBalance;
        Q_EMIT balanceChanged(m_cached_balances);
    }
}

void WalletModel::setWalletDefaultFee(CAmount fee)
{
    payTxFee = CFeeRate(fee);
}

bool WalletModel::hasWalletCustomFee()
{
    if (!optionsModel) return false;
    return optionsModel->data(optionsModel->index(OptionsModel::fUseCustomFee), Qt::EditRole).toBool();
}

bool WalletModel::getWalletCustomFee(CAmount& nFeeRet)
{
    nFeeRet = static_cast<CAmount>(optionsModel->data(optionsModel->index(OptionsModel::nCustomFee), Qt::EditRole).toLongLong());
    return hasWalletCustomFee();
}

void WalletModel::setWalletCustomFee(bool fUseCustomFee, const CAmount& nFee)
{
    if (!optionsModel) return;
    optionsModel->setData(optionsModel->index(OptionsModel::fUseCustomFee), fUseCustomFee);
    // do not update custom fee value when fUseCustomFee is set to false
    if (fUseCustomFee) {
        optionsModel->setData(optionsModel->index(OptionsModel::nCustomFee), static_cast<qlonglong>(nFee));
    }
}

void WalletModel::updateTransaction()
{
    // Balance and number of transactions might have changed
    fForceCheckBalanceChanged = true;
}

void WalletModel::updateAddressBook(const QString& address, const QString& label, bool isMine, const QString& purpose, int status)
{
    try {
        if (addressTableModel)
            addressTableModel->updateEntry(address, label, isMine, purpose, status);
    } catch (...) {
        std::cout << "Exception updateAddressBook" << std::endl;
    }
}

void WalletModel::updateWatchOnlyFlag(bool fHaveWatchonly)
{
    fHaveWatchOnly = fHaveWatchonly;
    Q_EMIT notifyWatchonlyChanged(fHaveWatchonly);
}

bool WalletModel::validateAddress(const QString& address)
{
    // Only regular base58 addresses and shielded addresses accepted here
    bool isStaking = false;
    CWDestination dest = Standard::DecodeDestination(address.toStdString(), isStaking);
    const auto regDest = boost::get<CTxDestination>(&dest);
    if (regDest && IsValidDestination(*regDest) && isStaking) return false;
    return Standard::IsValidDestination(dest);
}

bool WalletModel::validateAddress(const QString& address, bool fStaking)
{
    return IsValidDestinationString(address.toStdString(), fStaking);
}

bool WalletModel::validateAddress(const QString& address, bool fStaking, bool& isShielded)
{
    bool isStaking = false;
    CWDestination dest = Standard::DecodeDestination(address.toStdString(), isStaking);
    if (IsShieldedDestination(dest)) {
        isShielded = true;
        return true;
    }
    return Standard::IsValidDestination(dest) && (isStaking == fStaking);
}

bool WalletModel::updateAddressBookLabels(const CWDestination& dest, const std::string& strName, const std::string& strPurpose)
{
    auto optAdd = pwalletMain->GetAddressBookEntry(dest);
    // Check if we have a new address or an updated label
    if (!optAdd) {
        return wallet->SetAddressBook(dest, strName, strPurpose);
    } else if (optAdd->name != strName) {
        return wallet->SetAddressBook(dest, strName, ""); // "" means don't change purpose
    }
    return false;
}

WalletModel::SendCoinsReturn WalletModel::prepareTransaction(WalletModelTransaction* transaction, const CCoinControl* coinControl, bool fIncludeDelegations)
{
    CAmount total = 0;
    QList<SendCoinsRecipient> recipients = transaction->getRecipients();
    std::vector<CRecipient> vecSend;

    if (recipients.empty()) {
        return OK;
    }

    if (isStakingOnlyUnlocked()) {
        return StakingOnlyUnlocked;
    }

    QSet<QString> setAddress; // Used to detect duplicates
    int nAddresses = 0;

    // Pre-check input data for validity
    for (const SendCoinsRecipient& rcp : recipients) {
        if (rcp.paymentRequest.IsInitialized()) { // PaymentRequest...
            CAmount subtotal = 0;
            const payments::PaymentDetails& details = rcp.paymentRequest.getDetails();
            for (int i = 0; i < details.outputs_size(); i++) {
                const payments::Output& out = details.outputs(i);
                if (out.amount() <= 0) continue;
                subtotal += out.amount();
                const unsigned char* scriptStr = (const unsigned char*)out.script().data();
                CScript scriptPubKey(scriptStr, scriptStr + out.script().size());
                vecSend.emplace_back(scriptPubKey, static_cast<CAmount>(out.amount()), false);
            }
            if (subtotal <= 0) {
                return InvalidAmount;
            }
            total += subtotal;
        } else { // User-entered pivx address / amount:
            if (!validateAddress(rcp.address, rcp.isP2CS)) {
                return InvalidAddress;
            }
            if (rcp.amount <= 0) {
                return InvalidAmount;
            }
            setAddress.insert(rcp.address);
            ++nAddresses;

            CScript scriptPubKey;
            CTxDestination out = DecodeDestination(rcp.address.toStdString());

            if (rcp.isP2CS) {
                Destination ownerAdd;
                if (rcp.ownerAddress.isEmpty()) {
                    // Create new internal owner address
                    if (!getNewAddress(ownerAdd).result)
                        return CannotCreateInternalAddress;
                } else {
                    ownerAdd = Destination(DecodeDestination(rcp.ownerAddress.toStdString()), false);
                }

                const CKeyID* stakerId = boost::get<CKeyID>(&out);
                const CKeyID* ownerId = boost::get<CKeyID>(&ownerAdd.dest);
                if (!stakerId || !ownerId) {
                    return InvalidAddress;
                }

                scriptPubKey = GetScriptForStakeDelegation(*stakerId, *ownerId);
            } else {
                // Regular P2PK or P2PKH
                scriptPubKey = GetScriptForDestination(out);
            }
            vecSend.emplace_back(scriptPubKey, rcp.amount, false);

            total += rcp.amount;
        }
    }
    if (setAddress.size() != nAddresses) {
        return DuplicateAddress;
    }

    CAmount nSpendableBalance = getUnlockedBalance(coinControl, fIncludeDelegations);

    if (total > nSpendableBalance) {
        return AmountExceedsBalance;
    }

    {
        LOCK2(cs_main, wallet->cs_wallet);

        CReserveKey* keyChange = transaction->newPossibleKeyChange(wallet);
        CAmount nFeeRequired = 0;
        int nChangePosInOut = -1;
        std::string strFailReason;

        bool fCreated = wallet->CreateTransaction(vecSend,
                                                  transaction->getTransaction(),
                                                  *keyChange,
                                                  nFeeRequired,
                                                  nChangePosInOut,
                                                  strFailReason,
                                                  coinControl,
                                                  ALL_COINS,
                                                  true,
                                                  0,
                                                  fIncludeDelegations,
                                                  &transaction->fIsStakeDelegationVoided);
        transaction->setTransactionFee(nFeeRequired);

        if (!fCreated) {
            if ((total + nFeeRequired) > nSpendableBalance) {
                return SendCoinsReturn(AmountWithFeeExceedsBalance);
            }

            Q_EMIT message(tr("Send Coins"), tr("Transaction creation failed!\n%1").arg(
                    strFailReason == "Transaction too large" ?
                            tr("The size of the transaction is too big.\nSelect fewer inputs with coin control.") :
                            QString::fromStdString(strFailReason)),
                    CClientUIInterface::MSG_ERROR);
            return TransactionCreationFailed;
        }

        // reject insane fee
        if (nFeeRequired > ::minRelayTxFee.GetFee(transaction->getTransactionSize()) * 10000)
            return InsaneFee;
    }

    return SendCoinsReturn(OK);
}

WalletModel::SendCoinsReturn WalletModel::sendCoins(WalletModelTransaction& transaction)
{
    bool fColdStakingActive = isColdStakingNetworkelyEnabled();
    bool fSaplingActive = Params().GetConsensus().NetworkUpgradeActive(cachedNumBlocks, Consensus::UPGRADE_V5_0);

    // Double check the tx before doing anything
    CTransactionRef& newTx = transaction.getTransaction();
    CValidationState state;
    if (!CheckTransaction(*newTx, true, true, state, true, fColdStakingActive, fSaplingActive)) {
        return TransactionCheckFailed;
    }

    QByteArray transaction_array; /* store serialized transaction */

    {
        LOCK2(cs_main, wallet->cs_wallet);
        QList<SendCoinsRecipient> recipients = transaction.getRecipients();

        // Store PaymentRequests in wtx.vOrderForm in wallet.
        /* todo: clean this functionality, we don't have the payment request server nor using the vOrderForm field.
        for (const SendCoinsRecipient& rcp : recipients) {
            if (rcp.paymentRequest.IsInitialized()) {
                std::string key("PaymentRequest");
                std::string value;
                rcp.paymentRequest.SerializeToString(&value);
                newTx->vOrderForm.emplace_back(key, value);
            } else if (!rcp.message.isEmpty()) // Message from normal pivx:URI (pivx:XyZ...?message=example)
            {
                newTx->vOrderForm.emplace_back("Message", rcp.message.toStdString());
            }
        }*/

        CReserveKey* keyChange = transaction.getPossibleKeyChange();
        const CWallet::CommitResult& res = wallet->CommitTransaction(newTx, keyChange, g_connman.get());
        if (res.status != CWallet::CommitStatus::OK) {
            return SendCoinsReturn(res);
        }

        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << *newTx;
        transaction_array.append(&(ssTx[0]), ssTx.size());
    }

    // Add addresses / update labels that we've sent to to the address book,
    // and emit coinsSent signal for each recipient
    for (const SendCoinsRecipient& rcp : transaction.getRecipients()) {
        // Don't touch the address book when we have a payment request
        if (!rcp.paymentRequest.IsInitialized()) {
            bool isStaking = false;
            bool isShielded = false;
            auto address = Standard::DecodeDestination(rcp.address.toStdString(), isStaking, isShielded);
            std::string purpose = isShielded ? AddressBook::AddressBookPurpose::SHIELDED_SEND :
                                  isStaking ? AddressBook::AddressBookPurpose::COLD_STAKING_SEND : AddressBook::AddressBookPurpose::SEND;
            std::string strLabel = rcp.label.toStdString();
            updateAddressBookLabels(address, strLabel, purpose);
        }
        Q_EMIT coinsSent(wallet, rcp, transaction_array);
    }
    emitBalanceChanged(); // update balance immediately, otherwise there could be a short noticeable delay until pollBalanceChanged hits

    return SendCoinsReturn(OK);
}

OperationResult WalletModel::PrepareShieldedTransaction(WalletModelTransaction* modelTransaction,
                                                        bool fromTransparent,
                                                        const CCoinControl* coinControl)
{
    // Basic checks first

    // Check network status
    int nextBlockHeight = cachedNumBlocks + 1;
    if (!Params().GetConsensus().NetworkUpgradeActive(nextBlockHeight, Consensus::UPGRADE_V5_0)) {
        return errorOut("Error, cannot send transaction. Sapling is not activated");
    }

    // Load shieldedAddrRecipients.
    std::vector<SendManyRecipient> recipients;
    for (const auto& recipient : modelTransaction->getRecipients()) {
        if (recipient.isShieldedAddr) {
            auto pa = KeyIO::DecodeSaplingPaymentAddress(recipient.address.toStdString());
            if (!pa) return errorOut("Error, invalid shielded address");
            recipients.emplace_back(*pa, recipient.amount, recipient.message.toStdString());
        } else {
            auto dest = DecodeDestination(recipient.address.toStdString());
            if (!IsValidDestination(dest)) return errorOut("Error, invalid transparent address");
            recipients.emplace_back(dest, recipient.amount);
        }
    }

    // Now check the transaction size
    auto opResult = CheckTransactionSize(recipients, true);
    if (!opResult) return opResult;

    // Create the operation
    TransactionBuilder txBuilder = TransactionBuilder(Params().GetConsensus(), nextBlockHeight, wallet);
    SaplingOperation operation(txBuilder);
    auto operationResult = operation.setRecipients(recipients)
             ->setTransparentKeyChange(modelTransaction->getPossibleKeyChange())
             ->setSelectTransparentCoins(fromTransparent)
             ->setSelectShieldedCoins(!fromTransparent)
             ->setCoinControl(coinControl)
             ->setMinDepth(fromTransparent ? 1 : 5)
             ->build();

    if (!operationResult) {
        return operationResult;
    }

    // load the transaction and key change (if needed)
    CTransactionRef& txRef = modelTransaction->getTransaction();
    txRef = MakeTransactionRef(operation.getFinalTx());
    modelTransaction->setTransactionFee(operation.getFee()); // in the future, fee will be dynamically calculated.
    return operationResult;
}

const CWalletTx* WalletModel::getTx(uint256 id)
{
    return wallet->GetWalletTx(id);
}

OptionsModel* WalletModel::getOptionsModel()
{
    return optionsModel;
}

AddressTableModel* WalletModel::getAddressTableModel()
{
    return addressTableModel;
}

TransactionTableModel* WalletModel::getTransactionTableModel()
{
    return transactionTableModel;
}

RecentRequestsTableModel* WalletModel::getRecentRequestsTableModel()
{
    return recentRequestsTableModel;
}

WalletModel::EncryptionStatus WalletModel::getEncryptionStatus() const
{
    if (!wallet) throw std::runtime_error("Error, cannot get encryption status. Wallet doesn't exist");
    if (!wallet->IsCrypted()) {
        return Unencrypted;
    } else if (wallet->fWalletUnlockStaking) {
        return UnlockedForStaking;
    } else if (wallet->IsLocked()) {
        return Locked;
    } else {
        return Unlocked;
    }

}

bool WalletModel::setWalletEncrypted(bool encrypted, const SecureString& passphrase)
{
    if (encrypted) {
        // Encrypt
        return wallet->EncryptWallet(passphrase);
    } else {
        // Decrypt -- TODO; not supported yet
        return false;
    }
}

bool WalletModel::setWalletLocked(bool locked, const SecureString& passPhrase, bool stakingOnly)
{
    if (locked) {
        // Lock
        wallet->fWalletUnlockStaking = false;
        return wallet->Lock();
    } else {
        // Unlock
        return wallet->Unlock(passPhrase, stakingOnly);
    }
}

bool WalletModel::lockForStakingOnly(const SecureString& passPhrase)
{
    if (!wallet->IsLocked()) {
        wallet->fWalletUnlockStaking = true;
        return true;
    } else {
        setWalletLocked(false, passPhrase, true);
    }
    return false;
}

bool WalletModel::isStakingOnlyUnlocked()
{
    return wallet->fWalletUnlockStaking;
}

bool WalletModel::changePassphrase(const SecureString& oldPass, const SecureString& newPass)
{
    bool retval;
    {
        LOCK(wallet->cs_wallet);
        wallet->Lock(); // Make sure wallet is locked before attempting pass change
        retval = wallet->ChangeWalletPassphrase(oldPass, newPass);
    }
    return retval;
}

bool WalletModel::backupWallet(const QString& filename)
{
    //attempt regular backup
    if (!BackupWallet(*wallet, filename.toLocal8Bit().data())) {
        return error("ERROR: Failed to backup wallet!");
    }

    return true;
}


// Handlers for core signals
static void NotifyKeyStoreStatusChanged(WalletModel* walletmodel, CCryptoKeyStore* wallet)
{
    qDebug() << "NotifyKeyStoreStatusChanged";
    QMetaObject::invokeMethod(walletmodel, "updateStatus", Qt::QueuedConnection);
}

static void NotifyAddressBookChanged(WalletModel* walletmodel, CWallet* wallet, const CWDestination& address, const std::string& label, bool isMine, const std::string& purpose, ChangeType status)
{
    QString strAddress = QString::fromStdString(pwalletMain->ParseIntoAddress(address, purpose));
    QString strLabel = QString::fromStdString(label);
    QString strPurpose = QString::fromStdString(purpose);

    qDebug() << "NotifyAddressBookChanged : " + strAddress + " " + strLabel + " isMine=" + QString::number(isMine) + " purpose=" + strPurpose + " status=" + QString::number(status);
    QMetaObject::invokeMethod(walletmodel, "updateAddressBook", Qt::QueuedConnection,
        Q_ARG(QString, strAddress),
        Q_ARG(QString, strLabel),
        Q_ARG(bool, isMine),
        Q_ARG(QString, strPurpose),
        Q_ARG(int, status));
}

// queue notifications to show a non freezing progress dialog e.g. for rescan
static bool fQueueNotifications = false;
static std::vector<std::pair<uint256, ChangeType> > vQueueNotifications;
static void NotifyTransactionChanged(WalletModel* walletmodel, CWallet* wallet, const uint256& hash, ChangeType status)
{
    if (fQueueNotifications) {
        vQueueNotifications.emplace_back(hash, status);
        return;
    }

    QString strHash = QString::fromStdString(hash.GetHex());

    qDebug() << "NotifyTransactionChanged : " + strHash + " status= " + QString::number(status);
    QMetaObject::invokeMethod(walletmodel, "updateTransaction", Qt::QueuedConnection /*,
                              Q_ARG(QString, strHash),
                              Q_ARG(int, status)*/);
}

static void ShowProgress(WalletModel* walletmodel, const std::string& title, int nProgress)
{
    // emits signal "showProgress"
    QMetaObject::invokeMethod(walletmodel, "showProgress", Qt::QueuedConnection,
        Q_ARG(QString, QString::fromStdString(title)),
        Q_ARG(int, nProgress));
}

static void NotifyWatchonlyChanged(WalletModel* walletmodel, bool fHaveWatchonly)
{
    QMetaObject::invokeMethod(walletmodel, "updateWatchOnlyFlag", Qt::QueuedConnection,
        Q_ARG(bool, fHaveWatchonly));
}

static void NotifyWalletBacked(WalletModel* model, const bool& fSuccess, const std::string& filename)
{
    std::string message;
    std::string title = "Backup ";
    CClientUIInterface::MessageBoxFlags method;

    if (fSuccess) {
        message = "The wallet data was successfully saved to ";
        title += "Successful: ";
        method = CClientUIInterface::MessageBoxFlags::MSG_INFORMATION;
    } else {
        message = "There was an error trying to save the wallet data to ";
        title += "Failed: ";
        method = CClientUIInterface::MessageBoxFlags::MSG_ERROR;
    }

    message += _(filename.data());

    QMetaObject::invokeMethod(model, "message", Qt::QueuedConnection,
                              Q_ARG(QString, QString::fromStdString(title)),
                              Q_ARG(QString, QString::fromStdString(message)),
                              Q_ARG(unsigned int, (unsigned int)method));
}

void WalletModel::subscribeToCoreSignals()
{
    // Connect signals to wallet
    m_handler_notify_status_changed = interfaces::MakeHandler(wallet->NotifyStatusChanged.connect(std::bind(&NotifyKeyStoreStatusChanged, this, std::placeholders::_1)));
    m_handler_notify_addressbook_changed = interfaces::MakeHandler(wallet->NotifyAddressBookChanged.connect(std::bind(NotifyAddressBookChanged, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3, std::placeholders::_4, std::placeholders::_5, std::placeholders::_6)));
    m_handler_notify_transaction_changed = interfaces::MakeHandler(wallet->NotifyTransactionChanged.connect(std::bind(NotifyTransactionChanged, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)));
    m_handler_show_progress = interfaces::MakeHandler(wallet->ShowProgress.connect(std::bind(ShowProgress, this, std::placeholders::_1, std::placeholders::_2)));
    m_handler_notify_watch_only_changed = interfaces::MakeHandler(wallet->NotifyWatchonlyChanged.connect(std::bind(NotifyWatchonlyChanged, this, std::placeholders::_1)));
    m_handler_notify_walletbacked = interfaces::MakeHandler(wallet->NotifyWalletBacked.connect(std::bind(NotifyWalletBacked, this, std::placeholders::_1, std::placeholders::_2)));
}

void WalletModel::unsubscribeFromCoreSignals()
{
    // Disconnect signals from wallet
    m_handler_notify_status_changed->disconnect();
    m_handler_notify_addressbook_changed->disconnect();
    m_handler_notify_transaction_changed->disconnect();
    m_handler_show_progress->disconnect();
    m_handler_notify_watch_only_changed->disconnect();
    m_handler_notify_walletbacked->disconnect();
}

// WalletModel::UnlockContext implementation
WalletModel::UnlockContext WalletModel::requestUnlock()
{
    const WalletModel::EncryptionStatus status_before = getEncryptionStatus();
    if (status_before == Locked || status_before == UnlockedForStaking)
    {
        // Request UI to unlock wallet
        Q_EMIT requireUnlock();
    }
    // If wallet is still locked, unlock was failed or cancelled, mark context as invalid
    bool valid = isWalletUnlocked();

    return UnlockContext(this, valid, status_before);
}

WalletModel::UnlockContext::UnlockContext(WalletModel *_wallet, bool _valid, const WalletModel::EncryptionStatus& status_before):
        wallet(_wallet),
        valid(_valid),
        was_status(status_before),
        relock(status_before == Locked || status_before == UnlockedForStaking)
{
}

WalletModel::UnlockContext::~UnlockContext()
{
    if (valid && relock && wallet) {
        if (was_status == Locked) wallet->setWalletLocked(true);
        else if (was_status == UnlockedForStaking) wallet->lockForStakingOnly();
        wallet->updateStatus();
    }
}

void WalletModel::UnlockContext::CopyFrom(UnlockContext&& rhs)
{
    // Transfer context; old object no longer relocks wallet
    *this = rhs;
    rhs.relock = false;
}

bool WalletModel::getPubKey(const CKeyID& address, CPubKey& vchPubKeyOut) const
{
    return wallet->GetPubKey(address, vchPubKeyOut);
}

int64_t WalletModel::getCreationTime() const {
    return wallet->nTimeFirstKey;
}

int64_t WalletModel::getKeyCreationTime(const CPubKey& key)
{
    return pwalletMain->GetKeyCreationTime(key);
}

int64_t WalletModel::getKeyCreationTime(const CTxDestination& address)
{
    if (this->isMine(address)) {
        return pwalletMain->GetKeyCreationTime(address);
    }
    return 0;
}

int64_t WalletModel::getKeyCreationTime(const std::string& address)
{
    return pwalletMain->GetKeyCreationTime(Standard::DecodeDestination(address));
}

int64_t WalletModel::getKeyCreationTime(const libzcash::SaplingPaymentAddress& address)
{
    if (this->isMine(address)) {
        return pwalletMain->GetKeyCreationTime(address);
    }
    return 0;
}

PairResult WalletModel::getNewAddress(Destination& ret, std::string label) const
{
    CTxDestination dest;
    PairResult res = wallet->getNewAddress(dest, label);
    if (res.result) ret = Destination(dest, false);
    return res;
}

PairResult WalletModel::getNewStakingAddress(Destination& ret,std::string label) const
{
    CTxDestination dest;
    PairResult res = wallet->getNewStakingAddress(dest, label);
    if (res.result) ret = Destination(dest, true);
    return res;
}

PairResult WalletModel::getNewShieldedAddress(QString& shieldedAddrRet, std::string strLabel)
{
    shieldedAddrRet = QString::fromStdString(
            KeyIO::EncodePaymentAddress(wallet->GenerateNewSaplingZKey(strLabel)));
    return PairResult(true);
}

bool WalletModel::whitelistAddressFromColdStaking(const QString &addressStr)
{
    return updateAddressBookPurpose(addressStr, AddressBook::AddressBookPurpose::DELEGATOR);
}

bool WalletModel::blacklistAddressFromColdStaking(const QString &addressStr)
{
    return updateAddressBookPurpose(addressStr, AddressBook::AddressBookPurpose::DELEGABLE);
}

bool WalletModel::updateAddressBookPurpose(const QString &addressStr, const std::string& purpose)
{
    bool isStaking = false;
    CTxDestination address = DecodeDestination(addressStr.toStdString(), isStaking);
    if (isStaking)
        return error("Invalid PIVX address, cold staking address");
    CKeyID keyID;
    if (!getKeyId(address, keyID))
        return false;
    return pwalletMain->SetAddressBook(keyID, getLabelForAddress(address), purpose);
}

bool WalletModel::getKeyId(const CTxDestination& address, CKeyID& keyID)
{
    if (!IsValidDestination(address))
        return error("Invalid PIVX address");

    const CKeyID* inKeyID = boost::get<CKeyID>(&address);
    if (!inKeyID)
        return error("Unable to get KeyID from PIVX address");

    keyID = *inKeyID;
    return true;
}

std::string WalletModel::getLabelForAddress(const CTxDestination& address)
{
    std::string label = "";
    label = wallet->GetNameForAddressBookEntry(address);
    return label;
}

// returns a COutPoint of 10000 PIV if found
bool WalletModel::getMNCollateralCandidate(COutPoint& outPoint)
{
    CWallet::AvailableCoinsFilter coinsFilter;
    coinsFilter.fIncludeDelegated = false;
    coinsFilter.nCoinType = ONLY_10000;
    std::vector<COutput> vCoins;
    wallet->AvailableCoins(&vCoins, nullptr, coinsFilter);
    for (const COutput& out : vCoins) {
        // skip locked collaterals
        if (!isLockedCoin(out.tx->GetHash(), out.i)) {
            outPoint = COutPoint(out.tx->GetHash(), out.i);
            return true;
        }
    }
    return false;
}

bool WalletModel::isSpent(const COutPoint& outpoint) const
{
    LOCK2(cs_main, wallet->cs_wallet);
    return wallet->IsSpent(outpoint.hash, outpoint.n);
}

void WalletModel::listCoins(std::map<ListCoinsKey, std::vector<ListCoinsValue>>& mapCoins, bool fTransparent) const
{
    if (fTransparent) {
        listCoins(mapCoins);
    } else {
        listAvailableNotes(mapCoins);
    }
}

void WalletModel::listAvailableNotes(std::map<ListCoinsKey, std::vector<ListCoinsValue>>& mapCoins) const
{
    std::vector<SaplingNoteEntry> notes;
    Optional<libzcash::SaplingPaymentAddress> dummy = nullopt;
    wallet->GetSaplingScriptPubKeyMan()->GetFilteredNotes(notes, dummy);
    for (const auto& note : notes) {
        ListCoinsKey key{QString::fromStdString(KeyIO::EncodePaymentAddress(note.address)), false, nullopt};
        ListCoinsValue value{
            note.op.hash,
            (int)note.op.n,
            (CAmount)note.note.value(),
            0,
            note.confirmations
        };
        mapCoins[key].emplace_back(value);
    }
}

// AvailableCoins + LockedCoins grouped by wallet address (put change in one group with wallet address)
void WalletModel::listCoins(std::map<ListCoinsKey, std::vector<ListCoinsValue>>& mapCoins) const
{
    CWallet::AvailableCoinsFilter filter;
    filter.fIncludeLocked = true;
    std::vector<COutput> vCoins;
    wallet->AvailableCoins(&vCoins, nullptr, filter);

    for (const COutput& out : vCoins) {
        if (!out.fSpendable) continue;

        const CScript& scriptPubKey = out.tx->tx->vout[out.i].scriptPubKey;
        const bool isP2CS = scriptPubKey.IsPayToColdStaking();

        CTxDestination outputAddress;
        CTxDestination outputAddressStaker;
        if (isP2CS) {
            txnouttype type; std::vector<CTxDestination> addresses; int nRequired;
            if(!ExtractDestinations(scriptPubKey, type, addresses, nRequired)
                        || addresses.size() != 2) throw std::runtime_error("Cannot extract P2CS addresses from a stored transaction");
            outputAddressStaker = addresses[0];
            outputAddress = addresses[1];
        } else {
            if (!ExtractDestination(scriptPubKey, outputAddress))
                continue;
        }

        QString address = QString::fromStdString(EncodeDestination(outputAddress));
        Optional<QString> stakerAddr = IsValidDestination(outputAddressStaker) ?
            Optional<QString>(QString::fromStdString(EncodeDestination(outputAddressStaker, CChainParams::STAKING_ADDRESS))) :
            nullopt;

        ListCoinsKey key{address, wallet->IsChange(outputAddress), stakerAddr};
        ListCoinsValue value{
                out.tx->GetHash(),
                out.i,
                out.tx->tx->vout[out.i].nValue,
                out.tx->GetTxTime(),
                out.nDepth
        };
        mapCoins[key].emplace_back(value);
    }
}

bool WalletModel::isLockedCoin(uint256 hash, unsigned int n) const
{
    LOCK2(cs_main, wallet->cs_wallet);
    return wallet->IsLockedCoin(hash, n);
}

void WalletModel::lockCoin(COutPoint& output)
{
    LOCK2(cs_main, wallet->cs_wallet);
    wallet->LockCoin(output);
}

void WalletModel::unlockCoin(COutPoint& output)
{
    LOCK2(cs_main, wallet->cs_wallet);
    wallet->UnlockCoin(output);
}

std::set<COutPoint> WalletModel::listLockedCoins()
{
    LOCK(wallet->cs_wallet);
    return wallet->ListLockedCoins();
}

void WalletModel::loadReceiveRequests(std::vector<std::string>& vReceiveRequests)
{
    LOCK(wallet->cs_wallet);
    for (auto it = wallet->NewAddressBookIterator(); it.IsValid(); it.Next()) {
        for (const std::pair<std::string, std::string> &item2 : it.GetValue().destdata)
            if (item2.first.size() > 2 && item2.first.substr(0, 2) == "rr") // receive request
                vReceiveRequests.push_back(item2.second);
    }
}

bool WalletModel::saveReceiveRequest(const std::string& sAddress, const int64_t nId, const std::string& sRequest)
{
    CTxDestination dest = DecodeDestination(sAddress);

    std::stringstream ss;
    ss << nId;
    std::string key = "rr" + ss.str(); // "rr" prefix = "receive request" in destdata

    LOCK(wallet->cs_wallet);
    if (sRequest.empty())
        return wallet->EraseDestData(dest, key);
    else
        return wallet->AddDestData(dest, key, sRequest);
}

bool WalletModel::isMine(const CWDestination& address)
{
    return IsMine(*wallet, address);
}

bool WalletModel::isMine(const QString& addressStr)
{
    return IsMine(*wallet, DecodeDestination(addressStr.toStdString()));
}

bool WalletModel::IsShieldedDestination(const CWDestination& address)
{
    return boost::get<libzcash::SaplingPaymentAddress>(&address);
}

bool WalletModel::isUsed(CTxDestination address)
{
    return wallet->IsUsed(address);
}

Optional<QString> WalletModel::getShieldedAddressFromSpendDesc(const uint256& txHash, int index)
{
    Optional<libzcash::SaplingPaymentAddress> opAddr = wallet->GetSaplingScriptPubKeyMan()->GetAddressFromInputIfPossible(txHash, index);
    return opAddr ? Optional<QString>(QString::fromStdString(KeyIO::EncodePaymentAddress(*opAddr))) : nullopt;
}
