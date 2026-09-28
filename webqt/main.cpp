#include "ui/MainWindow.h"
#include <QApplication>
#include <QFile>
#include <QStyleFactory>
int main(int argc,char **argv) {
    QApplication app(argc,argv);
    app.setOrganizationName("TeeDSP");app.setApplicationName("TeeDSP Web");
    app.setStyle(QStyleFactory::create("Fusion"));
    QFile qss(":/theme.qss");if(qss.open(QIODevice::ReadOnly))app.setStyleSheet(QString::fromUtf8(qss.readAll()));
    MainWindow window;
    window.showFullScreen();
    return app.exec();
}
