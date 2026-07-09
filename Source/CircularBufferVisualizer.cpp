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
}

CircularBufferVisualizer::CircularBufferVisualizer (GrainReverb2AudioProcessor& processorToUse)
    : processor (processorToUse)
{
    startTimerHz (30);
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
    g.fillAll (juce::Colours::black);

    const auto& engineRef = processor.getEngine();
    const int ch = (voiceChannel == VoiceChannel::Left) ? 0 : 1;
    const auto& buf = engineRef.getDelayBuffer1 (ch);
    const double writeHead = engineRef.getWriteHead1();
    const double sampleRate = processor.getSampleRate();
    const auto& params = processor.getSharedState().params;

    if (buf.empty() || sampleRate <= 0.0)
        return;

    const double del1Len = std::floor ((params.bufferLenMs / 6000.0) * (double) buf.size());
    const double readSpan = juce::jmax (1.0, params.readScatter * del1Len);
    const double maxSeconds = readSpan / sampleRate;

    auto bounds = getLocalBounds();
    auto rulerArea = bounds.removeFromBottom (kVisualizerRulerHeight);
    // Left margin reserved here purely for alignment -- BreakpointEditor
    // draws its active curve's value labels into this same strip when
    // overlaid on top, so both components must reserve identical space.
    bounds.removeFromLeft (kVisualizerLeftMargin);
    bounds.removeFromTop (kVisualizerTopMargin); // headroom so a max-value point isn't top-clipped
    auto waveformArea = bounds.toFloat();

    const int width = juce::jmax (1, (int) waveformArea.getWidth());
    const float midY = waveformArea.getCentreY();
    const float halfH = waveformArea.getHeight() * 0.5f;

    // One column per pixel: downsample the [dn, dn+1px) span of the buffer
    // to a min/max pair. This scans the whole readSpan once per repaint
    // (~30fps, message thread) -- fine even at the largest readSpan
    // (~265k samples at max buffer length/scatter), nowhere near the
    // audio thread's per-sample budget.
    g.setColour (juce::Colours::limegreen);
    for (int x = 0; x < width; ++x)
    {
        const double dn0 = (double) x / (double) width;
        const double dn1 = (double) (x + 1) / (double) width;
        const int i0 = (int) (dn0 * readSpan);
        const int i1 = juce::jmax (i0 + 1, (int) (dn1 * readSpan));

        double lo = 1.0, hi = -1.0;
        for (int i = i0; i < i1; ++i)
        {
            // Wrap using del1Len (the ACTIVE portion the write head cycles
            // through), not buf.size() (the full 6s capacity). Those two
            // only match when bufferLenMs is maxed at 6000 -- otherwise
            // wrapping against buf.size() lands in the unused tail of the
            // vector (always silent/stale) instead of correctly wrapping
            // back to the start of the active region, which shows up as a
            // visible seam/jump sweeping across the display as the write
            // head cycles.
            const double v = buf[wrapIndex (writeHead - (double) i, (size_t) del1Len)];
            lo = juce::jmin (lo, v);
            hi = juce::jmax (hi, v);
        }
        if (lo > hi) { lo = 0.0; hi = 0.0; } // guard against a degenerate empty span

        const float xPix = waveformArea.getX() + (float) x;
        g.drawVerticalLine ((int) xPix, midY - (float) hi * halfH, midY - (float) lo * halfH);
    }

    // Write-head marker at the left edge (dn = 0, newest material).
    g.setColour (juce::Colours::orange.withAlpha (0.6f));
    g.drawVerticalLine ((int) waveformArea.getX(), waveformArea.getY(), waveformArea.getBottom());

    // Seconds ruler: exact dn -> seconds-since-written conversion (see the
    // class comment in the header for what this axis does and doesn't claim).
    constexpr int numTicks = 5;
    g.setFont (13.0f);
    for (int i = 0; i < numTicks; ++i)
    {
        const double frac = (double) i / (double) (numTicks - 1);
        const float x = waveformArea.getX() + (float) frac * waveformArea.getWidth();

        g.setColour (juce::Colours::grey.withAlpha (0.15f));
        g.drawVerticalLine ((int) x, waveformArea.getY(), waveformArea.getBottom());

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
