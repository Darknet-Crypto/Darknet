// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2016 The Dash developers
// Copyright (c) 2017-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PIVX_QT_WALLETMODEL_H
#define PIVX_QT_WALLETMODEL_H

#if defined(HAVE_CONFIG_H)
#include "config/pivx-config.h"
#endif

#include "interfaces/wallet.h"

#include "key.h"
#include "key_io.h"
#include "operationresult.h"
#include "support/allocators/zeroafterfree.h"
#include "pairresult.h"

#include <map>
#include <vector>

#include <QObject>
#include <QFuture>
#include <QSettings>

class AddressTableModel;
class CBudgetProposal;
class ClientModel;
class OptionsModel;
class RecentRequestsTableModel;
class TransactionTableModel;
class WalletModelTransaction;

class CCoinControl;
class CKeyID;
class COutPoint;
class OutPointWrapper;
class COutput;
class CPubKey;
class CWallet;
class uint256;

namespace interfaces {
    class Handler;
};

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

class SendCoinsRecipient
{
public:
    explicit SendCoinsRecipient() : amount(0), nVersion(SendCoinsRecipient::CURRENT_VERSION) {}
    explicit SendCoinsRecipient(const QString& addr, const QString& label, const CAmount& amount, const QString& message) : address(addr), label(label), amount(amount), message(message), nVersion(SendCoinsRecipient::CURRENT_VERSION) {}

    // If from an insecure payment request, this is used for storing
    // the addresses, e.g. address-A<br />address-B<br />address-C.
    // Info: As we don't need to process addresses in here when using
    // payment requests, we can abuse it for displaying an address list.
    // Todo: This is a hack, should be replaced with a cleaner solution!
    QString address{};
    QString label{};

    // Cold staking.
    bool isP2CS{false};
    QString ownerAddress{};

    // Quick flag to not have to check the address type more than once.
    bool isShieldedAddr{false};

    // Whether to subtract the tx fee from this recipient
    bool fSubtractFee{false};

    // Amount
    CAmount amount{0};
    // If from a payment request, this is used for storing the memo
    QString message{};

    // serialized string to ensure load/store is lossless
    std::string sPaymentRequest{};

    // Empty if no authentication or invalid signature/cert/etc.
    QString authenticatedMerchant{};

    static const int CURRENT_VERSION = 1;
    int nVersion;

    SERIALIZE_METHODS(SendCoinsRecipient, obj)
    {
        std::string address_str, label_str, message_str, auth_merchant_str;

        SER_WRITE(obj, address_str = obj.address.toStdString());
        SER_WRITE(obj, label_str = obj.label.toStdString());
        SER_WRITE(obj, message_str = obj.message.toStdString());
        SER_WRITE(obj, auth_merchant_str = obj.authenticatedMerchant.toStdString());

        READWRITE(obj.nVersion, address_str, label_str, obj.amount, message_str, obj.sPaymentRequest, auth_merchant_str);

        SER_READ(obj, obj.address = QString::fromStdString(address_str));
        SER_READ(obj, obj.label = QString::fromStdString(label_str));
        SER_READ(obj, obj.message = QString::fromStdString(message_str));
        SER_READ(obj, obj.authenticatedMerchant = QString::fromStdString(auth_merchant_str));
    }
};

/** Interface to PIVX wallet from Qt view code. */
class WalletModel : public QObject
{
    Q_OBJECT

public:
    explicit WalletModel(CWallet* wallet, OptionsModel* optionsModel, QObject* parent = 0);
    ~WalletModel();
    void init();

    enum StatusCode // Returned by sendCoins
    {
        OK,
        InvalidAmount,
        InvalidAddress,
        AmountExceedsBalance,
        AmountWithFeeExceedsBalance,
        DuplicateAddress,
        TransactionCreationFailed,
        TransactionCheckFailed,
        TransactionCommitFailed,
        StakingOnlyUnlocked,
        InsaneFee,
        CannotCreateInternalAddress
    };

    enum EncryptionStatus {
        Unencrypted,                 // !wallet->IsCrypted()
        Locked,                      // wallet->IsCrypted() && wallet->IsLocked()
        Unlocked,                    // wallet->IsCrypted() && !wallet->IsLocked()
        UnlockedForStaking          // wallet->IsCrypted() && !wallet->IsLocked() && wallet->fWalletUnlockStaking
    };

    OptionsModel* getOptionsModel();
    AddressTableModel* getAddressTableModel();
    TransactionTableModel* getTransactionTableModel();
    RecentRequestsTableModel* getRecentRequestsTableModel();

