#include "StartupRegistration.h"

#include <QCoreApplication>
#include <QDir>
#include <QSettings>

namespace ui::startup {

namespace {
constexpr const char *kRunKey  = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const char *kAppName = "TeeDsp";

QString quotedExePath(const QString &exePath)
{
    const QString p = exePath.isEmpty()
        ? QCoreApplication::applicationFilePath()
        : exePath;
    return QStringLiteral("\"") + QDir::toNativeSeparators(p) + QStringLiteral("\"");
}
}

bool isEnabled()
{
    QSettings s(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
    const QString val = s.value(QString::fromLatin1(kAppName)).toString();
    return !val.isEmpty();
}

bool setEnabled(bool enabled, const QString &exePath)
{
    QSettings s(QString::fromLatin1(kRunKey), QSettings::NativeFormat);
    if (enabled) {
        s.setValue(QString::fromLatin1(kAppName), quotedExePath(exePath));
    } else {
        s.remove(QString::fromLatin1(kAppName));
    }
    s.sync();
    return s.status() == QSettings::NoError;
}

} // namespace ui::startup
