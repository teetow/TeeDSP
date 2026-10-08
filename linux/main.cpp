#include "Processor.h"
#include "MasterVolume.h"
#include "dsp/LufsMeter.h"
#include "ui/widgets/WidgetMetrics.h"
#include "dsp/SpscRingBuffer.h"
#include "host/Fft.h"
#include <pipewire/filter.h>
#include <pipewire/pipewire.h>
#include <httplib.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

// resolv.h (via httplib) defines _res; old SPA macros use it as a local name.
#ifdef _res
#undef _res
#endif

using namespace std::chrono_literals;
struct App {
    Processor processor;
    MasterVolume master;
    std::mutex volumeMutex;
    Json volumeState() { return {{"volume", master.percent.load()}, {"muted", master.muted.load()}}; }
    pw_main_loop* loop = nullptr;
    pw_filter* filter = nullptr;
    void* inputs[2]{}; void* outputs[2]{};
    std::array<float, Processor::maxFrames> silence{}, scratch{};
    std::array<float, Processor::maxFrames * 2> samples{};
    dsp::SpscRingBuffer ring{32768};
    std::mutex configMutex, meterMutex;
    Json values = Processor::defaults();
    std::array<double, teedsp::kParamCount> desired{};
    bool dirty = true;
    std::atomic<bool> running{true}, observing{false};
    std::atomic<int64_t> lastObserverMs{0};
    std::atomic<int> state{PW_FILTER_STATE_UNCONNECTED};
    std::atomic<uint64_t> blocks{0}, frames{0};
    std::atomic<unsigned> quantum{0};
    std::atomic<float> peakIn{0}, peakOut{0};
    std::atomic<float> inPeakCh[2]{}, outPeakCh[2]{}, outLufsCh[2]{};
    std::atomic<float> outRmsDbfs{-120}, outLufsM{-120};
    dsp::LufsMeter lufs;
    std::array<float, Processor::maxFrames * 2> lufsSamples{};
    App() { lufs.prepare(48000, 2); }
    static float db(float amplitude) { return amplitude>1e-6f ? 20.f*std::log10(amplitude) : -120.f; }
    Json meter = {{"input", Json::array()}, {"output", Json::array()}};
    httplib::Server server;

