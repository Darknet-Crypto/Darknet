// Copyright (c) 2019-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/settings/settingsexportcsv.h"
#include "qt/pivx/settings/forms/ui_settingsexportcsv.h"
#include <QFile>
#include "csvmodelwriter.h"
#include "guiutil.h"
#include "optionsmodel.h"
#include "qt/pivx/qtutils.h"
#include "guiinterface.h"

SettingsExportCSV::SettingsExportCSV(PIVXGUI* _window, QWidget *parent) :
    PWidget(_window, parent),
    ui(new Ui::SettingsExportCSV)
{
    ui->setupUi(this);

    this->setStyleSheet(parent->styleSheet());

    /* Containers */
    ui->left->setProperty("cssClass", "container");
    ui->left->setContentsMargins(10,10,10,10);

    // Title
    ui->labelTitle->setProperty("cssClass", "text-title-screen");
    ui->labelSubtitle1->setProperty("cssClass", "text-subtitle");

    // Location
    ui->labelSubtitleLocation->setProperty("cssClass", "text-title");
    ui->labelSubtitleLocationAddress->setProperty("cssClass", "text-title");

    setCssProperty({ui->pushButtonDocuments, ui->pushButtonAddressDocuments}, "btn-edit-primary-folder");
    setShadow(ui->pushButtonDocuments);

    setShadow(ui->pushButtonAddressDocuments);
    ui->labelDivider->setProperty("cssClass", "container-divider");

    SortEdit* lineEdit = new SortEdit(ui->comboBoxSort);
    connect(lineEdit, &SortEdit::Mouse_Pressed, [this](){ui->comboBoxSort->showPopup();});
    setSortTx(ui->comboBoxSort, lineEdit);

    SortEdit* lineEditType = new SortEdit(ui->comboBoxSortType);
    connect(lineEditType, &SortEdit::Mouse_Pressed, [this](){ui->comboBoxSortType->showPopup();});
    setSortTxTypeFilter(ui->comboBoxSortType, lineEditType);
    ui->comboBoxSortType->setCurrentIndex(0);

    SortEdit* lineEditAddressBook = new SortEdit(ui->comboBoxSortAddressType);
    connect(lineEditAddressBook, &SortEdit::Mouse_Pressed, [this](){ui->comboBoxSortAddressType->showPopup();});
    setFilterAddressBook(ui->comboBoxSortAddressType, lineEditAddressBook);
    ui->comboBoxSortAddressType->setCurrentIndex(0);

    connect(ui->pushButtonDocuments, &QPushButton::clicked, [this](){selectFileOutput(true);});
    connect(ui->pushButtonAddressDocuments, &QPushButton::clicked, [this](){selectFileOutput(false);});
}

void SettingsExportCSV::selectFileOutput(const bool isTxExport)
{
    QString filename = GUIUtil::getSaveFileName(this,
                                        isTxExport ? tr("Export CSV") : tr("Export Address List"), QString(),
                                        isTxExport ? tr("PIVX_tx_csv_export(*.csv)") : tr("PIVX_addresses_csv_export(*.csv)"),
                                        nullptr);

    if (isTxExport) {
        if (!filename.isEmpty()) {
            ui->pushButtonDocuments->setText(filename);
            exportTxes(filename);
        } else {
            ui->pushButtonDocuments->setText(tr("Select folder..."));
        }
    } else {
        if (!filename.isEmpty()) {
            ui->pushButtonAddressDocuments->setText(filename);
            exportAddrs(filename);
        } else {
            ui->pushButtonAddressDocuments->setText(tr("Select folder..."));
        }
    }
}

