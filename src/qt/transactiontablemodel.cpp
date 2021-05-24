// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2016 The Dash developers
// Copyright (c) 2016-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "transactiontablemodel.h"

#include "addresstablemodel.h"
#include "guiconstants.h"
#include "guiutil.h"
#include "optionsmodel.h"
#include "transactionrecord.h"
#include "walletmodel.h"

#include "interfaces/handler.h"
#include "sync.h"
#include "uint256.h"
#include "wallet/wallet.h"

#include <algorithm>

#include <QColor>
#include <QDateTime>
#include <QIcon>
#include <QtConcurrent/QtConcurrent>
#include <QFuture>

#define SINGLE_THREAD_MAX_TXES_SIZE 4000

// Maximum amount of loaded records in ram in the first load.
// If the user has more and want to load them:
// TODO, add load on demand in pages (not every tx loaded all the time into the records list).
#define MAX_AMOUNT_LOADED_RECORDS 20000

// Amount column is right-aligned it contains numbers
static int column_alignments[] = {
    Qt::AlignLeft | Qt::AlignVCenter, /* status */
    Qt::AlignLeft | Qt::AlignVCenter, /* watchonly */
    Qt::AlignLeft | Qt::AlignVCenter, /* date */
    Qt::AlignLeft | Qt::AlignVCenter, /* type */
    Qt::AlignLeft | Qt::AlignVCenter, /* address */
    Qt::AlignRight | Qt::AlignVCenter /* amount */
};

// Comparison operator for sort/binary search of model tx list
struct TxLessThan {
    bool operator()(const TransactionRecord& a, const TransactionRecord& b) const
    {
        return a.hash < b.hash;
    }
    bool operator()(const TransactionRecord& a, const uint256& b) const
    {
        return a.hash < b;
    }
    bool operator()(const uint256& a, const TransactionRecord& b) const
    {
        return a < b.hash;
    }
};

struct ConvertTxToVectorResult
{
    QList<TransactionRecord> records;
    qint64 nFirstLoadedTxTime{0};
};

// Private implementation
class TransactionTablePriv
{
public:
    TransactionTablePriv(CWallet* wallet, TransactionTableModel* parent) : wallet(wallet),
                                                                           parent(parent)
    {
    }

    CWallet* wallet{nullptr};
    TransactionTableModel* parent;

    /* Local cache of wallet.
     * As it is in the same order as the CWallet, by definition
     * this is sorted by sha256.
     */
    QList<TransactionRecord> cachedWallet;

    /**
     * Time of the oldest transaction loaded into the model.
     * It can or not be the first tx in the wallet, the model only loads the last 20k txs.
     */
    qint64 nFirstLoadedTxTime{0};

