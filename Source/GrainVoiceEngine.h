#pragma once

#include <array>
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
// genuinely stereo del1/del2 buffers and one shared pool of 100 grains per
// bank; stereo width comes entirely from each grain's independent
// readChannel/writeChannel choices, not from routing separate mono engines.
// Voices are doubled to 200/bank (kNumVoices1/2) so total grain density
// matches the old two-engine design's 100-per-channel x 2 = 200 total.
class GrainVoiceEngine
{
public:
    GrainVoiceEngine();

    // Allocates del1L/R + del2L/R + grain arrays and seeds them from
    // shared's current tables. Must be called after shared.prepare().
    void prepare (double newSampleRate, const GrainReverbSharedState& shared);

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
    const std::vector<double>& getDelayBuffer2 (int channel) const { return channel == 0 ? del2L : del2R; }
    double getWriteHead1() const { return count1; }
    double getWriteHead2() const { return count2; }

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
    static double peekLinear (const std::vector<double>& buf, double pos);
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
    static double clampReadAgainstWriteHead (double rawRead, double dur, double rate, double spanLen);

    // Flips away from readChannel with probability dispersion/2 -- see the
    // Grain::writeChannel comment for why that specific formula gives
    // "always matches read channel" at dispersion=0 and "fair coin flip"
    // at dispersion=1.
    int chooseWriteChannel (int readChannel, double dispersion);

    double sampleRate = 44100.0;

    // Output highpass coefficient, exp(-2*pi*300/sampleRate). Computed once
    // in prepare() (depends only on sampleRate), not per sample.
    double hpCoeffG = 0.0;

    // Genuinely stereo now: 2 channels each, allocated at full capacity
    // (6s / 1s) in prepare() and never resized again. bufferLenMs only
    // ever changes del1Len, the *active* wrap boundary within that fixed
    // storage.
    std::vector<double> del1L, del1R, del2L, del2R;

    static constexpr int kNumVoices1 = 200; // was 100/engine x 2 engines
    static constexpr int kNumVoices2 = 200; // -- doubled here to match total density
    std::array<Grain, kNumVoices1> grains1;
    std::array<Grain, kNumVoices2> grains2;

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