void SettingsExportCSV::exportTxes(const QString& filename)
{
    if (filename.isNull()) {
        inform(tr("Please select a folder to export the csv file first."));
        return;
    }

    CSVModelWriter writer(filename);
    bool fExport = false;

    if (walletModel) {

        if (!txFilter) {
            // Set up transaction list
            txFilter = new TransactionFilterProxy();
            txFilter->setDynamicSortFilter(true);
            txFilter->setSortCaseSensitivity(Qt::CaseInsensitive);
            txFilter->setFilterCaseSensitivity(Qt::CaseInsensitive);
            txFilter->setSortRole(Qt::EditRole);
            txFilter->setSourceModel(walletModel->getTransactionTableModel());
        }

        // First type filter
        txFilter->setTypeFilter(ui->comboBoxSortType->itemData(ui->comboBoxSortType->currentIndex()).toInt());

        // Second tx filter.
        int columnIndex = TransactionTableModel::Date;
        Qt::SortOrder order = Qt::DescendingOrder;
        switch (ui->comboBoxSort->itemData(ui->comboBoxSort->currentIndex()).toInt()) {
            case SortTx::DATE_ASC:{
                columnIndex = TransactionTableModel::Date;
                order = Qt::AscendingOrder;
                break;
            }
            case SortTx::DATE_DESC:{
                columnIndex = TransactionTableModel::Date;
                break;
            }
            case SortTx::AMOUNT_ASC:{
                columnIndex = TransactionTableModel::Amount;
                order = Qt::AscendingOrder;
                break;
            }
            case SortTx::AMOUNT_DESC:{
                columnIndex = TransactionTableModel::Amount;
                break;
            }

        }
        txFilter->sort(columnIndex, order);

        // name, column, role
        writer.setModel(txFilter);
        writer.addColumn(tr("Confirmed"), 0, TransactionTableModel::ConfirmedRole);
        if (walletModel->haveWatchOnly())
            writer.addColumn(tr("Watch-only"), TransactionTableModel::Watchonly);
        writer.addColumn(tr("Date"), 0, TransactionTableModel::DateRole);
        writer.addColumn(tr("Type"), TransactionTableModel::Type, Qt::EditRole);
        writer.addColumn(tr("Label"), 0, TransactionTableModel::LabelRole);
        writer.addColumn(tr("Address"), 0, TransactionTableModel::AddressRole);
        writer.addColumn(BitcoinUnits::getAmountColumnTitle(walletModel->getOptionsModel()->getDisplayUnit()), 0, TransactionTableModel::FormattedAmountRole);
        writer.addColumn(tr("ID"), 0, TransactionTableModel::TxHashRole);
        fExport = writer.write();
    }

    if (fExport) {
        inform(tr("Exporting Successful\nThe transaction history was successfully saved to %1.").arg(filename));
    } else {
        inform(tr("Exporting Failed\nThere was an error trying to save the transaction history to %1.").arg(filename));
    }
}

void SettingsExportCSV::exportAddrs(const QString& filename)
{
    if (filename.isNull()) {
        inform(tr("Please select a folder to export the csv file first."));
        return;
    }

    bool fExport = false;
    if (walletModel) {

        if (!addressFilter) {
            addressFilter = new QSortFilterProxyModel(this);
            addressFilter->setSourceModel(walletModel->getAddressTableModel());
            addressFilter->setDynamicSortFilter(true);
            addressFilter->setFilterRole(AddressTableModel::TypeRole);
        }

        // Filter by type
        QString filterBy = ui->comboBoxSortAddressType->itemData(ui->comboBoxSortAddressType->currentIndex()).toString();
        addressFilter->setFilterFixedString(filterBy);

        if (addressFilter->rowCount() == 0) {
            inform(tr("No available addresses to export under the selected filter"));
            return;
        }

        CSVModelWriter writer(filename);
        // name, column, role
        writer.setModel(addressFilter);
        writer.addColumn("Label", AddressTableModel::Label, Qt::EditRole);
        writer.addColumn("Address", AddressTableModel::Address, Qt::EditRole);
        writer.addColumn("Type", AddressTableModel::Type, Qt::DisplayRole);
        fExport = writer.write();
    }

    if (fExport) {
        inform(tr("Exporting Successful\nThe address book was successfully saved to %1.").arg(filename));
    } else {
        inform(tr("Exporting Failed\nThere was an error trying to save the address list to %1. Please try again.").arg(filename));
    }
}

SettingsExportCSV::~SettingsExportCSV()
{
    delete ui;
}