    /* Query entire wallet anew from core.
     */
    void refreshWallet()
    {
        qDebug() << "TransactionTablePriv::refreshWallet";
        cachedWallet.clear();

        std::vector<CWalletTx> walletTxes = wallet->getWalletTxs();

        // Divide the work between multiple threads to speedup the process if the vector is larger than 4k txes
        std::size_t txesSize = walletTxes.size();
        if (txesSize > SINGLE_THREAD_MAX_TXES_SIZE) {

            // First check if the amount of txs exceeds the UI limit
            if (txesSize > MAX_AMOUNT_LOADED_RECORDS) {
                // Sort the txs by date just to be really really sure that them are ordered.
                // (this extra calculation should be removed in the future if can ensure that
                // txs are stored in order in the db, which is what should be happening)
                sort(walletTxes.begin(), walletTxes.end(),
                        [](const CWalletTx & a, const CWalletTx & b) -> bool {
                         return a.GetTxTime() > b.GetTxTime();
                     });

                // Only latest ones.
                walletTxes = std::vector<CWalletTx>(walletTxes.begin(), walletTxes.begin() + MAX_AMOUNT_LOADED_RECORDS);
                txesSize = walletTxes.size();
            }

            // Simple way to get the processors count
            std::size_t threadsCount = (QThreadPool::globalInstance()->maxThreadCount() / 2 ) + 1;

            // Size of the tx subsets
            std::size_t const subsetSize = txesSize / (threadsCount + 1);
            std::size_t totalSumSize = 0;
            QList<QFuture<ConvertTxToVectorResult>> tasks;

            // Subsets + run task
            for (std::size_t i = 0; i < threadsCount; ++i) {
                tasks.append(
                        QtConcurrent::run(
                                convertTxToRecords,
                                this,
                                wallet,
                                std::vector<CWalletTx>(walletTxes.begin() + totalSumSize, walletTxes.begin() + totalSumSize + subsetSize)
                        )
                 );
                totalSumSize += subsetSize;
            }

            // Now take the remaining ones and do the work here
            std::size_t const remainingSize = txesSize - totalSumSize;
            auto res = convertTxToRecords(this, wallet,
                                              std::vector<CWalletTx>(walletTxes.end() - remainingSize, walletTxes.end())
            );
            cachedWallet.append(res.records);
            nFirstLoadedTxTime = res.nFirstLoadedTxTime;

            for (auto &future : tasks) {
                future.waitForFinished();
                ConvertTxToVectorResult convertRes = future.result();
                cachedWallet.append(convertRes.records);
                if (nFirstLoadedTxTime > convertRes.nFirstLoadedTxTime) {
                    nFirstLoadedTxTime = convertRes.nFirstLoadedTxTime;
                }
            }
        } else {
            // Single thread flow
            ConvertTxToVectorResult convertRes = convertTxToRecords(this, wallet, walletTxes);
            cachedWallet.append(convertRes.records);
            nFirstLoadedTxTime = convertRes.nFirstLoadedTxTime;
        }
    }

    void emitTxLoaded(const TransactionRecord& rec)
    {
        Q_EMIT parent->txLoaded(QString::fromStdString(rec.hash.GetHex()),
                                rec.type, rec.status.status);
    }

    static ConvertTxToVectorResult convertTxToRecords(TransactionTablePriv* tablePriv,
                                                      const CWallet* wallet,
                                                      const std::vector<CWalletTx>& walletTxes)
    {
        ConvertTxToVectorResult res;
        for (const auto& tx : walletTxes) {
            QList<TransactionRecord> records = TransactionRecord::decomposeTransaction(wallet, tx);
            if (records.isEmpty()) continue;
            qint64 time = records.first().time;
            if (res.nFirstLoadedTxTime == 0 || res.nFirstLoadedTxTime > time) {
                res.nFirstLoadedTxTime = time;
            }
            for (const auto& rec : records) {
                tablePriv->emitTxLoaded(rec);
            }
            res.records.append(records);
        }
        return res;
    }

