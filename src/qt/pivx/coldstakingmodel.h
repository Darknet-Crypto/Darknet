// Copyright (c) 2019 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef COLDSTAKINGMODEL_H
#define COLDSTAKINGMODEL_H

#include <QAbstractTableModel>
#include "amount.h"
#include "transactiontablemodel.h"
#include "addresstablemodel.h"
#include "transactionrecord.h"
#include "walletmodel.h"

class CSDelegation {
public:

    CSDelegation() {};
    CSDelegation(const std::string& _stakingAddress, const std::string& _ownerAddress) :
                stakingAddress(_stakingAddress), ownerAddress(_ownerAddress) {}

    std::string stakingAddress{};
    std::string ownerAddress{};
    /// Map of txId --> index num for stakeable utxo delegations
    QMap<QString, int> delegatedUtxo{};
    // Sum of all delegations to this owner address
    CAmount cachedTotalAmount{0};

    // coin owner side, set to true if it can be spend
    bool isSpendable{false};

    bool operator==(const CSDelegation& obj) {
        return obj.ownerAddress == ownerAddress;
    }
};

class ColdStakingModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit ColdStakingModel(WalletModel* model, TransactionTableModel* _tableModel, AddressTableModel* _addressTableModel, QObject *parent = nullptr);
    ~ColdStakingModel() override {
        model = nullptr;
        tableModel = nullptr;
        addressTableModel = nullptr;
        cachedDelegations.clear();
    }

    enum ColumnIndex {
        OWNER_ADDRESS = 0,
        OWNER_ADDRESS_LABEL = 1,
        STAKING_ADDRESS = 2,
        STAKING_ADDRESS_LABEL = 3,
        IS_WHITELISTED = 4,
        IS_WHITELISTED_STRING = 5,
        DELEGATED_UTXO_IDS = 6,
        TOTAL_STACKEABLE_AMOUNT_STR = 7,
        TOTAL_STACKEABLE_AMOUNT = 8,
        IS_RECEIVED_DELEGATION = 9,
        COLUMN_COUNT = 10
    };

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool whitelist(const QModelIndex& modelIndex);
    bool blacklist(const QModelIndex& index);
    void removeRowAndEmitDataChanged(const int idx);
    void updateCSList();
    CAmount getTotalAmount() const { return cachedAmount; }

    void refresh();

public Q_SLOTS:
    void emitDataSetChanged();

private:
    WalletModel* model = nullptr;
    TransactionTableModel* tableModel = nullptr;
    AddressTableModel* addressTableModel = nullptr;

    /**
     * List with all of the grouped delegations received by this wallet
     */
    QList<CSDelegation> cachedDelegations;
    CAmount cachedAmount;

    bool parseCSDelegation(const CTxOut& out, CSDelegation& ret, const QString& txId, const int utxoIndex);
};

#endif // COLDSTAKINGMODEL_H
