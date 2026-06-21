#pragma once

// Apply a flat dsp::ChainParams snapshot onto a live dsp::ProcessorChain.
// This mirrors the CLAP plugin's per-parameter mapping (TeeDspPlugin::applyParam)
// so the APO and the plugin drive the chain identically. Safe to call from the
// audio thread: every setter underneath is a relaxed atomic store (EQ band
// changes mark a dirty flag that recomputes lazily inside process()).

#include "ChainParams.h"
#include "ParametricEQ.h"
#include "ProcessorChain.h"

namespace dsp {

inline void applyChainParams(ProcessorChain &chain, const ChainParams &p)
{
    chain.setBypass(p.bypassed);
    chain.setInputTrimDb(p.inputTrimDb);
    chain.setOutputTrimDb(p.outputTrimDb);
    chain.setStereoWidth(p.stereoWidth);

    // "Enabled" toggles map to !bypass on the relevant stage (matches plugin).
    chain.leveler().setBypass(!p.levelerEnabled);
    chain.spectralLeveler().setBypass(!p.spectralLevelerEnabled);
    chain.outputLeveler().setBypass(!p.outputLevelerEnabled);
    chain.eq().setBypass(!p.eqEnabled);

    ParametricEQ &eq = chain.eq();
    for (int b = 0; b < kEqBandCount; ++b) {
        const EqBandParams &e = p.eqBands[b];
        eq.setBandEnabled(b, e.enabled);
        eq.setBandType(b, static_cast<ParametricEQ::BandType>(e.type));
        eq.setBandFrequency(b, e.freqHz);
        eq.setBandQ(b, e.q);
        eq.setBandGainDb(b, e.gainDb);
        eq.setBandDynamicThresholdDb(b, e.dynThresholdDb);
        eq.setBandDynamicRatio(b, e.dynRatio);
        eq.setBandDynamicAttackMs(b, e.dynAttackMs);
        eq.setBandDynamicReleaseMs(b, e.dynReleaseMs);
        eq.setBandDynamicRangeDb(b, e.dynRangeDb);
    }

    Compressor &comp = chain.compressor();
    comp.setBypass(!p.compEnabled);
    comp.setThresholdDb(p.compThreshDb);
    comp.setRatio(p.compRatio);
    comp.setKneeDb(p.compKneeDb);
    comp.setAttackMs(p.compAttackMs);
    comp.setReleaseMs(p.compReleaseMs);
    comp.setMakeupDb(p.compMakeupDb);

    Exciter &ex = chain.exciter();
    ex.setBypass(!p.exciterEnabled);
    ex.setDrive(p.exciterDrive);
    ex.setMix(p.exciterMix);
    ex.setToneHz(p.exciterToneHz);
}

} // namespace dsp
