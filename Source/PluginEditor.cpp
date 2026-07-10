#include "PluginEditor.h"
#include <cmath>

namespace
{
    // Cheap, closed-form RT60 estimate (PDF Section 2.6) -- NOT a
    // measurement, just a fast always-available number computed on demand
    // from the current params when rt60Button is clicked. See the earlier
    // (reverted) live output-capture visualizer for why this stays a
    // one-shot snapshot rather than a continuously-updating view: scanning
    // enough history to redraw a scrolling waveform every frame was heavy
    // enough to audibly starve the audio thread.
    //
    // TWO independent self-feedback loops shape the tail, not one:
    //   - del1: write = input + fb*aud1 -- decays per readScatter/
    //     bufferLenMs, average grain delay 0.5*readScatter*bufferLenMs.
    //     Bank 1 grains ALSO multiply by the live tail curve every pass
    //     (processSample()'s `au *= curves->tail[idxT]`) -- a real,
    //     user-controlled extra attenuation completely separate from fb.
    //     Averaging the live tail table over dn approximates that (grain
    //     read offsets are uniform over [0, readSpan], so dn is uniform
    //     over [0,1] -- a flat average is a reasonable stand-in for "the
    //     typical grain's extra cut this pass"). The default curve alone
    //     is roughly a 7.5dB-per-pass extra cut that the first version of
    //     this estimate ignored entirely.
    //   - del2: write = fb*(aud1+aud2) -- a SEPARATE self-feedback loop,
    //     no tail curve applied (Bank 2 skips it by design). The actual
    //     audible output is aud2 (see processSample()'s
    //     outputL = aud2L * 2.0), not aud1. del2 is a fixed 1-second
    //     buffer (GrainVoiceEngine::prepare()'s del2L/R.assign) with no
    //     scatter param -- its own average grain delay is always 0.5s,
    //     independent of readScatter/bufferLenMs entirely.
    // In a pair of coupled decaying feedback loops, the faster one
    // generally dominates what's perceptible (the slower one's tail keeps
    // going, but far too quietly to matter once the faster one has died
    // out) -- taking the smaller of the two estimates tracks that.
    //
    // Past this, the model is still a simplification: it treats each pass
    // as ONE coherent signal multiplied by a scalar, but the real signal
    // is ~200 overlapping grains -- half with flipped polarity
    // (Grain::sign) -- reading from different points in the decay history
    // and summing INCOHERENTLY (which is exactly why the engine
    // RMS-normalizes by sqrt(gain1L) rather than dividing by grain count).
    // Per-grain lowpass filtering removes further energy per pass too.
    // Neither is folded in here -- a closed-form comb-filter formula
    // structurally can't capture ensemble decorrelation or filter energy
    // loss, so this stays a rough ballpark, not a precise number.
    constexpr double kDel2Seconds = 1.0; // must match GrainVoiceEngine's del2 allocation

    double estimateRT60Seconds (const GrainReverbSharedState& shared)
    {
        const auto& p = shared.params;
        const auto* curves = shared.getLiveCurves();

        const double fb = juce::jlimit (1.0e-6, 0.999999, p.fb);

        double tailSum = 0.0;
        for (double v : curves->tail)
            tailSum += v;
        const double avgTailGain = juce::jlimit (1.0e-6, 1.0, tailSum / (double) kNumTableSlots);

        const double effectiveGainDel1 = juce::jlimit (1.0e-6, 0.999999, fb * avgTailGain);
        const double logInvFbDel1 = std::log10 (1.0 / effectiveGainDel1);
        const double t1Del1 = 0.5 * p.readScatter * (p.bufferLenMs / 1000.0);
        const double rt60Del1 = 3.0 * t1Del1 / logInvFbDel1;

        const double logInvFb = std::log10 (1.0 / fb);
        const double t1Del2 = 0.5 * kDel2Seconds;
        const double rt60Del2 = 3.0 * t1Del2 / logInvFb;

        const double rt60 = juce::jmin (rt60Del1, rt60Del2);
        return juce::jlimit (0.05, 30.0, rt60);
    }
}

GrainReverb2AudioProcessorEditor::GrainReverb2AudioProcessorEditor (GrainReverb2AudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p), bufferVisualizer (p), breakpointEditor (p), dialsPanel (p)
{
    constexpr int radioGroupId = 2001;
    auto setup = [this] (juce::TextButton& b, CurveKind kind, bool initiallyOn)
    {
        addAndMakeVisible (b);
        b.setClickingTogglesState (true);
        b.setRadioGroupId (radioGroupId);
        b.setToggleState (initiallyOn, juce::dontSendNotification);
        b.onClick = [this, kind] { breakpointEditor.setActiveCurve (kind); };
    };
    setup (cutoffButton, CurveKind::Cutoff, true);
    setup (qButton, CurveKind::Q, false);
    setup (tailButton, CurveKind::Tail, false);

    addAndMakeVisible (rt60Button);
    rt60Button.onClick = [this]
    {
        const auto rt60 = estimateRT60Seconds (processorRef.getSharedState());
        rt60Readout.setText ("RT60 ~ " + juce::String (rt60, 2) + "s", juce::dontSendNotification);
    };

    addAndMakeVisible (rt60Readout);
    rt60Readout.setJustificationType (juce::Justification::centredLeft);
    rt60Readout.setFont (juce::FontOptions (13.0f));
    rt60Readout.setText ("RT60: --", juce::dontSendNotification);

    addAndMakeVisible (bufferVisualizer);
    addAndMakeVisible (breakpointEditor); // added after -- frontmost for paint + mouse
    addAndMakeVisible (dialsPanel);

    setSize (700, 680); // 3x3 dial grid is taller now the dials themselves are bigger
}

GrainReverb2AudioProcessorEditor::~GrainReverb2AudioProcessorEditor() {}

void GrainReverb2AudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
}

void GrainReverb2AudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced (10);

    auto buttonRow = bounds.removeFromTop (28);
    rt60Readout.setBounds (buttonRow.removeFromRight (110).reduced (4, 2));
    rt60Button.setBounds (buttonRow.removeFromRight (90).reduced (4, 2));
    const int w = buttonRow.getWidth() / 3;
    cutoffButton.setBounds (buttonRow.removeFromLeft (w).reduced (4, 2));
    qButton.setBounds (buttonRow.removeFromLeft (w).reduced (4, 2));
    tailButton.setBounds (buttonRow.reduced (4, 2));

    // Just visual breathing room below the button row -- the ACTUAL fix for
    // max-value points getting clipped is kVisualizerTopMargin, applied
    // inside both CircularBufferVisualizer's and BreakpointEditor's own
    // plot-area math (clipping happens at a component's own bounds, not
    // here, so this gap alone wouldn't have fixed it).
    bounds.removeFromTop (10);

    auto dialsArea = bounds.removeFromBottom (260); // taller now the dials themselves are bigger
    bounds.removeFromBottom (10); // gap

    // Identical bounds for both components -- this is what makes the
    // breakpoint overlay line up with the waveform underneath it.
    bufferVisualizer.setBounds (bounds);
    breakpointEditor.setBounds (bounds);

    dialsPanel.setBounds (dialsArea);
}
