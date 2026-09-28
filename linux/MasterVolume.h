#pragma once
#include <atomic>
#include <cmath>

// Independent of plugin bypass. Ten-millisecond ramps avoid slider clicks.
struct MasterVolume {
    std::atomic<float> percent{0};
    std::atomic<bool> muted{false};
    float current = 0, previousTarget = 0, step = 0;
    unsigned remaining = 0;
    static float gain(float value) { return value <= 0 ? 0 : std::pow(value / 100.f, 1.660964f); }
    void process(float* left, float* right, unsigned frames) {
        float target = muted.load() ? 0 : gain(percent.load());
        if (target != previousTarget) {
            remaining = 480; step = (target-current)/remaining; previousTarget = target;
        }
        for (unsigned i=0;i<frames;++i) {
            if (remaining && --remaining) current += step;
            else current = target;
            left[i] *= current; right[i] *= current;
        }
    }
};
