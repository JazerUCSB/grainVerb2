#include "GrainVoiceEngine.h"
#include <cmath>

GrainVoiceEngine::GrainVoiceEngine()
{
    rng.setSeedRandomly();
}

double GrainVoiceEngine::noise()
{
    return rng.nextDouble() * 2.0 - 1.0;
}

double GrainVoiceEngine::mstosamps (double ms) const
{
    return ms * sampleRate / 1000.0;
}

double GrainVoiceEngine::wrapValue (double v, double lo, double hi)
{
    const double range = hi - lo;
    if (range <= 0.0)
        return lo;

    v -= lo;
    while (v < 0.0)
        v += range;
    while (v >= range)
        v -= range;
    return v + lo;
}

// gen~'s peek(buf, pos, interp="linear"): fractional read between the
// floor and ceil sample, wrapping at the ACTIVE region's end (activeLen),
// NOT buf.size() -- see the header comment for why those two differ
// whenever Read Range is below its maximum, and what went wrong when
// this wrapped against buf.size() instead.
double GrainVoiceEngine::peekLinear (const std::vector<double>& buf, double pos, double activeLen)
{
    pos = wrapValue (pos, 0.0, activeLen);
    const auto i0 = (size_t) pos;
    const size_t i1 = (i0 + 1 >= (size_t) activeLen) ? 0 : i0 + 1;
    const double frac = pos - (double) i0;
    return buf[i0] + frac * (buf[i1] - buf[i0]);
}

// gen~'s poke(buf, val, idx): unconditional overwrite (no overdub blend --
// see Step 6's removal of the ovrDub parameter). Write positions (cnt1/
// cnt2) are always integral, so unlike peek() this never needs
// interpolation.
void GrainVoiceEngine::pokeWrite (std::vector<double>& buf, double pos, double val)
{
    const auto idx = (size_t) wrapValue (pos, 0.0, (double) buf.size());
    buf[idx] = val;
}

int GrainVoiceEngine::chooseWriteChannel (int readChannel, double dispersion)
{
    const bool flip = rng.nextDouble() < 0.5 * juce::jlimit (0.0, 1.0, dispersion);
    return flip ? (1 - readChannel) : readChannel;
}

double GrainVoiceEngine::clampReadAgainstWriteHead (double rawRead, double dur, double rate, double spanLen, double predelaySamples)
{
    const double wantedMargin = dur * std::abs (rate - 1.0);
    // Cap the margin at just under half of spanLen so [margin, spanLen-margin]
    // can never invert -- an extreme combination (very long grain, high
    // jitter, short buffer) could otherwise ask for more margin than the
    // buffer has. In that extreme case every grain collapses toward the
    // buffer's centre (less diversity) rather than risking a collision.
    const double margin = juce::jlimit (0.0, juce::jmax (0.0, spanLen * 0.5 - 1.0), wantedMargin);

    // predelaySamples pushes the lower bound up further still, capped so
    // it can never exceed what spanLen minus the margin can accommodate
    // (an oversized predelay just collapses toward the far/oldest end,
    // same spirit as the margin cap above -- see the header comment).
    const double safePredelay = juce::jmax (0.0, juce::jmin (predelaySamples, spanLen - margin - 1.0));
    const double lowerBound = juce::jmax (margin, safePredelay);

    return juce::jlimit (lowerBound, juce::jmax (lowerBound, spanLen - margin), rawRead);
}

double GrainVoiceEngine::minSpawnRead (double span)
{
    // 5% of the span, or 5 samples, whichever is larger -- see the header
    // comment for why a flat 5-sample floor stopped being meaningful once
    // early's buffer could be as short as ~2205 samples.
    return juce::jmax (span * 0.05, 5.0);
}