    /* Update our model of the wallet incrementally, to synchronize our model of the wallet
       with that of the core.

       Call with transaction that was added, removed or changed.
     */
    void updateWallet(const uint256& hash, int status, bool showTransaction, TransactionRecord& ret)
    {
        qDebug() << "TransactionTablePriv::updateWallet : " + QString::fromStdString(hash.ToString()) + " " + QString::number(status);

        // Find bounds of this transaction in model
        QList<TransactionRecord>::iterator lower = std::lower_bound(
            cachedWallet.begin(), cachedWallet.end(), hash, TxLessThan());
        QList<TransactionRecord>::iterator upper = std::upper_bound(
            cachedWallet.begin(), cachedWallet.end(), hash, TxLessThan());
        int lowerIndex = (lower - cachedWallet.begin());
        int upperIndex = (upper - cachedWallet.begin());
        bool inModel = (lower != upper);

        if (status == CT_UPDATED) {
            if (showTransaction && !inModel)
                status = CT_NEW; /* Not in model, but want to show, treat as new */
            if (!showTransaction && inModel)
                status = CT_DELETED; /* In model, but want to hide, treat as deleted */
        }

        qDebug() << "    inModel=" + QString::number(inModel) +
                        " Index=" + QString::number(lowerIndex) + "-" + QString::number(upperIndex) +
                        " showTransaction=" + QString::number(showTransaction) + " derivedStatus=" + QString::number(status);

        switch (status) {
            case CT_NEW:
                if (inModel) {
                    qWarning() << "TransactionTablePriv::updateWallet : Warning: Got CT_NEW, but transaction is already in model";
                    break;
                }
                if (showTransaction) {
                    // Find transaction in wallet
                    const CWalletTx* wtx = wallet->GetWalletTx(hash);
                    if (!wtx) {
                        qWarning() << "TransactionTablePriv::updateWallet : Warning: Got CT_NEW, but transaction is not in wallet";
                        break;
                    }

                    // As old transactions are still getting updated (+20k range),
                    // do not add them if we deliberately didn't load them at startup.
                    if (cachedWallet.size() >= MAX_AMOUNT_LOADED_RECORDS && wtx->GetTxTime() < nFirstLoadedTxTime) {
                        return;
                    }

                    // Added -- insert at the right position
                    QList<TransactionRecord> toInsert =
                        TransactionRecord::decomposeTransaction(wallet, *wtx);
                    if (!toInsert.isEmpty()) { /* only if something to insert */
                        parent->beginInsertRows(QModelIndex(), lowerIndex, lowerIndex + toInsert.size() - 1);
                        int insert_idx = lowerIndex;
                        for (const TransactionRecord& rec : toInsert) {
                            cachedWallet.insert(insert_idx, rec);
                            insert_idx += 1;
                            ret = rec; // Return record
                        }
                        parent->endInsertRows();
                    }
                }
                break;
            case CT_DELETED:
                if (!inModel) {
                    qWarning() << "TransactionTablePriv::updateWallet : Warning: Got CT_DELETED, but transaction is not in model";
                    break;
                }
                // Removed -- remove entire transaction from table
                parent->beginRemoveRows(QModelIndex(), lowerIndex, upperIndex - 1);
                cachedWallet.erase(lower, upper);
                parent->endRemoveRows();
                break;
            case CT_UPDATED:
                // Miscellaneous updates -- nothing to do, status update will take care of this, and is only computed for
                // visible transactions.
                for (int i = lowerIndex; i < upperIndex; i++) {
                    TransactionRecord *rec = &cachedWallet[i];
                    rec->status.needsUpdate = true;
                }
                break;
        }
    }

    int size()
    {
        return cachedWallet.size();
    }

    TransactionRecord* index(int cur_block_num, const uint256& cur_block_hash, int idx)
    {
        if (idx >= 0 && idx < cachedWallet.size()) {
            TransactionRecord* rec = &cachedWallet[idx];
            if (!cur_block_hash.IsNull() && rec->statusUpdateNeeded(cur_block_num)) {
                // If a status update is needed (blocks came in since last check),
                // update the status of this transaction from the wallet. Otherwise,
                // simply re-use the cached status.
                TRY_LOCK(wallet->cs_wallet, lockWallet);
                if (lockWallet) {
                    auto mi = wallet->mapWallet.find(rec->hash);
                    if (mi != wallet->mapWallet.end()) {
                        rec->updateStatus(mi->second, cur_block_num);
                    }
                }
            }
            return rec;
        }
        return nullptr;
    }
};

TransactionTableModel::TransactionTableModel(CWallet* wallet, WalletModel* parent) : QAbstractTableModel(parent),
                                                                                     wallet(wallet),
                                                                                     walletModel(parent),
                                                                                     priv(new TransactionTablePriv(wallet, this)),
                                                                                     fProcessingQueuedTransactions(false)
{
    columns << QString() << QString() << tr("Date") << tr("Type") << tr("Address") << BitcoinUnits::getAmountColumnTitle(walletModel->getOptionsModel()->getDisplayUnit());

    connect(walletModel->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &TransactionTableModel::updateDisplayUnit);
}

void TransactionTableModel::init()
{
    priv->refreshWallet();
    subscribeToCoreSignals();
}

TransactionTableModel::~TransactionTableModel()
{
    unsubscribeFromCoreSignals();
    delete priv;
}

