#include "Fft.h"

#include <cmath>

namespace host {

namespace {
constexpr double kPi = 3.14159265358979323846;
}

void Fft::realToComplex(const float *src, int count,
                        std::vector<std::complex<float>> &dst)
{
    dst.resize(count);
    for (int i = 0; i < count; ++i)
        dst[i] = std::complex<float>(src[i], 0.0f);
}

void Fft::hannWindow(float *data, int count)
{
    if (count < 2) return;
    const double denom = static_cast<double>(count - 1);
    for (int i = 0; i < count; ++i) {
        const double w = 0.5 * (1.0 - std::cos(2.0 * kPi * i / denom));
        data[i] *= static_cast<float>(w);
    }
}

void Fft::forward(std::vector<std::complex<float>> &data)
{
    const int n = static_cast<int>(data.size());
    if (!isPowerOfTwo(n)) return;

    // Bit-reversal permutation.
    int j = 0;
    for (int i = 1; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(data[i], data[j]);
    }

    // Cooley-Tukey decimation-in-time.
    for (int len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / len;
        const std::complex<float> wlen(static_cast<float>(std::cos(ang)),
                                       static_cast<float>(std::sin(ang)));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (int k = 0; k < len / 2; ++k) {
                const std::complex<float> u = data[i + k];
                const std::complex<float> v = data[i + k + len / 2] * w;
                data[i + k]             = u + v;
                data[i + k + len / 2]   = u - v;
                w *= wlen;
            }
        }
    }
}

} // namespace host
