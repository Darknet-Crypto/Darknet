// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2020 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "guiutil.h"

#include "bitcoinaddressvalidator.h"
#include "bitcoinunits.h"
#include "qvalidatedlineedit.h"
#include "walletmodel.h"

#include "init.h"
#include "policy/policy.h"
#include "primitives/transaction.h"
#include "protocol.h"
#include "script/script.h"
#include "script/standard.h"
#include "util/system.h"

#ifdef WIN32
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0501
#ifdef _WIN32_IE
#undef _WIN32_IE
#endif
#define _WIN32_IE 0x0501
#define WIN32_LEAN_AND_MEAN 1
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "shellapi.h"
#include "shlobj.h"
#include "shlwapi.h"
#endif

#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDesktopWidget>
#include <QRegExp>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QFileDialog>
#include <QFont>
#include <QLineEdit>
#include <QScreen>
#include <QSettings>
#include <QTextDocument> // for Qt::mightBeRichText
#include <QThread>
#include <QUrlQuery>
#include <QMouseEvent>


#if defined(Q_OS_MAC)
extern double NSAppKitVersionNumber;
#if !defined(NSAppKitVersionNumber10_8)
#define NSAppKitVersionNumber10_8 1187
#endif
#if !defined(NSAppKitVersionNumber10_9)
#define NSAppKitVersionNumber10_9 1265
#endif
#endif

#define URI_SCHEME "pivx"

#if defined(Q_OS_MAC)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#include <CoreServices/CoreServices.h>
#include <QProcess>

void ForceActivation();
#endif

