#include "ClapHost.h"

#include <clap/clap.h>

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace teedsp;

namespace host {

namespace {

// ---- clap_host callbacks --------------------------------------------------
// We provide no host extensions for v1; the plugin only uses audio-ports,
// params, state and our private telemetry ext (all plugin-side).
const void *hostGetExtension(const clap_host_t *, const char *) { return nullptr; }
void hostRequestRestart(const clap_host_t *) {}
void hostRequestProcess(const clap_host_t *) {}
void hostRequestCallback(const clap_host_t *) {}

// ---- input/output event adapters over a flat array ------------------------
struct EventList {
    const clap_event_param_value_t *events;
    uint32_t count;
};

uint32_t eventsSize(const clap_input_events_t *list)
{
    return static_cast<const EventList *>(list->ctx)->count;
}

const clap_event_header_t *eventsGet(const clap_input_events_t *list, uint32_t index)
{
    const auto *el = static_cast<const EventList *>(list->ctx);
    return &el->events[index].header;
}

bool outEventsTryPush(const clap_output_events_t *, const clap_event_header_t *)
{
    return true; // plugin emits nothing we consume
}

std::wstring exeDir()
{
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf, n);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring envPath(const wchar_t *var)
{
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(var, buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? std::wstring(buf, n) : std::wstring();
}

} // namespace

ClapHost::ClapHost()
    : m_paramQueue(2 * kMaxEventsPerBlock * 8)
{
    for (auto &v : m_paramCache)
        v.store(0.0f, std::memory_order_relaxed);
    // Seed the cache with declared defaults so paramValue() is meaningful even
    // before the controller pushes anything.
    for (int i = 0; i < kParamCount; ++i)
        m_paramCache[i].store(static_cast<float>(kParams[i].defVal), std::memory_order_relaxed);
}

ClapHost::~ClapHost()
{
    unload();
}

bool ClapHost::load()
{
    const wchar_t *name = L"teedsp.clap";

    std::vector<std::wstring> candidates;
    const std::wstring dir = exeDir();
    if (!dir.empty())
        candidates.push_back(dir + L"\\" + name);

    const std::wstring common = envPath(L"CommonProgramFiles");
    if (!common.empty())
        candidates.push_back(common + L"\\CLAP\\" + name);

    const std::wstring local = envPath(L"LOCALAPPDATA");
    if (!local.empty())
        candidates.push_back(local + L"\\Programs\\Common\\CLAP\\" + name);

    for (const auto &c : candidates) {
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (loadFrom(c))
                return true;
        }
    }
    if (m_error.empty())
        m_error = "teedsp.clap not found next to the app or in the CLAP search paths.";
    return false;
}

bool ClapHost::loadFrom(const std::wstring &path)
{
    HMODULE mod = LoadLibraryW(path.c_str());
    if (!mod) {
        m_error = "LoadLibrary failed for teedsp.clap (error " +
                  std::to_string(GetLastError()) + ").";
        return false;
    }

    auto *entry = reinterpret_cast<const clap_plugin_entry_t *>(
        GetProcAddress(mod, "clap_entry"));
    if (!entry) {
        m_error = "teedsp.clap has no clap_entry export.";
        FreeLibrary(mod);
        return false;
    }

    // Narrow the path to UTF-8 for entry->init (informational).
    char pathUtf8[MAX_PATH * 2] = {0};
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, pathUtf8, sizeof(pathUtf8), nullptr, nullptr);
    if (!entry->init(pathUtf8)) {
        m_error = "clap_entry->init failed.";
        FreeLibrary(mod);
        return false;
    }

    const auto *factory = static_cast<const clap_plugin_factory_t *>(
        entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory || factory->get_plugin_count(factory) == 0) {
        m_error = "teedsp.clap exposes no plugin factory.";
        entry->deinit();
        FreeLibrary(mod);
        return false;
    }

    const clap_plugin_descriptor_t *desc = factory->get_plugin_descriptor(factory, 0);

