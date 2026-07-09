#include "GrainReverbSharedState.h"
#include <cmath>

GrainReverbSharedState::GrainReverbSharedState()
{
    // Default curve state -- your keymapping spec: two fixed-x breakpoints
    // at the buffer edges. The curve itself doesn't enforce that the edges
    // stay put horizontally; the editor component (Step 7) does.
    cutoffCurve.points = { { 0.0, 10000.0, CurveInterpolation::Linear },
                            { 1.0, 500.0,  CurveInterpolation::Linear } };

    qCurve.points = { { 0.0, 0.707, CurveInterpolation::Linear },
                       { 1.0, 0.707, CurveInterpolation::Linear } };

    // Matches the old gen~ formula's endpoints (k = 10 - tail, tail = 4 ->
    // k = 6): exp(-6*0) = 1.0, exp(-6*1) ~= 0.00248. curvature = -6.0 here
    // is not arbitrary -- under the exponential-ease bend formula in
    // BreakpointCurve::evaluate(), (exp(k*t)-1)/(exp(k)-1) with k=-6
    // reduces algebraically to exactly (1-exp(-6t))/(1-exp(-6)), which
    // reproduces this exact exp(-6t) decay shape once blended between
    // y0=1.0 and y1=exp(-6). The second point's segmentType/curvature are
    // unused (no segment follows the last point).
    tailCurve.points = { { 0.0, 1.0,              CurveInterpolation::Exponential, -6.0 },
                          { 1.0, std::exp (-6.0), CurveInterpolation::Linear,       4.0 } };
}

void GrainReverbSharedState::prepare (double newSampleRate)
{
    sampleRate = newSampleRate;

    // Bake both halves identically so it doesn't matter which one
    // liveCurves/liveCoeffs starts pointing at.
    bakeTablesInto (curvesStorage[0], coeffStorage[0]);
    bakeTablesInto (curvesStorage[1], coeffStorage[1]);
    liveCurves.store (&curvesStorage[0], std::memory_order_release);
    liveCoeffs.store (&coeffStorage[0], std::memory_order_release);
}

void GrainReverbSharedState::rebake()
{
    // Bake into whichever half ISN'T currently live -- readers on the audio
    // thread keep seeing the old (still fully valid) table until the stores
    // below make the new one visible.
    const auto* liveC = liveCurves.load (std::memory_order_relaxed);
    const int backIndex = (liveC == &curvesStorage[0]) ? 1 : 0;

    bakeTablesInto (curvesStorage[backIndex], coeffStorage[backIndex]);

    liveCurves.store (&curvesStorage[backIndex], std::memory_order_release);
    liveCoeffs.store (&coeffStorage[backIndex], std::memory_order_release);
}

void GrainReverbSharedState::bakeTablesInto (Curves& curves, CoeffTable& coeffs) const
{
    // Render each breakpoint curve down to its dense 512-slot table.
    cutoffCurve.bakeInto (curves.cutoff);
    qCurve.bakeInto (curves.q);
    tailCurve.bakeInto (curves.tail);

    // ---- cutoff + Q -> baked RBJ lowpass coeffs, one slot at a time ----
    for (int j = 0; j < kNumTableSlots; ++j)
    {
        // Clamp in place so the visualizer (which reads curves.cutoff
        // directly) and the coefficient bake below always agree on the
        // actual value in effect.
        const double fc = juce::jmin (curves.cutoff[(size_t) j], sampleRate * 0.45);
        curves.cutoff[(size_t) j] = fc;

        const double q = curves.q[(size_t) j];

        const double w0 = juce::MathConstants<double>::twoPi * fc / sampleRate;
        const double cs = std::cos (w0);
        const double sn = std::sin (w0);
        const double alpha = sn / (2.0 * q);
        const double a0i = 1.0 + alpha;

        auto& c = coeffs.coeff[(size_t) j];
        c[0] = ((1.0 - cs) * 0.5) / a0i; // b0
        c[1] = (1.0 - cs) / a0i;         // b1
        c[2] = ((1.0 - cs) * 0.5) / a0i; // b2
        c[3] = (-2.0 * cs) / a0i;        // a1
        c[4] = (1.0 - alpha) / a0i;      // a2
    }
}