/** Updates the column title to "Amount (DisplayUnit)" and emits headerDataChanged() signal for table headers to react. */
void TransactionTableModel::updateAmountColumnTitle()
{
    columns[Amount] = BitcoinUnits::getAmountColumnTitle(walletModel->getOptionsModel()->getDisplayUnit());
    Q_EMIT headerDataChanged(Qt::Horizontal, Amount, Amount);
}

void TransactionTableModel::updateTransaction(const QString& hash, int status, bool showTransaction)
{
    uint256 updated;
    updated.SetHex(hash.toStdString());

    TransactionRecord rec(0);
    priv->updateWallet(updated, status, showTransaction, rec);

    if (!rec.isNull())
        Q_EMIT txArrived(hash, rec.isCoinStake(), rec.isAnyColdStakingType());
}

void TransactionTableModel::updateConfirmations()
{
    // Blocks came in since last poll.
    // Invalidate status (number of confirmations) and (possibly) description
    //  for all rows. Qt is smart enough to only actually request the data for the
    //  visible rows.
    Q_EMIT dataChanged(index(0, Status), index(priv->size() - 1, Status));
    Q_EMIT dataChanged(index(0, ToAddress), index(priv->size() - 1, ToAddress));
}

int TransactionTableModel::rowCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    return priv->size();
}

int TransactionTableModel::columnCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    return columns.length();
}

int TransactionTableModel::size() const{
    return priv->size();
}

QString TransactionTableModel::formatTxStatus(const TransactionRecord* wtx) const
{
    QString status;

    switch (wtx->status.status) {
    case TransactionStatus::OpenUntilBlock:
        status = tr("Open for %n more block(s)", "", wtx->status.open_for);
        break;
    case TransactionStatus::OpenUntilDate:
        status = tr("Open until %1").arg(GUIUtil::dateTimeStr(wtx->status.open_for));
        break;
    case TransactionStatus::Unconfirmed:
        status = tr("Unconfirmed");
        break;
    case TransactionStatus::Confirming:
        status = tr("Confirming (%1 of %2 recommended confirmations)").arg(wtx->status.depth).arg(TransactionRecord::RecommendedNumConfirmations);
        break;
    case TransactionStatus::Confirmed:
        status = tr("Confirmed (%1 confirmations)").arg(wtx->status.depth);
        break;
    case TransactionStatus::Conflicted:
        status = tr("Conflicted");
        break;
    case TransactionStatus::Immature:
        status = tr("Immature (%1 confirmations, will be available after %2)").arg(wtx->status.depth).arg(wtx->status.depth + wtx->status.matures_in);
        break;
    case TransactionStatus::NotAccepted:
        status = tr("Orphan Block - Generated but not accepted. This does not impact your holdings.");
        break;
    }

    return status;
}

QString TransactionTableModel::formatTxDate(const TransactionRecord* wtx) const
{
    if (wtx->time) {
        return GUIUtil::dateTimeStr(wtx->time);
    }
    return QString();
}

/* Look up address in address book, if found return label (address)
   otherwise just return (address)
 */
QString TransactionTableModel::lookupAddress(const std::string& address, bool tooltip) const
{
    QString label = walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(address));
    QString description;
    if (!label.isEmpty()) {
        description += label + QString(" ");
    }
    if (label.isEmpty() || tooltip) {
        description += QString::fromStdString(address);
    }
    return description;
}

