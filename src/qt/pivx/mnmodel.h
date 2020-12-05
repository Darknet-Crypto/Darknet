// Copyright (c) 2019-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef MNMODEL_H
#define MNMODEL_H

#include <QAbstractTableModel>
#include "masternode.h"
#include "masternodeconfig.h"

class MasternodeWrapper;

class MNModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit MNModel(QObject *parent = nullptr);
    ~MNModel() override {
        nodes.clear();
        collateralTxAccepted.clear();
    }

    enum ColumnIndex {
        ALIAS = 0,  /**< User specified MN alias */
        ADDRESS = 1, /**< Node address */
        PROTO_VERSION = 2, /**< Node protocol version */
        STATUS = 3, /**< Node status */
        ACTIVE_TIMESTAMP = 4, /**<  */
        PUB_KEY = 5,
        COLLATERAL_ID = 6,
        COLLATERAL_OUT_INDEX = 7,
        PRIV_KEY = 8,
        WAS_COLLATERAL_ACCEPTED = 9
    };

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool removeMn(const QModelIndex& index);
    bool addMn(CMasternodeConfig::CMasternodeEntry* entry);
    void updateMNList();


    bool isMNsNetworkSynced();
    // Returns the MN activeState field.
    int getMNState(QString alias);
    // Checks if the masternode is inactive
    bool isMNInactive(QString mnAlias);
    // Masternode is active if it's in PRE_ENABLED OR ENABLED state
    bool isMNActive(QString mnAlias);
    // Masternode collateral has enough confirmations
    bool isMNCollateralMature(QString mnAlias);
    // Validate string representing a masternode IP address
    static bool validateMNIP(const QString& addrStr);


private:
    // alias mn node ---> <ip, master node>
    QList<MasternodeWrapper> nodes;
    QMap<std::string, bool> collateralTxAccepted;

    const MasternodeWrapper* getMNWrapper(QString mnAlias);
};

class MasternodeWrapper
{
public:
    explicit MasternodeWrapper(
            QString _label,
            QString _ipPortStr,
            CMasternode* _masternode,
            COutPoint& _collateralId,
            Optional<QString> _mnPubKey) :
            label(_label), ipPort(_ipPortStr), masternode(_masternode), collateralId(_collateralId), mnPubKey(_mnPubKey) { };

    QString label;
    QString ipPort;
    CMasternode* masternode{nullptr};
    // Cache collateral id to be used if 'masternode' is null.
    // Denoting masternodes that were never initialized (appear on the conf file only)
    // or expired ones that were removed from the network list.
    // when masternode isn't null, the collateralId is directly pointing to masternode.vin.prevout.
    Optional<COutPoint> collateralId{nullopt};
    Optional<QString> mnPubKey{nullopt};
};

#endif // MNMODEL_H