    void refreshDesired() {
        for (int i = 0; i < teedsp::kParamCount; ++i)
            desired[i] = values.at(std::to_string(teedsp::kParams[i].id)).get<double>();
        dirty = true;
    }
    void persist(const Json& next) {
        std::ofstream file("/data/params.json.tmp");
        file << next.dump(2) << '\n'; file.flush();
        if (!file) throw std::runtime_error("Could not write settings");
        file.close();
        if (std::rename("/data/params.json.tmp", "/data/params.json"))
            throw std::runtime_error("Could not save settings");
    }
    void load() {
        std::ifstream volumeFile("/data/volume.json");
        if (volumeFile) {
            Json v; volumeFile >> v;
            master.percent.store(std::clamp(v.at("volume").get<float>(), 0.f, 100.f));
            master.muted.store(v.value("muted", false));
        }
        std::ifstream file("/data/params.json");
        if (file) { Json saved; file >> saved; Processor::validate(saved); values.update(saved); }
        refreshDesired();
    }
    static void process(void* user, spa_io_position* position) {
        auto& a = *static_cast<App*>(user);
        const unsigned n = position->clock.duration;
        a.quantum.store(n, std::memory_order_relaxed);
        if (n > Processor::maxFrames) return;
        float* in[2]; float* out[2];
        for (int ch = 0; ch < 2; ++ch) {
            in[ch] = static_cast<float*>(pw_filter_get_dsp_buffer(a.inputs[ch], n));
            out[ch] = static_cast<float*>(pw_filter_get_dsp_buffer(a.outputs[ch], n));
            if (!in[ch]) in[ch] = a.silence.data();
            if (!out[ch]) out[ch] = a.scratch.data();
        }
        // Never wait for HTTP/disk IO on the realtime thread.
        if (a.configMutex.try_lock()) {
            if (a.dirty) {
                for (int i = 0; i < teedsp::kParamCount; ++i)
                    a.processor.set(teedsp::kParams[i].id, a.desired[i]);
                a.dirty = false;
            }
            a.configMutex.unlock();
        }
        float prePeak = 0;
        float inPeaks[2]{}, outPeaks[2]{};
        double outSquares = 0;
        bool observing = a.observing.load(std::memory_order_relaxed);
        for (unsigned i = 0; i < n; ++i) {
            prePeak = std::max({prePeak, std::abs(in[0][i]), std::abs(in[1][i])});
            for(int c=0;c<2;++c) inPeaks[c]=std::max(inPeaks[c],std::abs(in[c][i]));
            if (observing) a.samples[2*i] = (in[0][i] + in[1][i]) * 0.5f;
        }
        a.processor.process(in[0], in[1], out[0], out[1], n);
        a.master.process(out[0], out[1], n);
        float postPeak = 0;
        for (unsigned i = 0; i < n; ++i) {
            postPeak = std::max({postPeak, std::abs(out[0][i]), std::abs(out[1][i])});
            for(int c=0;c<2;++c) {
                outPeaks[c]=std::max(outPeaks[c],std::abs(out[c][i]));
                outSquares+=double(out[c][i])*out[c][i];
                a.lufsSamples[2*i+c]=out[c][i];
            }
            if (observing) a.samples[2*i+1] = (out[0][i] + out[1][i]) * 0.5f;
        }
        if (observing) {
            // Write complete pre/post pairs, dropping telemetry when full.
            const auto count = std::min<size_t>(n * 2, a.ring.space() & ~size_t(1));
            a.ring.write(a.samples.data(), count);
        }
        a.lufs.process(a.lufsSamples.data(),n,2);
        for(int c=0;c<2;++c) {
            a.inPeakCh[c].store(db(inPeaks[c]),std::memory_order_relaxed);
            a.outPeakCh[c].store(db(outPeaks[c]),std::memory_order_relaxed);
            a.outLufsCh[c].store(a.lufs.channelLufs(c),std::memory_order_relaxed);
        }
        a.outRmsDbfs.store(n ? db(std::sqrt(outSquares/(2*n))) : -120.f,std::memory_order_relaxed);
        a.outLufsM.store(a.lufs.momentaryLufs(),std::memory_order_relaxed);
        a.peakIn.store(prePeak, std::memory_order_relaxed);
        a.peakOut.store(postPeak, std::memory_order_relaxed);
        a.frames.fetch_add(n, std::memory_order_relaxed);
        a.blocks.fetch_add(1, std::memory_order_relaxed);
    }
    void analyze() {
        constexpr unsigned size = 2048;
        std::array<float, 32768> pending{};
        std::array<float, size> pre{}, post{};
        unsigned cursor = 0;
        std::array<float, size/2+1> smoothPre, smoothPost;
        smoothPre.fill(-120);smoothPost.fill(-120);
        auto lastSpectrum=std::chrono::steady_clock::now();
        uint64_t lastBlocks = 0;
        std::vector<float> window(size, 1.f);
        host::Fft::hannWindow(window.data(), size);
        while (running) {
            const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            observing.store(nowMs - lastObserverMs.load() < 3000);
            auto count = ring.read(pending.data(), pending.size());
            for (size_t i = 0; i + 1 < count; i += 2) {
                pre[cursor] = pending[i]; post[cursor] = pending[i+1]; cursor = (cursor+1) % size;
            }
            const auto current = blocks.load();
            bool idle = current == lastBlocks;
            lastBlocks = current;
            if (idle) { pre.fill(0); post.fill(0); }
            const auto now=std::chrono::steady_clock::now();
            const float elapsed=std::chrono::duration<float,std::milli>(now-lastSpectrum).count();
            lastSpectrum=now;
            const float alpha=1.f-std::exp(-std::clamp(elapsed,1.f,100.f)/ui::widget_metrics::meter_runtime::kSpectrumFalloffTauMs);
            auto spectrum = [&](const auto& data, auto& smooth) {
                if (!observing.load()) return Json(std::vector<float>(size/2+1, -120.f));
                std::vector<std::complex<float>> fft(size);
                for (unsigned i = 0; i < size; ++i) fft[i] = data[(cursor+i)%size] * window[i];
                host::Fft::forward(fft);
                Json bins = Json::array();
                for (unsigned i = 0; i <= size/2; ++i) {
                    // Match the desktop analyzer's Hann scaling and release.
                    const float magnitude=std::abs(fft[i])*2.f/size;
                    const float db=magnitude>1e-7f ? 20.f*std::log10(magnitude) : -120.f;
                    smooth[i]=db>smooth[i] ? db : smooth[i]+alpha*(db-smooth[i]);
                    bins.push_back(smooth[i]);
                }
                return bins;
            };
            auto t = processor.telemetry();
            Json next = {{"input", spectrum(pre,smoothPre)}, {"output", spectrum(post,smoothPost)},
                {"peakIn", idle ? 0.f : peakIn.load()}, {"peakOut", idle ? 0.f : peakOut.load()},
                {"inPeakDbfs", {idle?-120.f:inPeakCh[0].load(),idle?-120.f:inPeakCh[1].load()}},
                {"outPeakDbfs", {idle?-120.f:outPeakCh[0].load(),idle?-120.f:outPeakCh[1].load()}},
                {"outLufsCh", {idle?-120.f:outLufsCh[0].load(),idle?-120.f:outLufsCh[1].load()}},
                {"outRmsDbfs", idle?-120.f:outRmsDbfs.load()}, {"outLufsM",idle?-120.f:outLufsM.load()},
                {"bandGrDb", t.bandGrDb},
                {"compression", t.compGrDb}, {"inputGain", t.levelerGainDb},
                {"outputGain", t.outputLevelerGainDb}, {"blocks", current},
                {"frames", frames.load()}, {"quantum", quantum.load()}, {"sampleRate", 48000},
                {"active", !idle}, {"spectralGain", t.spectralGainDb}};
            { std::lock_guard lock(meterMutex); meter = std::move(next); }
            std::this_thread::sleep_for(50ms);
        }
    }
    void serve() {
        server.Get("/api/volume", [&](const auto&, auto& res) {
            std::lock_guard lock(volumeMutex);
            res.set_content(volumeState().dump(), "application/json");
        });
        server.Post("/api/volume", [&](const auto& req, auto& res) {
            if (req.get_header_value("Content-Type").find("application/json") != 0) { res.status=415; return; }
            try {
                Json patch=Json::parse(req.body);
                if (!patch.is_object() || patch.empty()) throw std::invalid_argument("Expected volume or muted");
                for (auto it=patch.begin();it!=patch.end();++it) {
                    if (it.key()=="volume") {
                        if (!it.value().is_number()) throw std::invalid_argument("Volume must be numeric");
                        double v=it.value().get<double>();
                        if (!std::isfinite(v)||v<0||v>100) throw std::invalid_argument("Volume must be 0-100");
                    } else if (it.key()!="muted" || !it.value().is_boolean()) throw std::invalid_argument("Invalid volume field");
                }
                std::lock_guard lock(volumeMutex);
                Json next=volumeState(); next.update(patch);
                std::ofstream file("/data/volume.json.tmp"); file<<next.dump(); file.flush();
                if (!file) throw std::runtime_error("Could not save volume");
                file.close();
                if(std::rename("/data/volume.json.tmp","/data/volume.json")) throw std::runtime_error("Could not save volume");
                master.percent.store(next["volume"].get<float>()); master.muted.store(next["muted"].get<bool>());
                res.set_content(next.dump(),"application/json");
            } catch(const std::exception& e) { res.status=400; res.set_content(Json{{"error",e.what()}}.dump(),"application/json"); }
        });
        server.new_task_queue = [] { return new httplib::ThreadPool(4); };
        server.set_payload_max_length(16384);
        server.set_mount_point("/", "/app/web");
        server.set_pre_routing_handler([](const auto& req, auto& res) {
            if (req.path != "/") return httplib::Server::HandlerResponse::Unhandled;
            std::ifstream editor("/app/web/qt/index.html");
            res.set_redirect(editor ? "/qt/index.html" : "/index.html");
            return httplib::Server::HandlerResponse::Handled;
        });
        server.Get("/api/health", [&](const auto&, auto& res) {
            const bool connected = state.load() >= PW_FILTER_STATE_PAUSED;
            res.status = connected ? 200 : 503;
            res.set_content(Json{{"connected", connected}, {"blocks", blocks.load()}}.dump(), "application/json");
        });
        server.Get("/api/params", [&](const auto&, auto& res) {
            std::lock_guard lock(configMutex);
            res.set_content(Json{{"parameters", Processor::descriptors()}, {"values", values}}.dump(), "application/json");
        });
        server.Post("/api/params", [&](const auto& req, auto& res) {
            if (req.get_header_value("Content-Type").find("application/json") != 0) {
                res.status = 415; return;
            }
            try {
                Json patch = Json::parse(req.body); Processor::validate(patch);
                std::lock_guard lock(configMutex);
                Json next = values; next.update(patch); persist(next); values = std::move(next); refreshDesired();
                res.set_content(values.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 400; res.set_content(Json{{"error", e.what()}}.dump(), "application/json");
            }
        });
        server.Post("/api/leveler/relearn", [&](const auto& req, auto& res) {
            if (req.get_header_value("Content-Type").find("application/json") != 0) {
                res.status = 415; return;
            }
            try {
                const auto action = Json::parse(req.body);
                if (!action.is_object() || action.size() != 1 || !action.contains("stage")
                    || !action["stage"].is_string()
                    || (action["stage"] != "input" && action["stage"] != "output"))
                    throw std::invalid_argument("Expected stage: input or output");
                processor.relearnLeveler(action["stage"] == "output");
                res.set_content(Json{{"queued", true}}.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 400; res.set_content(Json{{"error", e.what()}}.dump(), "application/json");
            }
        });
        server.Get("/api/meters", [&](const auto&, auto& res) {
            lastObserverMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
            Json next;
            { std::lock_guard lock(meterMutex); next = meter; }
            try { std::ifstream route("/tmp/teedsp-route.json"); if (route) route >> next["route"]; } catch (...) {}
            res.set_header("Cache-Control", "no-store");
            res.set_content(next.dump(), "application/json");
        });
        if (!server.listen("0.0.0.0", 8080)) { std::cerr << "HTTP listen failed\n"; pw_main_loop_quit(loop); }
    }
};

int main(int argc, char** argv) {
    try {
        App app; app.load();
        pw_init(&argc, &argv);
        app.loop = pw_main_loop_new(nullptr);
        static const pw_filter_events events = [] {
            pw_filter_events e{}; e.version = PW_VERSION_FILTER_EVENTS;
            e.process = App::process;
            e.state_changed = [](void* user, pw_filter_state, pw_filter_state state, const char* error) {
                auto& a = *static_cast<App*>(user); a.state.store(state);
                if (state == PW_FILTER_STATE_ERROR) {
                    std::cerr << "PipeWire error: " << (error ? error : "unknown") << '\n';
                    pw_main_loop_quit(a.loop);
                }
            }; return e;
        }();
        app.filter = pw_filter_new_simple(pw_main_loop_get_loop(app.loop), "TeeDSP",
            pw_properties_new(PW_KEY_NODE_NAME, "teedsp", PW_KEY_NODE_DESCRIPTION, "TeeDSP",
                PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Filter", PW_KEY_MEDIA_ROLE, "DSP",
                "node.rate", "1/48000", "node.lock-rate", "true", nullptr), &events, &app);
        for (int ch = 0; ch < 2; ++ch) {
            const char* channel = ch == 0 ? "FL" : "FR";
            app.inputs[ch] = pw_filter_add_port(app.filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
                pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME,
                    ch == 0 ? "input_FL" : "input_FR", "audio.channel", channel, nullptr), nullptr, 0);
            app.outputs[ch] = pw_filter_add_port(app.filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_MAP_BUFFERS, 1,
                pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio", PW_KEY_PORT_NAME,
                    ch == 0 ? "output_FL" : "output_FR", "audio.channel", channel, nullptr), nullptr, 0);
        }
        if (pw_filter_connect(app.filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0)
            throw std::runtime_error("PipeWire connection failed");
        auto quit = [](void* user, int) { pw_main_loop_quit(static_cast<App*>(user)->loop); };
        pw_loop_add_signal(pw_main_loop_get_loop(app.loop), SIGINT, quit, &app);
        pw_loop_add_signal(pw_main_loop_get_loop(app.loop), SIGTERM, quit, &app);
        std::thread analyzer([&] { app.analyze(); });
        std::thread http([&] { app.serve(); });
        pw_main_loop_run(app.loop);
        app.running.store(false); app.server.stop(); analyzer.join(); http.join();
        pw_filter_destroy(app.filter); pw_main_loop_destroy(app.loop); pw_deinit();
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
