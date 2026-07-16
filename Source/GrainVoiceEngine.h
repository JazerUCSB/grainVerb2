#pragma once

#include <vector>
#include <juce_core/juce_core.h>
#include "GrainReverbSharedState.h"

// One voice in a 200-voice grain bank. Both banks now use every field --
// Bank 2 (grains2) originally had no filter per the PDF spec, but now
// applies the same per-grain cutoff/Q lowpass Bank 1 does (using its own
// dn = read/del2Len, since bank 2 has no readScatter-scaled span), so the
// cutoff/Q curves genuinely shape the final output, not just Bank 1's
// internal diffuse wash. Bank 2 still does NOT apply the tail decay curve
// -- that stays Bank 1-only, unchanged from the original design.
//
// When an engine is prepared with singleBufferDualFeedback (currently just
// earlyEngine), "del2Len"/"del2" in Bank 2's code paths below actually mean
// del1/del1Len -- see GrainVoiceEngine::prepare().
struct Grain
{
    double read = 0.0;   // distance behind the write head, in samples
    double rate = 1.0;   // playback rate (1 + jitter)
    double dur  = 0.0;   // grain duration, in samples
    double age  = 0.0;   // samples elapsed since (re)spawn
    double sign = 1.0;   // +1 / -1 polarity

    // Which del1/del2 channel this grain reads its source material from,
    // chosen randomly 50/50 at spawn -- ALWAYS random, independent of
    // dispersion.
    int readChannel = 0; // 0 = left buffer, 1 = right buffer

    // Which channel this grain's output is written to -- a HARD, discrete
    // choice (not a continuous pan blend). dispersion controls the
    // probability of flipping away from readChannel: at dispersion=0,
    // writeChannel always equals readChannel (a grain's output stays on
    // the side it came from); at dispersion=1, the flip probability is 0.5,
    // which makes writeChannel a fair coin flip independent of readChannel.
    // See GrainVoiceEngine::chooseWriteChannel().
    int writeChannel = 0;

    // RBJ lowpass coefficients, frozen at spawn (nearest-neighbor lookup
    // into CoeffTable at spawn time). Bank 2 grains leave these at 0/unused.
    double b0 = 0.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0; // TDF-II filter state, reset to 0 at spawn
};

// One-pole DC blocker: y[n] = x[n] - x[n-1] + R*y[n-1]. gen~'s dcblock()
// operator doesn't expose its internal coefficient, so R=0.995 is a
// standard stand-in that removes DC/sub-sonic buildup without audibly
// coloring the signal. The PDF (Section 6) only requires that each call
// site get its own independent instance/state -- which R is used matters
// far less than not sharing state between call sites.
struct DcBlocker
{
    double x1 = 0.0, y1 = 0.0;

    double process (double x)
    {
        constexpr double R = 0.995;
        const double y = x - x1 + R * y1;
        x1 = x;
        y1 = y;
        return y;
    }
};

// The ONE granular reverb engine (no longer one instance per channel). Owns
// genuinely stereo del1/del2 buffers and one shared pool of grains per bank;
// stereo width comes entirely from each grain's independent readChannel/
// writeChannel choices, not from routing separate mono engines.
//
// Voice counts and buffer capacities are configurable (via prepare()'s
// extra parameters, defaulted to the original late-reflections values) so
// the SAME class can be reused for a second, smaller early-reflections
// engine (much shorter buffers, far fewer voices) without duplicating any
// of this logic -- see PluginProcessor's lateEngine/earlyEngine.
class GrainVoiceEngine
{
public:
    GrainVoiceEngine();

    // Allocates del1L/R + grain arrays (+ del2L/R, unless
    // singleBufferDualFeedback) and seeds them from shared's current
    // tables. Must be called after shared.prepare(). numVoices1/2 and
    // del1/2MaxSeconds default to the original late-reflections sizing;
    // pass smaller values to configure a different-scaled engine (e.g.
    // early reflections).
    //
    // numVoices1/2 are the MAXIMUM voices allocated per bank, not
    // necessarily how many contribute to the output on a given sample --
    // shared.params.numGrainVoices (live-adjustable, no reallocation) picks
    // a smaller ACTIVE subset each sample, the same "allocate max, scale
    // the active portion live" pattern del1Len already uses relative to
    // del1L's fixed 6-second capacity.
    //
    // singleBufferDualFeedback (default false, preserving the original
    // two-buffer topology for lateEngine): when true, del2 is never
    // allocated at all -- Bank 2 (grains2) instead reads/respawns against
    // del1/del1Len/cnt1, i.e. the SAME buffer Bank 1 writes into. Both
    // banks' outputs (aud1, aud2) are combined into that one write via
    // fb1 (GrainReverbParams::fb) and fb2 (GrainReverbParams::fb2)
    // respectively; the separate del2 write is skipped entirely. This is
    // the earlyEngine's "one buffer, two independently-fed-back grain
    // banks" design -- see PluginProcessor::prepareToPlay().
    void prepare (double newSampleRate, const GrainReverbSharedState& shared,
                  int numVoices1 = 200, int numVoices2 = 200,
                  double del1MaxSeconds = 6.0, double del2MaxSeconds = 1.0,
                  bool singleBufferDualFeedback = false);

