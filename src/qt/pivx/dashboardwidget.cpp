// Copyright (c) 2019-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/dashboardwidget.h"
#include "qt/pivx/forms/ui_dashboardwidget.h"
#include "qt/pivx/sendconfirmdialog.h"
#include "qt/pivx/txrow.h"
#include "qt/pivx/qtutils.h"
#include "guiutil.h"
#include "walletmodel.h"
#include "clientmodel.h"
#include "optionsmodel.h"
#include "utiltime.h"
#include <QPainter>
#include <QModelIndex>
#include <QList>
#include <QGraphicsLayout>

#define DECORATION_SIZE 65
#define NUM_ITEMS 3
#define SHOW_EMPTY_CHART_VIEW_THRESHOLD 4000
#define REQUEST_LOAD_TASK 1
#define CHART_LOAD_MIN_TIME_INTERVAL 15

DashboardWidget::DashboardWidget(PIVXGUI* parent) :
    PWidget(parent),
    ui(new Ui::DashboardWidget)
{
    ui->setupUi(this);

    txHolder = new TxViewHolder(isLightTheme());
    txViewDelegate = new FurAbstractListItemDelegate(
        DECORATION_SIZE,
        txHolder,
        this
    );

    this->setStyleSheet(parent->styleSheet());
    this->setContentsMargins(0,0,0,0);

    // Containers
    setCssProperty({this, ui->left}, "container");
    ui->left->setContentsMargins(0,0,0,0);

    // Title
    setCssTitleScreen(ui->labelTitle);
    setCssTitleScreen(ui->labelTitle2);

    /* Subtitle */
    setCssSubtitleScreen(ui->labelSubtitle);

    // Staking Information
    setCssSubtitleScreen(ui->labelMessage);
    setCssProperty(ui->labelSquarePiv, "square-chart-piv");
    setCssProperty(ui->labelSquarezPiv, "square-chart-zpiv");
    setCssProperty(ui->labelPiv, "text-chart-piv");
    setCssProperty(ui->labelZpiv, "text-chart-zpiv");

    // Staking Amount
    QFont fontBold;
    fontBold.setWeight(QFont::Bold);

    setCssProperty(ui->labelChart, "legend-chart");
    setCssProperty(ui->labelAmountPiv, "text-stake-piv-disable");
    setCssProperty(ui->labelAmountZpiv, "text-stake-zpiv-disable");

    setCssProperty({ui->pushButtonAll,  ui->pushButtonMonth, ui->pushButtonYear}, "btn-check-time");
    setCssProperty({ui->comboBoxMonths,  ui->comboBoxYears}, "btn-combo-chart-selected");

    ui->comboBoxMonths->setView(new QListView());
    ui->comboBoxMonths->setStyleSheet("selection-background-color:transparent;");
    ui->comboBoxYears->setView(new QListView());
    ui->comboBoxYears->setStyleSheet("selection-background-color:transparent;");
    ui->pushButtonYear->setChecked(true);

    setCssProperty(ui->pushButtonChartArrow, "btn-chart-arrow");
    setCssProperty(ui->pushButtonChartRight, "btn-chart-arrow-right");

#ifdef USE_QTCHARTS
    setCssProperty(ui->right, "container-right");
    ui->right->setContentsMargins(20,20,20,0);
    connect(ui->comboBoxYears, static_cast<void (QComboBox::*)(const QString&)>(&QComboBox::currentTextChanged),
        this, &DashboardWidget::onChartYearChanged);
#else
    // hide charts container if not USE_QTCHARTS
    ui->right->setVisible(false);
#endif // USE_QTCHARTS

    // Sort Transactions
    SortEdit* lineEdit = new SortEdit(ui->comboBoxSort);
    connect(lineEdit, &SortEdit::Mouse_Pressed, [this](){ui->comboBoxSort->showPopup();});
    setSortTx(ui->comboBoxSort, lineEdit);
    connect(ui->comboBoxSort, static_cast<void (QComboBox::*)(const QString&)>(&QComboBox::currentTextChanged), this, &DashboardWidget::onSortChanged);

    // Sort type
    SortEdit* lineEditType = new SortEdit(ui->comboBoxSortType);
    connect(lineEditType, &SortEdit::Mouse_Pressed, [this](){ui->comboBoxSortType->showPopup();});
    setSortTxTypeFilter(ui->comboBoxSortType, lineEditType);
    ui->comboBoxSortType->setCurrentIndex(0);
    connect(ui->comboBoxSortType, static_cast<void (QComboBox::*)(const QString&)>(&QComboBox::currentTextChanged),
        this, &DashboardWidget::onSortTypeChanged);

    // Transactions
    setCssProperty(ui->listTransactions, "container");
    ui->listTransactions->setItemDelegate(txViewDelegate);
    ui->listTransactions->setIconSize(QSize(DECORATION_SIZE, DECORATION_SIZE));
    ui->listTransactions->setMinimumHeight(NUM_ITEMS * (DECORATION_SIZE + 2));
    ui->listTransactions->setAttribute(Qt::WA_MacShowFocusRect, false);
    ui->listTransactions->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->listTransactions->setUniformItemSizes(true);

    // Sync Warning
    ui->layoutWarning->setVisible(true);
    ui->lblWarning->setText(tr("Please wait until the wallet is fully synced to see your correct balance"));
    setCssProperty(ui->lblWarning, "text-warning");
    setCssProperty(ui->imgWarning, "ic-warning");

    //Empty List
    ui->emptyContainer->setVisible(false);
    setCssProperty(ui->pushImgEmpty, "img-empty-transactions");
    setCssProperty(ui->labelEmpty, "text-empty");
    setCssProperty(ui->chartContainer, "container-chart");
    setCssProperty(ui->pushImgEmptyChart, "img-empty-staking-on");

    setCssBtnSecondary(ui->btnHowTo);

    setCssProperty(ui->labelEmptyChart, "text-empty");
    setCssSubtitleScreen(ui->labelMessageEmpty);

    // Chart State
    ui->layoutChart->setVisible(false);
    ui->emptyContainerChart->setVisible(true);
    setShadow(ui->layoutShadow);

    connect(ui->listTransactions, &QListView::clicked, this, &DashboardWidget::handleTransactionClicked);

bool hasCharts = false;
#ifdef USE_QTCHARTS
    hasCharts = true;
    isLoading = false;
    setChartShow(YEAR);
    connect(ui->pushButtonYear, &QPushButton::clicked, [this](){setChartShow(YEAR);});
    connect(ui->pushButtonMonth, &QPushButton::clicked, [this](){setChartShow(MONTH);});
    connect(ui->pushButtonAll, &QPushButton::clicked, [this](){setChartShow(ALL);});
    if (window)
        connect(window, &PIVXGUI::windowResizeEvent, this, &DashboardWidget::windowResizeEvent);
#endif

    if (hasCharts) {
        ui->labelEmptyChart->setText(tr("You have no staking rewards"));
    } else {
        ui->labelEmptyChart->setText(tr("No charts library"));
    }
}