namespace GUIUtil
{
QString dateTimeStr(const QDateTime& date)
{
    return date.date().toString(Qt::SystemLocaleShortDate) + QString(" ") + date.toString("hh:mm");
}

QString dateTimeStrWithSeconds(const QDateTime& date)
{
    return date.date().toString(Qt::SystemLocaleShortDate) + QString(" ") + date.toString("hh:mm:ss");
}

QString dateTimeStr(qint64 nTime)
{
    return dateTimeStr(QDateTime::fromTime_t((qint32)nTime));
}

QFont bitcoinAddressFont()
{
    QFont font("Monospace");
    font.setStyleHint(QFont::Monospace);
    return font;
}

/**
 * Parse a string into a number of base monetary units and
 * return validity.
 * @note Must return 0 if !valid.
 */
CAmount parseValue(const QString& text, int displayUnit, bool* valid_out)
{
    CAmount val = 0;
    bool valid = BitcoinUnits::parse(displayUnit, text, &val);
    if (valid) {
        if (val < 0 || val > BitcoinUnits::maxMoney())
            valid = false;
    }
    if (valid_out)
        *valid_out = valid;
    return valid ? val : 0;
}

QString formatBalance(CAmount amount, int nDisplayUnit, bool isZpiv)
{
    return (amount == 0) ? ("0.00 " + BitcoinUnits::name(nDisplayUnit, isZpiv)) : BitcoinUnits::floorHtmlWithUnit(nDisplayUnit, amount, false, BitcoinUnits::separatorAlways, true, isZpiv);
}

QString formatBalanceWithoutHtml(CAmount amount, int nDisplayUnit, bool isZpiv)
{
    return (amount == 0) ? ("0.00 " + BitcoinUnits::name(nDisplayUnit, isZpiv)) : BitcoinUnits::floorWithUnit(nDisplayUnit, amount, false, BitcoinUnits::separatorAlways, true, isZpiv);
}

void setupAddressWidget(QValidatedLineEdit* widget, QWidget* parent)
{
    parent->setFocusProxy(widget);

    widget->setFont(bitcoinAddressFont());
    // We don't want translators to use own addresses in translations
    // and this is the only place, where this address is supplied.
    widget->setPlaceholderText(QObject::tr("Enter PIVX address (e.g. %1)").arg("D7VFR83SQbiezrW72hjcWJtcfip5krte2Z"));
    widget->setValidator(new BitcoinAddressEntryValidator(parent));
    widget->setCheckValidator(new BitcoinAddressCheckValidator(parent));
}

void setupAmountWidget(QLineEdit* widget, QWidget* parent)
{
    QRegularExpression rx("^(\\d{0,8})((\\.|,)\\d{1,8})?$");
    QValidator *validator = new QRegularExpressionValidator(rx, widget);
    widget->setValidator(validator);
}

void updateWidgetTextAndCursorPosition(QLineEdit* widget, const QString& str)
{
    const int cpos = widget->cursorPosition();
    widget->setText(str);
    if (cpos > str.size()) return;
    widget->setCursorPosition(cpos);
}

bool parseBitcoinURI(const QUrl& uri, SendCoinsRecipient* out)
{
    // return if URI is not valid or is no PIVX: URI
    if (!uri.isValid() || uri.scheme() != QString(URI_SCHEME))
        return false;

    SendCoinsRecipient rv;
    rv.address = uri.path();
    // Trim any following forward slash which may have been added by the OS
    if (rv.address.endsWith("/")) {
        rv.address.truncate(rv.address.length() - 1);
    }
    rv.amount = 0;

    QUrlQuery uriQuery(uri);
    QList<QPair<QString, QString> > items = uriQuery.queryItems();
    for (QList<QPair<QString, QString> >::iterator i = items.begin(); i != items.end(); i++)
    {
        bool fShouldReturnFalse = false;
        if (i->first.startsWith("req-")) {
            i->first.remove(0, 4);
            fShouldReturnFalse = true;
        }

        if (i->first == "label") {
            rv.label = i->second;
            fShouldReturnFalse = false;
        }
        if (i->first == "message") {
            rv.message = i->second;
            fShouldReturnFalse = false;
        } else if (i->first == "amount") {
            if (!i->second.isEmpty()) {
                if (!BitcoinUnits::parse(BitcoinUnits::PIV, i->second, &rv.amount)) {
                    return false;
                }
            }
            fShouldReturnFalse = false;
        }

        if (fShouldReturnFalse)
            return false;
    }
    if (out) {
        *out = rv;
    }
    return true;
}

bool parseBitcoinURI(QString uri, SendCoinsRecipient* out)
{
    // Convert pivx:// to pivx:
    //
    //    Cannot handle this later, because pivx:// will cause Qt to see the part after // as host,
    //    which will lower-case it (and thus invalidate the address).
    if (uri.startsWith(URI_SCHEME "://", Qt::CaseInsensitive)) {
        uri.replace(0, std::strlen(URI_SCHEME) + 3, URI_SCHEME ":");
    }
    QUrl uriInstance(uri);
    return parseBitcoinURI(uriInstance, out);
}

QString formatBitcoinURI(const SendCoinsRecipient& info)
{
    QString ret = QString(URI_SCHEME ":%1").arg(info.address);
    int paramCount = 0;

    if (info.amount) {
        ret += QString("?amount=%1").arg(BitcoinUnits::format(BitcoinUnits::PIV, info.amount, false, BitcoinUnits::separatorNever));
        paramCount++;
    }

    if (!info.label.isEmpty()) {
        QString lbl(QUrl::toPercentEncoding(info.label));
        ret += QString("%1label=%2").arg(paramCount == 0 ? "?" : "&").arg(lbl);
        paramCount++;
    }

    if (!info.message.isEmpty()) {
        QString msg(QUrl::toPercentEncoding(info.message));
        ret += QString("%1message=%2").arg(paramCount == 0 ? "?" : "&").arg(msg);
        paramCount++;
    }

    return ret;
}

bool isDust(const QString& address, const CAmount& amount)
{
    CTxDestination dest = DecodeDestination(address.toStdString());
    CScript script = GetScriptForDestination(dest);
    CTxOut txOut(amount, script);
    return IsDust(txOut, dustRelayFee);
}

QString HtmlEscape(const QString& str, bool fMultiLine)
{
    QString escaped = str.toHtmlEscaped();
    escaped = escaped.replace(" ", "&nbsp;");
    if (fMultiLine) {
        escaped = escaped.replace("\n", "<br>\n");
    }
    return escaped;
}

QString HtmlEscape(const std::string& str, bool fMultiLine)
{
    return HtmlEscape(QString::fromStdString(str), fMultiLine);
}

void copyEntryData(QAbstractItemView* view, int column, int role)
{
    if (!view || !view->selectionModel())
        return;
    QModelIndexList selection = view->selectionModel()->selectedRows(column);

    if (!selection.isEmpty()) {
        // Copy first item
        setClipboard(selection.at(0).data(role).toString());
    }
}

QVariant getEntryData(QAbstractItemView *view, int column, int role)
{
    if (!view || !view->selectionModel())
        return QVariant();
    QModelIndexList selection = view->selectionModel()->selectedRows(column);

    if (!selection.isEmpty()) {
        // Return first item
        return (selection.at(0).data(role));
    }
    return QVariant();
}

QString getSaveFileName(QWidget* parent, const QString& caption, const QString& dir, const QString& filter, QString* selectedSuffixOut)
{
    QString selectedFilter;
    QString myDir;
    if (dir.isEmpty()) // Default to user documents location
    {
        myDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }
    else
    {
        myDir = dir;
    }
    /* Directly convert path to native OS path separators */
    QString result = QDir::toNativeSeparators(QFileDialog::getSaveFileName(parent, caption, myDir, filter, &selectedFilter));

    /* Extract first suffix from filter pattern "Description (*.foo)" or "Description (*.foo *.bar ...) */
    QRegExp filter_re(".* \\(\\*\\.(.*)[ \\)]");
    QString selectedSuffix;
    if (filter_re.exactMatch(selectedFilter)) {
        selectedSuffix = filter_re.cap(1);
    }

    /* Add suffix if needed */
    QFileInfo info(result);
    if (!result.isEmpty()) {
        if (info.suffix().isEmpty() && !selectedSuffix.isEmpty()) {
            /* No suffix specified, add selected suffix */
            if (!result.endsWith("."))
                result.append(".");
            result.append(selectedSuffix);
        }
    }

    /* Return selected suffix if asked to */
    if (selectedSuffixOut) {
        *selectedSuffixOut = selectedSuffix;
    }
    return result;
}

QString getOpenFileName(QWidget* parent, const QString& caption, const QString& dir, const QString& filter, QString* selectedSuffixOut)
{
    QString selectedFilter;
    QString myDir;
    if (dir.isEmpty()) // Default to user documents location
    {
        myDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    }
    else
    {
        myDir = dir;
    }
    /* Directly convert path to native OS path separators */
    QString result = QDir::toNativeSeparators(QFileDialog::getOpenFileName(parent, caption, myDir, filter, &selectedFilter));

    if (selectedSuffixOut) {
        /* Extract first suffix from filter pattern "Description (*.foo)" or "Description (*.foo *.bar ...) */
        QRegExp filter_re(".* \\(\\*\\.(.*)[ \\)]");
        QString selectedSuffix;
        if (filter_re.exactMatch(selectedFilter)) {
            selectedSuffix = filter_re.cap(1);
        }
        *selectedSuffixOut = selectedSuffix;
    }
    return result;
}

Qt::ConnectionType blockingGUIThreadConnection()
{
    if (QThread::currentThread() != qApp->thread()) {
        return Qt::BlockingQueuedConnection;
    } else {
        return Qt::DirectConnection;
    }
}

bool checkPoint(const QPoint& p, const QWidget* w)
{
    QWidget* atW = QApplication::widgetAt(w->mapToGlobal(p));
    if (!atW) return false;
    return atW->topLevelWidget() == w;
}

bool isObscured(QWidget* w)
{
    return !(checkPoint(QPoint(0, 0), w) && checkPoint(QPoint(w->width() - 1, 0), w) && checkPoint(QPoint(0, w->height() - 1), w) && checkPoint(QPoint(w->width() - 1, w->height() - 1), w) && checkPoint(QPoint(w->width() / 2, w->height() / 2), w));
}

void bringToFront(QWidget* w)
{
#ifdef Q_OS_MAC
    ForceActivation();
#endif

    if (w) {
        // activateWindow() (sometimes) helps with keyboard focus on Windows
        if (w->isMinimized()) {
            w->showNormal();
        } else {
            w->show();
        }
        w->activateWindow();
        w->raise();
    }
}

/* Open file with the associated application */
bool openFile(fs::path path, bool isTextFile)
{
    bool ret = false;
    if (fs::exists(path)) {
        ret = QDesktopServices::openUrl(QUrl::fromLocalFile(boostPathToQString(path)));
#ifdef Q_OS_MAC
        // Workaround for macOS-specific behavior; see btc@15409.
        if (isTextFile && !ret) {
            ret = QProcess::startDetached("/usr/bin/open", QStringList{"-t", boostPathToQString(path)});
        }
#endif
    }
    return ret;
}

bool openDebugLogfile()
{
    return openFile(GetDataDir() / "debug.log", true);
}

bool openConfigfile()
{
    return openFile(GetConfigFile(gArgs.GetArg("-conf", PIVX_CONF_FILENAME)), true);
}

bool openMNConfigfile()
{
    return openFile(GetMasternodeConfigFile(), true);
}

bool showBackups()
{
    return openFile(GetDataDir() / "backups", false);
}

void SubstituteFonts(const QString& language)
{
#if defined(Q_OS_MAC)
// Background:
// OSX's default font changed in 10.9 and QT is unable to find it with its
// usual fallback methods when building against the 10.7 sdk or lower.
// The 10.8 SDK added a function to let it find the correct fallback font.
// If this fallback is not properly loaded, some characters may fail to
// render correctly.
//
// The same thing happened with 10.10. .Helvetica Neue DeskInterface is now default.
//
// Solution: If building with the 10.7 SDK or lower and the user's platform
// is 10.9 or higher at runtime, substitute the correct font. This needs to
// happen before the QApplication is created.
#if defined(MAC_OS_X_VERSION_MAX_ALLOWED) && MAC_OS_X_VERSION_MAX_ALLOWED < MAC_OS_X_VERSION_10_8
    if (floor(NSAppKitVersionNumber) > NSAppKitVersionNumber10_8) {
        if (floor(NSAppKitVersionNumber) <= NSAppKitVersionNumber10_9)
            /* On a 10.9 - 10.9.x system */
            QFont::insertSubstitution(".Lucida Grande UI", "Lucida Grande");
        else {
            /* 10.10 or later system */
            if (language == "zh_CN" || language == "zh_TW" || language == "zh_HK") // traditional or simplified Chinese
                QFont::insertSubstitution(".Helvetica Neue DeskInterface", "Heiti SC");
            else if (language == "ja") // Japanesee
                QFont::insertSubstitution(".Helvetica Neue DeskInterface", "Songti SC");
            else
                QFont::insertSubstitution(".Helvetica Neue DeskInterface", "Lucida Grande");
        }
    }
#endif
#endif
}

ToolTipToRichTextFilter::ToolTipToRichTextFilter(int size_threshold, QObject* parent) : QObject(parent),
                                                                                        size_threshold(size_threshold)
{
}

bool ToolTipToRichTextFilter::eventFilter(QObject* obj, QEvent* evt)
{
    if (evt->type() == QEvent::ToolTipChange) {
        QWidget* widget = static_cast<QWidget*>(obj);
        QString tooltip = widget->toolTip();
        if (tooltip.size() > size_threshold && !tooltip.startsWith("<qt")) {
            // Escape the current message as HTML and replace \n by <br> if it's not rich text
            if (!Qt::mightBeRichText(tooltip))
                tooltip = HtmlEscape(tooltip, true);
            // Envelop with <qt></qt> to make sure Qt detects every tooltip as rich text
            // and style='white-space:pre' to preserve line composition
            tooltip = "<qt style='white-space:pre'>" + tooltip + "</qt>";
            widget->setToolTip(tooltip);
            return true;
        }
    }
    return QObject::eventFilter(obj, evt);
}

void TableViewLastColumnResizingFixer::connectViewHeadersSignals()
{
    connect(tableView->horizontalHeader(), &QHeaderView::sectionResized, this, &TableViewLastColumnResizingFixer::on_sectionResized);
    connect(tableView->horizontalHeader(), &QHeaderView::geometriesChanged, this, &TableViewLastColumnResizingFixer::on_geometriesChanged);
}

// We need to disconnect these while handling the resize events, otherwise we can enter infinite loops.
void TableViewLastColumnResizingFixer::disconnectViewHeadersSignals()
{
    disconnect(tableView->horizontalHeader(), &QHeaderView::sectionResized, this, &TableViewLastColumnResizingFixer::on_sectionResized);
    disconnect(tableView->horizontalHeader(), &QHeaderView::geometriesChanged, this, &TableViewLastColumnResizingFixer::on_geometriesChanged);
}

// Setup the resize mode, handles compatibility for Qt5 and below as the method signatures changed.
// Refactored here for readability.
void TableViewLastColumnResizingFixer::setViewHeaderResizeMode(int logicalIndex, QHeaderView::ResizeMode resizeMode)
{
    tableView->horizontalHeader()->setSectionResizeMode(logicalIndex, resizeMode);
}

void TableViewLastColumnResizingFixer::resizeColumn(int nColumnIndex, int width)
{
    tableView->setColumnWidth(nColumnIndex, width);
    tableView->horizontalHeader()->resizeSection(nColumnIndex, width);
}

int TableViewLastColumnResizingFixer::getColumnsWidth()
{
    int nColumnsWidthSum = 0;
    for (int i = 0; i < columnCount; i++) {
        nColumnsWidthSum += tableView->horizontalHeader()->sectionSize(i);
    }
    return nColumnsWidthSum;
}

int TableViewLastColumnResizingFixer::getAvailableWidthForColumn(int column)
{
    int nResult = lastColumnMinimumWidth;
    int nTableWidth = tableView->horizontalHeader()->width();

    if (nTableWidth > 0) {
        int nOtherColsWidth = getColumnsWidth() - tableView->horizontalHeader()->sectionSize(column);
        nResult = std::max(nResult, nTableWidth - nOtherColsWidth);
    }

    return nResult;
}

// Make sure we don't make the columns wider than the tables viewport width.
void TableViewLastColumnResizingFixer::adjustTableColumnsWidth()
{
    disconnectViewHeadersSignals();
    resizeColumn(lastColumnIndex, getAvailableWidthForColumn(lastColumnIndex));
    connectViewHeadersSignals();

    int nTableWidth = tableView->horizontalHeader()->width();
    int nColsWidth = getColumnsWidth();
    if (nColsWidth > nTableWidth) {
        resizeColumn(secondToLastColumnIndex, getAvailableWidthForColumn(secondToLastColumnIndex));
    }
}

// Make column use all the space available, useful during window resizing.
void TableViewLastColumnResizingFixer::stretchColumnWidth(int column)
{
    disconnectViewHeadersSignals();
    resizeColumn(column, getAvailableWidthForColumn(column));
    connectViewHeadersSignals();
}

// When a section is resized this is a slot-proxy for ajustAmountColumnWidth().
void TableViewLastColumnResizingFixer::on_sectionResized(int logicalIndex, int oldSize, int newSize)
{
    adjustTableColumnsWidth();
    int remainingWidth = getAvailableWidthForColumn(logicalIndex);
    if (newSize > remainingWidth) {
        resizeColumn(logicalIndex, remainingWidth);
    }
}

// When the tabless geometry is ready, we manually perform the stretch of the "Message" column,
// as the "Stretch" resize mode does not allow for interactive resizing.
void TableViewLastColumnResizingFixer::on_geometriesChanged()
{
    if ((getColumnsWidth() - this->tableView->horizontalHeader()->width()) != 0) {
        disconnectViewHeadersSignals();
        resizeColumn(secondToLastColumnIndex, getAvailableWidthForColumn(secondToLastColumnIndex));
        connectViewHeadersSignals();
    }
}

/**
 * Initializes all internal variables and prepares the
 * the resize modes of the last 2 columns of the table and
 */
TableViewLastColumnResizingFixer::TableViewLastColumnResizingFixer(QTableView* table, int lastColMinimumWidth, int allColsMinimumWidth) : tableView(table),
                                                                                                                                          lastColumnMinimumWidth(lastColMinimumWidth),
                                                                                                                                          allColumnsMinimumWidth(allColsMinimumWidth)
{
    columnCount = tableView->horizontalHeader()->count();
    lastColumnIndex = columnCount - 1;
    secondToLastColumnIndex = columnCount - 2;
    tableView->horizontalHeader()->setMinimumSectionSize(allColumnsMinimumWidth);
    setViewHeaderResizeMode(secondToLastColumnIndex, QHeaderView::Interactive);
    setViewHeaderResizeMode(lastColumnIndex, QHeaderView::Interactive);
}

/**
 * Class constructor.
 * @param[in] seconds   Number of seconds to convert to a DHMS string
 */
DHMSTableWidgetItem::DHMSTableWidgetItem(const int64_t seconds) : QTableWidgetItem(),
                                                                  value(seconds)
{
    this->setText(QString::fromStdString(DurationToDHMS(seconds)));
}

/**
 * Comparator overload to ensure that the "DHMS"-type durations as used in
 * the "active-since" list in the masternode tab are sorted by the elapsed
 * duration (versus the string value being sorted).
 * @param[in] item      Right hand side of the less than operator
 */
bool DHMSTableWidgetItem::operator<(QTableWidgetItem const& item) const
{
    DHMSTableWidgetItem const* rhs =
        dynamic_cast<DHMSTableWidgetItem const*>(&item);

    if (!rhs)
        return QTableWidgetItem::operator<(item);

    return value < rhs->value;
}

#ifdef WIN32
fs::path static StartupShortcutPath()
{
    std::string chain = gArgs.GetChainName();
    if (chain == CBaseChainParams::TESTNET)
        return GetSpecialFolderPath(CSIDL_STARTUP) / "PIVX (testnet).lnk";
    else if (chain == CBaseChainParams::REGTEST)
        return GetSpecialFolderPath(CSIDL_STARTUP) / "PIVX (regtest).lnk";

    return GetSpecialFolderPath(CSIDL_STARTUP) / "PIVX.lnk";
}

bool GetStartOnSystemStartup()
{
    // check for PIVX*.lnk
    return fs::exists(StartupShortcutPath());
}

bool SetStartOnSystemStartup(bool fAutoStart)
{
    // If the shortcut exists already, remove it for updating
    fs::remove(StartupShortcutPath());

    if (fAutoStart) {
        CoInitialize(nullptr);

        // Get a pointer to the IShellLink interface.
        IShellLinkW* psl = nullptr;
        HRESULT hres = CoCreateInstance(CLSID_ShellLink, nullptr,
            CLSCTX_INPROC_SERVER, IID_IShellLinkW,
            reinterpret_cast<void**>(&psl));

        if (SUCCEEDED(hres)) {
            // Get the current executable path
            WCHAR pszExePath[MAX_PATH];
            GetModuleFileNameW(nullptr, pszExePath, ARRAYSIZE(pszExePath));

            // Start client minimized
            QString strArgs = "-min";
            // Set -testnet /-regtest options
            strArgs += QString::fromStdString(strprintf(" -testnet=%d -regtest=%d", gArgs.GetBoolArg("-testnet", false), gArgs.GetBoolArg("-regtest", false)));

            // Set the path to the shortcut target
            psl->SetPath(pszExePath);
            PathRemoveFileSpecW(pszExePath);
            psl->SetWorkingDirectory(pszExePath);
            psl->SetShowCmd(SW_SHOWMINNOACTIVE);
            psl->SetArguments(strArgs.toStdWString().c_str());

            // Query IShellLink for the IPersistFile interface for
            // saving the shortcut in persistent storage.
            IPersistFile* ppf = nullptr;
            hres = psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&ppf));
            if (SUCCEEDED(hres)) {
                // Save the link by calling IPersistFile::Save.
                hres = ppf->Save(StartupShortcutPath().wstring().c_str(), TRUE);
                ppf->Release();
                psl->Release();
                CoUninitialize();
                return true;
            }
            psl->Release();
        }
        CoUninitialize();
        return false;
    }
    return true;
}

