// Copyright (c) 2011-2014 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2019 The PIVX developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "intro.h"
#include "ui_intro.h"

#include "fs.h"
#include "guiutil.h"

#include "util/system.h"
#include "qt/pivx/qtutils.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>

/* Minimum free space (in bytes) needed for data directory */
static const uint64_t GB_BYTES = 1000000000LL;
static const uint64_t BLOCK_CHAIN_SIZE = 1LL * GB_BYTES;

/* Check free space asynchronously to prevent hanging the UI thread.

   Up to one request to check a path is in flight to this thread; when the check()
   function runs, the current path is requested from the associated Intro object.
   The reply is sent back through a signal.

   This ensures that no queue of checking requests is built up while the user is
   still entering the path, and that always the most recently entered path is checked as
   soon as the thread becomes available.
*/
class FreespaceChecker : public QObject
{
    Q_OBJECT

public:
    FreespaceChecker(Intro* intro);

    enum Status {
        ST_OK,
        ST_ERROR
    };

public Q_SLOTS:
    void check();

Q_SIGNALS:
    void reply(int status, const QString& message, quint64 available);

private:
    Intro* intro;
};

#include "intro.moc"

FreespaceChecker::FreespaceChecker(Intro* intro)
{
    this->intro = intro;
}

void FreespaceChecker::check()
{
    QString dataDirStr = intro->getPathToCheck();
    fs::path dataDir = GUIUtil::qstringToBoostPath(dataDirStr);
    uint64_t freeBytesAvailable = 0;
    int replyStatus = ST_OK;
    QString replyMessage = tr("A new data directory will be created.");

    /* Find first parent that exists, so that fs::space does not fail */
    fs::path parentDir = dataDir;
    fs::path parentDirOld = fs::path();
    while (parentDir.has_parent_path() && !fs::exists(parentDir)) {
        parentDir = parentDir.parent_path();

        /* Check if we make any progress, break if not to prevent an infinite loop here */
        if (parentDirOld == parentDir)
            break;

        parentDirOld = parentDir;
    }

    try {
        freeBytesAvailable = fs::space(parentDir).available;
        if (fs::exists(dataDir)) {
            if (fs::is_directory(dataDir)) {
                QString separator = "<code>" + QDir::toNativeSeparators("/") + tr("name") + "</code>";
                replyStatus = ST_OK;
                replyMessage = tr("Directory already exists. Add %1 if you intend to create a new directory here.").arg(separator);
            } else {
                replyStatus = ST_ERROR;
                replyMessage = tr("Path already exists, and is not a directory.");
            }
        }
    } catch (const fs::filesystem_error& e) {
        /* Parent directory does not exist or is not accessible */
        replyStatus = ST_ERROR;
        replyMessage = tr("Cannot create data directory here.");
    }
    Q_EMIT reply(replyStatus, replyMessage, freeBytesAvailable);
}


Intro::Intro(QWidget* parent) : QDialog(parent, Qt::WindowSystemMenuHint | Qt::WindowTitleHint | Qt::WindowCloseButtonHint),
                                ui(new Ui::Intro),
                                thread(0),
                                signalled(false)
{
    ui->setupUi(this);
    this->setStyleSheet(GUIUtil::loadStyleSheet());

    setCssProperty(ui->frame, "container-welcome-step2");
    setCssProperty(ui->container, "container-welcome-stack");
    setCssProperty(ui->frame_2, "container-welcome");
    setCssProperty(ui->label_2, "text-title-welcome");
    setCssProperty(ui->label_4, "text-intro-white");
    setCssProperty(ui->sizeWarningLabel, "text-intro-white");
    setCssProperty(ui->freeSpace, "text-intro-white");
    setCssProperty(ui->errorMessage, "text-intro-white");

    setCssProperty({ui->dataDirDefault, ui->dataDirCustom}, "radio-welcome");
    setCssProperty(ui->dataDirectory, "edit-primary-welcome");
    ui->dataDirectory->setAttribute(Qt::WA_MacShowFocusRect, 0);
    setCssProperty(ui->ellipsisButton, "btn-dots-welcome");
    setCssBtnPrimary(ui->pushButtonOk);
    setCssBtnSecondary(ui->pushButtonCancel);

    connect(ui->pushButtonOk, &QPushButton::clicked, this, &Intro::accept);
    connect(ui->pushButtonCancel, &QPushButton::clicked, this, &Intro::close);

    ui->sizeWarningLabel->setText(ui->sizeWarningLabel->text().arg(BLOCK_CHAIN_SIZE / GB_BYTES));
    startThread();
}

Intro::~Intro()
{
    delete ui;
    /* Ensure thread is finished before it is deleted */
    Q_EMIT stopThread();
    thread->wait();
}

QString Intro::getDataDirectory()
{
    return ui->dataDirectory->text();
}

void Intro::setDataDirectory(const QString& dataDir)
{
    ui->dataDirectory->setText(dataDir);
    if (dataDir == getDefaultDataDirectory()) {
        ui->dataDirDefault->setChecked(true);
        ui->dataDirectory->setEnabled(false);
        ui->ellipsisButton->setEnabled(false);
        updateDataDirStatus(false);
    } else {
        ui->dataDirCustom->setChecked(true);
        ui->dataDirectory->setEnabled(true);
        ui->ellipsisButton->setEnabled(true);
        updateDataDirStatus(true);
    }
}

