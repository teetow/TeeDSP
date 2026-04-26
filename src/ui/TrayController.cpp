#include "TrayController.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QIcon>
#include <QMainWindow>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QStyle>
#include <QSystemTrayIcon>

namespace ui {

namespace {
QIcon resolveBaseTrayIcon(QMainWindow *window)
{
    if (window && !window->windowIcon().isNull()) return window->windowIcon();
    if (!QApplication::windowIcon().isNull()) return QApplication::windowIcon();
    return QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
}

QPixmap toGrayscale(const QPixmap &src)
{
    if (src.isNull()) return src;
    QImage img = src.toImage().convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < img.height(); ++y) {
        QRgb *row = reinterpret_cast<QRgb *>(img.scanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const int a = qAlpha(row[x]);
            const int gray = qGray(row[x]);
            row[x] = qRgba(gray, gray, gray, a);
        }
    }
    return QPixmap::fromImage(img);
}

QIcon trayIconForState(const QIcon &baseIcon, bool running)
{
    QIcon out;
    for (const int size : {16, 24, 32}) {
        QPixmap pm = baseIcon.pixmap(size, size);
        if (pm.isNull()) {
            pm = QPixmap(size, size);
            pm.fill(Qt::transparent);
        }

        out.addPixmap(running ? pm : toGrayscale(pm));
    }
    return out;
}
} // namespace

TrayController::TrayController(QMainWindow *window, QObject *parent)
    : QObject(parent)
    , m_window(window)
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        // No tray — degrade gracefully; the rest of the app still works.
        return;
    }

    m_tray = new QSystemTrayIcon(this);
    m_tray->setIcon(trayIconForState(resolveBaseTrayIcon(m_window), false));
    m_tray->setToolTip(QStringLiteral("TeeDSP"));

    m_menu = new QMenu();

    m_showAction = m_menu->addAction(QStringLiteral("&Show TeeDSP"));
    connect(m_showAction, &QAction::triggered, this, &TrayController::onShowToggle);

    m_startStopAction = m_menu->addAction(QStringLiteral("&Start"));
    connect(m_startStopAction, &QAction::triggered, this, [this]() {
        emit startStopRequested();
    });

    m_menu->addSeparator();

    m_inputMenu = m_menu->addMenu(QStringLiteral("&Input"));
    m_outputMenu = m_menu->addMenu(QStringLiteral("&Output"));

    m_menu->addSeparator();

    m_bypassAction = m_menu->addAction(QStringLiteral("&Bypass DSP"));
    m_bypassAction->setCheckable(true);
    connect(m_bypassAction, &QAction::toggled, this, &TrayController::onBypass);

    m_startWithWindowsAction = m_menu->addAction(QStringLiteral("Start &with Windows"));
    m_startWithWindowsAction->setCheckable(true);
    connect(m_startWithWindowsAction, &QAction::toggled, this, &TrayController::onStartWithWindows);

    m_keepInjectedAction = m_menu->addAction(QStringLiteral("Keep TeeDSP &injected"));
    m_keepInjectedAction->setCheckable(true);
    m_keepInjectedAction->setToolTip(QStringLiteral(
        "When Windows changes the default output (e.g. Bluetooth headphones connecting), "
        "automatically restore TeeDSP as the system output so audio keeps flowing through DSP."));
    connect(m_keepInjectedAction, &QAction::toggled, this, &TrayController::onKeepInjected);

    m_menu->addSeparator();

    m_quitAction = m_menu->addAction(QStringLiteral("&Quit TeeDSP"));
    connect(m_quitAction, &QAction::triggered, this, &TrayController::onQuit);

    m_menu->addSeparator();

    // Use the executable's mtime so the timestamp reflects the actual binary
    // we're running, not just when this .cpp last compiled.
    const QFileInfo appBinary(QCoreApplication::applicationFilePath());
    const QString buildStamp = appBinary.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    QAction *buildInfo = m_menu->addAction(QStringLiteral("Build %1").arg(buildStamp));
    buildInfo->setEnabled(false);

    m_tray->setContextMenu(m_menu);
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason r) { onActivated(static_cast<int>(r)); });

    m_tray->show();
}

void TrayController::setStatusText(const QString &text)
{
    if (m_tray) m_tray->setToolTip(text);
}

void TrayController::setRunning(bool running)
{
    if (m_startStopAction) {
        m_startStopAction->setText(running ? QStringLiteral("Sto&p") : QStringLiteral("&Start"));
    }
    if (m_tray) m_tray->setIcon(trayIconForState(resolveBaseTrayIcon(m_window), running));
}

void TrayController::setBypass(bool bypass)
{
    if (m_bypassAction) {
        QSignalBlocker block(m_bypassAction);
        m_bypassAction->setChecked(bypass);
    }
}

void TrayController::setStartWithWindows(bool on)
{
    if (m_startWithWindowsAction) {
        QSignalBlocker block(m_startWithWindowsAction);
        m_startWithWindowsAction->setChecked(on);
    }
}

void TrayController::setKeepInjected(bool on)
{
    if (m_keepInjectedAction) {
        QSignalBlocker block(m_keepInjectedAction);
        m_keepInjectedAction->setChecked(on);
    }
}

void TrayController::setRoutingOptions(const QList<DeviceChoice> &inputs,
                                       const QString &selectedInputId,
                                       const QList<DeviceChoice> &outputs,
                                       const QString &selectedOutputId)
{
    const auto populate = [this](QMenu *menu,
                                 const QList<DeviceChoice> &items,
                                 const QString &selectedId,
                                 bool isInput) {
        if (!menu) return;
        menu->clear();

        if (items.isEmpty()) {
            QAction *none = menu->addAction(QStringLiteral("No devices"));
            none->setEnabled(false);
            return;
        }

        auto *group = new QActionGroup(menu);
        group->setExclusive(true);
        for (const auto &item : items) {
            QAction *a = menu->addAction(item.name);
            a->setCheckable(true);
            a->setData(item.id);
            a->setChecked(item.id == selectedId);
            group->addAction(a);
            connect(a, &QAction::triggered, this, [this, a, isInput](bool checked) {
                if (!checked) return;
                const QString id = a->data().toString();
                if (isInput) emit inputDeviceSelected(id);
                else emit outputDeviceSelected(id);
            });
        }
    };

    populate(m_inputMenu, inputs, selectedInputId, true);
    populate(m_outputMenu, outputs, selectedOutputId, false);
}

void TrayController::onActivated(int reason)
{
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        onShowToggle();
    }
}

void TrayController::onShowToggle()
{
    if (!m_window) return;
    if (m_window->isVisible() && !m_window->isMinimized()) {
        m_window->hide();
    } else {
        m_window->showNormal();
        m_window->raise();
        m_window->activateWindow();
    }
}

void TrayController::onBypass(bool b)               { emit bypassToggled(b); }
void TrayController::onStartWithWindows(bool b)     { emit startWithWindowsToggled(b); }
void TrayController::onKeepInjected(bool b)         { emit keepInjectedToggled(b); }
void TrayController::onQuit()                       { emit quitRequested(); }

} // namespace ui