#elif defined(Q_OS_LINUX)

// Follow the Desktop Application Autostart Spec:
// http://standards.freedesktop.org/autostart-spec/autostart-spec-latest.html

fs::path static GetAutostartDir()
{
    char* pszConfigHome = getenv("XDG_CONFIG_HOME");
    if (pszConfigHome) return fs::path(pszConfigHome) / "autostart";
    char* pszHome = getenv("HOME");
    if (pszHome) return fs::path(pszHome) / ".config" / "autostart";
    return fs::path();
}

fs::path static GetAutostartFilePath()
{
    return GetAutostartDir() / "pivx.desktop";
}

bool GetStartOnSystemStartup()
{
    fsbridge::ifstream optionFile(GetAutostartFilePath());
    if (!optionFile.good())
        return false;
    // Scan through file for "Hidden=true":
    std::string line;
    while (!optionFile.eof()) {
        getline(optionFile, line);
        if (line.find("Hidden") != std::string::npos &&
            line.find("true") != std::string::npos)
            return false;
    }
    optionFile.close();

    return true;
}

bool SetStartOnSystemStartup(bool fAutoStart)
{
    if (!fAutoStart) {
        fs::remove(GetAutostartFilePath());
    } else {
        char pszExePath[MAX_PATH+1];
        ssize_t r = readlink("/proc/self/exe", pszExePath, sizeof(pszExePath) - 1);
        if (r == -1)
            return false;
        pszExePath[r] = '\0';

        fs::create_directories(GetAutostartDir());

        fsbridge::ofstream optionFile(GetAutostartFilePath(), std::ios_base::out | std::ios_base::trunc);
        if (!optionFile.good())
            return false;
        // Write a pivx.desktop file to the autostart directory:
        optionFile << "[Desktop Entry]\n";
        optionFile << "Type=Application\n";
        if (gArgs.GetBoolArg("-testnet", false))
            optionFile << "Name=PIVX (testnet)\n";
        else if (gArgs.GetBoolArg("-regtest", false))
            optionFile << "Name=PIVX (regtest)\n";
        else
            optionFile << "Name=PIVX\n";
        optionFile << "Exec=" << pszExePath << strprintf(" -min -testnet=%d -regtest=%d\n", gArgs.GetBoolArg("-testnet", false), gArgs.GetBoolArg("-regtest", false));
        optionFile << "Terminal=false\n";
        optionFile << "Hidden=false\n";
        optionFile.close();
    }
    return true;
}