    void resetWalletOptions(QSettings& settings);
    bool isTestNetwork() const;
    bool isRegTestNetwork() const;
    bool isShutdownRequested();
    /** Whether cold staking is enabled or disabled in the network **/
    bool isColdStakingNetworkelyEnabled() const;
    bool isSaplingInMaintenance() const;
    bool isV6Enforced() const;
    CAmount getMinColdStakingAmount() const;
    /* current staking status from the miner thread **/
    bool isStakingStatusActive() const;

    bool hasWallet() { return wallet; };

    bool isHDEnabled() const;
    bool isSaplingWalletEnabled() const;
    bool upgradeWallet(std::string& upgradeError);

    // Returns the path to the first wallet db (future: add multi-wallet support)
    QString getWalletPath();

    interfaces::WalletBalances GetWalletBalances() { return m_cached_balances; };

    CAmount getBalance(const CCoinControl* coinControl = nullptr, bool fIncludeDelegated = true, bool fUnlockedOnly = false, bool fIncludeShielded = true) const;
    CAmount getUnlockedBalance(const CCoinControl* coinControl = nullptr, bool fIncludeDelegated = true, bool fIncludeShielded = true) const;
    CAmount getLockedBalance() const;
    bool haveWatchOnly() const;
    CAmount getDelegatedBalance() const;

    bool isColdStaking() const;
    void getAvailableP2CSCoins(std::vector<COutput>& vCoins) const;

    EncryptionStatus getEncryptionStatus() const;
    bool isWalletUnlocked() const;
    bool isWalletLocked(bool fFullUnlocked = true) const;
    void emitBalanceChanged(); // Force update of UI-elements even when no values have changed

    // Check address for validity
    bool validateAddress(const QString& address);
    // Check address for validity and type (whether cold staking address or not)
    bool validateAddress(const QString& address, bool fStaking);
    // Check address for validity and type (whether cold staking address or not),
    // plus return isShielded = true if the parsed address is a valid shielded address.
    bool validateAddress(const QString& address, bool fStaking, bool& isShielded);

    // Return the address from where the shielded spend is taking the funds from (if possible)
    Optional<QString> getShieldedAddressFromSpendDesc(const uint256& txHash, int index);

    // Return status record for SendCoins, contains error id + information
    struct SendCoinsReturn {
        SendCoinsReturn(StatusCode status = OK) : status(status) {}
        SendCoinsReturn(CWallet::CommitResult _commitRes) : commitRes(_commitRes)
        {
            status = (_commitRes.status == CWallet::CommitStatus::OK ? OK : TransactionCommitFailed);
        }
        StatusCode status;
        CWallet::CommitResult commitRes;
    };

    void setWalletDefaultFee(CAmount fee = DEFAULT_TRANSACTION_FEE);
    bool hasWalletCustomFee();
    bool getWalletCustomFee(CAmount& nFeeRet);
    void setWalletCustomFee(bool fUseCustomFee, const CAmount nFee = DEFAULT_TRANSACTION_FEE);

    void setWalletStakeSplitThreshold(const CAmount nStakeSplitThreshold);
    CAmount getWalletStakeSplitThreshold() const;
    /* Minimum stake split threshold*/
    double getSSTMinimum() const;

    const CWalletTx* getTx(uint256 id);

    // prepare transaction for getting txfee before sending coins
    SendCoinsReturn prepareTransaction(WalletModelTransaction* transaction, const CCoinControl* coinControl = NULL, bool fIncludeDelegations = true);

    // Send coins to a list of recipients
    SendCoinsReturn sendCoins(WalletModelTransaction& transaction);

    // Prepare shielded transaction.
    OperationResult PrepareShieldedTransaction(WalletModelTransaction* modelTransaction,
                                                            bool fromTransparent,
                                                            const CCoinControl* coinControl = nullptr);

    OperationResult createAndSendProposalFeeTx(CBudgetProposal& prop);

    // Wallet encryption
    bool setWalletEncrypted(bool encrypted, const SecureString& passphrase);
    // Passphrase only needed when unlocking
    bool setWalletLocked(bool locked, const SecureString& passPhrase = SecureString(), bool stakingOnly = false);

    // Method used to "lock" the wallet only for staking purposes. Just a flag that should prevent possible movements in the wallet.
    // Passphrase only needed when unlocking.
    bool lockForStakingOnly(const SecureString& passPhrase = SecureString());

    bool changePassphrase(const SecureString& oldPass, const SecureString& newPass);
    // Is wallet unlocked for staking only?
    bool isStakingOnlyUnlocked();
    // Wallet backup
    bool backupWallet(const QString& filename);

