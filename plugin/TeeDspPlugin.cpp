// TeeDSP CLAP plugin — the entire TeeDSP signal chain (Leveler -> trim -> EQ ->
// Exciter -> Compressor -> width -> output Leveler -> trim) packaged as a single
// "make it sound better" master effect.
//
// The plugin owns a dsp::ProcessorChain and drives it from CLAP parameters.
// Audio crosses the CLAP boundary as planar float; the chain works in
// interleaved float, so we interleave on the way in and de-interleave on the
// way out through a pre-allocated scratch buffer (no allocation in process).
//
// A private telemetry extension (TEEDSP_EXT_TELEMETRY) exposes the chain's live
// gain-reduction / leveler-gain meters to the host UI, since the plugin runs
// in-process in the TeeDSP app.

#include <clap/clap.h>

#include "dsp/ProcessorChain.h"
#include "dsp/ParametricEQ.h"
#include "shared/TeeDspParams.h"
#include "shared/TeeDspTelemetry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace teedsp;

namespace {

// ---------------------------------------------------------------------------
// Plugin descriptor
// ---------------------------------------------------------------------------
const char *kFeatures[] = {
    CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
    CLAP_PLUGIN_FEATURE_STEREO,
    nullptr,
};

const clap_plugin_descriptor_t kDescriptor = {
    CLAP_VERSION_INIT,
    "com.teedsp.master",
    "TeeDSP Master",
    "TeeDSP",
    "",
    "",
    "",
    "0.1.0",
    "The full TeeDSP mastering chain as a single effect.",
    kFeatures,
};

// O(1) map from param ID to its contiguous index in kParams / m_values.
// Globals occupy indices 0..PID_GlobalCount-1 (id == index). Band params form
// a contiguous block right after, in (band, field) order.
inline int paramIndexOf(uint32_t id)
{
    if (id < PID_GlobalCount)
        return static_cast<int>(id);
    if (isBandParam(id)) {
        const int blockIdx = static_cast<int>(id - kBandParamBase);
        if (blockIdx >= 0 && blockIdx < kBandCount * BF_Count)
            return PID_GlobalCount + blockIdx;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Plugin instance
// ---------------------------------------------------------------------------
struct TeeDspPlugin {
    clap_plugin_t          plugin;
    const clap_host_t     *host = nullptr;

    dsp::ProcessorChain    chain;
    double                 values[kParamCount];

    double                 sampleRate = 48000.0;
    uint32_t               channels = 2;
    std::vector<float>     scratch;   // interleaved, channels * maxFrames

    void applyParam(uint32_t id, double v)
    {
        const int idx = paramIndexOf(id);
        if (idx < 0)
            return;
        values[idx] = v;
        const bool on = v >= 0.5;

        if (isBandParam(id)) {
            const int band = bandOf(id);
            auto &eq = chain.eq();
            switch (fieldOf(id)) {
            case BF_Enabled:      eq.setBandEnabled(band, on); break;
            case BF_Type:         eq.setBandType(band, static_cast<dsp::ParametricEQ::BandType>(static_cast<int>(std::lround(v)))); break;
            case BF_Freq:         eq.setBandFrequency(band, static_cast<float>(v)); break;
            case BF_Q:            eq.setBandQ(band, static_cast<float>(v)); break;
            case BF_Gain:         eq.setBandGainDb(band, static_cast<float>(v)); break;
            case BF_DynThreshold: eq.setBandDynamicThresholdDb(band, static_cast<float>(v)); break;
            case BF_DynRatio:     eq.setBandDynamicRatio(band, static_cast<float>(v)); break;
            case BF_DynAttack:    eq.setBandDynamicAttackMs(band, static_cast<float>(v)); break;
            case BF_DynRelease:   eq.setBandDynamicReleaseMs(band, static_cast<float>(v)); break;
            case BF_DynRange:     eq.setBandDynamicRangeDb(band, static_cast<float>(v)); break;
            default: break;
            }
            return;
        }

        switch (id) {
        case PID_Bypass:               chain.setBypass(on); break;
        case PID_InputTrim:            chain.setInputTrimDb(static_cast<float>(v)); break;
        case PID_OutputTrim:           chain.setOutputTrimDb(static_cast<float>(v)); break;
        case PID_StereoWidth:          chain.setStereoWidth(static_cast<float>(v)); break;
        case PID_LevelerEnabled:       chain.leveler().setBypass(!on); break;
        case PID_OutputLevelerEnabled: chain.outputLeveler().setBypass(!on); break;
        case PID_EqEnabled:            chain.eq().setBypass(!on); break;
        case PID_CompEnabled:          chain.compressor().setBypass(!on); break;
        case PID_CompThreshold:        chain.compressor().setThresholdDb(static_cast<float>(v)); break;
        case PID_CompRatio:            chain.compressor().setRatio(static_cast<float>(v)); break;
        case PID_CompKnee:             chain.compressor().setKneeDb(static_cast<float>(v)); break;
        case PID_CompAttack:           chain.compressor().setAttackMs(static_cast<float>(v)); break;
        case PID_CompRelease:          chain.compressor().setReleaseMs(static_cast<float>(v)); break;
        case PID_CompMakeup:           chain.compressor().setMakeupDb(static_cast<float>(v)); break;
        case PID_ExciterEnabled:       chain.exciter().setBypass(!on); break;
        case PID_ExciterDrive:         chain.exciter().setDrive(static_cast<float>(v)); break;
        case PID_ExciterMix:           chain.exciter().setMix(static_cast<float>(v)); break;
        case PID_ExciterTone:          chain.exciter().setToneHz(static_cast<float>(v)); break;
        default: break;
        }
    }

    void applyAllToChain()
    {
        for (int i = 0; i < kParamCount; ++i)
            applyParam(kParams[i].id, values[i]);
    }
};

inline TeeDspPlugin *self(const clap_plugin_t *p)
{
    return static_cast<TeeDspPlugin *>(p->plugin_data);
}

// ---------------------------------------------------------------------------
// clap.audio-ports
// ---------------------------------------------------------------------------
uint32_t audioPortsCount(const clap_plugin_t *, bool) { return 1; }

bool audioPortsGet(const clap_plugin_t *, uint32_t index, bool /*isInput*/,
                   clap_audio_port_info_t *info)
{
    if (index != 0)
        return false;
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "Main");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = 0; // input port 0 <-> output port 0, in-place allowed
    return true;
}

const clap_plugin_audio_ports_t kAudioPorts = { audioPortsCount, audioPortsGet };

// ---------------------------------------------------------------------------
// clap.params
// ---------------------------------------------------------------------------
uint32_t paramsCount(const clap_plugin_t *) { return kParamCount; }

bool paramsGetInfo(const clap_plugin_t *, uint32_t index, clap_param_info_t *info)
{
    if (index >= static_cast<uint32_t>(kParamCount))
        return false;
    const ParamDescriptor &d = kParams[index];
    info->id = d.id;
    info->cookie = nullptr;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (d.stepped)
        info->flags |= CLAP_PARAM_IS_STEPPED;
    if (d.id == PID_Bypass)
        info->flags |= CLAP_PARAM_IS_BYPASS;
    info->min_value = d.minVal;
    info->max_value = d.maxVal;
    info->default_value = d.defVal;
    std::snprintf(info->name, sizeof(info->name), "%s", d.name);
    std::snprintf(info->module, sizeof(info->module), "%s", d.module);
    return true;
}

bool paramsGetValue(const clap_plugin_t *p, clap_id id, double *out)
{
    const int idx = paramIndexOf(id);
    if (idx < 0)
        return false;
    *out = self(p)->values[idx];
    return true;
}

bool paramsValueToText(const clap_plugin_t *, clap_id id, double value,
                       char *out, uint32_t cap)
{
    const ParamDescriptor *d = findParam(id);
    if (!d)
        return false;

    if (id == PID_Bypass || id == PID_LevelerEnabled || id == PID_OutputLevelerEnabled ||
        id == PID_EqEnabled || id == PID_CompEnabled || id == PID_ExciterEnabled ||
        (isBandParam(id) && fieldOf(id) == BF_Enabled)) {
        std::snprintf(out, cap, "%s", value >= 0.5 ? "On" : "Off");
        return true;
    }
    if (isBandParam(id) && fieldOf(id) == BF_Type) {
        const int t = static_cast<int>(std::lround(value));
        const char *n = (t == 1) ? "Low Shelf" : (t == 2) ? "High Shelf" : "Peaking";
        std::snprintf(out, cap, "%s", n);
        return true;
    }
    std::snprintf(out, cap, "%.2f", value);
    return true;
}

bool paramsTextToValue(const clap_plugin_t *, clap_id, const char *text, double *out)
{
    if (!text)
        return false;
    char *end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text)
        return false;
    *out = v;
    return true;
}

void applyInputEvents(TeeDspPlugin *tp, const clap_input_events_t *in)
{
    if (!in)
        return;
    const uint32_t n = in->size(in);
    for (uint32_t i = 0; i < n; ++i) {
        const clap_event_header_t *h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
            const auto *ev = reinterpret_cast<const clap_event_param_value_t *>(h);
            tp->applyParam(ev->param_id, ev->value);
        }
    }
}

