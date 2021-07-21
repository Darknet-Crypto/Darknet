// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "addresstablemodel.h"
#include "addressbook.h"

#include "guiutil.h"
#include "walletmodel.h"

#include "key_io.h"
#include "wallet/wallet.h"
#include "askpassphrasedialog.h"

#include "sapling/key_io_sapling.h"

#include <algorithm>

#include <QDebug>
#include <QFont>

const QString AddressTableModel::Send = "S";
const QString AddressTableModel::Receive = "R";
const QString AddressTableModel::Zerocoin = "X";
const QString AddressTableModel::Delegator = "D";
const QString AddressTableModel::Delegable = "E";
const QString AddressTableModel::ColdStaking = "C";
const QString AddressTableModel::ColdStakingSend = "T";
const QString AddressTableModel::ShieldedReceive = "U";
const QString AddressTableModel::ShieldedSend = "V";

struct AddressTableEntry {
    enum Type {
        Sending,
        Receiving,
        Zerocoin,
        Delegator,
        Delegable,
        ColdStaking,
        ColdStakingSend,
        ShieldedReceive,
        ShieldedSend,
        Hidden /* QSortFilterProxyModel will filter these out */
    };

    Type type;
    QString label{};
    QString address{};
    QString pubcoin{};
    uint creationTime{0};

    AddressTableEntry() = delete;   // need to specify a type
    AddressTableEntry(Type _type, const QString& _pubcoin):    type(_type), pubcoin(_pubcoin) {}
    AddressTableEntry(Type _type, const QString& _label, const QString& _address, const uint _creationTime) :
        type(_type),
        label(_label),
        address(_address),
        creationTime(_creationTime)
    {}
};

struct AddressTableEntryLessThan {
    bool operator()(const AddressTableEntry& a, const AddressTableEntry& b) const
    {
        return a.address < b.address;
    }
    bool operator()(const AddressTableEntry& a, const QString& b) const
    {
        return a.address < b;
    }
    bool operator()(const QString& a, const AddressTableEntry& b) const
    {
        return a < b.address;
    }
};

/* Determine address type from address purpose */
static AddressTableEntry::Type translateTransactionType(const QString& strPurpose, bool isMine)
{
    AddressTableEntry::Type addressType = AddressTableEntry::Hidden;
    // "refund" addresses aren't shown, and change addresses aren't in mapAddressBook at all.
    if (strPurpose ==  QString::fromStdString(AddressBook::AddressBookPurpose::SEND))
        addressType = AddressTableEntry::Sending;
    else if (strPurpose ==  QString::fromStdString(AddressBook::AddressBookPurpose::RECEIVE))
        addressType = AddressTableEntry::Receiving;
    else if (strPurpose == QString::fromStdString(AddressBook::AddressBookPurpose::DELEGATOR))
        addressType = AddressTableEntry::Delegator;
    else if (strPurpose == QString::fromStdString(AddressBook::AddressBookPurpose::DELEGABLE))
        addressType = AddressTableEntry::Delegable;
    else if (strPurpose == QString::fromStdString(AddressBook::AddressBookPurpose::COLD_STAKING))
        addressType = AddressTableEntry::ColdStaking;
    else if (strPurpose == QString::fromStdString(AddressBook::AddressBookPurpose::COLD_STAKING_SEND))
        addressType = AddressTableEntry::ColdStakingSend;
    else if (strPurpose == QString::fromStdString(AddressBook::AddressBookPurpose::SHIELDED_RECEIVE))
        addressType = AddressTableEntry::ShieldedReceive;
    else if (strPurpose == QString::fromStdString(AddressBook::AddressBookPurpose::SHIELDED_SEND))
        addressType = AddressTableEntry::ShieldedSend;
    else if (strPurpose == "unknown" || strPurpose == "") // if purpose not set, guess
        addressType = (isMine ? AddressTableEntry::Receiving : AddressTableEntry::Sending);
    return addressType;
}

