// Copyright (c) 2019-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef QTUTILS_H
#define QTUTILS_H

#include "qt/pivx/pivxgui.h"

#include <QAbstractAnimation>
#include <QColor>
#include <QComboBox>
#include <QDialog>
#include <QPixmap>
#include <QPoint>
#include <QPropertyAnimation>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QWidget>

#include <initializer_list>

// Repair parameters
const QString SALVAGEWALLET("-salvagewallet");
const QString RESCAN("-rescan");
const QString ZAPTXES1("-zapwallettxes=1");
const QString ZAPTXES2("-zapwallettxes=2");
const QString UPGRADEWALLET("-upgradewallet");
const QString REINDEX("-reindex");
const QString RESYNC("-resync");

extern Qt::Modifier SHORT_KEY;

bool openDialog(QDialog* widget, QWidget* gui);
void closeDialog(QDialog* widget, PIVXGUI* gui);
void openDialogFullScreen(QWidget* parent, QWidget* dialog);
bool openDialogWithOpaqueBackgroundY(QDialog* widget, PIVXGUI* gui, double posX = 3, int posY = 5, bool hideOpaqueBackground = true);
bool openDialogWithOpaqueBackground(QDialog* widget, PIVXGUI* gui, double posX = 3);
bool openDialogWithOpaqueBackgroundFullScreen(QDialog* widget, PIVXGUI* gui);

//
QPixmap encodeToQr(const QString& str, QString& errorStr, const QColor& qrColor = Qt::black);

// Helpers
void updateStyle(QWidget* widget);
QColor getRowColor(bool isLightTheme, bool isHovered, bool isSelected);

// filters
void setFilterAddressBook(QComboBox* filter, SortEdit* lineEdit);
void setSortTx(QComboBox* filter, SortEdit* lineEdit);
void setSortTxTypeFilter(QComboBox* filter, SortEdit* lineEdit);

// Settings
QSettings* getSettings();
void setupSettings(QSettings* settings);

bool isLightTheme();
void setTheme(bool isLight);

void initComboBox(QComboBox* combo, QLineEdit* lineEdit = nullptr, QString cssClass = "btn-combo");
void fillAddressSortControls(SortEdit* seType, SortEdit* seOrder, QComboBox* boxType, QComboBox* boxOrder);
void initCssEditLine(QLineEdit* edit, bool isDialog = false);
void setCssEditLine(QLineEdit* edit, bool isValid, bool forceUpdate = false);
void setCssEditLineDialog(QLineEdit* edit, bool isValid, bool forceUpdate = false);
void setShadow(QWidget* edit);

void setCssBtnPrimary(QPushButton* btn, bool forceUpdate = false);
void setCssBtnSecondary(QPushButton* btn, bool forceUpdate = false);
void setCssTitleScreen(QLabel* label);
void setCssSubtitleScreen(QWidget* wid);
void setCssTextBodyDialog(std::initializer_list<QWidget*> args);
void setCssTextBodyDialog(QWidget* widget);
void setCssProperty(std::initializer_list<QWidget*> args, const QString& value);
void setCssProperty(QWidget* wid, const QString& value, bool forceUpdate = false);
void forceUpdateStyle(QWidget* widget, bool forceUpdate);
void forceUpdateStyle(std::initializer_list<QWidget*> args);

#endif // QTUTILS_H