void paramsFlush(const clap_plugin_t *p, const clap_input_events_t *in,
                 const clap_output_events_t * /*out*/)
{
    applyInputEvents(self(p), in);
}

const clap_plugin_params_t kParamsExt = {
    paramsCount, paramsGetInfo, paramsGetValue,
    paramsValueToText, paramsTextToValue, paramsFlush,
};

// ---------------------------------------------------------------------------
// clap.state — versioned blob: magic, version, count, then `count` doubles.
// ---------------------------------------------------------------------------
constexpr uint32_t kStateMagic = 0x50534454; // 'TDSP' little-endian
constexpr uint32_t kStateVersion = 1;

bool stateSave(const clap_plugin_t *p, const clap_ostream_t *os)
{
    TeeDspPlugin *tp = self(p);
    const uint32_t header[3] = { kStateMagic, kStateVersion, static_cast<uint32_t>(kParamCount) };
    auto writeAll = [&](const void *buf, uint64_t sz) {
        const char *c = static_cast<const char *>(buf);
        uint64_t done = 0;
        while (done < sz) {
            const int64_t w = os->write(os, c + done, sz - done);
            if (w <= 0)
                return false;
            done += static_cast<uint64_t>(w);
        }
        return true;
    };
    if (!writeAll(header, sizeof(header)))
        return false;
    return writeAll(tp->values, sizeof(tp->values));
}