QString TransactionTableModel::formatTxType(const TransactionRecord* wtx) const
{
    switch (wtx->type) {
    case TransactionRecord::RecvWithAddress:
        return tr("Received with");
    case TransactionRecord::MNReward:
        return tr("Masternode Reward");
    case TransactionRecord::RecvFromOther:
        return tr("Received from");
    case TransactionRecord::SendToAddress:
    case TransactionRecord::SendToOther:
        return tr("Sent to");
    case TransactionRecord::SendToSelf:
        return tr("Payment to yourself");
    case TransactionRecord::SendToSelfShieldedAddress:
        return tr("Shielding coins to yourself");
    case TransactionRecord::SendToSelfShieldToTransparent:
        return tr("Unshielding coins to yourself");
    case TransactionRecord::SendToSelfShieldToShieldChangeAddress:
        return tr("Shielded change, transfer between own shielded addresses");
    case TransactionRecord::StakeMint:
        return tr("%1 Stake").arg(CURRENCY_UNIT.c_str());
    case TransactionRecord::StakeZPIV:
        return tr("z%1 Stake").arg(CURRENCY_UNIT.c_str());
    case TransactionRecord::StakeDelegated:
        return tr("%1 Cold Stake").arg(CURRENCY_UNIT.c_str());
    case TransactionRecord::StakeHot:
        return tr("%1 Stake on behalf of").arg(CURRENCY_UNIT.c_str());
    case TransactionRecord::P2CSDelegationSent:
    case TransactionRecord::P2CSDelegationSentOwner:
    case TransactionRecord::P2CSDelegation:
        return tr("Stake delegation");
    case TransactionRecord::P2CSUnlockOwner:
    case TransactionRecord::P2CSUnlockStaker:
        return tr("Stake delegation spent by");
    case TransactionRecord::Generated:
        return tr("Mined");
    case TransactionRecord::ZerocoinMint:
        return tr("Converted %1 to z%1").arg(CURRENCY_UNIT.c_str());
    case TransactionRecord::ZerocoinSpend:
        return tr("Spent z%1").arg(CURRENCY_UNIT.c_str());
    case TransactionRecord::RecvFromZerocoinSpend:
        return tr("Received %1 from z%1").arg(CURRENCY_UNIT.c_str());
    case TransactionRecord::ZerocoinSpend_Change_zPiv:
        return tr("Minted Change as z%1 from z%1 Spend").arg(CURRENCY_UNIT.c_str());
    case TransactionRecord::ZerocoinSpend_FromMe:
        return tr("Converted z%1 to %1").arg(CURRENCY_UNIT.c_str());
    case TransactionRecord::RecvWithShieldedAddress:
        return tr("Received with shielded");
    case TransactionRecord::SendToShielded:
        return tr("Shielded send to");
    case TransactionRecord::SendToNobody:
        return tr("Burned PIVs");
    default:
        return QString();
    }
}

QVariant TransactionTableModel::txAddressDecoration(const TransactionRecord* wtx) const
{
    switch (wtx->type) {
    case TransactionRecord::Generated:
    case TransactionRecord::StakeMint:
    case TransactionRecord::StakeZPIV:
    case TransactionRecord::MNReward:
        return QIcon(":/icons/tx_mined");
    case TransactionRecord::RecvWithAddress:
    case TransactionRecord::RecvFromOther:
    case TransactionRecord::RecvFromZerocoinSpend:
        return QIcon(":/icons/tx_input");
    case TransactionRecord::SendToAddress:
    case TransactionRecord::SendToOther:
    case TransactionRecord::ZerocoinSpend:
        return QIcon("://ic-transaction-sent");
    default:
        return QIcon(":/icons/tx_inout");
    }
}