void GrainVoiceEngine::prepare (double newSampleRate, const GrainReverbSharedState& shared,
                                 int numVoices1, int numVoices2,
                                 double del1MaxSecondsToUse, double del2MaxSecondsToUse,
                                 bool singleBufferDualFeedbackToUse)
{
    sampleRate = newSampleRate;
    del1MaxSeconds = del1MaxSecondsToUse;
    singleBufferDualFeedback = singleBufferDualFeedbackToUse;
    hpCoeffG = std::exp (-2.0 * juce::MathConstants<double>::pi * 300.0 / sampleRate);

    del1L.assign ((size_t) (del1MaxSeconds * sampleRate), 0.0);
    del1R.assign ((size_t) (del1MaxSeconds * sampleRate), 0.0);
    if (singleBufferDualFeedback)
    {
        // No second buffer -- Bank 2 shares del1L/del1R (see seedGrains()/
        // processSample()).
        del2L.clear();
        del2R.clear();
    }
    else
    {
        del2L.assign ((size_t) (del2MaxSecondsToUse * sampleRate), 0.0);
        del2R.assign ((size_t) (del2MaxSecondsToUse * sampleRate), 0.0);
    }

    grains1.assign ((size_t) numVoices1, Grain {});
    grains2.assign ((size_t) numVoices2, Grain {});

    count1 = count2 = 0.0;
    hpXL = hpYL = hpXR = hpYR = 0.0;
    dcAud1L = dcAud1R = dcAud2L = dcAud2R = DcBlocker {};
    dcWrite1L = dcWrite1R = dcWrite2L = dcWrite2R = DcBlocker {};

    prevMeanWindowSamps = mstosamps (shared.params.meanWindowMs);
    prevWindowRangeSamps = mstosamps (shared.params.windowRangeMs);

    // See the class comment on del1LenState -- ~30ms glide time is fast
    // enough that turning the Read Range dial still feels responsive,
    // but slow enough that any single sample's change in the wrap modulus
    // is inaudibly small. Initialized to the CURRENT bufferLenMs's target
    // (not 0) so there's no glide-up from zero the moment playback starts.
    constexpr double kDel1LenSmoothSeconds = 0.03;
    del1LenSmoothCoeff = 1.0 - std::exp (-1.0 / (kDel1LenSmoothSeconds * sampleRate));
    del1LenState = std::floor ((shared.params.bufferLenMs / (del1MaxSeconds * 1000.0)) * (double) del1L.size());

    seedGrains (shared);
}

void GrainVoiceEngine::seedGrains (const GrainReverbSharedState& shared)
{
    const auto& p = shared.params;
    const auto* coeffs = shared.getLiveCoeffs();

    const double del1Len = std::floor ((p.bufferLenMs / (del1MaxSeconds * 1000.0)) * (double) del1L.size());
    // In singleBufferDualFeedback mode there's no separate del2 -- Bank 2
    // scatters across the SAME active range as Bank 1 (see the class
    // comment in GrainVoiceEngine.h).
    const double del2Len = singleBufferDualFeedback ? del1Len : (double) del2L.size();
    const double readSpan = p.readScatter * del1Len;
    const double predelaySamples = mstosamps (p.predelayMs);

    for (auto& g : grains1)
    {
        g.rate = 1.0 + noise() * p.jitter;
        g.dur  = mstosamps (p.meanWindowMs + noise() * p.windowRangeMs);
        const double rawRead1 = juce::jmax (minSpawnRead (readSpan), std::abs (noise() * readSpan));
        g.read = clampReadAgainstWriteHead (rawRead1, g.dur, g.rate, del1Len, predelaySamples);
        g.age  = std::abs (noise() * del1Len);
        g.sign = noise() >= 0.0 ? 1.0 : -1.0;
        g.readChannel = (noise() >= 0.0) ? 0 : 1;
        g.writeChannel = chooseWriteChannel (g.readChannel, p.dispersion);

        // Frozen coeffs: nearest-neighbor lookup on dn, same as a respawn would do.
        const double dn = juce::jlimit (0.0, 1.0, g.read / readSpan);
        const int idx = juce::jlimit (0, kNumTableSlots - 1,
                                       (int) std::round (dn * (double) (kNumTableSlots - 1)));
        const auto& c = coeffs->coeff[(size_t) idx];
        g.b0 = c[0]; g.b1 = c[1]; g.b2 = c[2]; g.a1 = c[3]; g.a2 = c[4];
        g.z1 = g.z2 = 0.0;
    }

    for (auto& g : grains2)
    {
        g.rate = 1.0 + noise() * p.jitter;
        g.dur  = mstosamps (p.meanWindowMs + noise() * p.windowRangeMs);
        const double rawRead2 = juce::jmax (minSpawnRead (del2Len), std::abs (noise() * del2Len));
        g.read = clampReadAgainstWriteHead (rawRead2, g.dur, g.rate, del2Len, predelaySamples);
        g.age  = std::abs (noise() * del2Len);
        g.sign = noise() >= 0.0 ? 1.0 : -1.0;
        g.readChannel = (noise() >= 0.0) ? 0 : 1;
        g.writeChannel = chooseWriteChannel (g.readChannel, p.dispersion);

        // Frozen coeffs, same as Bank 1 -- but dn here is read/del2Len, NOT
        // read/readSpan: Bank 2 has no readScatter-scaled span of its own,
        // it always scatters across the full del2Len (see the respawn
        // formula above), so that's its natural "0..1" range.
        const double dn = juce::jlimit (0.0, 1.0, g.read / del2Len);
        const int idx = juce::jlimit (0, kNumTableSlots - 1,
                                       (int) std::round (dn * (double) (kNumTableSlots - 1)));
        const auto& c = coeffs->coeff[(size_t) idx];
        g.b0 = c[0]; g.b1 = c[1]; g.b2 = c[2]; g.a1 = c[3]; g.a2 = c[4];
        g.z1 = g.z2 = 0.0;
    }
}

