// Copyright (c) 2019-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef DASHBOARDWIDGET_H
#define DASHBOARDWIDGET_H

#include "qt/pivx/pwidget.h"
#include "qt/pivx/furabstractlistitemdelegate.h"
#include "qt/pivx/furlistrow.h"
#include "transactiontablemodel.h"
#include "qt/pivx/txviewholder.h"
#include "transactionfilterproxy.h"

#include <atomic>
#include <cstdlib>
#include <QWidget>
#include <QLineEdit>
#include <QMap>

#if defined(HAVE_CONFIG_H)
#include "config/pivx-config.h" /* for USE_QTCHARTS */
#endif

#ifdef USE_QTCHARTS

#include <QtCharts/QChartView>
#include <QtCharts/QBarSeries>
#include <QtCharts/QBarCategoryAxis>
#include <QtCharts/QBarSet>
#include <QtCharts/QChart>
#include <QtCharts/QValueAxis>

QT_CHARTS_USE_NAMESPACE

using namespace QtCharts;

#endif

class PIVXGUI;
class WalletModel;

namespace Ui {
class DashboardWidget;
}

class SortEdit : public QLineEdit{
    Q_OBJECT
public:
    explicit SortEdit(QWidget* parent = nullptr) : QLineEdit(parent){}

    inline void mousePressEvent(QMouseEvent *) override{
        Q_EMIT Mouse_Pressed();
    }

    ~SortEdit() override{}

Q_SIGNALS:
    void Mouse_Pressed();

};

enum SortTx {
    DATE_DESC = 0,
    DATE_ASC = 1,
    AMOUNT_DESC = 2,
    AMOUNT_ASC = 3
};

enum ChartShowType {
    ALL,
    YEAR,
    MONTH,
    DAY
};

class ChartData {
public:
    ChartData() {}

    QMap<int, std::pair<qint64, qint64>> amountsByCache;
    qreal maxValue = 0;
    qint64 totalPiv = 0;
    qint64 totalZpiv = 0;
    QList<qreal> valuesPiv;
    QList<qreal> valueszPiv;
    QStringList xLabels;
};

QT_BEGIN_NAMESPACE
class QModelIndex;
QT_END_NAMESPACE

class DashboardWidget : public PWidget
{
    Q_OBJECT

public:
    explicit DashboardWidget(PIVXGUI* _window);
    ~DashboardWidget();

    void loadWalletModel() override;
    void loadChart();

    void run(int type) override;
    void onError(QString error, int type) override;

public Q_SLOTS:
    void walletSynced(bool isSync);
    /**
     * Show incoming transaction notification for new transactions.
     * The new items are those between start and end inclusive, under the given parent item.
    */
    void processNewTransaction(const QModelIndex& parent, int start, int /*end*/);
Q_SIGNALS:
    /** Notify that a new transaction appeared */
    void incomingTransaction(const QString& date, int unit, const CAmount& amount, const QString& type, const QString& address);
private Q_SLOTS:
    void handleTransactionClicked(const QModelIndex &index);
    void changeTheme(bool isLightTheme, QString &theme) override;
    void onSortChanged(const QString&);
    void onSortTypeChanged(const QString& value);
    void updateDisplayUnit();
    void showList();
    void onTxArrived(const QString& hash, const bool isCoinStake, const bool isCSAnyType);

#ifdef USE_QTCHARTS
    void windowResizeEvent(QResizeEvent* event);
    void changeChartColors();
    void onChartYearChanged(const QString&);
    void onChartMonthChanged(const QString&);
    void onChartArrowClicked(bool goLeft);
#endif

private:
    Ui::DashboardWidget *ui{nullptr};
    FurAbstractListItemDelegate* txViewDelegate{nullptr};
    TransactionFilterProxy* filter{nullptr};
    TxViewHolder* txHolder{nullptr};
    TransactionTableModel* txModel{nullptr};
    int nDisplayUnit{-1};
    bool isSync{false};

    void changeSort(int nSortIndex);

#ifdef USE_QTCHARTS

    int64_t lastRefreshTime{0};
    std::atomic<bool> isLoading;

    // Chart
    TransactionFilterProxy* stakesFilter{nullptr};
    bool isChartInitialized{false};
    QChartView *chartView{nullptr};
    QBarSeries *series{nullptr};
    QBarSet *set0{nullptr};
    QBarSet *set1{nullptr};

    QBarCategoryAxis *axisX{nullptr};
    QValueAxis *axisY{nullptr};

    QChart *chart{nullptr};
    bool isChartMin{false};
    ChartShowType chartShow{YEAR};
    int yearFilter{0};
    int monthFilter{0};
    int dayStart{1};
    bool hasZpivStakes{false};

    ChartData* chartData{nullptr};
    bool hasStakes{false};
    bool fShowCharts{true};
    std::atomic<bool> filterUpdateNeeded{false};

    void initChart();
    void showHideEmptyChart(bool show, bool loading, bool forceView = false);
    bool refreshChart();
    void tryChartRefresh();
    void updateStakeFilter();
    const QMap<int, std::pair<qint64, qint64>> getAmountBy();
    bool loadChartData(bool withMonthNames);
    void updateAxisX(const QStringList *arg = nullptr);
    void setChartShow(ChartShowType type);
    std::pair<int, int> getChartRange(QMap<int, std::pair<qint64, qint64>> amountsBy);

private Q_SLOTS:
    void onChartRefreshed();
    void onHideChartsChanged(bool fHide);

#endif

};

#endif // DASHBOARDWIDGET_H