static QString translateTypeToString(AddressTableEntry::Type type)
{
    switch (type) {
        case AddressTableEntry::Sending:
            return QObject::tr("Contact");
        case AddressTableEntry::Receiving:
            return QObject::tr("Receiving");
        case AddressTableEntry::Delegator:
            return QObject::tr("Delegator");
        case AddressTableEntry::Delegable:
            return QObject::tr("Delegable");
        case AddressTableEntry::ColdStaking:
            return QObject::tr("Cold Staking");
        case AddressTableEntry::ColdStakingSend:
            return QObject::tr("Cold Staking Contact");
        case AddressTableEntry::ShieldedReceive:
            return QObject::tr("Receiving Shielded");
        case AddressTableEntry::ShieldedSend:
            return QObject::tr("Contact Shielded");
        case AddressTableEntry::Hidden:
            return QObject::tr("Hidden");
        default:
            return QObject::tr("Unknown");
    }
}

// Private implementation
class AddressTablePriv
{
public:
    CWallet* wallet{nullptr};
    QList<AddressTableEntry> cachedAddressTable;
    int sendNum = 0;
    int recvNum = 0;
    int dellNum = 0;
    int coldSendNum = 0;
    int shieldedSendNum = 0;
    AddressTableModel* parent;

    AddressTablePriv(CWallet* wallet, AddressTableModel* parent) : wallet(wallet), parent(parent) {}

    void refreshAddressTable()
    {
        cachedAddressTable.clear();
        {
            LOCK(wallet->cs_wallet);
            for (auto it = wallet->NewAddressBookIterator(); it.IsValid(); it.Next()) {
                auto addrBookData = it.GetValue();
                const CChainParams::Base58Type addrType =
                        AddressBook::IsColdStakingPurpose(addrBookData.purpose) ?
                        CChainParams::STAKING_ADDRESS : CChainParams::PUBKEY_ADDRESS;

                const CWDestination& dest = *it.GetDestKey();
                bool fMine = IsMine(*wallet, dest);
                QString addressStr = QString::fromStdString(Standard::EncodeDestination(dest, addrType));
                uint creationTime = 0;
                if (addrBookData.isReceivePurpose() || addrBookData.isShieldedReceivePurpose()) {
                    creationTime = static_cast<uint>(wallet->GetKeyCreationTime(dest));
                }

                AddressTableEntry::Type addressType = translateTransactionType(
                    QString::fromStdString(addrBookData.purpose), fMine);
                const std::string& strName = addrBookData.name;

                updatePurposeCachedCounted(addrBookData.purpose, true);
                cachedAddressTable.append(
                        AddressTableEntry(addressType,
                                          QString::fromStdString(strName),
                                          addressStr,
                                          creationTime
                        )
                );
            }
        }
        // std::lower_bound() and std::upper_bound() require our cachedAddressTable list to be sorted in asc order
        // Even though the map is already sorted this re-sorting step is needed because the originating map
        // is sorted by binary address, not by base58() address.
        std::sort(cachedAddressTable.begin(), cachedAddressTable.end(), AddressTableEntryLessThan());
    }

    // add shielded addresses num if needed..
    void updatePurposeCachedCounted(std::string purpose, bool add)
    {
        int *var = nullptr;
        if (purpose == AddressBook::AddressBookPurpose::RECEIVE) {
            var = &recvNum;
        } else if (purpose == AddressBook::AddressBookPurpose::SEND) {
            var = &sendNum;
        } else if (purpose == AddressBook::AddressBookPurpose::COLD_STAKING_SEND) {
            var = &coldSendNum;
        } else if (purpose == AddressBook::AddressBookPurpose::DELEGABLE || purpose == AddressBook::AddressBookPurpose::DELEGATOR) {
            var = &dellNum;
        } else if (purpose == AddressBook::AddressBookPurpose::SHIELDED_SEND) {
            var = &shieldedSendNum;
        } else {
            return;
        }
        if (var != nullptr) {
            if (add) (*var)++; else (*var)--;
        }
    }