void GrainVoiceEngine::processSample (double inputL, double inputR, const GrainReverbSharedState& shared,
                                       double& outputL, double& outputR)
{
    const auto& p = shared.params;
    const auto* coeffs = shared.getLiveCoeffs();
    const auto* curves = shared.getLiveCurves();

    double cnt1 = count1;

    // See the class comment on del1LenState -- glide toward the dial's raw
    // target rather than snapping straight to it, so cnt1/every grain's
    // read anchor (both wrapped modulo del1Len just below/further down)
    // never has the wrap modulus yanked out from under them in one sample.
    const double targetDel1Len = std::floor ((p.bufferLenMs / (del1MaxSeconds * 1000.0)) * (double) del1L.size());
    del1LenState += (targetDel1Len - del1LenState) * del1LenSmoothCoeff;
    const double del1Len = del1LenState;
    cnt1 = wrapValue (cnt1, 0.0, del1Len);

    // In singleBufferDualFeedback mode Bank 2 shares del1 entirely: same
    // buffer, same active length, same (already-wrapped) write head -- see
    // the class comment in GrainVoiceEngine.h. Aliasing here means every
    // "del2Len"/"cnt2" reference in Bank 2's loop below (unchanged from the
    // two-buffer design) transparently means del1Len/cnt1 instead.
    double cnt2 = singleBufferDualFeedback ? cnt1 : count2;
    const double del2Len = singleBufferDualFeedback ? del1Len : (double) del2L.size();
    const std::vector<double>& bank2SrcL = singleBufferDualFeedback ? del1L : del2L;
    const std::vector<double>& bank2SrcR = singleBufferDualFeedback ? del1R : del2R;

    const double readSpan = p.readScatter * del1Len;
    const double predelaySamples = mstosamps (p.predelayMs);

    // ---- grain duration refresh on window param change ----
    const double meanWindowSamps = mstosamps (p.meanWindowMs);
    const double windowRangeSamps = mstosamps (p.windowRangeMs);
    const bool durChanged = ! juce::approximatelyEqual (meanWindowSamps, prevMeanWindowSamps)
                          || ! juce::approximatelyEqual (windowRangeSamps, prevWindowRangeSamps);
    if (durChanged)
        for (auto& g : grains1)
            g.dur = meanWindowSamps + noise() * windowRangeSamps;
    prevMeanWindowSamps = meanWindowSamps;
    prevWindowRangeSamps = windowRangeSamps;

    // =====================================================================
    //  BANK 1 (filtered, on del1) -- each grain writes its FULL output to
    //  exactly one channel (writeChannel), not a blended pan.
    // =====================================================================
    // gain1L/gain1R are normalization energy scoped to EACH channel's own
    // contributing grains (whichever have writeChannel matching), not the
    // full 200-grain pool -- aud1L only ever receives contributions from
    // the subset that wrote to channel 0, so it must be normalized against
    // that subset's energy, not against both channels' combined energy
    // (which was the bug: a single shared gain1 double-counted grains that
    // never fed a given channel, under-normalizing both channels).
    double aud1L = 0.0, aud1R = 0.0, gain1L = 0.0, gain1R = 0.0;
    // Live-adjustable "how many of the allocated grains1 voices actually
    // contribute" -- see GrainReverbParams::numGrainVoices. Only the output
    // CONTRIBUTION is gated below; every allocated grain's age/read/respawn
    // bookkeeping still runs unconditionally every sample regardless of
    // this count (see the comment further down for why).
    const int activeVoices1 = juce::jlimit (1, (int) grains1.size(), (int) std::round (p.numGrainVoices));
    for (size_t gi = 0; gi < grains1.size(); ++gi)
    {
        auto& g = grains1[gi];

        if ((int) gi < activeVoices1)
        {
            const auto& srcBuf = (g.readChannel == 0) ? del1L : del1R;
            const double pos = wrapValue (cnt1 - g.read, 0.0, del1Len);
            const double readPos = wrapValue (pos + g.age * g.rate, 0.0, del1Len);
            double au = peekLinear (srcBuf, readPos, del1Len);

            // ---- per-grain TDF-II lowpass (coeffs frozen at spawn) ----
            const double yf = g.b0 * au + g.z1;
            g.z1 = g.b1 * au - g.a1 * yf + g.z2;
            g.z2 = g.b2 * au - g.a2 * yf;
            au = yf;
            // -----------------------------------------------------------

            const double rampUp = juce::jmin (1.0, g.age / p.fadeSamps);
            const double rampDwn = juce::jmin (1.0, (g.dur - g.age) / p.fadeSamps);
            const double trap = juce::jmin (rampUp, rampDwn);
            au *= trap;

            // tail: continuous lookup on the grain's CURRENT distance behind
            // the write head. Deliberately NOT g.read/readSpan -- g.read
            // increments by a flat 1.0 every sample (see below) purely as
            // bookkeeping to keep `pos` fixed; it does NOT track "how far
            // behind the write head is this grain currently reading," which
            // is cnt1 - readPos (constant at rate=1, drifting by (1-rate)
            // per sample otherwise). Using g.read directly here meant that
            // for any grain spawned with read close to del1Len -- likely
            // exactly as readScatter -> 1, since readSpan approaches
            // del1Len -- g.read would wrap from ~del1Len back to ~0 within a
            // handful of samples of spawning, snapping dnT from ~1.0
            // (heavily tail-attenuated) to ~0.0 (full volume) in a single
            // sample: a sudden, loud, still-heavily-lowpassed (frozen
            // coefficients don't update mid-life) blast of "old" content --
            // exactly the distorted echo, present at ANY jitter setting
            // since it's not a rate-drift bug, just worse at high scatter.
            const double trueGap = wrapValue (cnt1 - readPos, 0.0, del1Len);
            const double dnT = juce::jlimit (0.0, 1.0, trueGap / readSpan);
            const int idxT = juce::jlimit (0, kNumTableSlots - 1,
                                            (int) std::round (dnT * (double) (kNumTableSlots - 1)));
            au *= curves->tail[(size_t) idxT];

            const double signedAu = au * g.sign;
            if (g.writeChannel == 0)
            {
                aud1L += signedAu;
                gain1L += trap * trap;
            }
            else
            {
                aud1R += signedAu;
                gain1R += trap * trap;
            }
        }

        // Runs for EVERY allocated grain, active or not -- an inactive
        // grain's read anchor (pos = writeHead - read) only stays correctly
        // fixed if `read` keeps incrementing in lockstep with the write
        // head every single sample (see the clampReadAgainstWriteHead
        // comment). Skipping this while inactive would let a grain's anchor
        // silently drift out of sync, so when it's reactivated later
        // (raising the voice-count dial back up) it could read a sudden,
        // wrong jump in content -- reintroducing a variant of the exact
        // collision/glitch class already fixed once for this engine.
        g.age += 1.0;
        g.read += 1.0;
        g.read = wrapValue (g.read, 0.0, del1Len);

        if (g.age > g.dur)
        {
            g.age = 0.0;
            g.rate = 1.0 + noise() * p.jitter;
            g.dur  = mstosamps (p.meanWindowMs + noise() * p.windowRangeMs);
            const double rawRead1 = juce::jmax (minSpawnRead (readSpan), std::abs (noise() * readSpan));
            g.read = clampReadAgainstWriteHead (rawRead1, g.dur, g.rate, del1Len, predelaySamples);
            g.sign = noise() >= 0.0 ? 1.0 : -1.0;
            g.readChannel = (noise() >= 0.0) ? 0 : 1;
            g.writeChannel = chooseWriteChannel (g.readChannel, p.dispersion);

            // refresh frozen coeffs from the table for the new read position
            const double dn = juce::jlimit (0.0, 1.0, g.read / readSpan);
            const int idx = juce::jlimit (0, kNumTableSlots - 1,
                                           (int) std::round (dn * (double) (kNumTableSlots - 1)));
            const auto& c = coeffs->coeff[(size_t) idx];
            g.b0 = c[0]; g.b1 = c[1]; g.b2 = c[2]; g.a1 = c[3]; g.a2 = c[4];
            g.z1 = g.z2 = 0.0; // reset state so each grain starts clean
        }
    }

    // =====================================================================
    //  BANK 2 (on del2) -- now filtered same as Bank 1 (own dn = read/
    //  del2Len), still WITHOUT the tail decay curve, which stays Bank-1-only.
    // =====================================================================
    double aud2L = 0.0, aud2R = 0.0, gain2L = 0.0, gain2R = 0.0;
    const int activeVoices2 = juce::jlimit (1, (int) grains2.size(), (int) std::round (p.numGrainVoices));
    for (size_t gi = 0; gi < grains2.size(); ++gi)
    {
        auto& g = grains2[gi];

        if ((int) gi < activeVoices2)
        {
            const auto& srcBuf = (g.readChannel == 0) ? bank2SrcL : bank2SrcR;
            const double pos = wrapValue (cnt2 - g.read, 0.0, del2Len);
            const double readPos = wrapValue (pos + g.age * g.rate, 0.0, del2Len);
            double au = peekLinear (srcBuf, readPos, del2Len);

            // ---- per-grain TDF-II lowpass (coeffs frozen at spawn) ----
            const double yf2 = g.b0 * au + g.z1;
            g.z1 = g.b1 * au - g.a1 * yf2 + g.z2;
            g.z2 = g.b2 * au - g.a2 * yf2;
            au = yf2;
            // -----------------------------------------------------------

            const double rampUp = juce::jmin (1.0, g.age / p.fadeSamps);
            const double rampDwn = juce::jmin (1.0, (g.dur - g.age) / p.fadeSamps);
            const double trap = juce::jmin (rampUp, rampDwn);
            au *= trap;

            const double signedAu = au * g.sign;
            if (g.writeChannel == 0)
            {
                aud2L += signedAu;
                gain2L += trap * trap;
            }
            else
            {
                aud2R += signedAu;
                gain2R += trap * trap;
            }
        }

        // See Bank 1's identical comment above -- must run unconditionally.
        g.age += 1.0;
        g.read += 1.0;
        g.read = wrapValue (g.read, 0.0, del2Len);

        if (g.age > g.dur)
        {
            g.age = 0.0;
            g.rate = 1.0 + noise() * p.jitter;
            g.dur  = mstosamps (p.meanWindowMs + noise() * p.windowRangeMs);
            const double rawRead2 = juce::jmax (minSpawnRead (del2Len), std::abs (noise() * del2Len));
            g.read = clampReadAgainstWriteHead (rawRead2, g.dur, g.rate, del2Len, predelaySamples);
            g.sign = noise() >= 0.0 ? 1.0 : -1.0;
            g.readChannel = (noise() >= 0.0) ? 0 : 1;
            g.writeChannel = chooseWriteChannel (g.readChannel, p.dispersion);

            // refresh frozen coeffs from the table for the new read position
            const double dn2 = juce::jlimit (0.0, 1.0, g.read / del2Len);
            const int idx2 = juce::jlimit (0, kNumTableSlots - 1,
                                            (int) std::round (dn2 * (double) (kNumTableSlots - 1)));
            const auto& c2 = coeffs->coeff[(size_t) idx2];
            g.b0 = c2[0]; g.b1 = c2[1]; g.b2 = c2[2]; g.a1 = c2[3]; g.a2 = c2[4];
            g.z1 = g.z2 = 0.0; // reset state so each grain starts clean
        }
    }

    // =====================================================================
    //  OUTPUT STAGE -- run per channel, normalized against each channel's
    //  OWN contributing grains' energy (gain1L/gain1R/gain2L/gain2R), not a
    //  shared total -- see the gain1L/gain1R comment above for why.
    // =====================================================================
    const double g_ = hpCoeffG;
    const double a0hp = 0.5 * (1.0 + g_);

    const double hpOutL = a0hp * aud1L - a0hp * hpXL + g_ * hpYL;
    hpXL = aud1L; hpYL = hpOutL; aud1L = hpOutL;

    const double hpOutR = a0hp * aud1R - a0hp * hpXR + g_ * hpYR;
    hpXR = aud1R; hpYR = hpOutR; aud1R = hpOutR;

    aud1L = dcAud1L.process (aud1L / std::sqrt (juce::jmax (gain1L, 1.0e-4)));
    aud1R = dcAud1R.process (aud1R / std::sqrt (juce::jmax (gain1R, 1.0e-4)));
    aud2L = dcAud2L.process (aud2L / std::sqrt (juce::jmax (gain2L, 1.0e-4)));
    aud2R = dcAud2R.process (aud2R / std::sqrt (juce::jmax (gain2R, 1.0e-4)));

    // Safety soft-clip -- dividing by sqrt(gain) normalizes correctly for
    // grains that sum INCOHERENTLY (like noise, where N contributions add
    // up as sqrt(N)), but a short buffer packed with many largeish grains
    // can instead read heavily overlapping/near-identical delayed copies
    // of a sustained tone, which sum closer to LINEARLY (N, not sqrt(N)).
    // That gap lets a coherent tone overshoot the normalization by roughly
    // sqrt(activeVoices) -- confirmed empirically (a 0.3-amplitude 110Hz
    // tone through a 429ms buffer / 287ms grains / 50 voices produced
    // peaks over 1.7, and fed back through fb1/fb2 that never settled to
    // a steady level). tanh clamps that overshoot at its source, right
    // where it's about to feed back into the buffer AND become aud2's
    // audible output, without touching well-behaved (already near-unity)
    // signals -- tanh(x) ~= x for |x| well under 1.
    //
    // TRIED replacing this with a threshold-gated gain-reduction limiter
    // (instant attack/~50ms release, only engaging above 0.95) -- made
    // things WORSE: tanh's continuous, always-on gentle compression (even
    // for in-range signals) was quietly damping the feedback loop's
    // overall energy every single sample; a threshold-gated limiter does
    // NOTHING below its threshold, so the loop could build up more energy
    // per cycle than before, producing bigger peaks and a louder stored
    // buffer (visibly bigger waveform) once the limiter did engage.
    // Reverted -- tanh's constant, if imperfect, damping is load-bearing.
    aud1L = std::tanh (aud1L);
    aud1R = std::tanh (aud1R);
    aud2L = std::tanh (aud2L);
    aud2R = std::tanh (aud2R);

    // The x2 output makeup gain below predates this tanh (inherited from
    // the original gen~ patch, tuned back when aud2 alone was assumed to
    // sit comfortably under unity) -- 2*tanh(x) reaches up to 2.0, not
    // 1.0, so that x2 was UNDOING the ceiling this tanh was just meant to
    // guarantee, for the one signal (the actual output) that most needed
    // it. Clip again after the x2 so the signal actually handed back to
    // the host stays within (-1, 1).
    outputL = std::tanh (aud2L * 2.0);
    outputR = std::tanh (aud2R * 2.0);

    // singleBufferDualFeedback: Bank 2's own output feeds back into the
    // SAME del1 write, scaled by its own fb2 -- rather than a separate
    // del2 write. fb2ContribL/R is 0 for lateEngine (fb2 unset/unread),
    // so this collapses exactly to the original del1 write when the flag
    // is off.
    const double fb2ContribL = singleBufferDualFeedback ? p.fb2 * aud2L : 0.0;
    const double fb2ContribR = singleBufferDualFeedback ? p.fb2 * aud2R : 0.0;
    pokeWrite (del1L, cnt1, dcWrite1L.process (inputL + p.fb * aud1L + fb2ContribL));
    pokeWrite (del1R, cnt1, dcWrite1R.process (inputR + p.fb * aud1R + fb2ContribR));

    if (! singleBufferDualFeedback)
    {
        pokeWrite (del2L, cnt2, dcWrite2L.process (p.fb * (aud1L + aud2L)));
        pokeWrite (del2R, cnt2, dcWrite2R.process (p.fb * (aud1R + aud2R)));
    }

    cnt1 += 1.0;
    cnt1 = wrapValue (cnt1, 0.0, del1Len);
    count1 = cnt1;

    // count2 has no independent meaning in singleBufferDualFeedback mode
    // (Bank 2 already tracks cnt1 via the alias above) -- left untouched
    // (frozen at 0 from prepare()) rather than advanced a second time.
    if (! singleBufferDualFeedback)
    {
        cnt2 += 1.0;
        cnt2 = wrapValue (cnt2, 0.0, del2Len);
        count2 = cnt2;
    }
}
