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
// floor and ceil sample, wrapping at the buffer's end.
double GrainVoiceEngine::peekLinear (const std::vector<double>& buf, double pos)
{
    const auto n = (double) buf.size();
    pos = wrapValue (pos, 0.0, n);
    const auto i0 = (size_t) pos;
    const size_t i1 = (i0 + 1 == buf.size()) ? 0 : i0 + 1;
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

double GrainVoiceEngine::clampReadAgainstWriteHead (double rawRead, double dur, double rate, double spanLen)
{
    const double wantedMargin = dur * std::abs (rate - 1.0);
    // Cap the margin at just under half of spanLen so [margin, spanLen-margin]
    // can never invert -- an extreme combination (very long grain, high
    // jitter, short buffer) could otherwise ask for more margin than the
    // buffer has. In that extreme case every grain collapses toward the
    // buffer's centre (less diversity) rather than risking a collision.
    const double margin = juce::jlimit (0.0, juce::jmax (0.0, spanLen * 0.5 - 1.0), wantedMargin);
    return juce::jlimit (margin, juce::jmax (margin, spanLen - margin), rawRead);
}

void GrainVoiceEngine::prepare (double newSampleRate, const GrainReverbSharedState& shared,
                                 int numVoices1, int numVoices2,
                                 double del1MaxSecondsToUse, double del2MaxSecondsToUse)
{
    sampleRate = newSampleRate;
    del1MaxSeconds = del1MaxSecondsToUse;
    hpCoeffG = std::exp (-2.0 * juce::MathConstants<double>::pi * 300.0 / sampleRate);

    del1L.assign ((size_t) (del1MaxSeconds * sampleRate), 0.0);
    del1R.assign ((size_t) (del1MaxSeconds * sampleRate), 0.0);
    del2L.assign ((size_t) (del2MaxSecondsToUse * sampleRate), 0.0);
    del2R.assign ((size_t) (del2MaxSecondsToUse * sampleRate), 0.0);

    grains1.assign ((size_t) numVoices1, Grain {});
    grains2.assign ((size_t) numVoices2, Grain {});

    count1 = count2 = 0.0;
    hpXL = hpYL = hpXR = hpYR = 0.0;
    dcAud1L = dcAud1R = dcAud2L = dcAud2R = DcBlocker {};
    dcWrite1L = dcWrite1R = dcWrite2L = dcWrite2R = DcBlocker {};

    prevMeanWindowSamps = mstosamps (shared.params.meanWindowMs);
    prevWindowRangeSamps = mstosamps (shared.params.windowRangeMs);

    seedGrains (shared);
}

void GrainVoiceEngine::seedGrains (const GrainReverbSharedState& shared)
{
    const auto& p = shared.params;
    const auto* coeffs = shared.getLiveCoeffs();

    const double del1Len = std::floor ((p.bufferLenMs / (del1MaxSeconds * 1000.0)) * (double) del1L.size());
    const double del2Len = (double) del2L.size();
    const double readSpan = p.readScatter * del1Len;

    for (auto& g : grains1)
    {
        g.rate = 1.0 + noise() * p.jitter;
        g.dur  = mstosamps (p.meanWindowMs + noise() * p.windowRangeMs);
        const double rawRead1 = juce::jmax (5.0, std::abs (noise() * readSpan));
        g.read = clampReadAgainstWriteHead (rawRead1, g.dur, g.rate, del1Len);
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
        const double rawRead2 = juce::jmax (5.0, std::abs (noise() * del2Len));
        g.read = clampReadAgainstWriteHead (rawRead2, g.dur, g.rate, del2Len);
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
    double cnt2 = count2;

    const double del1Len = std::floor ((p.bufferLenMs / (del1MaxSeconds * 1000.0)) * (double) del1L.size());
    const double del2Len = (double) del2L.size();
    cnt1 = wrapValue (cnt1, 0.0, del1Len);

    const double readSpan = p.readScatter * del1Len;

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
            double au = peekLinear (srcBuf, readPos);

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
            const double rawRead1 = juce::jmax (5.0, std::abs (noise() * readSpan));
            g.read = clampReadAgainstWriteHead (rawRead1, g.dur, g.rate, del1Len);
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
            const auto& srcBuf = (g.readChannel == 0) ? del2L : del2R;
            const double pos = wrapValue (cnt2 - g.read, 0.0, del2Len);
            const double readPos = wrapValue (pos + g.age * g.rate, 0.0, del2Len);
            double au = peekLinear (srcBuf, readPos);

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
            const double rawRead2 = juce::jmax (5.0, std::abs (noise() * del2Len));
            g.read = clampReadAgainstWriteHead (rawRead2, g.dur, g.rate, del2Len);
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

    outputL = aud2L * 2.0;
    outputR = aud2R * 2.0;

    pokeWrite (del1L, cnt1, dcWrite1L.process (inputL + p.fb * aud1L));
    pokeWrite (del1R, cnt1, dcWrite1R.process (inputR + p.fb * aud1R));
    pokeWrite (del2L, cnt2, dcWrite2L.process (p.fb * (aud1L + aud2L)));
    pokeWrite (del2R, cnt2, dcWrite2R.process (p.fb * (aud1R + aud2R)));

    cnt1 += 1.0;
    cnt1 = wrapValue (cnt1, 0.0, del1Len);
    count1 = cnt1;

    cnt2 += 1.0;
    cnt2 = wrapValue (cnt2, 0.0, del2Len);
    count2 = cnt2;
}