void DashboardWidget::handleTransactionClicked(const QModelIndex &index)
{
    ui->listTransactions->setCurrentIndex(index);
    QModelIndex rIndex = filter->mapToSource(index);

    window->showHide(true);
    TxDetailDialog *dialog = new TxDetailDialog(window, false);
    dialog->setData(walletModel, rIndex);
    openDialogWithOpaqueBackgroundY(dialog, window, 3, 17);

    // Back to regular status
    ui->listTransactions->scrollTo(index);
    ui->listTransactions->clearSelection();
    ui->listTransactions->setFocus();
    dialog->deleteLater();
}

void DashboardWidget::loadWalletModel()
{
    if (walletModel && walletModel->getOptionsModel()) {
        txModel = walletModel->getTransactionTableModel();
        // Set up transaction list
        filter = new TransactionFilterProxy();
        filter->setDynamicSortFilter(true);
        filter->setSortCaseSensitivity(Qt::CaseInsensitive);
        filter->setFilterCaseSensitivity(Qt::CaseInsensitive);
        filter->setSortRole(Qt::EditRole);

        // Read filter settings
        QSettings settings;
        quint32 filterByType = settings.value("transactionType", TransactionFilterProxy::ALL_TYPES).toInt();
        int filterIndex = ui->comboBoxSortType->findData(filterByType); // Find index
        filterByType = (filterIndex == -1) ? TransactionFilterProxy::ALL_TYPES : filterByType;
        filter->setTypeFilter(filterByType); // Set filter
        ui->comboBoxSortType->setCurrentIndex(filterIndex); // Set item in ComboBox
        // Read sort settings
        changeSort(settings.value("transactionSort", SortTx::DATE_DESC).toInt());

        filter->setSourceModel(txModel);
        txHolder->setFilter(filter);
        ui->listTransactions->setModel(filter);
        ui->listTransactions->setModelColumn(TransactionTableModel::ToAddress);

        if (txModel->size() == 0) {
            ui->emptyContainer->setVisible(true);
            ui->listTransactions->setVisible(false);
            ui->comboBoxSortType->setVisible(false);
            ui->comboBoxSort->setVisible(false);
        }

        connect(ui->pushImgEmpty, &QPushButton::clicked, [this](){window->openFAQ();});
        connect(ui->btnHowTo, &QPushButton::clicked, [this](){window->openFAQ();});
        connect(txModel, &TransactionTableModel::txArrived, this, &DashboardWidget::onTxArrived);

        // Notification pop-up for new transaction
        connect(txModel, &TransactionTableModel::rowsInserted, this, &DashboardWidget::processNewTransaction);
#ifdef USE_QTCHARTS
        onHideChartsChanged(walletModel->getOptionsModel()->isHideCharts());
        connect(walletModel->getOptionsModel(), &OptionsModel::hideChartsChanged, this,
                &DashboardWidget::onHideChartsChanged);
#endif
    }
    // update the display unit, to not use the default ("PIV")
    updateDisplayUnit();
}