    void updateEntry(const QString& address, const QString& label, bool isMine, const QString& purpose, int status)
    {
        // Find address / label in model
        QList<AddressTableEntry>::iterator lower = std::lower_bound(
            cachedAddressTable.begin(), cachedAddressTable.end(), address, AddressTableEntryLessThan());
        QList<AddressTableEntry>::iterator upper = std::upper_bound(
            cachedAddressTable.begin(), cachedAddressTable.end(), address, AddressTableEntryLessThan());
        int lowerIndex = (lower - cachedAddressTable.begin());
        int upperIndex = (upper - cachedAddressTable.begin());
        bool inModel = (lower != upper);
        AddressTableEntry::Type newEntryType = translateTransactionType(purpose, isMine);

        switch (status) {
        case CT_NEW: {
            if (inModel) {
                qWarning() << "AddressTablePriv::updateEntry : Warning: Got CT_NEW, but entry is already in model";
                break;
            }
            uint creationTime = 0;

            std::string stdPurpose = purpose.toStdString();
            if (stdPurpose == AddressBook::AddressBookPurpose::RECEIVE ||
                stdPurpose == AddressBook::AddressBookPurpose::SHIELDED_RECEIVE) {
                creationTime = static_cast<uint>(wallet->GetKeyCreationTime(Standard::DecodeDestination(address.toStdString())));
            }

            updatePurposeCachedCounted(stdPurpose, true);

            parent->beginInsertRows(QModelIndex(), lowerIndex, lowerIndex);
            cachedAddressTable.insert(lowerIndex, AddressTableEntry(newEntryType, label, address, creationTime));
            parent->endInsertRows();
            break;
        }
        case CT_UPDATED: {
            if (!inModel) {
                qWarning() << "AddressTablePriv::updateEntry : Warning: Got CT_UPDATED, but entry is not in model";
                break;
            }
            lower->type = newEntryType;
            lower->label = label;
            parent->emitDataChanged(lowerIndex);
            break;
        }
        case CT_DELETED: {
            if (!inModel) {
                qWarning() << "AddressTablePriv::updateEntry : Warning: Got CT_DELETED, but entry is not in model";
                break;
            }
            parent->beginRemoveRows(QModelIndex(), lowerIndex, upperIndex - 1);
            cachedAddressTable.erase(lower, upper);
            parent->endRemoveRows();
            updatePurposeCachedCounted(purpose.toStdString(), false);
            break;
            }
        }
    }

    void updateEntry(const QString &pubCoin, const QString &isUsed, int status)
    {
        // Find address / label in model
        QList<AddressTableEntry>::iterator lower = std::lower_bound(
            cachedAddressTable.begin(), cachedAddressTable.end(), pubCoin, AddressTableEntryLessThan());
        QList<AddressTableEntry>::iterator upper = std::upper_bound(
            cachedAddressTable.begin(), cachedAddressTable.end(), pubCoin, AddressTableEntryLessThan());
        int lowerIndex = (lower - cachedAddressTable.begin());
        bool inModel = (lower != upper);
        AddressTableEntry::Type newEntryType = AddressTableEntry::Zerocoin;

        switch(status)
        {
            case CT_NEW:
                if (inModel) {
                    qWarning() << "AddressTablePriv_ZC::updateEntry : Warning: Got CT_NEW, but entry is already in model";
                }
                parent->beginInsertRows(QModelIndex(), lowerIndex, lowerIndex);
                cachedAddressTable.insert(lowerIndex, AddressTableEntry(newEntryType, isUsed, pubCoin, 0));
                parent->endInsertRows();
                break;
            case CT_UPDATED:
                if (!inModel) {
                    qWarning() << "AddressTablePriv_ZC::updateEntry : Warning: Got CT_UPDATED, but entry is not in model";
                    break;
                }
                lower->type = newEntryType;
                lower->label = isUsed;
                parent->emitDataChanged(lowerIndex);
                break;
        }
    }

