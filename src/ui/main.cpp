#include "MainWindow.h"
#include "Theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QPalette>

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationName(QStringLiteral("TeeDSP"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("local.teedsp"));
    QCoreApplication::setApplicationName(QStringLiteral("TeeDSP"));

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

    MainWindow window;
    window.show();
    return app.exec();
}