void DashboardWidget::onTxArrived(const QString& hash, const bool isCoinStake, const bool isCSAnyType)
{
    showList();
    if (!isVisible()) return;
#ifdef USE_QTCHARTS
    if (isCoinStake) {
        // Update value if this is our first stake
        if (!hasStakes && stakesFilter)
            hasStakes = stakesFilter->rowCount() > 0;
        tryChartRefresh();
    }
#endif
}

void DashboardWidget::showList()
{
    if (txModel->size() == 0) {
        ui->emptyContainer->setVisible(true);
        ui->listTransactions->setVisible(false);
        ui->comboBoxSortType->setVisible(false);
        ui->comboBoxSort->setVisible(false);
    } else {
        ui->emptyContainer->setVisible(false);
        ui->listTransactions->setVisible(true);
        ui->comboBoxSortType->setVisible(true);
        ui->comboBoxSort->setVisible(true);
    }
}

void DashboardWidget::updateDisplayUnit()
{
    if (walletModel && walletModel->getOptionsModel()) {
        nDisplayUnit = walletModel->getOptionsModel()->getDisplayUnit();
        txHolder->setDisplayUnit(nDisplayUnit);
        ui->listTransactions->update();
    }
}

void DashboardWidget::onSortChanged(const QString& value)
{
    if (!filter) return;

    if (!value.isNull()) {
        changeSort(ui->comboBoxSort->currentIndex());
    } else {
        changeSort(SortTx::DATE_DESC);
    }
}

void DashboardWidget::changeSort(int nSortIndex)
{
    int nColumnIndex = TransactionTableModel::Date;
    Qt::SortOrder order = Qt::DescendingOrder;

    switch (nSortIndex) {
        case SortTx::DATE_DESC:
        {
            nColumnIndex = TransactionTableModel::Date;
            break;
        }
        case SortTx::DATE_ASC:
        {
            nColumnIndex = TransactionTableModel::Date;
            order = Qt::AscendingOrder;
            break;
        }
        case SortTx::AMOUNT_DESC:
        {
            nColumnIndex = TransactionTableModel::Amount;
            break;
        }
        case SortTx::AMOUNT_ASC:
        {
            nColumnIndex = TransactionTableModel::Amount;
            order = Qt::AscendingOrder;
            break;
        }
    }

    ui->comboBoxSort->setCurrentIndex(nSortIndex);
    filter->sort(nColumnIndex, order);

    // Store settings
    QSettings settings;
    settings.setValue("transactionSort", nSortIndex);
}