QString TransactionTableModel::formatTxToAddress(const TransactionRecord* wtx, bool tooltip) const
{
    QString watchAddress;
    if (tooltip) {
        // Mark transactions involving watch-only addresses by adding " (watch-only)"
        watchAddress = wtx->involvesWatchAddress ? QString(" (") + tr("watch-only") + QString(")") : "";
    }

    switch (wtx->type) {
    case TransactionRecord::RecvFromOther:
        return QString::fromStdString(wtx->address) + watchAddress;
    case TransactionRecord::RecvWithAddress:
    case TransactionRecord::MNReward:
    case TransactionRecord::SendToAddress:
    case TransactionRecord::Generated:
    case TransactionRecord::StakeMint:
    case TransactionRecord::ZerocoinSpend:
    case TransactionRecord::ZerocoinSpend_FromMe:
    case TransactionRecord::RecvFromZerocoinSpend:
        return lookupAddress(wtx->address, tooltip);
    case TransactionRecord::RecvWithShieldedAddress:
    case TransactionRecord::SendToShielded:
        // todo: add addressbook support for shielded addresses.
        return QString::fromStdString(wtx->address);
    case TransactionRecord::SendToOther:
        return QString::fromStdString(wtx->address) + watchAddress;
    case TransactionRecord::SendToNobody:
        return QString::fromStdString(wtx->address); // the address here is storing the op_return data.
    case TransactionRecord::ZerocoinMint:
    case TransactionRecord::ZerocoinSpend_Change_zPiv:
    case TransactionRecord::StakeZPIV:
        return tr("Anonymous");
    case TransactionRecord::P2CSDelegation:
    case TransactionRecord::P2CSDelegationSent:
    case TransactionRecord::P2CSDelegationSentOwner:
    case TransactionRecord::P2CSUnlockOwner:
    case TransactionRecord::P2CSUnlockStaker:
    case TransactionRecord::StakeDelegated:
    case TransactionRecord::StakeHot:
    case TransactionRecord::SendToSelf: {
        QString label = walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(wtx->address));
        return label.isEmpty() ? "" : label;
    }
    case TransactionRecord::SendToSelfShieldedAddress:
    case TransactionRecord::SendToSelfShieldToTransparent:
    case TransactionRecord::SendToSelfShieldToShieldChangeAddress:
        // Do not show the send to self address. todo: add addressbook for shielded addr
        return "";
    default: {
        if (watchAddress.isEmpty()) {
            return tr("No information");
        } else {
            return tr("(n/a)") + watchAddress;
        }
    }
    }
}

QVariant TransactionTableModel::addressColor(const TransactionRecord* wtx) const
{
    switch (wtx->type) {
    // Show addresses without label in a less visible color
    case TransactionRecord::RecvWithAddress:
    case TransactionRecord::SendToAddress:
    case TransactionRecord::Generated:
    case TransactionRecord::MNReward: {
        QString label = walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(wtx->address));
        if (label.isEmpty())
            return COLOR_BAREADDRESS;
    }
    case TransactionRecord::SendToSelf:
    default:
        // To avoid overriding above conditional formats a default text color for this QTableView is not defined in stylesheet,
        // so we must always return a color here
        return COLOR_BLACK;
    }
}

QString TransactionTableModel::formatTxAmount(const TransactionRecord* wtx, bool showUnconfirmed, BitcoinUnits::SeparatorStyle separators) const
{
    QString str = BitcoinUnits::format(walletModel->getOptionsModel()->getDisplayUnit(), wtx->credit + wtx->debit, false, separators);
    if (showUnconfirmed) {
        if (!wtx->status.countsForBalance) {
            str = QString("[") + str + QString("]");
        }
    }
    return QString(str);
}

QVariant TransactionTableModel::txStatusDecoration(const TransactionRecord* wtx) const
{
    switch (wtx->status.status) {
    case TransactionStatus::OpenUntilBlock:
    case TransactionStatus::OpenUntilDate:
        return COLOR_TX_STATUS_OPENUNTILDATE;
    case TransactionStatus::Unconfirmed:
        return QIcon(":/icons/transaction_0");
    case TransactionStatus::Confirming:
        switch (wtx->status.depth) {
        case 1:
            return QIcon(":/icons/transaction_1");
        case 2:
            return QIcon(":/icons/transaction_2");
        case 3:
            return QIcon(":/icons/transaction_3");
        case 4:
            return QIcon(":/icons/transaction_4");
        default:
            return QIcon(":/icons/transaction_5");
        };
    case TransactionStatus::Confirmed:
        return QIcon(":/icons/transaction_confirmed");
    case TransactionStatus::Conflicted:
        return QIcon(":/icons/transaction_conflicted");
    case TransactionStatus::Immature: {
        int total = wtx->status.depth + wtx->status.matures_in;
        int part = (wtx->status.depth * 5 / total) + 1;
        return QIcon(QString(":/icons/transaction_%1").arg(part));
    }
    case TransactionStatus::NotAccepted:
        return QIcon(":/icons/transaction_0");
    default:
        return COLOR_BLACK;
    }
}