    // Runs one sample of the full gen~ signal chain, now stereo in/out.
    void processSample (double inputL, double inputR, const GrainReverbSharedState& shared,
                         double& outputL, double& outputR);

    // Read-only access for the visualizer (Step 6), called from the message
    // thread's repaint timer while the audio thread concurrently writes.
    // Deliberately-accepted, effectively benign data race -- see the
    // reasoning in the original Step 6 comment: buffer SIZE never changes
    // after prepare(), nothing here feeds back into DSP, and at worst a
    // torn read shows one visually-wrong sample for a single ~33ms frame.
    const std::vector<double>& getDelayBuffer1 (int channel) const { return channel == 0 ? del1L : del1R; }
    // Empty (size 0) and count2 frozen at 0 when this engine was prepared
    // with singleBufferDualFeedback -- there's no second buffer to show.
    const std::vector<double>& getDelayBuffer2 (int channel) const { return channel == 0 ? del2L : del2R; }
    double getWriteHead1() const { return count1; }
    double getWriteHead2() const { return count2; }

    // This engine's configured del1 capacity in seconds (6.0 for the
    // original late-reflections sizing, smaller for e.g. early reflections)
    // -- visualizer/curve-editor components need this to convert del1Len
    // (bufferLenMs as a fraction of THIS engine's own capacity) correctly,
    // since it's no longer a hardcoded 6 seconds.
    double getDel1MaxSeconds() const { return del1MaxSeconds; }

private:
    void seedGrains (const GrainReverbSharedState& shared);

    // ---- small math helpers mirroring the gen~ vocabulary ----
    double noise();                 // uniform [-1, 1]
    double mstosamps (double ms) const;

    // Euclidean-modulo wrap into [lo, hi). Every call site in this class
    // passes a v that's already within roughly one range-width of [lo, hi),
    // so a bounded while-loop of cheap branches beats a division-based
    // std::fmod -- see the original Step 3 fix, which resolved a real
    // audio-thread real-time overload caused by fmod's overhead here.
    static double wrapValue (double v, double lo, double hi);

    // activeLen is the ACTIVE portion of buf that's actually kept in the
    // read/write loop (del1Len or del2Len) -- NOT buf.size(), the buffer's
    // full allocated capacity. Those two only coincide when Read Range
    // is at its maximum. Interpolating past the last active sample must
    // wrap its neighbor back to index 0 of the ACTIVE region, not index 0
    // of the full buffer via buf.size() -- getting this wrong means the
    // interpolation neighbor at the boundary is STALE data (zero-filled,
    // or leftover from a previous, larger Read Range setting) that was
    // never touched by pokeWrite (which only ever writes within
    // [0, activeLen)), producing a periodic (once per active-loop cycle)
    // discontinuity right at that boundary -- a glitch that only
    // disappears when activeLen == buf.size() exactly, i.e. Read Range
    // at its maximum, which is exactly the reported symptom.
    static double peekLinear (const std::vector<double>& buf, double pos, double activeLen);
    static void pokeWrite (std::vector<double>& buf, double pos, double val);

    // A grain's read anchor (pos = writeHead - read, at spawn) does NOT
    // drift with the write head, but its actual read pointer
    // (pos + age*rate) does -- catching up to the write head if rate>1,
    // or falling further behind (toward the OTHER end, spanLen) if
    // rate<1. If `read` starts too close to either 0 or spanLen, the
    // write head can catch/lap the grain mid-life, causing a sudden jump
    // from old buffer content to freshly-written (or stale, lapped)
    // content -- an audible glitch. This reserves a margin, sized to this
    // grain's own worst-case drift over its lifetime (dur * |rate-1|), on
    // BOTH ends so the write head can never reach the grain's read
    // pointer before it respawns. Must be called with dur/rate already
    // assigned (margin depends on both), and spanLen = del1Len for Bank 1
    // or del2Len for Bank 2 -- NOT readSpan, since the wrap boundary is
    // the buffer's active length, not the (possibly smaller) scatter-
    // limited draw range.
    //
    // predelaySamples pushes the LOWER bound further up, on top of
    // (never smaller than) that same collision margin: no grain may
    // spawn reading closer to the write head than predelaySamples allows,
    // creating a genuine silent gap before anything is audible -- the
    // GrainReverbParams::predelayMs control, shared by both engines. An
    // oversized predelay (bigger than spanLen can accommodate) is capped
    // rather than inverting the range, collapsing every grain toward the
    // far/oldest end instead -- same graceful-degradation spirit as the
    // margin cap already had.
    static double clampReadAgainstWriteHead (double rawRead, double dur, double rate, double spanLen, double predelaySamples);