void DashboardWidget::onSortTypeChanged(const QString& value)
{
    if (!filter) return;
    int filterIndex = ui->comboBoxSortType->currentIndex();
    int filterByType = ui->comboBoxSortType->itemData(filterIndex).toInt();

    filter->setTypeFilter(filterByType);
    ui->listTransactions->update();

    if (filter->rowCount() == 0) {
        ui->emptyContainer->setVisible(true);
        ui->listTransactions->setVisible(false);
    } else {
        showList();
    }

    // Store settings
    QSettings settings;
    settings.setValue("transactionType", filterByType);
}

void DashboardWidget::walletSynced(bool sync)
{
    if (this->isSync != sync) {
        this->isSync = sync;
        ui->layoutWarning->setVisible(!this->isSync);
#ifdef USE_QTCHARTS
        if (!isVisible()) return;
        tryChartRefresh();
#endif
    }
}

void DashboardWidget::changeTheme(bool isLightTheme, QString& theme)
{
    static_cast<TxViewHolder*>(this->txViewDelegate->getRowFactory())->isLightTheme = isLightTheme;
#ifdef USE_QTCHARTS
    if (chart) this->changeChartColors();
#endif
}

#ifdef USE_QTCHARTS

void DashboardWidget::tryChartRefresh()
{
    if (!fShowCharts)
        return;
    if (hasStakes) {
        // First check that everything was loaded properly.
        if (!chart) {
            loadChart();
        } else {
            // Check for min update time to not reload the UI so often if the node is syncing.
            int64_t now = GetTime();
            int chartLoadIntervalTime = CHART_LOAD_MIN_TIME_INTERVAL;
            if (clientModel->inInitialBlockDownload()) chartLoadIntervalTime *= 6; // 90 seconds update
            if (lastRefreshTime + chartLoadIntervalTime < now) {
                lastRefreshTime = now;
                refreshChart();
            }
        }
    }
}

void DashboardWidget::setChartShow(ChartShowType type)
{
    this->chartShow = type;
    if (chartShow == MONTH) {
        ui->containerChartArrow->setVisible(true);
    } else {
        ui->containerChartArrow->setVisible(false);
    }
    if (isChartInitialized) refreshChart();
}

const QStringList monthsNames = {QObject::tr("Jan"), QObject::tr("Feb"), QObject::tr("Mar"), QObject::tr("Apr"),
                                 QObject::tr("May"), QObject::tr("Jun"), QObject::tr("Jul"), QObject::tr("Aug"),
                                 QObject::tr("Sep"), QObject::tr("Oct"), QObject::tr("Nov"), QObject::tr("Dec")};

void DashboardWidget::loadChart()
{
    if (hasStakes) {
        if (!chart) {
            showHideEmptyChart(false, false);
            initChart();
            QDate currentDate = QDate::currentDate();
            monthFilter = currentDate.month();
            yearFilter = currentDate.year();
            for (int i = 1; i < 13; ++i) ui->comboBoxMonths->addItem(QString(monthsNames[i-1]), QVariant(i));
            ui->comboBoxMonths->setCurrentIndex(monthFilter - 1);
            connect(ui->comboBoxMonths, static_cast<void (QComboBox::*)(const QString&)>(&QComboBox::currentTextChanged),
                this, &DashboardWidget::onChartMonthChanged);
            connect(ui->pushButtonChartArrow, &QPushButton::clicked, [this](){ onChartArrowClicked(true); });
            connect(ui->pushButtonChartRight, &QPushButton::clicked, [this](){ onChartArrowClicked(false); });
        }
        refreshChart();
        changeChartColors();
    } else {
        showHideEmptyChart(true, false);
    }
}

void DashboardWidget::showHideEmptyChart(bool showEmpty, bool loading, bool forceView)
{
    if ((stakesFilter && stakesFilter->rowCount() > SHOW_EMPTY_CHART_VIEW_THRESHOLD) || forceView) {
        ui->layoutChart->setVisible(!showEmpty);
        ui->emptyContainerChart->setVisible(showEmpty);
    }
    // Enable/Disable sort buttons
    bool invLoading = !loading;
    ui->comboBoxMonths->setEnabled(invLoading);
    ui->comboBoxYears->setEnabled(invLoading);
    ui->pushButtonMonth->setEnabled(invLoading);
    ui->pushButtonAll->setEnabled(invLoading);
    ui->pushButtonYear->setEnabled(invLoading);
    ui->labelEmptyChart->setText(loading ? tr("Loading chart..") : tr("You have no staking rewards"));
}

