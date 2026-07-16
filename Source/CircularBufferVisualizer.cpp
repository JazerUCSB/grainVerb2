#include "CircularBufferVisualizer.h"
#include <cmath>

namespace
{
    // Local wrap helper for indexing into the raw buffer for display --
    // separate from GrainVoiceEngine::wrapValue since this runs on the
    // message thread against a plain vector, not the audio-thread grain
    // math (no need to avoid std::fmod here -- this runs ~30 times/sec on
    // the message thread, not ~1600 times/sample on the audio thread).
    size_t wrapIndex (double v, size_t size)
    {
        const double n = (double) size;
        double r = std::fmod (v, n);
        if (r < 0.0)
            r += n;
        return (size_t) r;
    }

    // Display-only amplitude boost -- sqrt curve maps [-1, 1] to itself
    // (0 stays 0, +-1 stays +-1) while disproportionately stretching
    // near-zero values (e.g. sqrt(0.01) = 0.1, a 10x visual boost for a 1%
    // amplitude signal), so quiet passages are actually visible in the
    // waveform without touching the real audio or clipping loud ones past
    // the +-1 ceiling they already can't exceed.
    double visualAmplitudeBoost (double v)
    {
        return v >= 0.0 ? std::sqrt (v) : -std::sqrt (-v);
    }
}

CircularBufferVisualizer::CircularBufferVisualizer (GrainReverb2AudioProcessor& processorForSampleRate,
                                                      const GrainVoiceEngine& engineToShow,
                                                      const GrainReverbSharedState& stateToShow,
                                                      juce::Colour backgroundColourToUse)
    : processor (processorForSampleRate), engine (engineToShow), sharedState (stateToShow),
      backgroundColour (backgroundColourToUse)
{
    // 24fps rather than 30 -- extra headroom now that this repaints both
    // channels' worth of buffer-scan work instead of one.
    startTimerHz (24);
}

CircularBufferVisualizer::~CircularBufferVisualizer()
{
    stopTimer();
}

void CircularBufferVisualizer::timerCallback()
{
    repaint();
}

