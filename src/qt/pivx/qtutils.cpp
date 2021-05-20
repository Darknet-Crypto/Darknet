// Copyright (c) 2019-2020 The PIVX developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "qt/pivx/qtutils.h"

#include "guiconstants.h"
#include "qrencode.h"
#include "qt/pivx/snackbar.h"

#include <QFile>
#include <QGraphicsDropShadowEffect>
#include <QListView>

Qt::Modifier SHORT_KEY
#ifdef Q_OS_MAC
    = Qt::CTRL;
#else
    = Qt::ALT;
#endif

// Open dialog at the bottom
bool openDialog(QDialog* widget, QWidget* gui)
{
    widget->setWindowFlags(Qt::CustomizeWindowHint);
    widget->setAttribute(Qt::WA_TranslucentBackground, true);
    QPropertyAnimation* animation = new QPropertyAnimation(widget, "pos");
    animation->setDuration(300);
    animation->setStartValue(QPoint(0, gui->height()));
    animation->setEndValue(QPoint(0, gui->height() - widget->height()));
    animation->setEasingCurve(QEasingCurve::OutQuad);
    animation->start(QAbstractAnimation::DeleteWhenStopped);
    widget->activateWindow();
    widget->raise();
    return widget->exec();
}

void closeDialog(QDialog* widget, PIVXGUI* gui)
{
    widget->setWindowFlags(Qt::CustomizeWindowHint);
    widget->setAttribute(Qt::WA_TranslucentBackground, true);
    QPropertyAnimation* animation = new QPropertyAnimation(widget, "pos");
    animation->setDuration(300);
    animation->setStartValue(widget->pos());
    animation->setEndValue(QPoint(0, gui->height() + 100));
    animation->setEasingCurve(QEasingCurve::OutQuad);
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void openDialogFullScreen(QWidget* parent, QWidget* dialog)
{
    dialog->setWindowFlags(Qt::CustomizeWindowHint);
    dialog->move(0, 0);
    dialog->show();
    dialog->activateWindow();
    dialog->resize(parent->width(), parent->height());
}

bool openDialogWithOpaqueBackgroundY(QDialog* widget, PIVXGUI* gui, double posX, int posY, bool hideOpaqueBackground)
{
    widget->setWindowFlags(Qt::CustomizeWindowHint);
    widget->setAttribute(Qt::WA_TranslucentBackground, true);
    QPropertyAnimation* animation = new QPropertyAnimation(widget, "pos");
    animation->setDuration(300);
    int xPos = gui->width() / posX;
    animation->setStartValue(QPoint(xPos, gui->height()));
    animation->setEndValue(QPoint(xPos, gui->height() / posY)); //- (gui->height()) / posY  ));
    animation->setEasingCurve(QEasingCurve::OutQuad);
    animation->start(QAbstractAnimation::DeleteWhenStopped);
    widget->activateWindow();
    bool res = widget->exec();
    if (hideOpaqueBackground) gui->showHide(false);
    return res;
}

bool openDialogWithOpaqueBackground(QDialog* widget, PIVXGUI* gui, double posX)
{
    return openDialogWithOpaqueBackgroundY(widget, gui, posX, 5);
}

bool openDialogWithOpaqueBackgroundFullScreen(QDialog* widget, PIVXGUI* gui)
{
    widget->setWindowFlags(Qt::CustomizeWindowHint);
    widget->setAttribute(Qt::WA_TranslucentBackground, true);

    widget->activateWindow();
    widget->resize(gui->width(), gui->height());

    QPropertyAnimation* animation = new QPropertyAnimation(widget, "pos");
    animation->setDuration(300);
    int xPos = 0;
    animation->setStartValue(QPoint(xPos, gui->height()));
    animation->setEndValue(QPoint(xPos, 0));
    animation->setEasingCurve(QEasingCurve::OutQuad);
    animation->start(QAbstractAnimation::DeleteWhenStopped);
    widget->activateWindow();
    bool res = widget->exec();
    gui->showHide(false);
    return res;
}

QPixmap encodeToQr(const QString& str, QString& errorStr, const QColor& qrColor)
{
    if (!str.isEmpty()) {
        // limit URI length
        if (str.length() > MAX_URI_LENGTH) {
            errorStr = "Resulting URI too long, try to reduce the text for label / message.";
            return QPixmap();
        } else {
            QRcode* code = QRcode_encodeString(str.toUtf8().constData(), 0, QR_ECLEVEL_L, QR_MODE_8, 1);
            if (!code) {
                errorStr = "Error encoding URI into QR Code.";
                return QPixmap();
            }
            QImage myImage = QImage(code->width + 2, code->width + 2, QImage::Format_RGB32);
            myImage.fill(0xffffff);
            unsigned char* p = code->data;
            for (int y = 0; y < code->width; y++) {
                for (int x = 0; x < code->width; x++) {
                    myImage.setPixel(x + 1, y + 1, ((*p & 1) ? qrColor.rgb() : 0xffffff));
                    p++;
                }
            }
            QRcode_free(code);
            return QPixmap::fromImage(myImage);
        }
    }
    return QPixmap();
}

void setFilterAddressBook(QComboBox* filter, SortEdit* lineEdit)
{
    initComboBox(filter, lineEdit);
    filter->addItem(QObject::tr("All"), "");
    filter->addItem(QObject::tr("Receiving"), AddressTableModel::Receive);
    filter->addItem(QObject::tr("Contacts"), AddressTableModel::Send);
    filter->addItem(QObject::tr("Cold Staking"), AddressTableModel::ColdStaking);
    filter->addItem(QObject::tr("Delegator"), AddressTableModel::Delegator);
    filter->addItem(QObject::tr("Delegable"), AddressTableModel::Delegable);
    filter->addItem(QObject::tr("Staking Contacts"), AddressTableModel::ColdStakingSend);
    filter->addItem(QObject::tr("Shielded Recv"), AddressTableModel::ShieldedReceive);
    filter->addItem(QObject::tr("Shielded Contact"), AddressTableModel::ShieldedSend);
}

void setSortTx(QComboBox* filter, SortEdit* lineEdit)
{
    // Sort Transactions
    initComboBox(filter, lineEdit);
    filter->addItem(QObject::tr("Date desc"), SortTx::DATE_DESC);
    filter->addItem(QObject::tr("Date asc"), SortTx::DATE_ASC);
    filter->addItem(QObject::tr("Amount desc"), SortTx::AMOUNT_ASC);
    filter->addItem(QObject::tr("Amount asc"), SortTx::AMOUNT_DESC);
}

void setSortTxTypeFilter(QComboBox* filter, SortEdit* lineEditType)
{
    initComboBox(filter, lineEditType);
    filter->addItem(QObject::tr("All"), TransactionFilterProxy::ALL_TYPES);
    filter->addItem(QObject::tr("Received"),
                    TransactionFilterProxy::TYPE(TransactionRecord::RecvWithAddress) |
                    TransactionFilterProxy::TYPE(TransactionRecord::RecvFromOther) |
                    TransactionFilterProxy::TYPE(TransactionRecord::RecvWithShieldedAddress));
    filter->addItem(QObject::tr("Sent"),
                    TransactionFilterProxy::TYPE(TransactionRecord::SendToAddress) |
                    TransactionFilterProxy::TYPE(TransactionRecord::SendToOther) |
                    TransactionFilterProxy::TYPE(TransactionRecord::SendToShielded) |
                    TransactionFilterProxy::TYPE(TransactionRecord::SendToNobody));
    filter->addItem(QObject::tr("Shield"),
                    TransactionFilterProxy::TYPE(TransactionRecord::RecvWithShieldedAddress) |
                    TransactionFilterProxy::TYPE(TransactionRecord::SendToShielded) |
                    TransactionFilterProxy::TYPE(TransactionRecord::SendToSelfShieldToShieldChangeAddress) |
                    TransactionFilterProxy::TYPE(TransactionRecord::SendToSelfShieldToTransparent) |
                    TransactionFilterProxy::TYPE(TransactionRecord::SendToSelfShieldedAddress));
    filter->addItem(QObject::tr("Mined"), TransactionFilterProxy::TYPE(TransactionRecord::Generated));
    filter->addItem(QObject::tr("Minted"), TransactionFilterProxy::TYPE(TransactionRecord::StakeMint));
    filter->addItem(QObject::tr("MN reward"), TransactionFilterProxy::TYPE(TransactionRecord::MNReward));
    filter->addItem(QObject::tr("To yourself"), TransactionFilterProxy::TYPE(TransactionRecord::SendToSelf) |
                                            TransactionFilterProxy::TYPE(TransactionRecord::SendToSelfShieldedAddress) |
                                            TransactionFilterProxy::TYPE(TransactionRecord::SendToSelfShieldToShieldChangeAddress) |
                                            TransactionFilterProxy::TYPE(TransactionRecord::SendToSelfShieldToTransparent));
    filter->addItem(QObject::tr("Cold stakes"), TransactionFilterProxy::TYPE(TransactionRecord::StakeDelegated));
    filter->addItem(QObject::tr("Hot stakes"), TransactionFilterProxy::TYPE(TransactionRecord::StakeHot));
    filter->addItem(QObject::tr("Delegated"), TransactionFilterProxy::TYPE(TransactionRecord::P2CSDelegationSent) | TransactionFilterProxy::TYPE(TransactionRecord::P2CSDelegationSentOwner));
    filter->addItem(QObject::tr("Delegations"), TransactionFilterProxy::TYPE(TransactionRecord::P2CSDelegation));
}

void setupSettings(QSettings* settings)
{
    if (!settings->contains("lightTheme")) {
        settings->setValue("lightTheme", true);
    }
}

QSettings* settings = nullptr;

QSettings* getSettings()
{
    if (!settings) {
        settings = new QSettings();
        // Setup initial values if them are not there
        setupSettings(settings);
    }

    return settings;
}

bool isLightTheme()
{
    return getSettings()->value("lightTheme", true).toBool();
}

void setTheme(bool isLight)
{
    QSettings* settings = getSettings();
    settings->setValue("theme", isLight ? "default" : "default-dark");
    settings->setValue("lightTheme", isLight);
}


// Style

void updateStyle(QWidget* widget)
{
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}


QColor getRowColor(bool isLightTheme, bool isHovered, bool isSelected)
{
    if (isSelected) {
        return QColor("#25b088ff");
    } else if (isHovered) {
        return QColor("#25bababa");
    } else {
        return isLightTheme ? QColor("#ffffff") : QColor("#0f0b16");
    }
}

void initComboBox(QComboBox* combo, QLineEdit* lineEdit, QString cssClass)
{
    setCssProperty(combo, std::move(cssClass));
    combo->setEditable(true);
    if (lineEdit) {
        lineEdit->setReadOnly(true);
        lineEdit->setAlignment(Qt::AlignRight);
        combo->setLineEdit(lineEdit);
    }
    combo->setStyleSheet("selection-background-color:transparent;");
    combo->setView(new QListView());
}

void fillAddressSortControls(SortEdit* seType, SortEdit* seOrder, QComboBox* boxType, QComboBox* boxOrder)
{
    // Sort Type
    initComboBox(boxType, seType, "btn-combo-small");
    boxType->addItem(QObject::tr("by Label"), AddressTableModel::Label);
    boxType->addItem(QObject::tr("by Address"), AddressTableModel::Address);
    boxType->addItem(QObject::tr("by Date"), AddressTableModel::Date);
    boxType->setCurrentIndex(0);
    // Sort Order
    initComboBox(boxOrder, seOrder, "btn-combo-small");
    boxOrder->addItem("asc", Qt::AscendingOrder);
    boxOrder->addItem("desc", Qt::DescendingOrder);
    boxOrder->setCurrentIndex(0);
}

void initCssEditLine(QLineEdit* edit, bool isDialog)
{
    if (isDialog)
        setCssEditLineDialog(edit, true, false);
    else
        setCssEditLine(edit, true, false);
    setShadow(edit);
    edit->setAttribute(Qt::WA_MacShowFocusRect, false);
}

void setCssEditLine(QLineEdit* edit, bool isValid, bool forceUpdate)
{
    setCssProperty(edit, isValid ? "edit-primary" : "edit-primary-error", forceUpdate);
}

void setCssEditLineDialog(QLineEdit* edit, bool isValid, bool forceUpdate)
{
    setCssProperty(edit, isValid ? "edit-primary-dialog" : "edit-primary-dialog-error", forceUpdate);
}

void setShadow(QWidget* edit)
{
    QGraphicsDropShadowEffect* shadowEffect = new QGraphicsDropShadowEffect();
    shadowEffect->setColor(QColor(0, 0, 0, 22));
    shadowEffect->setXOffset(0);
    shadowEffect->setYOffset(3);
    shadowEffect->setBlurRadius(6);
    edit->setGraphicsEffect(shadowEffect);
}

void setCssBtnPrimary(QPushButton* btn, bool forceUpdate)
{
    setCssProperty(btn, "btn-primary", forceUpdate);
}

void setCssBtnSecondary(QPushButton* btn, bool forceUpdate)
{
    setCssProperty(btn, "btn-secundary", forceUpdate);
}

void setCssTextBodyDialog(std::initializer_list<QWidget*> args)
{
    for (QWidget* w : args) {
        setCssTextBodyDialog(w);
    }
}

void setCssTextBodyDialog(QWidget* widget)
{
    setCssProperty(widget, "text-body1-dialog", false);
}

void setCssTitleScreen(QLabel* label)
{
    setCssProperty(label, "text-title-screen", false);
}

void setCssSubtitleScreen(QWidget* wid)
{
    setCssProperty(wid, "text-subtitle", false);
}

void setCssProperty(std::initializer_list<QWidget*> args, const QString& value)
{
    for (QWidget* w : args) {
        setCssProperty(w, value);
    }
}

void setCssProperty(QWidget* wid, const QString& value, bool forceUpdate)
{
    wid->setProperty("cssClass", value);
    forceUpdateStyle(wid, forceUpdate);
}

void forceUpdateStyle(QWidget* widget, bool forceUpdate)
{
    if (forceUpdate)
        updateStyle(widget);
}

void forceUpdateStyle(std::initializer_list<QWidget*> args)
{
    for (QWidget* w : args) {
        forceUpdateStyle(w, true);
    }
}
