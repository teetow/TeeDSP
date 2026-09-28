#include "Client.h"
#include "Params.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkReply>
#include <QUrl>
#ifdef __EMSCRIPTEN__
#include <emscripten/val.h>
#endif

namespace remote {
Client::Client(QObject *parent):QObject(parent) {
    m_meterTimer.setInterval(50);
    connect(&m_meterTimer,&QTimer::timeout,this,[this] {
        sendPending();sendVolume();
        if (m_gettingMeters) return;
        m_gettingMeters=true;
        request("meters",nullptr,[this](bool ok,QJsonObject o) {
            m_gettingMeters=false;
            if (!ok) return;
            meters=o; m_lastMeter.restart(); emit metersReceived();
        });
    });
    m_stateTimer.setInterval(500);
    connect(&m_stateTimer,&QTimer::timeout,this,[this] {
        pollParams();
        if (m_gettingVolume || m_postingVolume || !m_pendingVolume.isEmpty()) return;
        m_gettingVolume=true;
        const auto generation=m_volumeGeneration;
        request("volume",nullptr,[this,generation](bool ok,QJsonObject o) {
            m_gettingVolume=false;
            if (ok && generation==m_volumeGeneration) {volume=o;emit volumeReceived();}
        });
    });
}
void Client::start() { pollParams();m_meterTimer.start();m_stateTimer.start(); }
void Client::request(const QString &path,const QJsonObject *patch,std::function<void(bool,QJsonObject)> done) {
#ifdef __EMSCRIPTEN__
    const auto origin=QString::fromStdString(emscripten::val::global("location")["origin"].as<std::string>());
#else
    const auto origin=qEnvironmentVariable("TEEDSP_URL","http://cm3588.lan:8790");
#endif
    QNetworkRequest req(QUrl(origin+"/api/"+path));
    req.setTransferTimeout(3000);
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute,QNetworkRequest::AlwaysNetwork);
    req.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");
    auto *reply=patch ? m_network.post(req,QJsonDocument(*patch).toJson(QJsonDocument::Compact)) : m_network.get(req);
    connect(reply,&QNetworkReply::finished,this,[this,reply,done] {
        const auto doc=QJsonDocument::fromJson(reply->readAll());
        const bool ok=reply->error()==QNetworkReply::NoError && doc.isObject();
        if (!ok) m_error="Connection/save failed: "+reply->errorString();
        done(ok,doc.object()); reply->deleteLater();
    });
}
void Client::pollParams() {
    if(m_gettingParams || m_posting || !m_pending.isEmpty()) return;
    m_gettingParams=true;
    const auto generation=m_generation;
    request("params",nullptr,[this,generation](bool ok,QJsonObject o) {
        m_gettingParams=false;
        if(!ok || generation!=m_generation) return;
        const auto raw=o["values"].toObject();
        if(raw.size()!=teedsp::kParamCount) return;
        const auto values=encode(decode(raw));
        const bool changed=!m_ready || values!=m_values;
        m_values=values;m_ready=true;m_error.clear();
        if(changed) emit parametersReceived(decode(values));
    });
}
void Client::submit(const dsp::ChainParams &p) {
    if(!m_ready) return;
    auto values=encode(p);
    for(auto i=values.begin();i!=values.end();++i)
        if(m_values[i.key()]!=i.value()) m_pending[i.key()]=i.value();
    m_values=values;++m_generation;
}
void Client::sendPending() {
    if(m_posting || m_pending.isEmpty()) return;
    const auto patch=m_pending;m_pending={};m_posting=true;
    request("params",&patch,[this,patch](bool ok,QJsonObject) {
        m_posting=false;
        if(!ok) {
            // Preserve newer gestures when retrying an older failed write.
            for(auto i=patch.begin();i!=patch.end();++i)
                if(!m_pending.contains(i.key())) m_pending[i.key()]=i.value();
            return;
        }
        m_error.clear();sendPending();
    });
}
void Client::setVolume(const QJsonObject &patch) {
    for(auto i=patch.begin();i!=patch.end();++i) m_pendingVolume[i.key()]=i.value();
    ++m_volumeGeneration;
}
void Client::sendVolume() {
    if(m_postingVolume || m_pendingVolume.isEmpty()) return;
    const auto patch=m_pendingVolume;m_pendingVolume={};m_postingVolume=true;
    const auto generation=m_volumeGeneration;
    request("volume",&patch,[this,patch,generation](bool ok,QJsonObject o) {
        m_postingVolume=false;
        if(!ok) {
            for(auto i=patch.begin();i!=patch.end();++i)
                if(!m_pendingVolume.contains(i.key())) m_pendingVolume[i.key()]=i.value();
        } else if(generation==m_volumeGeneration) {volume=o;emit volumeReceived();}
    });
}
QString Client::statusText() const {
    if(!m_error.isEmpty()) return m_error;
    if(!m_ready || !connected()) return "Connecting to CM3588…";
    if(!meters["route"].toObject()["routed"].toBool()) return "Waiting for AirPlay routing";
    return QString("CM3588 • AirPlay → TeeDSP → UA-25 • 48 kHz • %1 frames").arg(meters["quantum"].toInt());
}
host::ApoSharedClient::ApoMeters Client::meterSnapshot() const {
    host::ApoSharedClient::ApoMeters m;
    if(!connected()) return m;
    const auto array=[this](const char *name,auto &out,float fallback=0.f) {
        const auto a=meters[name].toArray();
        for(unsigned i=0;i<std::size(out);++i) out[i]=a[int(i)].toDouble(fallback);
    };
    array("inPeakDbfs",m.inPeakDbfs,-120);array("outPeakDbfs",m.outPeakDbfs,-120);
    array("outLufsCh",m.outLufsCh,-120);array("bandGrDb",m.bandGrDb);
    array("spectralGain",m.spectralGainDb);
    m.outRmsDbfs=meters["outRmsDbfs"].toDouble(-120);
    m.outLufsM=meters["outLufsM"].toDouble(-120);
    m.compGrDb=meters["compression"].toDouble();
    m.levelerGainDb=meters["inputGain"].toDouble();
    m.outLevelerGainDb=meters["outputGain"].toDouble();
    return m;
}
}
