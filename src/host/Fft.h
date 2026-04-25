#pragma once

#include <vector>
#include <complex>

namespace host {

// Minimal in-place radix-2 FFT. Twiddle factors are recomputed on every call —
// for our use (one or two FFTs per UI tick) the cost is irrelevant.
//
// Length must be a power of two. Pass real input through realToComplex() and
// then fft() in-place; magnitudes are read from the lower half of the buffer.
class Fft
{
public:
    static bool isPowerOfTwo(int n) { return n > 0 && (n & (n - 1)) == 0; }

    // In-place forward FFT. data.size() must be a power of two.
    static void forward(std::vector<std::complex<float>> &data);

    // Helper: copy a real signal into a complex buffer for forward().
    static void realToComplex(const float *src, int count,
                              std::vector<std::complex<float>> &dst);

    // Apply a Hann window in place — squashes spectral leakage from the
    // implicit rectangular framing of a finite buffer.
    static void hannWindow(float *data, int count);
};

} // namespace host