#elif defined(Q_OS_MAC)
// based on: https://github.com/Mozketo/LaunchAtLoginController/blob/master/LaunchAtLoginController.m

LSSharedFileListItemRef findStartupItemInList(LSSharedFileListRef list, CFURLRef findUrl);
LSSharedFileListItemRef findStartupItemInList(LSSharedFileListRef list, CFURLRef findUrl)
{
    CFArrayRef listSnapshot = LSSharedFileListCopySnapshot(list, nullptr);
    if (listSnapshot == nullptr) {
        return nullptr;
    }

    // loop through the list of startup items and try to find the pivx app
    for (int i = 0; i < CFArrayGetCount(listSnapshot); i++) {
        LSSharedFileListItemRef item = (LSSharedFileListItemRef)CFArrayGetValueAtIndex(listSnapshot, i);
        UInt32 resolutionFlags = kLSSharedFileListNoUserInteraction | kLSSharedFileListDoNotMountVolumes;
        CFURLRef currentItemURL = nullptr;

#if defined(MAC_OS_X_VERSION_MAX_ALLOWED) && MAC_OS_X_VERSION_MAX_ALLOWED >= 10100
    if (&LSSharedFileListItemCopyResolvedURL)
        currentItemURL = LSSharedFileListItemCopyResolvedURL(item, resolutionFlags, nullptr);
#if defined(MAC_OS_X_VERSION_MIN_REQUIRED) && MAC_OS_X_VERSION_MIN_REQUIRED < 10100
    else
        LSSharedFileListItemResolve(item, resolutionFlags, &currentItemURL, nullptr);
#endif
#else
    LSSharedFileListItemResolve(item, resolutionFlags, &currentItemURL, nullptr);
#endif

        if (currentItemURL) {
            if (CFEqual(currentItemURL, findUrl)) {
                // found
                CFRelease(listSnapshot);
                CFRelease(currentItemURL);
                return item;
            }
            CFRelease(currentItemURL);
        }
    }

    CFRelease(listSnapshot);
    return nullptr;
}

