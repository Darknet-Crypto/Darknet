// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "bitcoinunits.h"
#include "chainparams.h"
#include "policy/feerate.h"
#include "primitives/transaction.h"

#include <QSettings>
#include <QStringList>

#include <iostream>

BitcoinUnits::BitcoinUnits(QObject* parent) : QAbstractListModel(parent),
                                              unitlist(availableUnits())
{
}

QList<BitcoinUnits::Unit> BitcoinUnits::availableUnits()
{
    QList<BitcoinUnits::Unit> unitlist;
    unitlist.append(PIV);
    unitlist.append(mPIV);
    unitlist.append(uPIV);
    return unitlist;
}

bool BitcoinUnits::valid(int unit)
{
    switch (unit) {
    case PIV:
    case mPIV:
    case uPIV:
        return true;
    default:
        return false;
    }
}

QString BitcoinUnits::id(int unit)
{
    switch (unit) {
    case PIV:
        return QString("pivx");
    case mPIV:
        return QString("mpivx");
    case uPIV:
        return QString::fromUtf8("upivx");
    default:
        return QString("???");
    }
}

QString BitcoinUnits::name(int unit, bool isZpiv)
{
    const QString CURR_UNIT = QString(CURRENCY_UNIT.c_str());
    QString z = "";
    if(isZpiv) z = "z";
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        switch (unit) {
        case PIV:
            return z + CURR_UNIT;
        case mPIV:
            return z + QString("m") + CURR_UNIT;
        case uPIV:
            return z + QString::fromUtf8("μ") + CURR_UNIT;
        default:
            return QString("???");
        }
    } else {
        switch (unit) {
        case PIV:
            return z + QString("t") + CURR_UNIT;
        case mPIV:
            return z + QString("mt") + CURR_UNIT;
        case uPIV:
            return z + QString::fromUtf8("μt") + CURR_UNIT;
        default:
            return QString("???");
        }
    }
}

QString BitcoinUnits::description(int unit)
{
    const QString CURR_UNIT = QString(CURRENCY_UNIT.c_str());
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        switch (unit) {
        case PIV:
            return CURR_UNIT;
        case mPIV:
            return QString("Milli-") + CURR_UNIT + QString(" (1 / 1" THIN_SP_UTF8 "000)");
        case uPIV:
            return QString("Micro-") + CURR_UNIT + QString(" (1 / 1" THIN_SP_UTF8 "000" THIN_SP_UTF8 "000)");
        default:
            return QString("???");
        }
    } else {
        switch (unit) {
        case PIV:
            return QString("Test") + CURR_UNIT;
        case mPIV:
            return QString("Milli-Test") + CURR_UNIT + QString(" (1 / 1" THIN_SP_UTF8 "000)");
        case uPIV:
            return QString("Micro-Test") + CURR_UNIT + QString(" (1 / 1" THIN_SP_UTF8 "000" THIN_SP_UTF8 "000)");
        default:
            return QString("???");
        }
    }
}

qint64 BitcoinUnits::factor(int unit)
{
    switch (unit) {
    case PIV:
        return 100000000;
    case mPIV:
        return 100000;
    case uPIV:
        return 100;
    default:
        return 100000000;
    }
}

int BitcoinUnits::decimals(int unit)
{
    switch (unit) {
    case PIV:
        return 8;
    case mPIV:
        return 5;
    case uPIV:
        return 2;
    default:
        return 0;
    }
}

QString BitcoinUnits::format(int unit, const CAmount& nIn, bool fPlus, SeparatorStyle separators, bool cleanRemainderZeros)
{
    // Note: not using straight sprintf here because we do NOT want
    // localized number formatting.
    if (!valid(unit)){
        return QString(); // Refuse to format invalid unit
    }
    qint64 n = (qint64)nIn;
    qint64 coin = factor(unit);
    int num_decimals = decimals(unit);
    qint64 n_abs = (n > 0 ? n : -n);
    qint64 quotient = n_abs / coin;
    qint64 remainder = n_abs % coin;
    QString quotient_str = QString::number(quotient);
    QString remainder_str = QString::number(remainder).rightJustified(num_decimals, '0');

    // Use SI-style thin space separators as these are locale independent and can't be
    // confused with the decimal marker.
    QChar thin_sp(THIN_SP_CP);
    int q_size = quotient_str.size();
    if (separators == separatorAlways || (separators == separatorStandard && q_size > 4))
        for (int i = 3; i < q_size; i += 3)
            quotient_str.insert(q_size - i, thin_sp);

    if (n < 0)
        quotient_str.insert(0, '-');
    else if (fPlus && n > 0)
        quotient_str.insert(0, '+');

    if (num_decimals <= 0)
        return quotient_str;

    if(cleanRemainderZeros) {
        // Clean remainder
        QString cleanRemainder = remainder_str;
        for (int i = (remainder_str.length() - 1); i > 1; i--) {
            if (remainder_str.at(i) == QChar('0')) {
                cleanRemainder = cleanRemainder.left(cleanRemainder.lastIndexOf("0"));
            } else
                break;
        }
        return quotient_str + QString(".") + cleanRemainder;
    }

    return quotient_str + QString(".") + remainder_str;
}