QVariant TransactionTableModel::txWatchonlyDecoration(const TransactionRecord* wtx) const
{
    if (wtx->involvesWatchAddress)
        return QIcon(":/icons/eye");
    else
        return QVariant();
}

QString TransactionTableModel::formatTooltip(const TransactionRecord* rec) const
{
    return formatTxStatus(rec);
}

QVariant TransactionTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return QVariant();
    TransactionRecord* rec = static_cast<TransactionRecord*>(index.internalPointer());

    switch (role) {
    case Qt::DecorationRole:
        switch (index.column()) {
        case Status:
            return txStatusDecoration(rec);
        case Watchonly:
            return txWatchonlyDecoration(rec);
        case ToAddress:
            return txAddressDecoration(rec);
        }
        break;
    case Qt::DisplayRole:
        switch (index.column()) {
        case Date:
            return formatTxDate(rec);
        case Type:
            return formatTxType(rec);
        case ToAddress:
            return formatTxToAddress(rec, false);
        case Amount:
            return formatTxAmount(rec, true, BitcoinUnits::separatorAlways);
        }
        break;
    case Qt::EditRole:
        // Edit role is used for sorting, so return the unformatted values
        switch (index.column()) {
        case Status:
            return QString::fromStdString(rec->status.sortKey);
        case Date:
            return rec->time;
        case Type:
            return formatTxType(rec);
        case Watchonly:
            return (rec->involvesWatchAddress ? 1 : 0);
        case ToAddress:
            return formatTxToAddress(rec, true);
        case Amount:
            return qint64(rec->credit + rec->debit);
        }
        break;
    case Qt::ToolTipRole:
        return formatTooltip(rec);
    case Qt::TextAlignmentRole:
        return column_alignments[index.column()];
    case Qt::ForegroundRole:
        // Minted
        if (rec->type == TransactionRecord::Generated || rec->type == TransactionRecord::StakeMint ||
                rec->type == TransactionRecord::StakeZPIV || rec->type == TransactionRecord::MNReward) {
            if (rec->status.status == TransactionStatus::Conflicted || rec->status.status == TransactionStatus::NotAccepted)
                return COLOR_ORPHAN;
            else
                return COLOR_STAKE;
        }
        // Conflicted tx
        if (rec->status.status == TransactionStatus::Conflicted || rec->status.status == TransactionStatus::NotAccepted) {
            return COLOR_CONFLICTED;
        }
        // Unconfimed or immature
        if ((rec->status.status == TransactionStatus::Unconfirmed) || (rec->status.status == TransactionStatus::Immature)) {
            return COLOR_UNCONFIRMED;
        }
        if (index.column() == Amount && (rec->credit + rec->debit) < 0) {
            return COLOR_NEGATIVE;
        }
        if (index.column() == ToAddress) {
            return addressColor(rec);
        }

        // To avoid overriding above conditional formats a default text color for this QTableView is not defined in stylesheet,
        // so we must always return a color here
        return COLOR_BLACK;
    case TypeRole:
        return rec->type;
    case SizeRole:
        return rec->size;
    case DateRole:
        return QDateTime::fromTime_t(static_cast<uint>(rec->time));
    case WatchonlyRole:
        return rec->involvesWatchAddress;
    case WatchonlyDecorationRole:
        return txWatchonlyDecoration(rec);
    case AddressRole:
        return QString::fromStdString(rec->address);
    case LabelRole:
        return walletModel->getAddressTableModel()->labelForAddress(QString::fromStdString(rec->address));
    case AmountRole:
        return qint64(rec->credit + rec->debit);
    case ShieldedCreditAmountRole:
        return  rec->shieldedCredit ? qint64(*rec->shieldedCredit) : 0;
    case TxHashRole:
        return QString::fromStdString(rec->hash.ToString());
    case ConfirmedRole:
        return rec->status.countsForBalance;
    case FormattedAmountRole:
        // Used for copy/export, so don't include separators
        return formatTxAmount(rec, false, BitcoinUnits::separatorNever);
    case StatusRole:
        return rec->status.status;
    }
    return QVariant();
}

