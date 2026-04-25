#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFont>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QSurfaceFormat>

#include <memory>

namespace {

QPixmap renderAppIcon(int size)
{
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    const qreal r = size * 0.18;
    QPainterPath body;
    body.addRoundedRect(QRectF(0, 0, size, size), r, r);
    p.fillPath(body, ui::theme::kBgPanel);

    p.setPen(QPen(ui::theme::kAccent, std::max(1.0, size / 32.0)));
    p.drawPath(body);

    QFont f;
    f.setFamily(QStringLiteral("Segoe UI"));
    f.setBold(true);
    f.setPointSizeF(size * 0.55);
    p.setFont(f);
    p.setPen(ui::theme::kAccent);
    p.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, QStringLiteral("T"));
    return pm;
}

QIcon buildAppIcon()
{
    QIcon icon;
    for (int s : {16, 24, 32, 48, 64, 128, 256}) {
        icon.addPixmap(renderAppIcon(s));
    }
    return icon;
}

QString readTextFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll());
}

QString resolveStylesheetPath()
{
    const QByteArray envPath = qgetenv("TEEDSP_QSS_PATH");
    if (!envPath.isEmpty()) {
        const QString p = QString::fromLocal8Bit(envPath);
        if (QFileInfo::exists(p)) return p;
    }

#ifdef TEEDSP_SOURCE_DIR
    const QString sourcePath =
        QString::fromUtf8(TEEDSP_SOURCE_DIR) + QStringLiteral("/src/ui/theme.qss");
    if (QFileInfo::exists(sourcePath)) return sourcePath;
#endif

    const QString appLocalPath =
        QCoreApplication::applicationDirPath() + QStringLiteral("/theme.qss");
    if (QFileInfo::exists(appLocalPath)) return appLocalPath;

    return {};
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationName(QStringLiteral("TeeDSP"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("local.teedsp"));
    QCoreApplication::setApplicationName(QStringLiteral("TeeDSP"));

    // Configure the GL surface used by every QOpenGLWidget in the app.
    // SwapInterval 1 = present on vsync, which is what gates EqCurve's repaint
    // rate to the display refresh. 4× MSAA keeps curves smooth without a
    // separate antialiasing pass.
    {
        QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
        fmt.setSwapInterval(1);
        fmt.setSamples(4);
        QSurfaceFormat::setDefaultFormat(fmt);
    }

    QApplication app(argc, argv);

    // Apply dark palette so anything that bypasses the stylesheet still reads dark.
    QPalette pal = app.palette();
    pal.setColor(QPalette::Window,         ui::theme::kBgDeep);
    pal.setColor(QPalette::WindowText,     ui::theme::kTextPrimary);
    pal.setColor(QPalette::Base,           ui::theme::kBgSunken);
    pal.setColor(QPalette::AlternateBase,  ui::theme::kBgPanel);
    pal.setColor(QPalette::ToolTipBase,    ui::theme::kBgPanel);
    pal.setColor(QPalette::ToolTipText,    ui::theme::kTextPrimary);
    pal.setColor(QPalette::Text,           ui::theme::kTextPrimary);
    pal.setColor(QPalette::Button,         ui::theme::kBgPanel);
    pal.setColor(QPalette::ButtonText,     ui::theme::kTextPrimary);
    pal.setColor(QPalette::BrightText,     ui::theme::kAccent);
    pal.setColor(QPalette::Link,           ui::theme::kAccent);
    pal.setColor(QPalette::Highlight,      ui::theme::kAccent);
    pal.setColor(QPalette::HighlightedText,ui::theme::kBgDeep);
    app.setPalette(pal);

    const QString stylesheetPath = resolveStylesheetPath();
    const auto applyStylesheet = [&app, &stylesheetPath]() {
        QString css;
        if (!stylesheetPath.isEmpty())
            css = readTextFile(stylesheetPath);
        if (css.isEmpty())
            css = ui::theme::globalStylesheet();
        app.setStyleSheet(css);
    };

    applyStylesheet();

    std::unique_ptr<QFileSystemWatcher> styleWatcher;
    if (!stylesheetPath.isEmpty()) {
        styleWatcher = std::make_unique<QFileSystemWatcher>();
        styleWatcher->addPath(stylesheetPath);
        QFileSystemWatcher *watcher = styleWatcher.get();
        QObject::connect(watcher, &QFileSystemWatcher::fileChanged,
                         &app, [watcher, &applyStylesheet, stylesheetPath](const QString &) {
            applyStylesheet();
            // Some editors replace the file atomically, which drops the watch.
            if (QFileInfo::exists(stylesheetPath) && !watcher->files().contains(stylesheetPath)) {
                watcher->addPath(stylesheetPath);
            }
        });
    }

    const QIcon appIcon = buildAppIcon();
    app.setWindowIcon(appIcon);

    MainWindow window;
    window.setWindowIcon(appIcon);
    window.show();
    return app.exec();
}
