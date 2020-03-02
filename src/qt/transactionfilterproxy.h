// Copyright (c) 2011-2013 The Bitcoin developers
// Copyright (c) 2017-2019 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_TRANSACTIONFILTERPROXY_H
#define BITCOIN_QT_TRANSACTIONFILTERPROXY_H

#include "amount.h"

#include <QDateTime>
#include <QSortFilterProxyModel>

/** Filter the transaction list according to pre-specified rules. */
class TransactionFilterProxy : public QSortFilterProxyModel
{
    Q_OBJECT

public:
    explicit TransactionFilterProxy(QObject* parent = 0);

    /** Earliest date that can be represented (far in the past) */
    static const QDateTime MIN_DATE;
    /** Last date that can be represented (far in the future) */
    static const QDateTime MAX_DATE;
    /** Type filter bit field (all types) */
    static const quint32 ALL_TYPES = 0xFFFFFFFF;
    /** Type filter bit field (all types but Obfuscation-SPAM ... enum 0-14 are common) */
    static const quint32 COMMON_TYPES = 0x0005FFFF;

    static quint32 TYPE(int type) { return 1 << type; }

    enum WatchOnlyFilter {
        WatchOnlyFilter_All,
        WatchOnlyFilter_Yes,
        WatchOnlyFilter_No
    };

    void setDateRange(const QDateTime& from, const QDateTime& to);
    void clearDateRange() {
        if (dateFrom != MIN_DATE || dateTo == MAX_DATE)
            setDateRange(MIN_DATE, MAX_DATE);
    }

    void setAddressPrefix(const QString& addrPrefix);
    /**
      @note Type filter takes a bit field created with TYPE() or ALL_TYPES
     */
    void setTypeFilter(quint32 modes);
    void setMinAmount(const CAmount& minimum);
    void setWatchOnlyFilter(WatchOnlyFilter filter);

    /** Set maximum number of rows returned, -1 if unlimited. */
    void setLimit(int limit);

    /** Set whether to show conflicted transactions. */
    void setShowInactive(bool showInactive);

    /** Set whether to hide orphan stakes. */
    void setHideOrphans(bool fHide);

    /** Only zc txes **/
    void setShowZcTxes(bool fOnlyZc);

    /** Only stakes txes **/
    void setOnlyStakes(const bool& fOnlyStakes, const bool& fIncludeZPIV = false, const bool& fShowColdStakes = false, const bool& fIncludeMNReward = false);

    /** Shows only p2cs-p2cs && xxx-p2cs **/
    void setOnlyColdStakes(bool fOnlyColdStakes);

    int rowCount(const QModelIndex& parent = QModelIndex()) const;
    static bool isOrphan(const int status, const int type);

    //QVariant dataFromSourcePos(int sourceRow, int role) const;

protected:
    bool filterAcceptsRow(int source_row, const QModelIndex& source_parent) const;

private:
    QDateTime dateFrom;
    QDateTime dateTo;
    QString addrPrefix;
    quint32 typeFilter;
    WatchOnlyFilter watchOnlyFilter;
    CAmount minAmount;
    int limitRows;
    bool showInactive;
    bool fHideOrphans = true;
    bool fOnlyZc = false;
    bool fOnlyStakes = false;
    bool fShowOwnColdStakes = false;
    bool fShowZpivStakes = true;
    bool fIncludeMNReward = true;
    bool fOnlyColdStaking = false;

    bool isZcTx(int type) const;
    bool isStakeTx(int type) const;
    bool isColdStake(int type) const;
    bool isColdStakeMine(int type) const;
};

#endif // BITCOIN_QT_TRANSACTIONFILTERPROXY_H