QVariant TransactionTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal) {
        if (role == Qt::DisplayRole) {
            return columns[section];
        } else if (role == Qt::TextAlignmentRole) {
            return column_alignments[section];
        } else if (role == Qt::ToolTipRole) {
            switch (section) {
            case Status:
                return tr("Transaction status. Hover over this field to show number of confirmations.");
            case Date:
                return tr("Date and time that the transaction was received.");
            case Type:
                return tr("Type of transaction.");
            case Watchonly:
                return tr("Whether or not a watch-only address is involved in this transaction.");
            case ToAddress:
                return tr("Destination address of transaction.");
            case Amount:
                return tr("Amount removed from or added to balance.");
            }
        }
    }
    return QVariant();
}

QModelIndex TransactionTableModel::index(int row, int column, const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    TransactionRecord* data = priv->index(walletModel->getLastBlockProcessedNum(),
                                          walletModel->getLastBlockProcessed(),
                                          row);
    if (data) {
        return createIndex(row, column, data);
    }
    return QModelIndex();
}

void TransactionTableModel::updateDisplayUnit()
{
    // emit dataChanged to update Amount column with the current unit
    updateAmountColumnTitle();
    Q_EMIT dataChanged(index(0, Amount), index(priv->size() - 1, Amount));
}

// queue notifications to show a non freezing progress dialog e.g. for rescan
struct TransactionNotification {
public:
    TransactionNotification() = delete; // need to specify hash and status
    TransactionNotification(uint256 hash, ChangeType status) : hash(hash), status(status) {}

    void invoke(QObject* ttm)
    {
        QString strHash = QString::fromStdString(hash.GetHex());
        qDebug() << "NotifyTransactionChanged : " + strHash + " status= " + QString::number(status);
        QMetaObject::invokeMethod(ttm, "updateTransaction", Qt::QueuedConnection,
            Q_ARG(QString, strHash),
            Q_ARG(int, status),
            Q_ARG(bool, true));
    }

private:
    uint256 hash;
    ChangeType status;
};

static bool fQueueNotifications = false;
static std::vector<TransactionNotification> vQueueNotifications;

static void NotifyTransactionChanged(TransactionTableModel* ttm, CWallet* wallet, const uint256& hash, ChangeType status)
{

    TransactionNotification notification(hash, status);

    if (fQueueNotifications)
    {
        vQueueNotifications.push_back(notification);
        return;
    }
    notification.invoke(ttm);
}

static void ShowProgress(TransactionTableModel* ttm, const std::string& title, int nProgress)
{
    if (nProgress == 0)
        fQueueNotifications = true;

    if (nProgress == 100) {
        fQueueNotifications = false;
        if (vQueueNotifications.size() > 10) // prevent balloon spam, show maximum 10 balloons
            QMetaObject::invokeMethod(ttm, "setProcessingQueuedTransactions", Qt::QueuedConnection, Q_ARG(bool, true));
        for (unsigned int i = 0; i < vQueueNotifications.size(); ++i) {
            if (vQueueNotifications.size() - i <= 10)
                QMetaObject::invokeMethod(ttm, "setProcessingQueuedTransactions", Qt::QueuedConnection, Q_ARG(bool, false));

            vQueueNotifications[i].invoke(ttm);
        }
        std::vector<TransactionNotification>().swap(vQueueNotifications); // clear
    }
}

void TransactionTableModel::subscribeToCoreSignals()
{
    // Connect signals to wallet
    m_handler_transaction_changed = interfaces::MakeHandler(wallet->NotifyTransactionChanged.connect(std::bind(NotifyTransactionChanged, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)));
    m_handler_show_progress = interfaces::MakeHandler(wallet->ShowProgress.connect(std::bind(ShowProgress, this, std::placeholders::_1, std::placeholders::_2)));
}

void TransactionTableModel::unsubscribeFromCoreSignals()
{
    // Disconnect signals from wallet
    m_handler_transaction_changed->disconnect();
    m_handler_show_progress->disconnect();
}
