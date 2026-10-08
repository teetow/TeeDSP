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
Client::Client(QObject *parent):editor::Transport(parent) {
    m_meterTimer.setInterval(50);
    connect(&m_meterTimer,&QTimer::timeout,this,[this] {
        sendPending();sendVolume();
        if (m_gettingMeters) return;
        m_gettingMeters=true;
        request("meters",nullptr,[this](bool ok,QJsonObject o) {
            m_gettingMeters=false;
            if (!ok) return;
            meterData=o; m_lastMeter.restart(); emit metersReceived();
            if(m_spectrumVisible) {
                const auto convert=[](QJsonArray a) {
                    QVector<float> v;v.reserve(a.size());
                    for(auto x:a)v.append(x.toDouble(-120));
                    return v;
                };
                const auto input=convert(o["input"].toArray());
                const auto output=convert(o["output"].toArray());
                emit spectraUpdated(input,output,o["sampleRate"].toDouble(48000),
                                    input.size()>1 ? (input.size()-1)*2 : 0);
            }
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
void Client::relearnLeveler(bool output) {
    if (!m_ready || !connected()) return;
    const QJsonObject action{{"stage", output ? "output" : "input"}};
    request("leveler/relearn", &action, [this](bool ok, QJsonObject) {
        if (ok) m_error.clear();
        emit metersReceived();
    });
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
    if(!meterData["route"].toObject()["routed"].toBool()) return "Waiting for AirPlay routing";
    return QString("CM3588 • AirPlay → TeeDSP → UA-25 • 48 kHz • %1 frames").arg(meterData["quantum"].toInt());
}
editor::Meters Client::meters() const {
    editor::Meters m;
    if(!connected()) return m;
    const auto array=[this](const char *name,auto &out,float fallback=0.f) {
        const auto a=meterData[name].toArray();
        for(unsigned i=0;i<std::size(out);++i) out[i]=a[int(i)].toDouble(fallback);
    };
    array("inPeakDbfs",m.inPeakDbfs,-120);array("outPeakDbfs",m.outPeakDbfs,-120);
    array("outLufsCh",m.outLufsCh,-120);array("bandGrDb",m.bandGrDb);
    array("spectralGain",m.spectralGainDb);
    m.outRmsDbfs=meterData["outRmsDbfs"].toDouble(-120);
    m.outLufsM=meterData["outLufsM"].toDouble(-120);
    m.compGrDb=meterData["compression"].toDouble();
    m.levelerGainDb=meterData["inputGain"].toDouble();
    m.outLevelerGainDb=meterData["outputGain"].toDouble();
    return m;
}
void Client::flush() {sendPending();sendVolume();}
void Client::setEditorVisible(bool visible) {m_meterTimer.setInterval(visible ? 50 : 200);}
void Client::setSpectrumVisible(bool visible) {m_spectrumVisible=visible;}
}
namespace editor {
Transport *createTransport(QObject *parent) {return new remote::Client(parent);}
}