    auto *clapHost = new clap_host_t{};
    clapHost->clap_version = CLAP_VERSION;
    clapHost->host_data = this;
    clapHost->name = "TeeDSP";
    clapHost->vendor = "TeeDSP";
    clapHost->url = "";
    clapHost->version = "0.1.0";
    clapHost->get_extension = hostGetExtension;
    clapHost->request_restart = hostRequestRestart;
    clapHost->request_process = hostRequestProcess;
    clapHost->request_callback = hostRequestCallback;

    const clap_plugin_t *plugin = factory->create_plugin(factory, clapHost, desc->id);
    if (!plugin) {
        m_error = "create_plugin failed.";
        delete clapHost;
        entry->deinit();
        FreeLibrary(mod);
        return false;
    }
    if (!plugin->init(plugin)) {
        m_error = "plugin->init failed.";
        plugin->destroy(plugin);
        delete clapHost;
        entry->deinit();
        FreeLibrary(mod);
        return false;
    }

    m_module = mod;
    m_entry = entry;
    m_clapHost = clapHost;
    m_plugin = plugin;
    m_params = static_cast<const clap_plugin_params_t *>(
        plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    m_telemetry = static_cast<const teedsp_telemetry *>(
        plugin->get_extension(plugin, TEEDSP_EXT_TELEMETRY));

    for (auto &p : m_planar)
        p.assign(kMaxBlock, 0.0f);

    m_error.clear();
    return true;
}

void ClapHost::unload()
{
    if (m_plugin) {
        if (m_active)
            deactivate();
        m_plugin->destroy(m_plugin);
        m_plugin = nullptr;
    }
    if (m_entry) {
        m_entry->deinit();
        m_entry = nullptr;
    }
    if (m_clapHost) {
        delete m_clapHost;
        m_clapHost = nullptr;
    }
    if (m_module) {
        FreeLibrary(static_cast<HMODULE>(m_module));
        m_module = nullptr;
    }
    m_params = nullptr;
    m_telemetry = nullptr;
}

void ClapHost::flushAllParams()
{
    if (!m_params || !m_plugin)
        return;

    clap_event_param_value_t evs[kParamCount];
    for (int i = 0; i < kParamCount; ++i) {
        clap_event_param_value_t &e = evs[i];
        std::memset(&e, 0, sizeof(e));
        e.header.size = sizeof(clap_event_param_value_t);
        e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        e.header.type = CLAP_EVENT_PARAM_VALUE;
        e.param_id = kParams[i].id;
        e.note_id = -1;
        e.port_index = -1;
        e.channel = -1;
        e.key = -1;
        e.value = m_paramCache[i].load(std::memory_order_relaxed);
    }
    EventList el{ evs, static_cast<uint32_t>(kParamCount) };
    const clap_input_events_t in{ &el, eventsSize, eventsGet };
    const clap_output_events_t out{ nullptr, outEventsTryPush };
    m_params->flush(m_plugin, &in, &out);
}

bool ClapHost::activate(double sampleRate)
{
    if (!m_plugin)
        return false;
    if (m_active)
        deactivate();

    m_sampleRate = sampleRate;
    if (!m_plugin->activate(m_plugin, sampleRate, 1, kMaxBlock)) {
        m_error = "plugin->activate failed.";
        return false;
    }
    m_active = true;

    // Discard any backlog consumer-side (this runs on the capture thread, the
    // same thread that drains in process(), so it's SPSC-safe). Then push the
    // host's authoritative state so the plugin matches the UI on the first
    // block — params queued before this activate() would otherwise be lost.
    {
        float sink[64];
        while (m_paramQueue.read(sink, 64) > 0) {}
    }
    flushAllParams();

    m_processing = m_plugin->start_processing(m_plugin);
    return true;
}

void ClapHost::deactivate()
{
    if (!m_plugin || !m_active)
        return;
    if (m_processing) {
        m_plugin->stop_processing(m_plugin);
        m_processing = false;
    }
    m_plugin->deactivate(m_plugin);
    m_active = false;
}

void ClapHost::setParam(uint32_t id, double value)
{
    const int idx = [&]() -> int {
        if (id < PID_GlobalCount) return static_cast<int>(id);
        if (isBandParam(id)) {
            const int b = static_cast<int>(id - kBandParamBase);
            if (b >= 0 && b < kBandCount * BF_Count) return PID_GlobalCount + b;
        }
        return -1;
    }();
    if (idx < 0)
        return;

    m_paramCache[idx].store(static_cast<float>(value), std::memory_order_relaxed);

    const float pair[2] = { static_cast<float>(id), static_cast<float>(value) };
    m_paramQueue.write(pair, 2); // drops if full; next push wins, cache stays correct
}

float ClapHost::paramValue(uint32_t id) const
{
    if (id < PID_GlobalCount)
        return m_paramCache[id].load(std::memory_order_relaxed);
    if (isBandParam(id)) {
        const int b = static_cast<int>(id - kBandParamBase);
        if (b >= 0 && b < kBandCount * BF_Count)
            return m_paramCache[PID_GlobalCount + b].load(std::memory_order_relaxed);
    }
    return 0.0f;
}

void ClapHost::readTelemetry(teedsp_telemetry_data &out) const
{
    if (m_telemetry && m_plugin)
        m_telemetry->read(m_plugin, &out);
    else
        std::memset(&out, 0, sizeof(out));
}

void ClapHost::process(float *interleaved, uint32_t frames, uint32_t channels)
{
    if (!m_plugin || !m_active || !m_processing || frames == 0 || channels == 0) {
        return; // leave audio untouched (effectively bypass)
    }

    // Drain queued param changes once per packet; attach to the first chunk.
    clap_event_param_value_t evbuf[kMaxEventsPerBlock];
    uint32_t evCount = 0;
    {
        float tmp[2 * kMaxEventsPerBlock];
        const std::size_t got = m_paramQueue.read(tmp, 2 * kMaxEventsPerBlock);
        const std::size_t pairs = got / 2;
        for (std::size_t i = 0; i < pairs && evCount < kMaxEventsPerBlock; ++i) {
            clap_event_param_value_t &e = evbuf[evCount++];
            e.header.size = sizeof(clap_event_param_value_t);
            e.header.time = 0;
            e.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
            e.header.type = CLAP_EVENT_PARAM_VALUE;
            e.header.flags = 0;
            e.param_id = static_cast<clap_id>(std::lround(tmp[i * 2 + 0]));
            e.cookie = nullptr;
            e.note_id = -1;
            e.port_index = -1;
            e.channel = -1;
            e.key = -1;
            e.value = tmp[i * 2 + 1];
        }
    }

    float *planar[2] = { m_planar[0].data(), m_planar[1].data() };

    clap_audio_buffer_t audioIn{};
    audioIn.data32 = planar;
    audioIn.data64 = nullptr;
    audioIn.channel_count = 2;
    audioIn.latency = 0;
    audioIn.constant_mask = 0;
    clap_audio_buffer_t audioOut = audioIn; // in-place: same planar storage

    const clap_output_events_t outEvents{ nullptr, outEventsTryPush };

    for (uint32_t offset = 0; offset < frames; offset += kMaxBlock) {
        const uint32_t n = std::min(kMaxBlock, frames - offset);

        // interleaved -> planar (duplicate mono into both channels)
        for (uint32_t f = 0; f < n; ++f) {
            const uint32_t base = (offset + f) * channels;
            planar[0][f] = interleaved[base + 0];
            planar[1][f] = interleaved[base + (channels > 1 ? 1 : 0)];
        }

        EventList el{ evbuf, (offset == 0) ? evCount : 0u };
        const clap_input_events_t inEvents{ &el, eventsSize, eventsGet };

        clap_process_t cp{};
        cp.steady_time = -1;
        cp.frames_count = n;
        cp.transport = nullptr;
        cp.audio_inputs = &audioIn;
        cp.audio_outputs = &audioOut;
        cp.audio_inputs_count = 1;
        cp.audio_outputs_count = 1;
        cp.in_events = &inEvents;
        cp.out_events = &outEvents;

        m_plugin->process(m_plugin, &cp);

        // planar -> interleaved (write back the channels the caller gave us)
        for (uint32_t f = 0; f < n; ++f) {
            const uint32_t base = (offset + f) * channels;
            interleaved[base + 0] = planar[0][f];
            if (channels > 1)
                interleaved[base + 1] = planar[1][f];
        }
    }
}

} // namespace host
