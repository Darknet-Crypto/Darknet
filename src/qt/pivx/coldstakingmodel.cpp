// Copyright (c) 2019 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/coldstakingmodel.h"
#include "uint256.h"
#include "bitcoinunits.h"
#include "guiutil.h"
#include <iostream>
#include "addressbook.h"

ColdStakingModel::ColdStakingModel(WalletModel* _model,
                                   TransactionTableModel* _tableModel,
                                   AddressTableModel* _addressTableModel,
                                   QObject *parent) : QAbstractTableModel(parent), model(_model), tableModel(_tableModel), addressTableModel(_addressTableModel), cachedAmount(0){
}

void ColdStakingModel::updateCSList()
{
    refresh();
    QMetaObject::invokeMethod(this, "emitDataSetChanged", Qt::QueuedConnection);
}

void ColdStakingModel::emitDataSetChanged()
{
    Q_EMIT dataChanged(index(0, 0, QModelIndex()), index(cachedDelegations.size(), COLUMN_COUNT, QModelIndex()) );
}

void ColdStakingModel::refresh()
{
    cachedDelegations.clear();
    cachedAmount = 0;
    // First get all of the p2cs utxo inside the wallet
    std::vector<COutput> utxoList;
    model->getAvailableP2CSCoins(utxoList);

    if (!utxoList.empty()) {
        // Loop over each COutput into a CSDelegation
        for (const auto& utxo : utxoList) {

            const auto *wtx = utxo.tx;
            const QString txId = QString::fromStdString(wtx->GetHash().GetHex());
            const CTxOut& out = wtx->tx->vout[utxo.i];

            // First parse the cs delegation
            CSDelegation delegation;
            if (!parseCSDelegation(out, delegation, txId, utxo.i))
                continue;

            // it's spendable only when this wallet has the keys to spend it, a.k.a is the owner
            delegation.isSpendable = utxo.fSpendable;
            delegation.cachedTotalAmount += out.nValue;
            delegation.delegatedUtxo.insert(txId, utxo.i);

            // Now verify if the delegation exists in the cached list
            int indexDel = cachedDelegations.indexOf(delegation);
            if (indexDel == -1) {
                // If it doesn't, let's append it.
                cachedDelegations.append(delegation);
            } else {
                CSDelegation& del = cachedDelegations[indexDel];
                del.delegatedUtxo.unite(delegation.delegatedUtxo);
                del.cachedTotalAmount += delegation.cachedTotalAmount;
            }

            // add amount to cachedAmount if either:
            // - this is a owned delegation
            // - this is a staked delegation, and the owner is whitelisted
            if (!delegation.isSpendable && !addressTableModel->isWhitelisted(delegation.ownerAddress)) continue;
            cachedAmount += delegation.cachedTotalAmount;
        }
    }
}

bool ColdStakingModel::parseCSDelegation(const CTxOut& out, CSDelegation& ret, const QString& txId, const int utxoIndex)
{
    txnouttype type;
    std::vector<CTxDestination> addresses;
    int nRequired;

    if (!ExtractDestinations(out.scriptPubKey, type, addresses, nRequired) || addresses.size() != 2) {
        return error("%s : Error extracting P2CS destinations for utxo: %s-%d",
                __func__, txId.toStdString(), utxoIndex);
    }

    std::string stakingAddressStr = EncodeDestination(
            addresses[0],
            CChainParams::STAKING_ADDRESS
    );

    std::string ownerAddressStr = EncodeDestination(
            addresses[1],
            CChainParams::PUBKEY_ADDRESS
    );

    ret = CSDelegation(stakingAddressStr, ownerAddressStr);

    return true;
}

int ColdStakingModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return cachedDelegations.size();
}

int ColdStakingModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return COLUMN_COUNT;
}


QVariant ColdStakingModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
            return QVariant();

    int row = index.row();
    CSDelegation rec = cachedDelegations[row];
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
            case OWNER_ADDRESS:
                return QString::fromStdString(rec.ownerAddress);
            case OWNER_ADDRESS_LABEL:
                return addressTableModel->labelForAddress(QString::fromStdString(rec.ownerAddress));
            case STAKING_ADDRESS:
                return QString::fromStdString(rec.stakingAddress);
            case STAKING_ADDRESS_LABEL:
                return addressTableModel->labelForAddress(QString::fromStdString(rec.stakingAddress));
            case IS_WHITELISTED:
                return addressTableModel->purposeForAddress(rec.ownerAddress).compare(AddressBook::AddressBookPurpose::DELEGATOR) == 0;
            case IS_WHITELISTED_STRING:
                return (addressTableModel->purposeForAddress(rec.ownerAddress) == AddressBook::AddressBookPurpose::DELEGATOR ? "Staking" : "Not staking");
            case TOTAL_STACKEABLE_AMOUNT_STR:
                return GUIUtil::formatBalance(rec.cachedTotalAmount);
            case TOTAL_STACKEABLE_AMOUNT:
                return qint64(rec.cachedTotalAmount);
            case IS_RECEIVED_DELEGATION:
                return !rec.isSpendable;
        }
    }

    return QVariant();
}

bool ColdStakingModel::whitelist(const QModelIndex& modelIndex)
{
    QString address = modelIndex.data(Qt::DisplayRole).toString();
    if (addressTableModel->isWhitelisted(address.toStdString())) {
        return error("trying to whitelist already whitelisted address");
    }

    if (!model->whitelistAddressFromColdStaking(address)) return false;

    // address whitelisted - update cached amount and row data
    const int idx = modelIndex.row();
    cachedAmount += cachedDelegations[idx].cachedTotalAmount;
    removeRowAndEmitDataChanged(idx);

    return true;
}

bool ColdStakingModel::blacklist(const QModelIndex& modelIndex)
{
    QString address = modelIndex.data(Qt::DisplayRole).toString();
    if (!addressTableModel->isWhitelisted(address.toStdString())) {
        return error("trying to blacklist already blacklisted address");
    }

    if (!model->blacklistAddressFromColdStaking(address)) return false;

    // address blacklisted - update cached amount and row data
    const int idx = modelIndex.row();
    cachedAmount -= cachedDelegations[idx].cachedTotalAmount;
    removeRowAndEmitDataChanged(idx);

    return true;
}

void ColdStakingModel::removeRowAndEmitDataChanged(const int idx)
{
    beginRemoveRows(QModelIndex(), idx, idx);
    endRemoveRows();
    Q_EMIT dataChanged(index(idx, 0, QModelIndex()), index(idx, COLUMN_COUNT, QModelIndex()) );
}

