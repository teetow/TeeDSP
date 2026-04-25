#include "Theme.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>

namespace ui::theme {

namespace {

QString readTextFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll());
}

} // namespace

QString stylesheetPath()
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

QString globalStylesheet()
{
    const QString path = stylesheetPath();
    if (path.isEmpty()) return {};
    return readTextFile(path);
}

} // namespace ui::theme