bool GetStartOnSystemStartup()
{
    CFURLRef bitcoinAppUrl = CFBundleCopyBundleURL(CFBundleGetMainBundle());
    if (bitcoinAppUrl == nullptr) {
        return false;
    }

    LSSharedFileListRef loginItems = LSSharedFileListCreate(nullptr, kLSSharedFileListSessionLoginItems, nullptr);
    LSSharedFileListItemRef foundItem = findStartupItemInList(loginItems, bitcoinAppUrl);

    CFRelease(bitcoinAppUrl);
    return !!foundItem; // return boolified object
}

bool SetStartOnSystemStartup(bool fAutoStart)
{
    CFURLRef bitcoinAppUrl = CFBundleCopyBundleURL(CFBundleGetMainBundle());
    if (bitcoinAppUrl == nullptr) {
        return false;
    }

    LSSharedFileListRef loginItems = LSSharedFileListCreate(nullptr, kLSSharedFileListSessionLoginItems, nullptr);
    LSSharedFileListItemRef foundItem = findStartupItemInList(loginItems, bitcoinAppUrl);

    if (fAutoStart && !foundItem) {
        // add pivx app to startup item list
        LSSharedFileListInsertItemURL(loginItems, kLSSharedFileListItemBeforeFirst, nullptr, nullptr, bitcoinAppUrl, nullptr, nullptr);
    } else if (!fAutoStart && foundItem) {
        // remove item
        LSSharedFileListItemRemove(loginItems, foundItem);
    }

    CFRelease(bitcoinAppUrl);
    return true;
}
#pragma GCC diagnostic pop
#else

