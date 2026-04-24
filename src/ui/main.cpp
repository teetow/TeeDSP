#include "MainWindow.h"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char *argv[])
{
    QCoreApplication::setOrganizationName(QStringLiteral("TeeDSP"));
    QCoreApplication::setOrganizationDomain(QStringLiteral("local.teedsp"));
    QCoreApplication::setApplicationName(QStringLiteral("TeeDSP"));

    QApplication app(argc, argv);
    MainWindow window;
    window.show();
    return app.exec();
}
