#pragma once

#include <algorithm>
#include <cmath>

#if defined(__SSE__) || defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace sonar::dsp {

// The floor for the logarithm, which has no answer at zero.
inline constexpr float kTinyLinear = 1e-6f;

[[nodiscard]] inline float dbToLin(float db) { return std::pow(10.0f, db * 0.05f); }

[[nodiscard]] inline float linToDb(float lin) {
    return 20.0f * std::log10(std::max(std::abs(lin), kTinyLinear));
}

// One-pole smoothing coefficient for a time constant in milliseconds. Zero (or
// negative) means "immediately", which is what a 0 ms attack has to mean.
[[nodiscard]] inline float envCoef(float ms, float sampleRate) {
    return ms <= 0.0f ? 0.0f : std::exp(-1.0f / (ms * 0.001f * sampleRate));
}

// Flushes denormals to zero for the scope it lives in, then restores the
// previous mode. Filters and envelopes decaying through silence otherwise land in
// the denormal range (below ~1e-38), which x86 computes about a hundred times
// slower; flushing changes nothing audible. Restored on exit because the
// realtime thread is shared with code that is not ours.
class ScopedFlushDenormals {
public:
#if defined(__SSE__) || defined(__x86_64__)
    ScopedFlushDenormals() : saved_(_mm_getcsr()) { _mm_setcsr(saved_ | kFtzDaz); }
    ~ScopedFlushDenormals() { _mm_setcsr(saved_); }
#else
    ScopedFlushDenormals() = default;  // other architectures: nothing to do
#endif
    ScopedFlushDenormals(const ScopedFlushDenormals&) = delete;
    ScopedFlushDenormals& operator=(const ScopedFlushDenormals&) = delete;

#if defined(__SSE__) || defined(__x86_64__)
private:
    static constexpr unsigned kFtzDaz = 0x8040;  // flush-to-zero | denormals-are-zero
    unsigned saved_;
#endif
};

}  // namespace sonar::dsp