void DashboardWidget::initChart()
{
    chart = new QChart();
    axisX = new QBarCategoryAxis();
    axisY = new QValueAxis();

    // Chart style
    chart->legend()->setVisible(false);
    chart->legend()->setAlignment(Qt::AlignTop);
    chart->layout()->setContentsMargins(0, 0, 0, 0);
    chart->setMargins({0, 0, 0, 0});
    chart->setBackgroundRoundness(0);
    // Axis
    chart->addAxis(axisX, Qt::AlignBottom);
    chart->addAxis(axisY, Qt::AlignRight);
    chart->setAnimationOptions(QChart::SeriesAnimations);

    chartView = new QChartView(chart);
    chartView->setRenderHint(QPainter::Antialiasing);
    chartView->setRubberBand( QChartView::HorizontalRubberBand );
    chartView->setContentsMargins(0,0,0,0);

    QHBoxLayout *baseScreensContainer = new QHBoxLayout(this);
    baseScreensContainer->setMargin(0);
    baseScreensContainer->addWidget(chartView);
    ui->chartContainer->setLayout(baseScreensContainer);
    ui->chartContainer->setContentsMargins(0,0,0,0);
    setCssProperty(ui->chartContainer, "container-chart");
}

void DashboardWidget::changeChartColors()
{
    QColor gridLineColorX;
    QColor linePenColorY;
    QColor backgroundColor;
    QColor gridY;
    if (isLightTheme()) {
        gridLineColorX = QColor(255,255,255);
        linePenColorY = gridLineColorX;
        backgroundColor = linePenColorY;
        axisY->setGridLineColor(QColor("#1a000000"));
    } else {
        gridY = QColor("#40ffffff");
        axisY->setGridLineColor(gridY);
        gridLineColorX = QColor(15,11,22);
        linePenColorY =  gridLineColorX;
        backgroundColor = linePenColorY;
    }

    axisX->setGridLineColor(gridLineColorX);
    axisY->setLinePenColor(linePenColorY);
    chart->setBackgroundBrush(QBrush(backgroundColor));
    if (set0) set0->setBorderColor(gridLineColorX);
    if (set1) set1->setBorderColor(gridLineColorX);
}

void DashboardWidget::updateStakeFilter()
{
    if (!stakesFilter) return;
    if (chartShow != ALL) {
        bool filterByMonth = false;
        if (monthFilter != 0 && chartShow == MONTH) {
            filterByMonth = true;
        }
        if (yearFilter != 0) {
            if (filterByMonth) {
                QDate monthFirst = QDate(yearFilter, monthFilter, 1);
                stakesFilter->setDateRange(
                        QDateTime(monthFirst),
                        QDateTime(QDate(yearFilter, monthFilter, monthFirst.daysInMonth()))
                );
            } else {
                stakesFilter->setDateRange(
                        QDateTime(QDate(yearFilter, 1, 1)),
                        QDateTime(QDate(yearFilter, 12, 31))
                );
            }
        } else if (filterByMonth) {
            QDate currentDate = QDate::currentDate();
            QDate monthFirst = QDate(currentDate.year(), monthFilter, 1);
            stakesFilter->setDateRange(
                    QDateTime(monthFirst),
                    QDateTime(QDate(currentDate.year(), monthFilter, monthFirst.daysInMonth()))
            );
            ui->comboBoxYears->setCurrentText(QString::number(currentDate.year()));
        } else {
            stakesFilter->clearDateRange();
        }
    } else {
        stakesFilter->clearDateRange();
    }
}