    // RAI object for unlocking wallet, returned by requestUnlock()
    class UnlockContext
    {
    public:
        UnlockContext(WalletModel *wallet, bool valid, const WalletModel::EncryptionStatus& status_before);
        ~UnlockContext();

        bool isValid() const { return valid; }

        // Copy constructor is disabled
        UnlockContext(const UnlockContext&) = delete;
        // Copy operator and constructor transfer the context
        UnlockContext(UnlockContext&& obj) { CopyFrom(std::move(obj)); }
        UnlockContext& operator=(UnlockContext&& rhs) { CopyFrom(std::move(rhs)); return *this; }

    private:
        WalletModel *wallet;
        bool valid;
        WalletModel::EncryptionStatus was_status;   // original status
        mutable bool relock; // mutable, as it can be set to false by copying

        UnlockContext& operator=(const UnlockContext&) = default;
        void CopyFrom(UnlockContext&& rhs);
    };

    UnlockContext requestUnlock();

    bool getPubKey(const CKeyID& address, CPubKey& vchPubKeyOut) const;
    int64_t getCreationTime() const;
    int64_t getKeyCreationTime(const CPubKey& key);
    int64_t getKeyCreationTime(const CTxDestination& address);
    int64_t getKeyCreationTime(const std::string& address);
    int64_t getKeyCreationTime(const libzcash::SaplingPaymentAddress& address);
    PairResult getNewAddress(Destination& ret, std::string label = "") const;
    /**
     * Return a new address used to receive for delegated cold stake purpose.
     */
    PairResult getNewStakingAddress(Destination& ret, std::string label = "") const;

    //! Return a new shielded address.
    PairResult getNewShieldedAddress(QString& shieldedAddrRet, std::string strLabel = "");

    //! Return new wallet rescan reserver
    WalletRescanReserver getRescanReserver() const { return WalletRescanReserver(wallet); }

    bool whitelistAddressFromColdStaking(const QString &addressStr);
    bool blacklistAddressFromColdStaking(const QString &address);
    bool updateAddressBookPurpose(const QString &addressStr, const std::string& purpose);
    std::string getLabelForAddress(const CTxDestination& address);
    QString getSaplingAddressString(const CWalletTx* wtx, const SaplingOutPoint& op) const;
    bool getKeyId(const CTxDestination& address, CKeyID& keyID);
    bool getKey(const CKeyID& keyID, CKey& key) const { return wallet->GetKey(keyID, key); }
    bool haveKey(const CKeyID& keyID) const { return wallet->HaveKey(keyID); }
    bool addKeys(const CKey& key, const CPubKey& pubkey, WalletRescanReserver& reserver);

    bool isMine(const CWDestination& address);
    bool isMine(const QString& addressStr);
    bool IsShieldedDestination(const CWDestination& address);
    bool isUsed(CTxDestination address);
    bool getMNCollateralCandidate(COutPoint& outPoint);
    // Depth of a wallet transaction or -1 if not found
    int getWalletTxDepth(const uint256& txHash) const;
    bool isSpent(const COutPoint& outpoint) const;

    class ListCoinsKey {
    public:
        QString address;
        bool isChange;
        Optional<QString> stakerAddress; // used only for P2CS utxo

        bool operator==(const ListCoinsKey& key2) const {
            return address == key2.address && stakerAddress == key2.stakerAddress;
        }

        bool operator<(const ListCoinsKey& key2) const {
            return this->address < key2.address;
        }
    };

    class ListCoinsValue {
    public:
        ListCoinsValue() = delete;
        ListCoinsValue(const uint256& _txhash, int _outIndex, CAmount _nValue, int64_t _nTime, int _nDepth) :
            txhash(_txhash), outIndex(_outIndex), nValue(_nValue), nTime(_nTime), nDepth(_nDepth)
        {}

        uint256 txhash;
        int outIndex;
        CAmount nValue;
        int64_t nTime;
        int nDepth;
    };

    void listCoins(std::map<ListCoinsKey, std::vector<ListCoinsValue>>& mapCoins, bool fSelectTransparent) const;
    void listCoins(std::map<ListCoinsKey, std::vector<ListCoinsValue>>& mapCoins) const;
    void listAvailableNotes(std::map<ListCoinsKey, std::vector<ListCoinsValue>>& mapCoins) const;

    bool isLockedCoin(uint256 hash, unsigned int n) const;
    void lockCoin(COutPoint& output);
    void unlockCoin(COutPoint& output);
    std::set<COutPoint> listLockedCoins();

    void loadReceiveRequests(std::vector<std::string>& vReceiveRequests);
    bool saveReceiveRequest(const std::string& sAddress, const int64_t nId, const std::string& sRequest);