bool stateLoad(const clap_plugin_t *p, const clap_istream_t *is)
{
    TeeDspPlugin *tp = self(p);
    auto readAll = [&](void *buf, uint64_t sz) {
        char *c = static_cast<char *>(buf);
        uint64_t done = 0;
        while (done < sz) {
            const int64_t r = is->read(is, c + done, sz - done);
            if (r <= 0)
                return false;
            done += static_cast<uint64_t>(r);
        }
        return true;
    };
    uint32_t header[3] = { 0, 0, 0 };
    if (!readAll(header, sizeof(header)))
        return false;
    if (header[0] != kStateMagic)
        return false;
    const uint32_t count = std::min<uint32_t>(header[2], kParamCount);
    if (!readAll(tp->values, static_cast<uint64_t>(count) * sizeof(double)))
        return false;
    tp->applyAllToChain();
    return true;
}

const clap_plugin_state_t kStateExt = { stateSave, stateLoad };

// ---------------------------------------------------------------------------
// teedsp.telemetry (private extension)
// ---------------------------------------------------------------------------
void telemetryRead(const clap_plugin_t *p, teedsp_telemetry_data *out)
{
    if (!out)
        return;
    TeeDspPlugin *tp = self(p);
    auto &eq = tp->chain.eq();
    for (int i = 0; i < kBandCount; ++i)
        out->bandGrDb[i] = eq.bandDynamicGainReductionDb(i);
    out->compGrDb = tp->chain.compressor().currentGainReductionDb();
    out->levelerGainDb = tp->chain.leveler().currentGainDb();
    out->outputLevelerGainDb = tp->chain.outputLeveler().currentGainDb();
}

const teedsp_telemetry kTelemetryExt = { telemetryRead };

// ---------------------------------------------------------------------------
// clap_plugin_t lifecycle
// ---------------------------------------------------------------------------
bool pluginInit(const clap_plugin_t *) { return true; }

void pluginDestroy(const clap_plugin_t *p)
{
    delete self(p);
}

bool pluginActivate(const clap_plugin_t *p, double sampleRate,
                    uint32_t /*minFrames*/, uint32_t maxFrames)
{
    TeeDspPlugin *tp = self(p);
    tp->sampleRate = sampleRate;
    tp->channels = 2;
    tp->scratch.assign(static_cast<size_t>(maxFrames) * tp->channels, 0.0f);
    tp->chain.prepare(sampleRate, tp->channels);
    tp->chain.reset();
    tp->applyAllToChain();
    return true;
}