bool GetStartOnSystemStartup()
{
    return false;
}
bool SetStartOnSystemStartup(bool fAutoStart) { return false; }

#endif

void saveWindowGeometry(const QString& strSetting, QWidget* parent)
{
    QSettings settings;
    settings.setValue(strSetting + "Pos", parent->pos());
    settings.setValue(strSetting + "Size", parent->size());
}

void restoreWindowGeometry(const QString& strSetting, const QSize& defaultSize, QWidget* parent)
{
    QSettings settings;
    QPoint pos = settings.value(strSetting + "Pos").toPoint();
    QSize size = settings.value(strSetting + "Size", defaultSize).toSize();

    if (!pos.x() && !pos.y()) {
        QRect screen = QGuiApplication::primaryScreen()->geometry();
        pos.setX((screen.width() - size.width()) / 2);
        pos.setY((screen.height() - size.height()) / 2);
    }

    parent->resize(size);
    parent->move(pos);
}

// Check whether a theme is not build-in
bool isExternal(QString theme)
{
    if (theme.isEmpty())
        return false;

    return (theme.operator!=("default") && theme.operator!=("default-dark"));
}

// Open CSS when configured
QString loadStyleSheet()
{
    QString styleSheet;
    QSettings settings;
    QString cssName;
    QString theme = settings.value("theme", "").toString();

    if (isExternal(theme)) {
        // External CSS
        settings.setValue("fCSSexternal", true);
        fs::path pathAddr = GetDataDir() / "themes/";
        cssName = pathAddr.string().c_str() + theme + "/css/theme.css";
    } else {
        // Build-in CSS
        settings.setValue("fCSSexternal", false);
        if (!theme.isEmpty()) {
            cssName = QString(":/css/") + theme;
        } else {
            cssName = QString(":/css/default");
            settings.setValue("theme", "default");
        }
    }

    QFile qFile(cssName);
    if (qFile.open(QFile::ReadOnly)) {
        styleSheet = QLatin1String(qFile.readAll());
    }

    return styleSheet;
}

