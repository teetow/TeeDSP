#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFont>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QSurfaceFormat>

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

    app.setStyleSheet(ui::theme::globalStylesheet());

    const QIcon appIcon = buildAppIcon();
    app.setWindowIcon(appIcon);

    MainWindow window;
    window.setWindowIcon(appIcon);
    window.show();
    return app.exec();
}