// pair PIV, zPIV
const QMap<int, std::pair<qint64, qint64>> DashboardWidget::getAmountBy()
{
    if (filterUpdateNeeded) {
        filterUpdateNeeded = false;
        updateStakeFilter();
    }
    const int size = stakesFilter->rowCount();
    QMap<int, std::pair<qint64, qint64>> amountBy;
    // Get all of the stakes
    for (int i = 0; i < size; ++i) {
        QModelIndex modelIndex = stakesFilter->index(i, TransactionTableModel::ToAddress);
        qint64 amount = llabs(modelIndex.data(TransactionTableModel::AmountRole).toLongLong());
        QDate date = modelIndex.data(TransactionTableModel::DateRole).toDateTime().date();
        bool isPiv = modelIndex.data(TransactionTableModel::TypeRole).toInt() != TransactionRecord::StakeZPIV;

        int time = 0;
        switch (chartShow) {
            case YEAR: {
                time = date.month();
                break;
            }
            case ALL: {
                time = date.year();
                break;
            }
            case MONTH: {
                time = date.day();
                break;
            }
            default:
                inform(tr("Error loading chart, invalid show option"));
                return amountBy;
        }
        if (amountBy.contains(time)) {
            if (isPiv) {
                amountBy[time].first += amount;
            } else
                amountBy[time].second += amount;
        } else {
            if (isPiv) {
                amountBy[time] = std::make_pair(amount, 0);
            } else {
                amountBy[time] = std::make_pair(0, amount);
                hasZpivStakes = true;
            }
        }
    }
    return amountBy;
}

bool DashboardWidget::loadChartData(bool withMonthNames)
{
    if (chartData) {
        delete chartData;
        chartData = nullptr;
    }

    chartData = new ChartData();
    chartData->amountsByCache = getAmountBy(); // pair PIV, zPIV

    std::pair<int,int> range = getChartRange(chartData->amountsByCache);
    if (range.first == 0 && range.second == 0) {
        // Problem loading the chart.
        return false;
    }

    bool isOrderedByMonth = chartShow == MONTH;
    int daysInMonth = QDate(yearFilter, monthFilter, 1).daysInMonth();

    for (int j = range.first; j < range.second; j++) {
        int num = (isOrderedByMonth && j > daysInMonth) ? (j % daysInMonth) : j;
        qreal piv = 0;
        qreal zpiv = 0;
        if (chartData->amountsByCache.contains(num)) {
            std::pair <qint64, qint64> pair = chartData->amountsByCache[num];
            piv = (pair.first != 0) ? pair.first / 100000000 : 0;
            zpiv = (pair.second != 0) ? pair.second / 100000000 : 0;
            chartData->totalPiv += pair.first;
            chartData->totalZpiv += pair.second;
        }

        chartData->xLabels << ((withMonthNames) ? monthsNames[num - 1] : QString::number(num));

        chartData->valuesPiv.append(piv);
        chartData->valueszPiv.append(zpiv);

        int max = std::max(piv, zpiv);
        if (max > chartData->maxValue) {
            chartData->maxValue = max;
        }
    }

    return true;
}

void DashboardWidget::onChartYearChanged(const QString& yearStr)
{
    if (isChartInitialized) {
        int newYear = yearStr.toInt();
        if (newYear != yearFilter) {
            yearFilter = newYear;
            filterUpdateNeeded = true;
            refreshChart();
        }
    }
}

void DashboardWidget::onChartMonthChanged(const QString& monthStr)
{
    if (isChartInitialized) {
        int newMonth = ui->comboBoxMonths->currentData().toInt();
        if (newMonth != monthFilter) {
            monthFilter = newMonth;
            filterUpdateNeeded = true;
            refreshChart();
#ifndef Q_OS_MAC
        // quick hack to re paint the chart view.
        chart->removeSeries(series);
        chart->addSeries(series);
#endif
        }
    }
}

bool DashboardWidget::refreshChart()
{
    if (isLoading) return false;
    isLoading = true;
    isChartMin = width() < 1300;
    isChartInitialized = false;
    showHideEmptyChart(true, true);
    return execute(REQUEST_LOAD_TASK);
}