void setClipboard(const QString& str)
{
    QApplication::clipboard()->setText(str, QClipboard::Clipboard);
    QApplication::clipboard()->setText(str, QClipboard::Selection);
}

fs::path qstringToBoostPath(const QString& path)
{
    return fs::path(path.toStdString());
}

QString boostPathToQString(const fs::path& path)
{
    return QString::fromStdString(path.string());
}

QString formatDurationStr(int secs)
{
    QStringList strList;
    int days = secs / 86400;
    int hours = (secs % 86400) / 3600;
    int mins = (secs % 3600) / 60;
    int seconds = secs % 60;

    if (days)
        strList.append(QString(QObject::tr("%1 d")).arg(days));
    if (hours)
        strList.append(QString(QObject::tr("%1 h")).arg(hours));
    if (mins)
        strList.append(QString(QObject::tr("%1 m")).arg(mins));
    if (seconds || (!days && !hours && !mins))
        strList.append(QString(QObject::tr("%1 s")).arg(seconds));

    return strList.join(" ");
}

QString formatServicesStr(quint64 mask)
{
    QStringList strList;

    // Just scan the last 8 bits for now.
    for (int i = 0; i < 8; i++) {
        uint64_t check = 1 << i;
        if (mask & check) {
            switch (check) {
            case NODE_NETWORK:
                strList.append(QObject::tr("NETWORK"));
                break;
            case NODE_BLOOM:
            case NODE_BLOOM_WITHOUT_MN:
                strList.append(QObject::tr("BLOOM"));
                break;
            default:
                strList.append(QString("%1[%2]").arg(QObject::tr("UNKNOWN")).arg(check));
            }
        }
    }

    if (strList.size())
        return strList.join(" & ");
    else
        return QObject::tr("None");
}

QString formatPingTime(double dPingTime)
{
    return dPingTime == 0 ? QObject::tr("N/A") : QString(QObject::tr("%1 ms")).arg(QString::number((int)(dPingTime * 1000), 10));
}

QString formatTimeOffset(int64_t nTimeOffset)
{
  return QString(QObject::tr("%1 s")).arg(QString::number((int)nTimeOffset, 10));
}

} // namespace GUIUtil