    // Floor for a freshly-(re)spawned grain's raw read distance, used
    // before clampReadAgainstWriteHead's own margin is applied. A flat
    // 5-sample floor (the original constant here) is meaningless once
    // span shrinks to a couple thousand samples (a 50ms/2205-sample early
    // buffer): it lets a grain spawn practically AT the write head --
    // read = a few samples, i.e. a near-zero-delay, near-unity-gain
    // feedback tap (minimal tail attenuation and, with default curves,
    // minimal filtering, since both are keyed to dn = read/span, and
    // dn~0). With many grains all feeding back into the SAME buffer
    // (singleBufferDualFeedback) or even just del1's own single-fb loop,
    // enough near-zero-delay taps add up to a comb-filter/resonator whose
    // fundamental sits at 1/bufferLength -- e.g. 20Hz for a 50ms buffer,
    // right at the edge of pitch/click perception, which is what reads as
    // periodic "clicking" locked to buffer length. Scaling the floor to a
    // fraction of span keeps every engine's shortest possible tap
    // meaningfully non-zero regardless of how short its buffer is.
    static double minSpawnRead (double span);

    // Flips away from readChannel with probability dispersion/2 -- see the
    // Grain::writeChannel comment for why that specific formula gives
    // "always matches read channel" at dispersion=0 and "fair coin flip"
    // at dispersion=1.
    int chooseWriteChannel (int readChannel, double dispersion);

    double sampleRate = 44100.0;

    // Output highpass coefficient, exp(-2*pi*300/sampleRate). Computed once
    // in prepare() (depends only on sampleRate), not per sample.
    double hpCoeffG = 0.0;

    // Configured by prepare()'s del1MaxSeconds param -- del1Len (the
    // *active* wrap boundary within del1's fixed storage) is bufferLenMs
    // as a fraction of THIS, not a hardcoded 6 seconds. del2 has no
    // equivalent scaling: del2Len is always del2L.size() outright (del2's
    // "bufferLenMs" is effectively fixed at del2MaxSeconds*1000).
    double del1MaxSeconds = 6.0;

    // del1Len doubles as a WRAP MODULUS -- cnt1 (the write head) and every
    // grain's read anchor get folded through wrapValue(..., del1Len) every
    // sample. Deriving del1Len straight from the live bufferLenMs dial each
    // sample (as processSample() used to) means moving that dial instantly
    // changes the modulus mid-note: any cnt1/read value currently above
    // the new (smaller) del1Len gets wrapped to an essentially unrelated
    // position by modulo arithmetic -- an audible click, worst right at
    // the wrap boundary (that's the "values at the very end of the
    // visualizer" -- the CircularBufferVisualizer computes the same
    // del1Len formula independently to decide how much of the buffer to
    // draw, so a relocated read/write position shows up there too).
    // del1LenState glides toward the raw target by del1LenSmoothCoeff each
    // sample instead (both set in prepare()), so any single sample's
    // change in the modulus is tiny enough to be inaudible. Only del1Len
    // needs this: del2Len is either permanently fixed (lateEngine) or
    // aliased to del1Len/del1LenState (singleBufferDualFeedback), never an
    // independent live modulus of its own.
    double del1LenState = 0.0;
    double del1LenSmoothCoeff = 0.0;

    // Set once by prepare(); see its doc comment above. Read throughout
    // seedGrains()/processSample() to redirect Bank 2's buffer/length/
    // write-head references from del2/del2Len/cnt2 onto del1/del1Len/cnt1.
    bool singleBufferDualFeedback = false;

    // Genuinely stereo now: 2 channels each, allocated at full capacity
    // (del1MaxSeconds / del2MaxSeconds seconds) in prepare() and never
    // resized again (del2L/del2R stay empty when singleBufferDualFeedback).
    // bufferLenMs only ever changes del1Len, the *active* wrap boundary
    // within that fixed storage.
    std::vector<double> del1L, del1R, del2L, del2R;

    // Sized once in prepare() (from numVoices1/2), never resized again --
    // a std::vector here is exactly as real-time-safe as the std::array it
    // replaces, since nothing ever pushes/pops/reallocates it after that.
    std::vector<Grain> grains1;
    std::vector<Grain> grains2;

    double count1 = 0.0, count2 = 0.0; // write heads (History in gen~), shared by both channels

    // Output highpass state -- one instance per channel (was one total per
    // mono engine; now two channels' worth of state live in this one
    // merged engine).
    double hpXL = 0.0, hpYL = 0.0, hpXR = 0.0, hpYR = 0.0;

    // DC blockers -- one per call site PER CHANNEL (aud1, aud2, del1 write,
    // del2 write) x 2 channels = 8 total. Sharing one instance across sites
    // or channels would mix unrelated signals into one state variable.
    DcBlocker dcAud1L, dcAud1R, dcAud2L, dcAud2R;
    DcBlocker dcWrite1L, dcWrite1R, dcWrite2L, dcWrite2R;

    // Tracks the previous meanWindowMs/windowRangeMs (in samples) so we can
    // detect a change and re-randomize durations, matching gen~'s delta()
    // operator (Section 6 translation notes).
    double prevMeanWindowSamps = 0.0, prevWindowRangeSamps = 0.0;

    juce::Random rng;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrainVoiceEngine)
};