    int size() { return cachedAddressTable.size(); }
    int sizeSend() { return sendNum; }
    int sizeRecv() { return recvNum; }
    int sizeDell() { return dellNum; }
    int SizeColdSend() { return coldSendNum; }
    int sizeShieldedSend() { return shieldedSendNum; }

    AddressTableEntry* index(int idx)
    {
        if (idx >= 0 && idx < cachedAddressTable.size()) {
            return &cachedAddressTable[idx];
        } else {
            return 0;
        }
    }
};

AddressTableModel::AddressTableModel(CWallet* wallet, WalletModel* parent) : QAbstractTableModel(parent), walletModel(parent), wallet(wallet), priv(0)
{
    columns << tr("Label") << tr("Address") << tr("Date") << tr("Type");
    priv = new AddressTablePriv(wallet, this);
    priv->refreshAddressTable();
}

AddressTableModel::~AddressTableModel()
{
    delete priv;
}

int AddressTableModel::rowCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    return priv->size();
}

int AddressTableModel::columnCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    return columns.length();
}

int AddressTableModel::sizeSend() const { return priv->sizeSend(); }
int AddressTableModel::sizeRecv() const { return priv->sizeRecv(); }
int AddressTableModel::sizeDell() const { return priv->sizeDell(); }
int AddressTableModel::sizeColdSend() const { return priv->SizeColdSend(); }
int AddressTableModel::sizeShieldedSend() const { return priv->sizeShieldedSend(); }

QVariant AddressTableModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return QVariant();

    AddressTableEntry* rec = static_cast<AddressTableEntry*>(index.internalPointer());

    if (role == Qt::DisplayRole || role == Qt::EditRole) {
        switch (index.column()) {
        case Label:
            if (rec->label.isEmpty() && role == Qt::DisplayRole) {
                return tr("(no label)");
            } else {
                return rec->label;
            }
        case Address:
            return rec->address;
        case Date:
            return rec->creationTime;
        case Type:
            return translateTypeToString(rec->type);
        }
    } else if (role == Qt::FontRole) {
        QFont font;
        if (index.column() == Address) {
            font = GUIUtil::bitcoinAddressFont();
        }
        return font;
    } else if (role == TypeRole) {
        switch (rec->type) {
            case AddressTableEntry::Sending:
                return Send;
            case AddressTableEntry::Receiving:
                return Receive;
            case AddressTableEntry::Delegator:
                return Delegator;
            case AddressTableEntry::Delegable:
                return Delegable;
            case AddressTableEntry::ColdStaking:
                return ColdStaking;
            case AddressTableEntry::ColdStakingSend:
                return ColdStakingSend;
            case AddressTableEntry::ShieldedReceive:
                return ShieldedReceive;
            case AddressTableEntry::ShieldedSend:
                return ShieldedSend;
            default:
                break;
        }
    }
    return QVariant();
}

