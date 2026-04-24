#pragma once

#include <QObject>
#include <QString>

namespace dsp { class ApoSharedClient; }

class SystemOutputController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool active READ active NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString statusText READ statusText NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString detailText READ detailText NOTIFY statusChanged FINAL)
    Q_PROPERTY(QString errorText READ errorText NOTIFY statusChanged FINAL)

public:
    explicit SystemOutputController(dsp::ApoSharedClient *apoClient,
                                    QObject *parent = nullptr);

    bool active() const;
    QString statusText() const;
    QString detailText() const;
    QString errorText() const;

    Q_INVOKABLE bool installOrRepair();

signals:
    void statusChanged();

private:
    dsp::ApoSharedClient *m_apoClient = nullptr;
    QString m_actionError;
};
