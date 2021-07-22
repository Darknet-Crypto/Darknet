// Copyright (c) 2011-2013 The Bitcoin developers
// Copyright (c) 2017-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "transactionfilterproxy.h"

#include "transactionrecord.h"
#include "transactiontablemodel.h"

#include <cstdlib>

// Earliest date that can be represented (far in the past)
const QDateTime TransactionFilterProxy::MIN_DATE = QDateTime::fromTime_t(0);
// Last date that can be represented (far in the future)
const QDateTime TransactionFilterProxy::MAX_DATE = QDateTime::fromTime_t(0xFFFFFFFF);

TransactionFilterProxy::TransactionFilterProxy(QObject* parent) : QSortFilterProxyModel(parent),
                                                                  dateFrom(MIN_DATE),
                                                                  dateTo(MAX_DATE),
                                                                  addrPrefix(),
                                                                  typeFilter(ALL_TYPES),
                                                                  watchOnlyFilter(WatchOnlyFilter_All),
                                                                  minAmount(0),
                                                                  limitRows(-1),
                                                                  showInactive(true),
                                                                  fHideOrphans(true)
{
}

bool TransactionFilterProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
    QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);

    int status = index.data(TransactionTableModel::StatusRole).toInt();
    if (!showInactive && status == TransactionStatus::Conflicted)
        return false;

    int type = index.data(TransactionTableModel::TypeRole).toInt();
    if (fHideOrphans && isOrphan(status, type)) return false;
    if (!(bool)(TYPE(type) & typeFilter)) return false;
    if (fOnlyZc && !isZcTx(type)) return false;
    if (fOnlyStakes && !isStakeTx(type)) return false;
    if (fOnlyColdStaking && !isColdStake(type)) return false;

    bool involvesWatchAddress = index.data(TransactionTableModel::WatchonlyRole).toBool();
    if (involvesWatchAddress && watchOnlyFilter == WatchOnlyFilter_No)
        return false;
    if (!involvesWatchAddress && watchOnlyFilter == WatchOnlyFilter_Yes)
        return false;

    QDateTime datetime = index.data(TransactionTableModel::DateRole).toDateTime();
    if (datetime < dateFrom || datetime > dateTo)
        return false;

    QString address = index.data(TransactionTableModel::AddressRole).toString();
    QString label = index.data(TransactionTableModel::LabelRole).toString();
    if (!addrPrefix.isEmpty()) {
        if (!address.contains(addrPrefix, Qt::CaseInsensitive) && !label.contains(addrPrefix, Qt::CaseInsensitive))
            return false;
    }

    qint64 amount = llabs(index.data(TransactionTableModel::AmountRole).toLongLong());
    if (amount < minAmount)
        return false;

    return true;
}

void TransactionFilterProxy::setDateRange(const QDateTime& from, const QDateTime& to)
{
    if (from == this->dateFrom && to == this->dateTo)
        return; // No need to set the range.
    this->dateFrom = from;
    this->dateTo = to;
    invalidateFilter();
}

void TransactionFilterProxy::setAddressPrefix(const QString& addrPrefix)
{
    this->addrPrefix = addrPrefix;
    invalidateFilter();
}

void TransactionFilterProxy::setTypeFilter(quint32 modes)
{
    if (typeFilter == modes) return;
    this->typeFilter = modes;
    invalidateFilter();
}

void TransactionFilterProxy::setMinAmount(const CAmount& minimum)
{
    this->minAmount = minimum;
    invalidateFilter();
}

void TransactionFilterProxy::setWatchOnlyFilter(WatchOnlyFilter filter)
{
    this->watchOnlyFilter = filter;
    invalidateFilter();
}

void TransactionFilterProxy::setLimit(int limit)
{
    this->limitRows = limit;
}

void TransactionFilterProxy::setShowInactive(bool showInactive)
{
    this->showInactive = showInactive;
    invalidateFilter();
}

void TransactionFilterProxy::setHideOrphans(bool fHide)
{
    this->fHideOrphans = fHide;
    invalidateFilter();
}

void TransactionFilterProxy::setOnlyStakes(bool fOnlyStakes)
{
    this->fOnlyStakes = fOnlyStakes;
    invalidateFilter();
}

int TransactionFilterProxy::rowCount(const QModelIndex& parent) const
{
    if (limitRows != -1) {
        return std::min(QSortFilterProxyModel::rowCount(parent), limitRows);
    } else {
        return QSortFilterProxyModel::rowCount(parent);
    }
}

bool TransactionFilterProxy::isOrphan(const int status, const int type)
{
    return ( (type == TransactionRecord::Generated || type == TransactionRecord::StakeMint ||
            type == TransactionRecord::StakeZPIV || type == TransactionRecord::MNReward)
            && (status == TransactionStatus::Conflicted || status == TransactionStatus::NotAccepted) );
}

bool TransactionFilterProxy::isZcTx(int type) const {
    return (type == TransactionRecord::ZerocoinMint || type == TransactionRecord::ZerocoinSpend || type == TransactionRecord::ZerocoinSpend_Change_zPiv
            || type == TransactionRecord::ZerocoinSpend_FromMe || type == TransactionRecord::RecvFromZerocoinSpend);
}

bool TransactionFilterProxy::isStakeTx(int type) const {
    return type == TransactionRecord::StakeMint || type == TransactionRecord::Generated || type == TransactionRecord::StakeZPIV || type == TransactionRecord::StakeDelegated;
}

bool TransactionFilterProxy::isColdStake(int type) const {
    return type == TransactionRecord::P2CSDelegation || type == TransactionRecord::P2CSDelegationSent || type == TransactionRecord::P2CSDelegationSentOwner || type == TransactionRecord::StakeDelegated || type == TransactionRecord::StakeHot;
}
