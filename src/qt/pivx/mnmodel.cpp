// Copyright (c) 2019-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/mnmodel.h"

#include "activemasternode.h"
#include "masternode-sync.h"
#include "masternodeman.h"
#include "net.h"        // for validateMasternodeIP
#include "uint256.h"
#include "wallet/wallet.h"

MNModel::MNModel(QObject *parent) : QAbstractTableModel(parent) {}

void MNModel::init()
{
    updateMNList();
}

void MNModel::updateMNList()
{
    int end = nodes.size();
    nodes.clear();
    collateralTxAccepted.clear();
    for (const CMasternodeConfig::CMasternodeEntry& mne : masternodeConfig.getEntries()) {
        int nIndex;
        if (!mne.castOutputIndex(nIndex))
            continue;
        const uint256& txHash = uint256S(mne.getTxHash());
        CTxIn txIn(txHash, uint32_t(nIndex));
        CMasternode* pmn = mnodeman.Find(txIn.prevout);
        if (!pmn) {
            pmn = new CMasternode();
            pmn->vin = txIn;
        }
        nodes.insert(QString::fromStdString(mne.getAlias()), std::make_pair(QString::fromStdString(mne.getIp()), pmn));
        if (walletModel) {
            collateralTxAccepted.insert(mne.getTxHash(), walletModel->getWalletTxDepth(txHash) >= MasternodeCollateralMinConf());
        }
    }
    Q_EMIT dataChanged(index(0, 0, QModelIndex()), index(end, 5, QModelIndex()) );
}

int MNModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return nodes.size();
}

int MNModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return 6;
}


QVariant MNModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
            return QVariant();

    // rec could be null, always verify it.
    CMasternode* rec = static_cast<CMasternode*>(index.internalPointer());
    bool isAvailable = rec;
    int row = index.row();
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
            case ALIAS:
                return nodes.uniqueKeys().value(row);
            case ADDRESS:
                return nodes.values().value(row).first;
            case PUB_KEY:
                return (isAvailable) ? QString::fromStdString(nodes.values().value(row).second->pubKeyMasternode.GetHash().GetHex()) : "Not available";
            case COLLATERAL_ID:
                return (isAvailable) ? QString::fromStdString(rec->vin.prevout.hash.GetHex()) : "Not available";
            case COLLATERAL_OUT_INDEX:
                return (isAvailable) ? QString::number(rec->vin.prevout.n) : "Not available";
            case STATUS: {
                std::pair<QString, CMasternode*> pair = nodes.values().value(row);
                std::string status = "MISSING";
                if (pair.second) {
                    status = pair.second->Status();
                    // Quick workaround to the current Masternode status types.
                    // If the status is REMOVE and there is no pubkey associated to the Masternode
                    // means that the MN is not in the network list and was created in
                    // updateMNList(). Which.. denotes a not started masternode.
                    // This will change in the future with the MasternodeWrapper introduction.
                    if (status == "REMOVE" && !pair.second->pubKeyCollateralAddress.IsValid()) {
                        return "MISSING";
                    }
                }
                return QString::fromStdString(status);
            }
            case PRIV_KEY: {
                if (isAvailable) {
                    for (CMasternodeConfig::CMasternodeEntry mne : masternodeConfig.getEntries()) {
                        if (mne.getTxHash().compare(rec->vin.prevout.hash.GetHex()) == 0) {
                            return QString::fromStdString(mne.getPrivKey());
                        }
                    }
                }
                return "Not available";
            }
            case WAS_COLLATERAL_ACCEPTED:{
                if (!isAvailable) return false;
                std::string txHash = rec->vin.prevout.hash.GetHex();
                if (!collateralTxAccepted.value(txHash) && walletModel) {
                    return walletModel->getWalletTxDepth(rec->vin.prevout.hash) > 0;
                }
                return true;
            }
        }
    }
    return QVariant();
}

QModelIndex MNModel::index(int row, int column, const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    std::pair<QString, CMasternode*> pair = nodes.values().value(row);
    CMasternode* data = pair.second;
    if (data) {
        return createIndex(row, column, data);
    } else if (!pair.first.isEmpty()) {
        return createIndex(row, column, nullptr);
    } else {
        return QModelIndex();
    }
}


bool MNModel::removeMn(const QModelIndex& modelIndex)
{
    QString alias = modelIndex.data(Qt::DisplayRole).toString();
    int idx = modelIndex.row();
    beginRemoveRows(QModelIndex(), idx, idx);
    nodes.take(alias);
    endRemoveRows();
    Q_EMIT dataChanged(index(idx, 0, QModelIndex()), index(idx, 5, QModelIndex()) );
    return true;
}

bool MNModel::addMn(CMasternodeConfig::CMasternodeEntry* mne)
{
    beginInsertRows(QModelIndex(), nodes.size(), nodes.size());
    int nIndex;
    if (!mne->castOutputIndex(nIndex))
        return false;

    CMasternode* pmn = mnodeman.Find(COutPoint(uint256S(mne->getTxHash()), uint32_t(nIndex)));
    nodes.insert(QString::fromStdString(mne->getAlias()), std::make_pair(QString::fromStdString(mne->getIp()), pmn));
    endInsertRows();
    return true;
}

int MNModel::getMNState(QString mnAlias)
{
    QMap<QString, std::pair<QString, CMasternode*>>::const_iterator it = nodes.find(mnAlias);
    if (it != nodes.end()) return it.value().second->GetActiveState();
    throw std::runtime_error(std::string("Masternode alias not found"));
}

bool MNModel::isMNInactive(QString mnAlias)
{
    int activeState = getMNState(mnAlias);
    return activeState == CMasternode::MASTERNODE_EXPIRED || activeState == CMasternode::MASTERNODE_REMOVE;
}

bool MNModel::isMNActive(QString mnAlias)
{
    int activeState = getMNState(mnAlias);
    return activeState == CMasternode::MASTERNODE_PRE_ENABLED || activeState == CMasternode::MASTERNODE_ENABLED;
}

bool MNModel::isMNCollateralMature(QString mnAlias)
{
    QMap<QString, std::pair<QString, CMasternode*>>::const_iterator it = nodes.find(mnAlias);
    if (it != nodes.end()) return collateralTxAccepted.value(it.value().second->vin.prevout.hash.GetHex());
    throw std::runtime_error(std::string("Masternode alias not found"));
}

bool MNModel::isMNsNetworkSynced()
{
    return masternodeSync.IsSynced();
}

bool MNModel::validateMNIP(const QString& addrStr)
{
    return validateMasternodeIP(addrStr.toStdString());
}