bool AddressTableModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
    if (!index.isValid())
        return false;
    AddressTableEntry* rec = static_cast<AddressTableEntry*>(index.internalPointer());
    std::string strPurpose = (rec->type == AddressTableEntry::Sending ?
                                AddressBook::AddressBookPurpose::SEND :
                                AddressBook::AddressBookPurpose::RECEIVE);
    editStatus = OK;

    if (role == Qt::EditRole) {
        LOCK(wallet->cs_wallet); /* For SetAddressBook / DelAddressBook */
        CTxDestination curAddress = DecodeDestination(rec->address.toStdString());
        if (index.column() == Label) {
            // Do nothing, if old label == new label
            if (rec->label == value.toString()) {
                editStatus = NO_CHANGES;
                return false;
            }
            wallet->SetAddressBook(curAddress, value.toString().toStdString(), strPurpose);
        } else if (index.column() == Address) {
            CTxDestination newAddress = DecodeDestination(value.toString().toStdString());
            // Refuse to set invalid address, set error status and return false
            if (!IsValidDestination(newAddress)) {
                editStatus = INVALID_ADDRESS;
                return false;
            }
            // Do nothing, if old address == new address
            else if (newAddress == curAddress) {
                editStatus = NO_CHANGES;
                return false;
            }
            // Check for duplicate addresses to prevent accidental deletion of addresses, if you try
            // to paste an existing address over another address (with a different label)
            else if (wallet->HasAddressBook(newAddress)) {
                editStatus = DUPLICATE_ADDRESS;
                return false;
            }
            // Double-check that we're not overwriting a receiving address
            else if (rec->type == AddressTableEntry::Sending) {
                // Remove old entry
                wallet->DelAddressBook(curAddress);
                // Add new entry with new address
                wallet->SetAddressBook(newAddress, rec->label.toStdString(), strPurpose);
            }
        }
        return true;
    }
    return false;
}

QVariant AddressTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal) {
        if (role == Qt::DisplayRole && section < columns.size()) {
            return columns[section];
        }
    }
    return QVariant();
}

Qt::ItemFlags AddressTableModel::flags(const QModelIndex& index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    AddressTableEntry* rec = static_cast<AddressTableEntry*>(index.internalPointer());

    Qt::ItemFlags retval = Qt::ItemIsSelectable | Qt::ItemIsEnabled;
    // Can edit address and label for sending addresses,
    // and only label for receiving addresses.
    if (rec->type == AddressTableEntry::Sending ||
        (rec->type == AddressTableEntry::Receiving && index.column() == Label)) {
        retval |= Qt::ItemIsEditable;
    }
    return retval;
}

QModelIndex AddressTableModel::index(int row, int column, const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    AddressTableEntry* data = priv->index(row);
    if (data) {
        return createIndex(row, column, priv->index(row));
    } else {
        return QModelIndex();
    }
}

void AddressTableModel::updateEntry(const QString& address,
    const QString& label,
    bool isMine,
    const QString& purpose,
    int status)
{
    // Update address book model from Pivx core
    priv->updateEntry(address, label, isMine, purpose, status);
}


void AddressTableModel::updateEntry(const QString &pubCoin, const QString &isUsed, int status)
{
    // Update stealth address book model from Bitcoin core
    priv->updateEntry(pubCoin, isUsed, status);
}



QString AddressTableModel::addRow(const QString& type, const QString& label, const QString& address)
{
    std::string strLabel = label.toStdString();
    std::string strAddress = address.toStdString();

    editStatus = OK;

    if (type == Send) {
        if (!walletModel->validateAddress(address)) {
            editStatus = INVALID_ADDRESS;
            return QString();
        }
        // Check for duplicate addresses
        {
            LOCK(wallet->cs_wallet);
            if (wallet->HasAddressBook(DecodeDestination(strAddress))) {
                editStatus = DUPLICATE_ADDRESS;
                return QString();
            }
        }
    } else if (type == Receive) {
        // Generate a new address to associate with given label
        CPubKey newKey;
        if (!wallet->GetKeyFromPool(newKey)) {
            WalletModel::UnlockContext ctx(walletModel->requestUnlock());
            if (!ctx.isValid()) {
                // Unlock wallet failed or was cancelled
                editStatus = WALLET_UNLOCK_FAILURE;
                return QString();
            }
            if (!wallet->GetKeyFromPool(newKey)) {
                editStatus = KEY_GENERATION_FAILURE;
                return QString();
            }
        }
        strAddress = EncodeDestination(newKey.GetID());
    } else {
        return QString();
    }

    // Add entry
    {
        LOCK(wallet->cs_wallet);
        wallet->SetAddressBook(DecodeDestination(strAddress), strLabel,
            (type == Send ? AddressBook::AddressBookPurpose::SEND : AddressBook::AddressBookPurpose::RECEIVE));
    }
    return QString::fromStdString(strAddress);
}

