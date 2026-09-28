#pragma once
#include "dsp/ChainParams.h"
#include "shared/TeeDspParams.h"
#include <QJsonObject>
#include <algorithm>
#include <cmath>
namespace remote {
inline QJsonObject encode(const dsp::ChainParams &p) {
    QJsonObject o;
    o[QString::number(0)] = double(p.bypassed);
    o[QString::number(1)] = double(p.inputTrimDb);
    o[QString::number(2)] = double(p.outputTrimDb);
    o[QString::number(3)] = double(p.stereoWidth);
    o[QString::number(4)] = double(p.levelerEnabled);
    o[QString::number(5)] = double(p.outputLevelerEnabled);
    o[QString::number(6)] = double(p.eqEnabled);
    o[QString::number(7)] = double(p.compEnabled);
    o[QString::number(8)] = double(p.compThreshDb);
    o[QString::number(9)] = double(p.compRatio);
    o[QString::number(10)] = double(p.compKneeDb);
    o[QString::number(11)] = double(p.compAttackMs);
    o[QString::number(12)] = double(p.compReleaseMs);
    o[QString::number(13)] = double(p.compMakeupDb);
    o[QString::number(14)] = double(p.exciterEnabled);
    o[QString::number(15)] = double(p.exciterDrive);
    o[QString::number(16)] = double(p.exciterMix);
    o[QString::number(17)] = double(p.exciterToneHz);
    o[QString::number(18)] = double(p.spectralLevelerEnabled);
    for (int b=0; b<teedsp::kBandCount; ++b) {
        o[QString::number(100+b*10+0)] = double(p.eqBands[b].enabled);
        o[QString::number(100+b*10+1)] = double(p.eqBands[b].type);
        o[QString::number(100+b*10+2)] = double(p.eqBands[b].freqHz);
        o[QString::number(100+b*10+3)] = double(p.eqBands[b].q);
        o[QString::number(100+b*10+4)] = double(p.eqBands[b].gainDb);
        o[QString::number(100+b*10+5)] = double(p.eqBands[b].dynThresholdDb);
        o[QString::number(100+b*10+6)] = double(p.eqBands[b].dynRatio);
        o[QString::number(100+b*10+7)] = double(p.eqBands[b].dynAttackMs);
        o[QString::number(100+b*10+8)] = double(p.eqBands[b].dynReleaseMs);
        o[QString::number(100+b*10+9)] = double(p.eqBands[b].dynRangeDb);
    }
    // Qt gestures can extend beyond the plugin's limits (e.g. mouse wheel Q).
    for (const auto &d : teedsp::kParams) {
        const auto key = QString::number(d.id);
        double v = std::clamp(o[key].toDouble(), d.minVal, d.maxVal);
        o[key] = d.stepped ? std::round(v) : v;
    }
    return o;
}
inline dsp::ChainParams decode(const QJsonObject &o) {
    dsp::ChainParams p;
    p.bypassed = o[QString::number(0)].toDouble();
    p.inputTrimDb = o[QString::number(1)].toDouble();
    p.outputTrimDb = o[QString::number(2)].toDouble();
    p.stereoWidth = o[QString::number(3)].toDouble();
    p.levelerEnabled = o[QString::number(4)].toDouble();
    p.outputLevelerEnabled = o[QString::number(5)].toDouble();
    p.eqEnabled = o[QString::number(6)].toDouble();
    p.compEnabled = o[QString::number(7)].toDouble();
    p.compThreshDb = o[QString::number(8)].toDouble();
    p.compRatio = o[QString::number(9)].toDouble();
    p.compKneeDb = o[QString::number(10)].toDouble();
    p.compAttackMs = o[QString::number(11)].toDouble();
    p.compReleaseMs = o[QString::number(12)].toDouble();
    p.compMakeupDb = o[QString::number(13)].toDouble();
    p.exciterEnabled = o[QString::number(14)].toDouble();
    p.exciterDrive = o[QString::number(15)].toDouble();
    p.exciterMix = o[QString::number(16)].toDouble();
    p.exciterToneHz = o[QString::number(17)].toDouble();
    p.spectralLevelerEnabled = o[QString::number(18)].toDouble();
    for (int b=0; b<teedsp::kBandCount; ++b) {
        p.eqBands[b].enabled = o[QString::number(100+b*10+0)].toDouble();
        p.eqBands[b].type = o[QString::number(100+b*10+1)].toDouble();
        p.eqBands[b].freqHz = o[QString::number(100+b*10+2)].toDouble();
        p.eqBands[b].q = o[QString::number(100+b*10+3)].toDouble();
        p.eqBands[b].gainDb = o[QString::number(100+b*10+4)].toDouble();
        p.eqBands[b].dynThresholdDb = o[QString::number(100+b*10+5)].toDouble();
        p.eqBands[b].dynRatio = o[QString::number(100+b*10+6)].toDouble();
        p.eqBands[b].dynAttackMs = o[QString::number(100+b*10+7)].toDouble();
        p.eqBands[b].dynReleaseMs = o[QString::number(100+b*10+8)].toDouble();
        p.eqBands[b].dynRangeDb = o[QString::number(100+b*10+9)].toDouble();
    }
    return p;
}
}