void DashboardWidget::onChartRefreshed()
{
    if (chart) {
        if (series) {
            series->clear();
            series->detachAxis(axisX);
            series->detachAxis(axisY);
        }
        axisX->clear();
    }
    // init sets
    set0 = new QBarSet(CURRENCY_UNIT.c_str());
    set1 = new QBarSet("z" + QString(CURRENCY_UNIT.c_str()));
    set0->setColor(QColor(92,75,125));
    set1->setColor(QColor(176,136,255));

    if (!series) {
        series = new QBarSeries();
        chart->addSeries(series);
    }
    series->attachAxis(axisX);
    series->attachAxis(axisY);

    set0->append(chartData->valuesPiv);
    set1->append(chartData->valueszPiv);

    // Total
    nDisplayUnit = walletModel->getOptionsModel()->getDisplayUnit();
    if (chartData->totalPiv > 0 || chartData->totalZpiv > 0) {
        setCssProperty(ui->labelAmountPiv, "text-stake-piv");
        setCssProperty(ui->labelAmountZpiv, "text-stake-zpiv");
    } else {
        setCssProperty(ui->labelAmountPiv, "text-stake-piv-disable");
        setCssProperty(ui->labelAmountZpiv, "text-stake-zpiv-disable");
    }
    forceUpdateStyle({ui->labelAmountPiv, ui->labelAmountZpiv});
    ui->labelAmountPiv->setText(GUIUtil::formatBalance(chartData->totalPiv, nDisplayUnit));
    ui->labelAmountZpiv->setText(GUIUtil::formatBalance(chartData->totalZpiv, nDisplayUnit, true));

    series->append(set0);
    if (hasZpivStakes)
        series->append(set1);

    // bar width
    if (chartShow == YEAR)
        series->setBarWidth(0.8);
    else {
        series->setBarWidth(0.3);
    }
    axisX->append(chartData->xLabels);
    axisY->setRange(0, chartData->maxValue);

    // Controllers
    switch (chartShow) {
        case ALL: {
            ui->container_chart_dropboxes->setVisible(false);
            break;
        }
        case YEAR: {
            ui->container_chart_dropboxes->setVisible(true);
            ui->containerBoxMonths->setVisible(false);
            break;
        }
        case MONTH: {
            ui->container_chart_dropboxes->setVisible(true);
            ui->containerBoxMonths->setVisible(true);
            break;
        }
        default: break;
    }

    // Refresh years filter, first address created is the start
    int yearStart = QDateTime::fromTime_t(static_cast<uint>(walletModel->getCreationTime())).date().year();
    int currentYear = QDateTime::currentDateTime().date().year();

    QString selection;
    if (ui->comboBoxYears->count() > 0) {
        selection = ui->comboBoxYears->currentText();
        isChartInitialized = false;
    }
    ui->comboBoxYears->clear();
    if (yearStart == currentYear) {
        ui->comboBoxYears->addItem(QString::number(currentYear));
    } else {
        for (int i = yearStart; i < (currentYear + 1); ++i)ui->comboBoxYears->addItem(QString::number(i));
    }

    if (!selection.isEmpty()) {
        ui->comboBoxYears->setCurrentText(selection);
        isChartInitialized = true;
    } else {
        ui->comboBoxYears->setCurrentText(QString::number(currentYear));
    }

    // back to normal
    isChartInitialized = true;
    showHideEmptyChart(false, false, true);
    isLoading = false;
}

std::pair<int, int> DashboardWidget::getChartRange(QMap<int, std::pair<qint64, qint64>> amountsBy)
{
    switch (chartShow) {
        case YEAR:
            return std::make_pair(1, 13);
        case ALL: {
            QList<int> keys = amountsBy.uniqueKeys();
            if (keys.isEmpty()) {
                // This should never happen, ALL means from the beginning of time and if this is called then it must have at least one stake..
                inform(tr("Error loading chart, invalid data"));
                return std::make_pair(0, 0);
            }
            std::sort(keys.begin(), keys.end());
            return std::make_pair(keys.first(), keys.last() + 1);
        }
        case MONTH:
            return std::make_pair(dayStart, dayStart + 9);
        default:
            inform(tr("Error loading chart, invalid show option"));
            return std::make_pair(0, 0);
    }
}

void DashboardWidget::updateAxisX(const QStringList* args)
{
    axisX->clear();
    QStringList months;
    std::pair<int,int> range = getChartRange(chartData->amountsByCache);
    if (args) {
        months = *args;
    } else {
        for (int i = range.first; i < range.second; i++) months << QString::number(i);
    }
    axisX->append(months);
}

