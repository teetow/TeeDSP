#pragma once
#include <clap/clap.h>
#include "shared/TeeDspParams.h"
#include "shared/TeeDspTelemetry.h"
#include "shared/TeeDspActions.h"
#include <nlohmann/json.hpp>
#include <array>
#include <cmath>
#include <stdexcept>

extern "C" const clap_plugin_entry_t clap_entry;
using Json = nlohmann::json;

// This app hosts the existing plugin statically; no plugin discovery or GUI host.
// Only the audio thread calls set/process. HTTP owns a separate desired snapshot.
class Processor {
    clap_host_t host{CLAP_VERSION_INIT, nullptr, "TeeDSP PipeWire", "TeeDSP", "", "0.1",
        [](const clap_host*, const char*) -> const void* { return nullptr; },
        [](const clap_host*) {}, [](const clap_host*) {}, [](const clap_host*) {}};
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_params_t* params = nullptr;
    const teedsp_telemetry* meters = nullptr;
    const teedsp_actions* actions = nullptr;
public:
    static constexpr unsigned maxFrames = 8192;
    Processor() {
        if (!clap_entry.init("teedsp")) throw std::runtime_error("CLAP entry init failed");
        auto factory = static_cast<const clap_plugin_factory_t*>(clap_entry.get_factory(CLAP_PLUGIN_FACTORY_ID));
        plugin = factory->create_plugin(factory, &host, factory->get_plugin_descriptor(factory, 0)->id);
        if (!plugin || !plugin->init(plugin) || !plugin->activate(plugin, 48000, 1, maxFrames)
            || !plugin->start_processing(plugin)) throw std::runtime_error("CLAP activation failed");
        params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
        actions = static_cast<const teedsp_actions*>(plugin->get_extension(plugin, TEEDSP_EXT_ACTIONS));
        meters = static_cast<const teedsp_telemetry*>(plugin->get_extension(plugin, TEEDSP_EXT_TELEMETRY));
    }
    ~Processor() {
        plugin->stop_processing(plugin); plugin->deactivate(plugin); plugin->destroy(plugin); clap_entry.deinit();
    }
    static Json defaults() {
        Json result = Json::object();
        for (const auto& p : teedsp::kParams) result[std::to_string(p.id)] = p.defVal;
        return result;
    }
    static Json descriptors() {
        Json result = Json::array();
        for (const auto& p : teedsp::kParams)
            result.push_back({{"id", p.id}, {"module", p.module}, {"name", p.name},
                {"min", p.minVal}, {"max", p.maxVal}, {"default", p.defVal}, {"stepped", p.stepped}});
        return result;
    }
    static void validate(const Json& patch) {
        if (!patch.is_object() || patch.empty()) throw std::invalid_argument("Expected a parameter object");
        for (auto it = patch.begin(); it != patch.end(); ++it) {
            const teedsp::ParamDescriptor* d = nullptr;
            for (const auto& p : teedsp::kParams) if (it.key() == std::to_string(p.id)) d = &p;
            if (!d || !it.value().is_number()) throw std::invalid_argument("Unknown parameter or non-numeric value");
            double value = it.value().get<double>();
            if (!std::isfinite(value) || value < d->minVal || value > d->maxVal
                || (d->stepped && value != std::round(value))) throw std::invalid_argument("Parameter outside its allowed range");
        }
    }
    void relearnLeveler(bool output) { actions->relearn_leveler(plugin, output); }
    void set(unsigned id, double value) {
        clap_event_param_value_t event{};
        event.header.size = sizeof(event); event.header.type = CLAP_EVENT_PARAM_VALUE;
        event.param_id = id; event.note_id = -1; event.port_index = -1; event.channel = -1; event.key = -1;
        event.value = value;
        clap_input_events_t events{&event,
            [](const clap_input_events*) -> uint32_t { return 1; },
            [](const clap_input_events* e, uint32_t) -> const clap_event_header_t* {
                return &static_cast<const clap_event_param_value_t*>(e->ctx)->header;
            }};
        params->flush(plugin, &events, nullptr);
    }
    void process(float* left, float* right, float* outLeft, float* outRight, unsigned frames) {
        float* inChannels[] = {left, right}; float* outChannels[] = {outLeft, outRight};
        clap_audio_buffer_t input{}; input.data32 = inChannels; input.channel_count = 2;
        clap_audio_buffer_t output{}; output.data32 = outChannels; output.channel_count = 2;
        clap_process_t block{}; block.frames_count = frames; block.steady_time = -1;
        block.audio_inputs = &input; block.audio_inputs_count = 1;
        block.audio_outputs = &output; block.audio_outputs_count = 1;
        plugin->process(plugin, &block);
    }
    teedsp_telemetry_data telemetry() const { teedsp_telemetry_data data{}; meters->read(plugin, &data); return data; }
};