// TODO: Review all remaining calls to BitcoinUnits::formatWithUnit to
// TODO: determine whether the output is used in a plain text context
// TODO: or an HTML context (and replace with
// TODO: BtcoinUnits::formatHtmlWithUnit in the latter case). Hopefully
// TODO: there aren't instances where the result could be used in
// TODO: either context.

// NOTE: Using formatWithUnit in an HTML context risks wrapping
// quantities at the thousands separator. More subtly, it also results
// in a standard space rather than a thin space, due to a bug in Qt's
// XML whitespace canonicalisation
//
// Please take care to use formatHtmlWithUnit instead, when
// appropriate.

QString BitcoinUnits::formatWithUnit(int unit, const CAmount& amount, bool plussign, SeparatorStyle separators)
{
    return format(unit, amount, plussign, separators) + QString(" ") + name(unit);
}

QString BitcoinUnits::formatHtmlWithUnit(int unit, const CAmount& amount, bool plussign, SeparatorStyle separators)
{
    QString str(formatWithUnit(unit, amount, plussign, separators));
    str.replace(QChar(THIN_SP_CP), QString(COMMA_HTML));
    return QString("<span style='white-space: nowrap;'>%1</span>").arg(str);
}

QString BitcoinUnits::floorWithUnit(int unit, const CAmount& amount, bool plussign, SeparatorStyle separators, bool cleanRemainderZeros, bool isZPIV)
{
    QSettings settings;
    int digits = settings.value("digits").toInt();

    QString result = format(unit, amount, plussign, separators, cleanRemainderZeros);
    if(decimals(unit) > digits) {
        if (!cleanRemainderZeros) {
            result.chop(decimals(unit) - digits);
        } else {
            int lenght = result.mid(result.indexOf("."), result.length() - 1).length() - 1;
            if (lenght > digits) {
                result.chop(lenght - digits);
            }
        }
    }

    return result + QString(" ") + name(unit, isZPIV);
}

QString BitcoinUnits::floorHtmlWithUnit(int unit, const CAmount& amount, bool plussign, SeparatorStyle separators, bool cleanRemainderZeros, bool isZPIV)
{
    QString str(floorWithUnit(unit, amount, plussign, separators, cleanRemainderZeros, isZPIV));
    str.replace(QChar(THIN_SP_CP), QString(COMMA_HTML));
    return QString("<span style='white-space: nowrap;'>%1</span>").arg(str);
}

bool BitcoinUnits::parse(int unit, const QString& value, CAmount* val_out)
{
    if (!valid(unit) || value.isEmpty())
        return false; // Refuse to parse invalid unit or empty string
    int num_decimals = decimals(unit);

    // Ignore spaces and thin spaces when parsing
    QStringList parts = removeSpaces(value).replace(",", ".").split(".");

    if (parts.size() > 2) {
        return false; // More than one dot
    }
    QString whole = parts[0];
    QString decimals;

    if (parts.size() > 1) {
        decimals = parts[1];
    }
    if (decimals.size() > num_decimals) {
        return false; // Exceeds max precision
    }
    bool ok = false;
    QString str = whole + decimals.leftJustified(num_decimals, '0');

    if (str.size() > 18) {
        return false; // Longer numbers will exceed 63 bits
    }
    CAmount retvalue(str.toLongLong(&ok));
    if (val_out) {
        *val_out = retvalue;
    }
    return ok;
}

QString BitcoinUnits::getAmountColumnTitle(int unit)
{
    QString amountTitle = QObject::tr("Amount");
    if (BitcoinUnits::valid(unit)) {
        amountTitle += " (" + BitcoinUnits::name(unit) + ")";
    }
    return amountTitle;
}

int BitcoinUnits::rowCount(const QModelIndex& parent) const
{
    Q_UNUSED(parent);
    return unitlist.size();
}

QVariant BitcoinUnits::data(const QModelIndex& index, int role) const
{
    int row = index.row();
    if (row >= 0 && row < unitlist.size()) {
        Unit unit = unitlist.at(row);
        switch (role) {
        case Qt::EditRole:
        case Qt::DisplayRole:
            return QVariant(name(unit));
        case Qt::ToolTipRole:
            return QVariant(description(unit));
        case UnitRole:
            return QVariant(static_cast<int>(unit));
        }
    }
    return QVariant();
}

CAmount BitcoinUnits::maxMoney()
{
    return MAX_MONEY;
}