bool AddressTableModel::removeRows(int row, int count, const QModelIndex& parent)
{
    Q_UNUSED(parent);
    AddressTableEntry* rec = priv->index(row);
    if (count != 1 || !rec || rec->type == AddressTableEntry::Receiving || rec->type == AddressTableEntry::ColdStaking) {
        // Can only remove one row at a time, and cannot remove rows not in model.
        // Also refuse to remove receiving addresses.
        return false;
    }
    const CChainParams::Base58Type addrType = (rec->type == AddressTableEntry::ColdStakingSend) ? CChainParams::STAKING_ADDRESS : CChainParams::PUBKEY_ADDRESS;
    {
        LOCK(wallet->cs_wallet);
        return wallet->DelAddressBook(Standard::DecodeDestination(rec->address.toStdString()), addrType);
    }
}

/* Look up label for address in address book, if not found return empty string.
 */
QString AddressTableModel::labelForAddress(const QString& address) const
{
    // TODO: Check why do we have empty addresses..
    if (!address.isEmpty()) {
        CWDestination dest = Standard::DecodeDestination(address.toStdString());
        return QString::fromStdString(wallet->GetNameForAddressBookEntry(dest));
    }
    return QString();
}

/* Look up purpose for address in address book
 */
std::string AddressTableModel::purposeForAddress(const std::string& address) const
{
    return wallet->GetPurposeForAddressBookEntry(Standard::DecodeDestination(address));
}

int AddressTableModel::lookupAddress(const QString& address) const
{
    QModelIndexList lst = match(index(0, Address, QModelIndex()),
        Qt::EditRole, address, 1, Qt::MatchExactly);
    if (lst.isEmpty()) {
        return -1;
    } else {
        return lst.at(0).row();
    }
}

bool AddressTableModel::isWhitelisted(const std::string& address) const
{
    return purposeForAddress(address).compare(AddressBook::AddressBookPurpose::DELEGATOR) == 0;
}

/**
 * Return an unused address
 * @return
 */
QString AddressTableModel::getAddressToShow(bool isShielded) const
{
    LOCK(wallet->cs_wallet);

    for (auto it = wallet->NewAddressBookIterator(); it.IsValid(); it.Next()) {
        const auto addrData = it.GetValue();

        if (!isShielded) {
            if (addrData.purpose == AddressBook::AddressBookPurpose::RECEIVE) {
                const auto &address = *it.GetCTxDestKey();
                if (IsValidDestination(address) && IsMine(*wallet, address) && !wallet->IsUsed(address)) {
                    return QString::fromStdString(EncodeDestination(address));
                }
            }
        } else {
            // todo: add shielded address support to IsUsed
            if (addrData.purpose == AddressBook::AddressBookPurpose::SHIELDED_RECEIVE) {
                const auto &address = *it.GetShieldedDestKey();
                if (IsValidPaymentAddress(address) && IsMine(*wallet, address)) {
                    return QString::fromStdString(KeyIO::EncodePaymentAddress(address));
                }
            }
        }
    }

    // For some reason we don't have any address in our address book, let's create one
    CallResult<Destination> res(false, "");
    QString addressStr;
    if (!isShielded) {
        res = walletModel->getNewAddress("Default");
        if (res) { addressStr = QString::fromStdString(res.getObjResult()->ToString()); }
    } else {
        res = walletModel->getNewShieldedAddress(addressStr, "default shielded");
    }
    return addressStr;
}

void AddressTableModel::emitDataChanged(int idx)
{
    Q_EMIT dataChanged(index(idx, 0, QModelIndex()), index(idx, columns.length() - 1, QModelIndex()));
}

void AddressTableModel::notifyChange(const QModelIndex &_index)
{
    int idx = _index.row();
    emitDataChanged(idx);
}