QString Intro::getDefaultDataDirectory()
{
    return GUIUtil::boostPathToQString(GetDefaultDataDir());
}

bool Intro::pickDataDirectory()
{
    QSettings settings;
    /* If data directory provided on command line, no need to look at settings
       or show a picking dialog */
    if (!gArgs.GetArg("-datadir", "").empty())
        return true;
    /* 1) Default data directory for operating system */
    QString dataDir = getDefaultDataDirectory();
    /* 2) Allow QSettings to override default dir */
    dataDir = settings.value("strDataDir", dataDir).toString();


    if (!fs::exists(GUIUtil::qstringToBoostPath(dataDir)) || gArgs.GetBoolArg("-choosedatadir", DEFAULT_CHOOSE_DATADIR)) {
        // If current default data directory does not exist, let the user choose one
        Intro intro;
        intro.setDataDirectory(dataDir);
        intro.setWindowIcon(QIcon(":icons/bitcoin"));

        while (true) {
            if (!intro.exec()) {
                // Cancel clicked
                return false;
            }
            dataDir = intro.getDataDirectory();
            try {
                if (TryCreateDirectories(GUIUtil::qstringToBoostPath(dataDir))) {
                    // If a new data directory has been created, make wallets subdirectory too
                    TryCreateDirectories(GUIUtil::qstringToBoostPath(dataDir) / "wallets");
                }
                break;
            } catch (const fs::filesystem_error& e) {
                QMessageBox::critical(0, tr("PIVX Core"),
                    tr("Error: Specified data directory \"%1\" cannot be created.").arg(dataDir));
                // fall through, back to choosing screen
            }
        }

        settings.setValue("strDataDir", dataDir);
    }

    /* Only override -datadir if different from the default, to make it possible to
     * override -datadir in the pivx.conf file in the default data directory
     * (to be consistent with pivxd behavior)
     */

    if (dataDir != getDefaultDataDirectory())
        gArgs.SoftSetArg("-datadir", GUIUtil::qstringToBoostPath(dataDir).string()); // use OS locale for path setting
    return true;
}

void Intro::setStatus(int status, const QString& message, quint64 bytesAvailable)
{
    switch (status) {
    case FreespaceChecker::ST_OK:
        ui->errorMessage->setText(message);
        ui->errorMessage->setStyleSheet("");
        break;
    case FreespaceChecker::ST_ERROR:
        ui->errorMessage->setText(tr("Error") + ": " + message);
        ui->errorMessage->setStyleSheet("QLabel { color: #f84444 }");
        break;
    }
    /* Indicate number of bytes available */
    if (status == FreespaceChecker::ST_ERROR) {
        ui->freeSpace->setText("");
    } else {
        QString freeString = tr("%1 GB of free space available").arg(bytesAvailable / GB_BYTES);
        if (bytesAvailable < BLOCK_CHAIN_SIZE) {
            freeString += " " + tr("(of %1 GB needed)").arg(BLOCK_CHAIN_SIZE / GB_BYTES);
            ui->freeSpace->setStyleSheet("QLabel { color: #800000 }");
        } else {
            ui->freeSpace->setStyleSheet("");
        }
        ui->freeSpace->setText(freeString + ".");
    }
    /* Don't allow confirm in ERROR state */
    ui->pushButtonOk->setEnabled(status != FreespaceChecker::ST_ERROR);
}

void Intro::updateDataDirStatus(bool enabled){
    if(enabled){
        setCssProperty(ui->dataDirectory, "edit-primary-welcome", true);
    } else {
        setCssProperty(ui->dataDirectory, "edit-primary-welcome-disabled", true);

    }
}

void Intro::on_dataDirectory_textChanged(const QString& dataDirStr)
{
    /* Disable OK button until check result comes in */
    ui->pushButtonOk->setEnabled(false);
    checkPath(dataDirStr);
}

void Intro::on_ellipsisButton_clicked()
{
    QString dir = QDir::toNativeSeparators(QFileDialog::getExistingDirectory(0, "Choose data directory", ui->dataDirectory->text()));
    if (!dir.isEmpty())
        ui->dataDirectory->setText(dir);
}

void Intro::on_dataDirDefault_clicked()
{
    setDataDirectory(getDefaultDataDirectory());
    updateDataDirStatus(false);
}

void Intro::on_dataDirCustom_clicked()
{
    ui->dataDirectory->setEnabled(true);
    ui->ellipsisButton->setEnabled(true);
    updateDataDirStatus(true);
}

void Intro::startThread()
{
    thread = new QThread(this);
    FreespaceChecker* executor = new FreespaceChecker(this);
    executor->moveToThread(thread);

    connect(executor, &FreespaceChecker::reply, this, &Intro::setStatus);
    connect(this, &Intro::requestCheck, executor, &FreespaceChecker::check);
    /*  make sure executor object is deleted in its own thread */
    connect(this, &Intro::stopThread, executor, &QObject::deleteLater);
    connect(this, &Intro::stopThread, thread, &QThread::quit);

    thread->start();
}

void Intro::checkPath(const QString& dataDir)
{
    mutex.lock();
    pathToCheck = dataDir;
    if (!signalled) {
        signalled = true;
        Q_EMIT requestCheck();
    }
    mutex.unlock();
}

QString Intro::getPathToCheck()
{
    QString retval;
    mutex.lock();
    retval = pathToCheck;
    signalled = false; /* new request can be queued now */
    mutex.unlock();
    return retval;
}
