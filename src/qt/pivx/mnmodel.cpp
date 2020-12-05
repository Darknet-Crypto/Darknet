// Copyright (c) 2019-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/mnmodel.h"

#include "masternode-sync.h"
#include "masternodeman.h"
#include "net.h"        // for validateMasternodeIP
#include "sync.h"
#include "uint256.h"
#include "wallet/wallet.h"

MNModel::MNModel(QObject *parent) : QAbstractTableModel(parent)
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
        uint256 txHash(mne.getTxHash());
        CTxIn txIn(txHash, uint32_t(nIndex));
        CMasternode* pmn = mnodeman.Find(txIn.prevout);
        nodes.append(MasternodeWrapper(
                     QString::fromStdString(mne.getAlias()),
                     QString::fromStdString(mne.getIp()),
                     pmn,
                     pmn ? pmn->vin.prevout : txIn.prevout,
                     Optional<QString>(QString::fromStdString(mne.getPubKeyStr())))
                     );
        if (pwalletMain) {
            bool txAccepted = false;
            {
                LOCK2(cs_main, pwalletMain->cs_wallet);
                const CWalletTx *walletTx = pwalletMain->GetWalletTx(txHash);
                if (walletTx && walletTx->GetDepthInMainChain() >= MasternodeCollateralMinConf()) {
                    txAccepted = true;
                }
            }
            collateralTxAccepted.insert(mne.getTxHash(), txAccepted);
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

    int row = index.row();
    const MasternodeWrapper& mnWrapper = nodes.at(row);
    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
            case ALIAS:
                return mnWrapper.label;
            case ADDRESS:
                return mnWrapper.ipPort;
            case PUB_KEY:
                return mnWrapper.mnPubKey ? *mnWrapper.mnPubKey : "Not available";
            case COLLATERAL_ID:
                return mnWrapper.collateralId ? QString::fromStdString(mnWrapper.collateralId->hash.GetHex()) : "Not available";
            case COLLATERAL_OUT_INDEX:
                return mnWrapper.collateralId ? QString::number(mnWrapper.collateralId->n) : "Not available";
            case STATUS: {
                return mnWrapper.masternode ? QString::fromStdString(mnWrapper.masternode->Status()) : "MISSING";
            }
            case PRIV_KEY: {
                if (mnWrapper.collateralId) {
                    for (const CMasternodeConfig::CMasternodeEntry& mne : masternodeConfig.getEntries()) {
                        if (mne.getTxHash() == mnWrapper.collateralId->hash.GetHex()) {
                            return QString::fromStdString(mne.getPrivKeyStr());
                        }
                    }
                }
                return "Not available";
            }
            case WAS_COLLATERAL_ACCEPTED:{
                if (!mnWrapper.masternode) return false;
                std::string txHash = mnWrapper.collateralId->hash.GetHex();
                if (!collateralTxAccepted.value(txHash)) {
                    bool txAccepted = false;
                    {
                        LOCK2(cs_main, pwalletMain->cs_wallet);
                        const CWalletTx *walletTx = pwalletMain->GetWalletTx(mnWrapper.collateralId->hash);
                        txAccepted = walletTx && walletTx->GetDepthInMainChain() > 0;
                    }
                    return txAccepted;
                }
                return true;
            }
        }
    }
    return QVariant();
}

bool MNModel::removeMn(const QModelIndex& modelIndex)
{
    int idx = modelIndex.row();
    beginRemoveRows(QModelIndex(), idx, idx);
    nodes.removeAt(idx);
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

    COutPoint collateralId = COutPoint(uint256S(mne->getTxHash()), uint32_t(nIndex));
    CMasternode* pmn = mnodeman.Find(collateralId);
    nodes.append(MasternodeWrapper(
                 QString::fromStdString(mne->getAlias()),
                 QString::fromStdString(mne->getIp()),
                 pmn, pmn ? pmn->vin.prevout : collateralId,
                 Optional<QString>(QString::fromStdString(mne->getPubKeyStr())))
                 );
    endInsertRows();
    return true;
}

const MasternodeWrapper* MNModel::getMNWrapper(QString mnAlias)
{
    for (const auto& it : nodes) {
        if (it.label == mnAlias) {
            return &it;
        }
    }
    return nullptr;
}

int MNModel::getMNState(QString mnAlias)
{
    const MasternodeWrapper* mn = getMNWrapper(mnAlias);
    if (!mn) {
        throw std::runtime_error(std::string("Masternode alias not found"));
    }
    return mn->masternode ? mn->masternode->GetActiveState() : -1;
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
    const MasternodeWrapper* mn = getMNWrapper(mnAlias);
    if (!mn) {
        throw std::runtime_error(std::string("Masternode alias not found"));
    }
    return mn->masternode ? collateralTxAccepted.value(mn->masternode->vin.prevout.hash.GetHex()) : false;
}

bool MNModel::isMNsNetworkSynced()
{
    return masternodeSync.IsSynced();
}

bool MNModel::validateMNIP(const QString& addrStr)
{
    return validateMasternodeIP(addrStr.toStdString());
}
