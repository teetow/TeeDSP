#include "ApoBindingStatus.h"

#include <QHash>
#include <QProcess>
#include <QSettings>
#include <QStringList>

namespace host {

ApoBindingInfo queryApoBinding(const QString &renderDeviceId)
{
    ApoBindingInfo info;
    if (renderDeviceId.isEmpty())
        return info;

    // deviceId looks like "{0.0.0.00000000}.{<endpoint-guid>}" — the registry
    // key is filed under just the trailing guid.
    const int dot = renderDeviceId.lastIndexOf(QLatin1Char('.'));
    const QString guid = (dot >= 0) ? renderDeviceId.mid(dot + 1) : renderDeviceId;
    const QString base =
        QStringLiteral("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion"
                       "\\MMDevices\\Audio\\Render\\") + guid;

    QSettings fx(base + QStringLiteral("\\FxProperties"), QSettings::NativeFormat);
    const QString teeDspClsid = QStringLiteral("{B7E1A0C0-7E5D-4D8B-9E2A-1C4F8D3A2B11}");
    const QString pkeyBase = QStringLiteral("{d04e05a6-594b-4fb6-a80d-01af5eed7d1d}");
    const auto isTeeDsp = [&](const QString &slot) {
        return fx.value(pkeyBase + slot).toString().compare(teeDspClsid, Qt::CaseInsensitive) == 0;
    };

    if (isTeeDsp(QStringLiteral(",14")))      info = {true, QStringLiteral("MFX (composite)")};
    else if (isTeeDsp(QStringLiteral(",6")))  info = {true, QStringLiteral("MFX")};
    else if (isTeeDsp(QStringLiteral(",5")))  info = {true, QStringLiteral("SFX")};

    return info;
}

namespace {
QString friendlyPackageLabel(const QString &originalInfName)
{
    static const QHash<QString, QString> kKnownLabels = {
        {QStringLiteral("teedspapocomponent.inf"), QStringLiteral("TeeDSP APO component")},
        {QStringLiteral("teedsprealtekextension.inf"), QStringLiteral("Realtek analog-out extension")},
        {QStringLiteral("teedspairpodsextension.inf"), QStringLiteral("AirPods Pro extension")},
    };
    const auto it = kKnownLabels.constFind(originalInfName.toLower());
    // Unrecognized teedsp*.inf names (e.g. a leftover extension for a device
    // no longer in apo/driver/) fall back to the raw filename rather than
    // being silently dropped — that mismatch is exactly what this is for.
    return it != kKnownLabels.constEnd() ? it.value() : originalInfName;
}

QString packagePurpose(const QString &originalInfName)
{
    if (originalInfName.compare(QStringLiteral("teedspapocomponent.inf"),
                                Qt::CaseInsensitive) == 0)
        return QStringLiteral("DSP DLL (single loaded component)");
    if (originalInfName.endsWith(QStringLiteral("extension.inf"),
                                 Qt::CaseInsensitive))
        return QStringLiteral("Device binding only");
    return QStringLiteral("Unknown TeeDSP package");
}
} // namespace

QList<InstalledApoPackage> queryInstalledApoPackages()
{
    QList<InstalledApoPackage> packages;

    // Read-only enumeration — unlike /add-driver or /delete-driver, this does
    // not require elevation.
    QProcess proc;
    proc.start(QStringLiteral("pnputil"), {QStringLiteral("/enum-drivers")});
    if (!proc.waitForFinished(5000))
        return packages;

    const QStringList lines =
        QString::fromLocal8Bit(proc.readAllStandardOutput()).split(QLatin1Char('\n'));

    // Fields for a package arrive as "Published Name:" then "Original Name:"
    // then (a few lines later) "Driver Version:" — finalize on the last one
    // so both preceding fields are already buffered.
    QString currentPublished;
    QString currentOriginal;
    for (const QString &rawLine : lines) {
        const QString line = rawLine.trimmed();
        if (line.startsWith(QStringLiteral("Published Name:"))) {
            currentPublished = line.section(QLatin1Char(':'), 1).trimmed();
            currentOriginal.clear();
        } else if (line.startsWith(QStringLiteral("Original Name:"))) {
            currentOriginal = line.section(QLatin1Char(':'), 1).trimmed();
        } else if (line.startsWith(QStringLiteral("Driver Version:"))) {
            const QString version = line.section(QLatin1Char(':'), 1).trimmed();
            if (!currentOriginal.startsWith(QStringLiteral("teedsp"), Qt::CaseInsensitive))
                continue;
            InstalledApoPackage pkg;
            pkg.label = friendlyPackageLabel(currentOriginal);
            pkg.purpose = packagePurpose(currentOriginal);
            pkg.originalInfName = currentOriginal;
            pkg.publishedName = currentPublished;
            pkg.driverVersion = version;
            packages.append(pkg);
        }
    }
    return packages;
}

} // namespace host