void DashboardWidget::onChartArrowClicked(bool goLeft)
{
    bool updateMonth = false;
    bool updateYear = false;
    int dataenddate = getChartRange(chartData->amountsByCache).second;
    QDate currentDate = QDate::currentDate();
    if (goLeft) {
        dayStart--;
        if (dayStart == 0) {
            updateMonth = true;
            if (monthFilter == 1) {
                // Prev year
                monthFilter = 12;
                yearFilter--;
                updateYear = true;
            } else {
                monthFilter--; // Prev month
            }
            dayStart = QDate(yearFilter, monthFilter, 1).daysInMonth();
        }
    } else {
        int dayInMonth = QDate(yearFilter, monthFilter, dayStart).daysInMonth();
        dayStart++;
        if (dayStart > dayInMonth) {
            dayStart = 1;
            updateMonth = true;
            if (monthFilter == 12) {
                // Next year
                monthFilter = 1;
                yearFilter++;
                updateYear = true;
            } else {
                monthFilter++; // Next month
            }
        }
    }
    filterUpdateNeeded = true;
    refreshChart();
    //Check if data end day is current date and monthfilter is current month
    bool fEndDayisCurrent = dataenddate  == currentDate.day() && monthFilter == currentDate.month();

    if (updateMonth)
        ui->comboBoxMonths->setCurrentIndex(monthFilter - 1);

    if (updateYear)
        ui->comboBoxYears->setCurrentText(QString::number(yearFilter));

    // enable/disable the pushButtonChartRight.
    ui->pushButtonChartRight->setEnabled(!fEndDayisCurrent);


}

void DashboardWidget::windowResizeEvent(QResizeEvent* event)
{
    if (hasStakes && axisX) {
        if (width() > 1300) {
            if (isChartMin) {
                isChartMin = false;
                switch (chartShow) {
                    case YEAR: {
                        updateAxisX(&monthsNames);
                        break;
                    }
                    case ALL: break;
                    case MONTH: {
                        updateAxisX();
                        break;
                    }
                    default:
                        inform(tr("Error loading chart, invalid show option"));
                        return;
                }
                chartView->repaint();
            }
        } else {
            if (!isChartMin) {
                updateAxisX();
                isChartMin = true;
            }
        }
    }
}

void DashboardWidget::onHideChartsChanged(bool fHide)
{
    fShowCharts = !fHide;

    if (fShowCharts) {
        if (!stakesFilter) {
            stakesFilter = new TransactionFilterProxy(this);
            stakesFilter->setDynamicSortFilter(false);
            stakesFilter->setSortCaseSensitivity(Qt::CaseInsensitive);
            stakesFilter->setFilterCaseSensitivity(Qt::CaseInsensitive);
            stakesFilter->setOnlyStakes(true);
        }
        stakesFilter->setSourceModel(txModel);
        hasStakes = stakesFilter->rowCount() > 0;
    } else {
        if (stakesFilter) {
            stakesFilter->setSourceModel(nullptr);
        }
    }

    // Hide charts if requested
    ui->right->setVisible(fShowCharts);
    if (fShowCharts) tryChartRefresh();
}

#endif

void DashboardWidget::run(int type)
{
#ifdef USE_QTCHARTS
    if (type == REQUEST_LOAD_TASK) {
        bool withMonthNames = !isChartMin && (chartShow == YEAR);
        if (loadChartData(withMonthNames))
            QMetaObject::invokeMethod(this, "onChartRefreshed", Qt::QueuedConnection);
    }
#endif
}
void DashboardWidget::onError(QString error, int type)
{
    inform(tr("Error loading chart: %1").arg(error));
}

void DashboardWidget::processNewTransaction(const QModelIndex& parent, int start, int /*end*/)
{
    // Prevent notifications-spam when initial block download is in progress
    if (!walletModel || !clientModel || clientModel->inInitialBlockDownload())
        return;

    if (!txModel || txModel->processingQueuedTransactions())
        return;

    QString date = txModel->index(start, TransactionTableModel::Date, parent).data().toString();
    qint64 amount = txModel->index(start, TransactionTableModel::Amount, parent).data(Qt::EditRole).toULongLong();
    QString type = txModel->index(start, TransactionTableModel::Type, parent).data().toString();
    QString address = txModel->index(start, TransactionTableModel::ToAddress, parent).data().toString();

    Q_EMIT incomingTransaction(date, walletModel->getOptionsModel()->getDisplayUnit(), amount, type, address);
}

DashboardWidget::~DashboardWidget()
{
#ifdef USE_QTCHARTS
    delete chart;
#endif
    delete ui;
}
