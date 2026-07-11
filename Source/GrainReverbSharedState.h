#pragma once

#include <array>
#include <atomic>
#include <juce_core/juce_core.h>
#include "BreakpointCurve.h"

// Dense, audio-thread-readable form of the three user-facing curves. All
// three share the dn index space defined in BreakpointCurve.h.
struct Curves
{
    std::array<double, kNumTableSlots> cutoff {}; // fc (Hz) per slot
    std::array<double, kNumTableSlots> q {};      // Q per slot
    std::array<double, kNumTableSlots> tail {};   // decay gain per slot
};

// Derived from Curves::cutoff + Curves::q off the audio thread; grains do a
// nearest-neighbor lookup into this at spawn and copy the 5 coefficients
// into their own state, frozen for the grain's life (no per-sample trig).
struct CoeffTable
{
    std::array<std::array<double, 5>, kNumTableSlots> coeff {}; // b0,b1,b2,a1,a2
};

// Plain-value snapshot of every scalar (non-curve) parameter both channels
// read. PluginProcessor copies APVTS values into this once per processBlock
// (Step 4's syncParams).
struct GrainReverbParams
{
    double fadeSamps     = 200.0;
    double meanWindowMs  = 200.0;
    double windowRangeMs = 50.0;
    double bufferLenMs   = 4000.0;
    double fb            = 0.5;
    double readScatter   = 0.9;
    double jitter        = 0.0;

    // Not part of the gen~ patch (which is mono) -- this is our own control
    // for per-grain output placement (GrainVoiceEngine::computePanGains):
    // each grain's pan = noise() * dispersion at spawn, so 0 = every grain
    // centered/mono, 1 = grains scatter across the full stereo field. This
    // used to be a top-level "swap L/R engines" control; it's now applied
    // per grain inside the single merged engine (see GrainVoiceEngine.h).
    double dispersion    = 1.0;

    // How many of this engine's ALLOCATED grain voices (per bank) actually
    // contribute to the output right now -- live-adjustable without any
    // audio-thread reallocation, since GrainVoiceEngine always allocates
    // grains1/grains2 at their configured MAXIMUM size (see prepare()) and
    // just gates output contribution by this count. See
    // GrainVoiceEngine::processSample() for why the age/read/respawn
    // bookkeeping still runs for every allocated grain regardless of this
    // value -- skipping it would desync a grain's read anchor from the
    // write head while inactive.
    double numGrainVoices = 100.0;
};

// Shared, control-rate state: the three user-editable breakpoint curves,
// the dense tables baked from them, and the scalar parameters. The single
// GrainVoiceEngine reads this every sample; it never owns it, and never
// writes to it -- only prepare() (here, at startup) and later the
// message-thread rebake (Step 5) do.
class GrainReverbSharedState
{
public:
    GrainReverbSharedState();

    // Bakes both halves of the double buffer. Must be called before either
    // voice engine seeds its grains -- a grain's first coefficient lookup
    // needs a valid table to read from.
    void prepare (double newSampleRate);

    // Call this from the MESSAGE THREAD ONLY, after mutating cutoffCurve/
    // qCurve/tailCurve directly (e.g. dragging a breakpoint). Bakes into
    // whichever storage half ISN'T currently live, then atomically swaps
    // both pointers to point at it. The audio thread only ever reads
    // liveCurves/liveCoeffs -- it never sees a half-written table, and this
    // never allocates (both halves were sized once, in prepare()).
    void rebake();

    const CoeffTable* getLiveCoeffs() const { return liveCoeffs.load (std::memory_order_acquire); }
    const Curves* getLiveCurves() const { return liveCurves.load (std::memory_order_acquire); }

    BreakpointCurve cutoffCurve, qCurve, tailCurve;
    GrainReverbParams params;

private:
    void bakeTablesInto (Curves& curves, CoeffTable& coeffs) const;

    double sampleRate = 44100.0;

    // Double-buffered so publishing an edit (Step 5) never allocates:
    // liveCurves/liveCoeffs point at whichever half is currently "live";
    // a rebake fills the other half first, then atomically swaps the
    // pointer -- readers either see the whole old table or the whole new
    // one, never a half-written mix.
    Curves curvesStorage[2];
    CoeffTable coeffStorage[2];
    std::atomic<Curves*> liveCurves { nullptr };
    std::atomic<CoeffTable*> liveCoeffs { nullptr };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrainReverbSharedState)
};
