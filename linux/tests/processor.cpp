#include "Processor.h"
#include "MasterVolume.h"
#include "dsp/LufsMeter.h"
#include <algorithm>
#include <iostream>
#include <limits>
#include <vector>

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* name) { std::cout << (ok ? "PASS " : "FAIL ") << name << '\n'; if (!ok) ++failures; };
    Processor p;
    check(Processor::descriptors().size() == 69, "all existing plugin parameters exposed");
    for (Json invalid : {Json{{"bogus", 1}}, Json{{"0", 0.5}}, Json{{"1", 25}}, Json{{"1", "3"}}, Json::array()}) {
        bool rejected = false;
        try { Processor::validate(invalid); } catch (...) { rejected = true; }
        check(rejected, "invalid API parameter rejected");
    }
    std::vector<float> left(Processor::maxFrames), right(left.size()), outL(left.size()), outR(left.size());
    for (size_t i = 0; i < left.size(); ++i) { left[i] = 0.1f * std::sin(i * 0.09f); right[i] = -left[i]; }
    p.set(0, 1);
    for (unsigned n : {32u, 256u, 1024u, Processor::maxFrames}) {
        p.process(left.data(), right.data(), outL.data(), outR.data(), n);
        check(std::equal(left.begin(), left.begin()+n, outL.begin()) && std::equal(right.begin(), right.begin()+n, outR.begin()), "bypass preserves stereo samples across quantum sizes");
    }
    p.set(0, 0); p.set(6, 0); p.set(7, 0); p.set(14, 0); p.set(1, -6.020599913);
    // The spectral leveler deliberately crossfades its bypass after activation.
    for (int i=0;i<100;++i) p.process(left.data(), right.data(), outL.data(), outR.data(), 1024);
    float error = 0;
    for (int i=0;i<1024;++i) error = std::max(error, std::abs(outL[i] - left[i]*0.5f));
    check(error < 1e-6f, "parameter events control the real DSP chain");
    p.set(1, 0); p.set(3, 0);
    p.process(left.data(), right.data(), outL.data(), outR.data(), 1024);
    error = 0; for (int i=0;i<1024;++i) error = std::max({error, std::abs(outL[i]), std::abs(outR[i])});
    check(error < 1e-6f, "mono width cancels opposite stereo signals");
    MasterVolume master;
    master.percent=50;
    std::fill(outL.begin(),outL.end(),1); std::fill(outR.begin(),outR.end(),1);
    master.process(outL.data(),outR.data(),1024);
    check(std::abs(outL[1023]-0.3162277f)<1e-6f,"50 percent is minus 10 dB after ramp");
    master.muted=true;
    std::fill(outL.begin(),outL.end(),1); std::fill(outR.begin(),outR.end(),1);
    master.process(outL.data(),outR.data(),1024);
    check(outL[1023]==0 && outR[1023]==0,"master mute reaches exact silence");
    p.set(0,1); master.muted=false;master.percent=25;
    p.process(left.data(),right.data(),outL.data(),outR.data(),1024);
    master.process(outL.data(),outR.data(),1024);
    check(std::abs(outL[1023]-left[1023]*0.1f)<1e-6f,"master volume remains active with DSP bypassed");
    p.set(0,0);p.set(3,1);p.set(7,1);p.set(9,1);p.set(13,-6.020599913);
    p.process(left.data(),right.data(),outL.data(),outR.data(),1024);
    check(std::abs(outL[1023]-left[1023]*0.5f)<1e-6f,
          "negative compressor makeup matches Qt editor range");
    Processor::validate(Json{{"13",-12},{"123",20}});
    dsp::LufsMeter loudness;
    loudness.prepare(48000,2);
    std::vector<float> stereo(48000*2);
    for(int i=0;i<48000;++i) {
        stereo[i*2]=0.1f*std::sin(2*3.141592653589793*1000*i/48000);
        stereo[i*2+1]=stereo[i*2]*0.5f;
    }
    loudness.process(stereo.data(),48000,2);
    check(std::abs(loudness.channelLufs(0)-loudness.channelLufs(1)-6.0206f)<0.02f,
          "LUFS preserves stereo channel level differences");
    check(std::abs(loudness.momentaryLufs()-loudness.channelLufs(0)-0.9691f)<0.02f,
          "combined LUFS sums channel power");
    loudness.process(nullptr,48000,2);
    check(loudness.channelLufs(0)<-100 && loudness.channelLufs(1)<-100,
          "LUFS settles to silence after playback stops");
    return failures ? 1 : 0;
}
