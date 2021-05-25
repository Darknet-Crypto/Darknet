// Copyright (c) 2021 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/mnselectiondialog.h"
#include "qt/pivx/forms/ui_mnselectiondialog.h"
#include "qt/pivx/mnmodel.h"
#include "qt/pivx/qtutils.h"

MnSelectionDialog::MnSelectionDialog(QWidget *parent) :
        QDialog(parent),
        ui(new Ui::MnSelectionDialog)
{
    ui->setupUi(this);
    this->setStyleSheet(parent->styleSheet());
    setCssProperty(ui->frame, "container-dialog");
    setCssProperty(ui->labelTitle, "text-title-dialog");
    setCssProperty(ui->labelMessage, "text-main-grey");
    setCssProperty(ui->btnEsc, "ic-chevron-left");
    setCssProperty(ui->btnCancel, "btn-dialog-cancel");
    setCssProperty(ui->btnSave, "btn-primary");
    setCssProperty(ui->containerAmountOfVotes, "container-border-purple");
    setCssProperty(ui->labelAmountOfVotesText, "text-purple");
    setCssProperty(ui->labelAmountOfVotes, "text-purple");
    setCssProperty(ui->btnSelectAll, "btn-dialog-secondary");

    ui->treeWidget->setColumnWidth(COLUMN_CHECKBOX, colCheckBoxWidth_treeMode);
    ui->treeWidget->setColumnWidth(COLUMN_NAME, 110);
    ui->treeWidget->setColumnWidth(COLUMN_STATUS, 60);
    ui->treeWidget->header()->setStretchLastSection(true);
    ui->treeWidget->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    ui->treeWidget->setRootIsDecorated(false);
    ui->treeWidget->setFocusPolicy(Qt::NoFocus);

    connect(ui->btnEsc, SIGNAL(clicked()), this, SLOT(close()));
    connect(ui->btnCancel, SIGNAL(clicked()), this, SLOT(close()));
    connect(ui->btnSave, SIGNAL(clicked()), this, SLOT(accept()));
    connect(ui->treeWidget, &QTreeWidget::itemChanged, this, &MnSelectionDialog::viewItemChanged);
}

void MnSelectionDialog::setModel(MNModel* _mnModel)
{
    mnModel = _mnModel;
    updateView();
}

class MnInfo {
public:
    explicit MnInfo(const QString& _alias,
                    const QString& _status) : alias(_alias), status(_status) {}
    ~MnInfo() {}

    QString alias;
    QString status;
};

void MnSelectionDialog::viewItemChanged(QTreeWidgetItem* item, int column)
{
    if (column == COLUMN_CHECKBOX) {
        MnInfo mnInfo(item->text(COLUMN_NAME), item->text(COLUMN_STATUS));
        if (mnInfo.alias.isEmpty()) return;
        auto it = std::find(selectedMnList.begin(), selectedMnList.end(), mnInfo.alias.toStdString());
        if (item->checkState(COLUMN_CHECKBOX) == Qt::Unchecked) {
            if (it != selectedMnList.end()) {
                selectedMnList.erase(it);
                ui->labelAmountOfVotes->setText(QString::number((int)selectedMnList.size()));
            }
        } else if (item->isDisabled()) {
            item->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);
        } else {
            if (it == selectedMnList.end()) {
                selectedMnList.emplace_back(mnInfo.alias.toStdString());
                ui->labelAmountOfVotes->setText(QString::number((int)selectedMnList.size()));
            }
        }
    }
}

void MnSelectionDialog::updateView()
{
    ui->treeWidget->clear();
    ui->treeWidget->setEnabled(false); // performance, otherwise updateLabels would be called for every checked checkbox
    QFlags<Qt::ItemFlag> flgCheckbox = Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable;
    QFlags<Qt::ItemFlag> flgTristate = Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsTristate;

    for (int i = 0; i < mnModel->rowCount(); ++i) {
        QString alias = mnModel->index(i, MNModel::ALIAS, QModelIndex()).data().toString();
        QString status = mnModel->index(i, MNModel::STATUS, QModelIndex()).data().toString();
        appendItem(flgCheckbox, flgTristate, alias, status);
    }

    // save COLUMN_CHECKBOX width for tree-mode
    colCheckBoxWidth_treeMode = std::max(110, ui->treeWidget->columnWidth(COLUMN_CHECKBOX));
    // minimize COLUMN_CHECKBOX width in list-mode (need to display only the check box)
    ui->treeWidget->setColumnWidth(COLUMN_CHECKBOX, 40);

    ui->treeWidget->setEnabled(true);
}

void MnSelectionDialog::appendItem(QFlags<Qt::ItemFlag> flgCheckbox,
                                   QFlags<Qt::ItemFlag> flgTristate,
                                   const QString& mnName,
                                   const QString& mnStatus)
{
    QTreeWidgetItem* itemOutput = new QTreeWidgetItem(ui->treeWidget);
    itemOutput->setFlags(flgCheckbox);
    itemOutput->setCheckState(COLUMN_CHECKBOX, Qt::Unchecked);
    itemOutput->setText(COLUMN_NAME, mnName);
    itemOutput->setToolTip(COLUMN_NAME, "Masternode name");
    itemOutput->setText(COLUMN_STATUS, mnStatus);
    itemOutput->setToolTip(COLUMN_STATUS, "Masternode status");

    if (mnStatus != "ENABLED") {
        itemOutput->setDisabled(true);
        // TODO: add disable icon.
        //itemOutput->setIcon(COLUMN_CHECKBOX, QIcon(":/icons/check_disabled"));
    }
}

std::vector<std::string> MnSelectionDialog::getSelectedMnAlias()
{
    return selectedMnList;
}

MnSelectionDialog::~MnSelectionDialog()
{
    delete ui;
}