    ClientModel& clientModel() const { return *m_client_model; }
    void setClientModel(ClientModel* client_model);

    uint256 getLastBlockProcessed() const;
    int getLastBlockProcessedNum() const;

    bool hasForceCheckBalance() { return fForceCheckBalanceChanged; }
    void setCacheNumBlocks(int _cachedNumBlocks) { cachedNumBlocks = _cachedNumBlocks; }
    int getCacheNumBLocks() { return cachedNumBlocks; }
    void setCacheBlockHash(const uint256& _blockHash) { m_cached_best_block_hash = _blockHash; }
    void setfForceCheckBalanceChanged(bool _fForceCheckBalanceChanged) { fForceCheckBalanceChanged = _fForceCheckBalanceChanged; }
    Q_INVOKABLE void checkBalanceChanged(const interfaces::WalletBalances& new_balances);
    bool processBalanceChangeInternal();

    void stop();

private:
    CWallet* wallet{nullptr};
    // Simple Wallet interface.
    // todo: Goal would be to move every CWallet* call to the wallet wrapper and
    //  in the model only perform the data organization (and QT wrappers) to be presented on the UI.
    interfaces::Wallet walletWrapper;

    // Listeners
    std::unique_ptr<interfaces::Handler> m_handler_notify_status_changed;
    std::unique_ptr<interfaces::Handler> m_handler_notify_addressbook_changed;
    std::unique_ptr<interfaces::Handler> m_handler_notify_sst_changed;
    std::unique_ptr<interfaces::Handler> m_handler_notify_transaction_changed;
    std::unique_ptr<interfaces::Handler> m_handler_show_progress;
    std::unique_ptr<interfaces::Handler> m_handler_notify_watch_only_changed;
    std::unique_ptr<interfaces::Handler> m_handler_notify_walletbacked;
    ClientModel* m_client_model;

    bool fHaveWatchOnly;
    bool fForceCheckBalanceChanged;

    // Wallet has an options model for wallet-specific options
    // (transaction fee, for example)
    OptionsModel* optionsModel;

    AddressTableModel* addressTableModel;
    TransactionTableModel* transactionTableModel;
    RecentRequestsTableModel* recentRequestsTableModel;

    // Cache balance to be able to detect changes
    interfaces::WalletBalances m_cached_balances;

    EncryptionStatus cachedEncryptionStatus;
    int cachedNumBlocks;
    uint256 m_cached_best_block_hash;

    QTimer* pollTimer;
    QFuture<void> pollFuture;

    interfaces::WalletBalances getBalances() { return walletWrapper.getBalances(); };

    void subscribeToCoreSignals();
    void unsubscribeFromCoreSignals();

Q_SIGNALS:
    // Signal that balance in wallet changed
    void balanceChanged(const interfaces::WalletBalances& walletBalances);

    // Encryption status of wallet changed
    void encryptionStatusChanged(int status);

    // Signal emitted when wallet needs to be unlocked
    // It is valid behaviour for listeners to keep the wallet locked after this signal;
    // this means that the unlocking failed or was cancelled.
    void requireUnlock();

    // Fired when a message should be reported to the user
    void message(const QString& title, const QString& body, unsigned int style, bool* ret = nullptr);

    // Coins sent: from wallet, to recipient, in (serialized) transaction:
    void coinsSent(CWallet* wallet, const SendCoinsRecipient& recipient, const QByteArray& transaction);

    // Show progress dialog e.g. for rescan
    void showProgress(const QString& title, int nProgress);

    // Watch-only address added
    void notifyWatchonlyChanged(bool fHaveWatchonly);

    // Receive tab address may have changed
    void notifyReceiveAddressChanged();

    /** notify stake-split threshold changed */
    void notifySSTChanged(const double sstVal);

public Q_SLOTS:
    /* Wallet balances changes */
    void balanceNotify();
    /* Update transaction model after wallet changes */
    void updateTxModelData();
    /* Balance polling process finished */
    void pollFinished();
    /* Wallet status might have changed */
    void updateStatus();
    /* New transaction, or transaction changed status */
    void updateTransaction();
    /* New, updated or removed address book entry */
    void updateAddressBook(const QString& address, const QString& label, bool isMine, const QString& purpose, int status);
    /* Watch-only added */
    void updateWatchOnlyFlag(bool fHaveWatchonly);
    /* Current, immature or unconfirmed balance might have changed - emit 'balanceChanged' if so */
    void pollBalanceChanged();
    /* Update address book labels in the database */
    bool updateAddressBookLabels(const CWDestination& address, const std::string& strName, const std::string& strPurpose);
};

#endif // PIVX_QT_WALLETMODEL_H