void pluginDeactivate(const clap_plugin_t *) {}
bool pluginStartProcessing(const clap_plugin_t *) { return true; }
void pluginStopProcessing(const clap_plugin_t *) {}

void pluginReset(const clap_plugin_t *p)
{
    self(p)->chain.reset();
}

clap_process_status pluginProcess(const clap_plugin_t *p, const clap_process_t *proc)
{
    TeeDspPlugin *tp = self(p);

    // Param changes first, then audio for the whole block. Sample-accurate
    // ordering isn't needed for a master-bus processor.
    applyInputEvents(tp, proc->in_events);

    const uint32_t frames = proc->frames_count;
    if (frames == 0)
        return CLAP_PROCESS_CONTINUE;

    const uint32_t ch = tp->channels;
    if (proc->audio_inputs_count == 0 || proc->audio_outputs_count == 0)
        return CLAP_PROCESS_CONTINUE;

    const clap_audio_buffer_t &in = proc->audio_inputs[0];
    const clap_audio_buffer_t &out = proc->audio_outputs[0];
    if (!in.data32 || !out.data32)
        return CLAP_PROCESS_CONTINUE;

    const uint32_t inCh = std::min<uint32_t>(in.channel_count, ch);

    // planar in -> interleaved scratch
    float *s = tp->scratch.data();
    for (uint32_t f = 0; f < frames; ++f)
        for (uint32_t c = 0; c < ch; ++c)
            s[f * ch + c] = (c < inCh) ? in.data32[c][f] : 0.0f;

    tp->chain.process(s, frames);

    // interleaved scratch -> planar out
    const uint32_t outCh = std::min<uint32_t>(out.channel_count, ch);
    for (uint32_t f = 0; f < frames; ++f)
        for (uint32_t c = 0; c < outCh; ++c)
            out.data32[c][f] = s[f * ch + c];

    return CLAP_PROCESS_CONTINUE;
}

const void *pluginGetExtension(const clap_plugin_t *, const char *id)
{
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) return &kAudioPorts;
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0)      return &kParamsExt;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0)       return &kStateExt;
    if (std::strcmp(id, TEEDSP_EXT_TELEMETRY) == 0) return &kTelemetryExt;
    return nullptr;
}

void pluginOnMainThread(const clap_plugin_t *) {}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------
uint32_t factoryGetPluginCount(const clap_plugin_factory_t *) { return 1; }

const clap_plugin_descriptor_t *factoryGetDescriptor(const clap_plugin_factory_t *, uint32_t index)
{
    return index == 0 ? &kDescriptor : nullptr;
}

const clap_plugin_t *factoryCreatePlugin(const clap_plugin_factory_t *,
                                         const clap_host_t *host, const char *id)
{
    if (!id || std::strcmp(id, kDescriptor.id) != 0)
        return nullptr;

    auto *tp = new TeeDspPlugin();
    tp->host = host;
    for (int i = 0; i < kParamCount; ++i)
        tp->values[i] = kParams[i].defVal;

    tp->plugin.desc = &kDescriptor;
    tp->plugin.plugin_data = tp;
    tp->plugin.init = pluginInit;
    tp->plugin.destroy = pluginDestroy;
    tp->plugin.activate = pluginActivate;
    tp->plugin.deactivate = pluginDeactivate;
    tp->plugin.start_processing = pluginStartProcessing;
    tp->plugin.stop_processing = pluginStopProcessing;
    tp->plugin.reset = pluginReset;
    tp->plugin.process = pluginProcess;
    tp->plugin.get_extension = pluginGetExtension;
    tp->plugin.on_main_thread = pluginOnMainThread;
    return &tp->plugin;
}

const clap_plugin_factory_t kFactory = {
    factoryGetPluginCount,
    factoryGetDescriptor,
    factoryCreatePlugin,
};

// ---------------------------------------------------------------------------
// Entry
// ---------------------------------------------------------------------------
bool entryInit(const char *) { return true; }
void entryDeinit() {}

const void *entryGetFactory(const char *factoryId)
{
    if (std::strcmp(factoryId, CLAP_PLUGIN_FACTORY_ID) == 0)
        return &kFactory;
    return nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT,
    entryInit,
    entryDeinit,
    entryGetFactory,
};