void CircularBufferVisualizer::paint (juce::Graphics& g)
{
    g.fillAll (backgroundColour);

    const auto& bufL = engine.getDelayBuffer1 (0);
    const auto& bufR = engine.getDelayBuffer1 (1);
    const double writeHead = engine.getWriteHead1();
    const double sampleRate = processor.getSampleRate();
    const auto& params = sharedState.params;

    if (bufL.empty() || sampleRate <= 0.0)
        return;

    const double del1Len = std::floor ((params.bufferLenMs / (engine.getDel1MaxSeconds() * 1000.0)) * (double) bufL.size());
    // readScatter is now always 1.0 (see GrainReverbParams::readScatter) --
    // this equals del1Len exactly, i.e. the FULL active buffer, which is
    // what makes labeling the axis "Read Range" below accurate rather
    // than an overclaim.
    const double readSpan = juce::jmax (1.0, params.readScatter * del1Len);
    const double maxSeconds = readSpan / sampleRate;

    auto bounds = getLocalBounds();
    auto rulerArea = bounds.removeFromBottom (kVisualizerRulerHeight);
    // Left margin reserved here purely for alignment -- BreakpointEditor
    // draws its active curve's value labels into this same strip when
    // overlaid on top, so both components must reserve identical space.
    // Also doubles as room for the L/R tags drawn per channel below.
    auto leftMarginArea = bounds.removeFromLeft (kVisualizerLeftMargin);
    bounds.removeFromTop (kVisualizerTopMargin); // headroom so a max-value point isn't top-clipped
    bounds.removeFromRight (kVisualizerRightMargin); // ditto for a point at dn = 1 (far right)

    // Stacked L (top) / R (bottom) instead of a toggle -- with per-grain
    // panning inside one merged engine, neither channel alone shows the
    // stereo picture; showing both at once does.
    constexpr int kChannelGap = 4;
    auto topArea = bounds.removeFromTop ((bounds.getHeight() - kChannelGap) / 2);
    bounds.removeFromTop (kChannelGap);
    auto bottomArea = bounds;

    // One column per pixel: downsample the [dn, dn+1px) span of the buffer
    // to a min/max pair. A full per-sample scan of readSpan was fine at
    // single-channel scale, but drawing BOTH channels doubles that work on
    // the message thread at 30fps -- the same class of bug that caused an
    // earlier (reverted) visualizer to audibly starve the audio thread, just
    // smaller. Fixed the same way: cap samples scanned per column so total
    // work per repaint stays bounded regardless of how large readSpan gets,
    // trading a little min/max precision at high zoom-out for headroom.
    constexpr int kMaxSamplesPerColumn = 64;
    auto drawChannel = [&] (const std::vector<double>& buf, juce::Rectangle<int> areaInt, const juce::String& tag)
    {
        auto area = areaInt.toFloat();
        const int width = juce::jmax (1, (int) area.getWidth());
        const float midY = area.getCentreY();
        const float halfH = area.getHeight() * 0.5f;

        g.setColour (juce::Colours::limegreen);
        for (int x = 0; x < width; ++x)
        {
            const double dn0 = (double) x / (double) width;
            const double dn1 = (double) (x + 1) / (double) width;
            const int i0 = (int) (dn0 * readSpan);
            const int i1 = juce::jmax (i0 + 1, (int) (dn1 * readSpan));
            const int span = i1 - i0;
            const int step = juce::jmax (1, span / kMaxSamplesPerColumn);

            double lo = 1.0, hi = -1.0;
            for (int i = i0; i < i1; i += step)
            {
                // Wrap using del1Len (the ACTIVE portion the write head cycles
                // through), not buf.size() (the full 6s capacity) -- see the
                // original Step 3 fix for why.
                const double v = buf[wrapIndex (writeHead - (double) i, (size_t) del1Len)];
                lo = juce::jmin (lo, v);
                hi = juce::jmax (hi, v);
            }
            if (lo > hi) { lo = 0.0; hi = 0.0; } // guard against a degenerate empty span
            lo = visualAmplitudeBoost (lo);
            hi = visualAmplitudeBoost (hi);

            const float xPix = area.getX() + (float) x;
            g.drawVerticalLine ((int) xPix, midY - (float) hi * halfH, midY - (float) lo * halfH);
        }

        // Write-head marker at the left edge (dn = 0, newest material).
        g.setColour (juce::Colours::orange.withAlpha (0.6f));
        g.drawVerticalLine ((int) area.getX(), area.getY(), area.getBottom());

        // Channel tag in the reserved left margin, vertically centred on
        // this channel's own half.
        g.setColour (juce::Colours::grey);
        g.setFont (13.0f);
        g.drawText (tag, juce::Rectangle<float> ((float) leftMarginArea.getX(), area.getY(),
                                                  (float) leftMarginArea.getWidth() - 4.0f, area.getHeight()),
                    juce::Justification::centred);
    };

    drawChannel (bufL, topArea, "L");
    drawChannel (bufR, bottomArea, "R");

    // Seconds ruler: exact dn -> seconds-since-written conversion (see the
    // class comment in the header for what this axis does and doesn't
    // claim). Shared by both channels since they're drawn to the same
    // x-mapping.
    const auto rulerWaveformX = topArea.toFloat().getX();
    const auto rulerWaveformWidth = topArea.toFloat().getWidth();

    // Axis caption in the bottom-left corner -- below the left margin, in
    // the ruler row -- which is otherwise always blank (neither the
    // waveform loop above nor BreakpointEditor's identical margin math ever
    // draws there), so this can't collide with or shift anything else's
    // alignment. Now genuinely "Read Range" (not "Read Span" or "Buffer
    // Length" -- renamed since a grain reading position is a fixed delay
    // TAP somewhere within this range, not a container a grain has to fit
    // inside): readScatter is pinned to 1.0 for both engines (see
    // GrainReverbParams::readScatter), so readSpan == del1Len == the full
    // active range exactly -- no longer an overclaim. Two lines
    // (drawFittedText wraps "Buffer"/"Range" onto their own line each --
    // kVisualizerRulerHeight is sized to fit both) rather than one, so the
    // caption stays readable at this width. No background panel -- text
    // drawn straight over the ruler row.
    {
        auto badgeArea = juce::Rectangle<float> (2.0f, (float) rulerArea.getY() + 2.0f,
                                                   (float) kVisualizerLeftMargin - 4.0f, (float) rulerArea.getHeight() - 4.0f);
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawFittedText ("Read Range",
                           { (int) badgeArea.getX(), (int) badgeArea.getY(), (int) badgeArea.getWidth(), (int) badgeArea.getHeight() },
                           juce::Justification::centred, 2);
    }

    constexpr int numTicks = 5;
    for (int i = 0; i < numTicks; ++i)
    {
        const double frac = (double) i / (double) (numTicks - 1);
        const float x = rulerWaveformX + (float) frac * rulerWaveformWidth;

        g.setColour (juce::Colours::grey.withAlpha (0.15f));
        g.drawVerticalLine ((int) x, (float) kVisualizerTopMargin, (float) rulerArea.getY());

        g.setColour (juce::Colours::grey);
        constexpr float labelW = 60.0f;
        auto just = juce::Justification::centred;
        float labelX = x - labelW * 0.5f;
        if (i == 0)             { just = juce::Justification::centredLeft;  labelX = x; }
        if (i == numTicks - 1)  { just = juce::Justification::centredRight; labelX = x - labelW; }

        g.drawText (juce::String (frac * maxSeconds, 2) + "s",
                    juce::Rectangle<float> (labelX, (float) rulerArea.getY(), labelW, (float) rulerArea.getHeight()),
                    just);
    }
}
