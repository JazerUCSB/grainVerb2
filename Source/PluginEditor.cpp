#include "PluginEditor.h"
#include "PaneTheme.h"

namespace
{
    // Empirical RT60 measurement -- runs a completely separate, throwaway
    // GrainVoiceEngine (never the live one, so it can't disturb what's
    // actually playing) on a background thread, feeds it a single-sample
    // impulse followed by silence, and captures the output. This can run
    // MUCH faster than real time: processSample() has no dependency on
    // actual wall-clock time, only on how many samples have been fed to
    // it, so a tight loop can push through a 30-second "virtual" decay in
    // a handful of seconds of real compute rather than needing to wait out
    // the full 30 seconds.
    //
    // Measures via Schroeder backward integration (the standard acoustic
    // RT60 method): reverse-cumulative energy E(t) = sum of e(s) for s>=t
    // is guaranteed monotonically non-increasing, so unlike a raw envelope
    // (which oscillates through zero) there's exactly one crossing point
    // to find, no smoothing/windowing needed. E(0) is the reference (0dB);
    // RT60 is the first t where E(t) has dropped 60dB (energy ratio 1e-6,
    // since dB-for-energy uses 10*log10 not 20*log10) below that.
    //
    // ALWAYS captures the full 30s window before analyzing, deliberately --
    // an earlier version tried to save time with growing blocks (analyze
    // after 5s, then 10s, etc., stopping at the first crossing found) and
    // treated whatever had been captured SO FAR as if it were the complete
    // signal. That silently under-measured whenever a slower contribution
    // (e.g. del1's own loop, which gets slower as bufferLenMs grows) was
    // still feeding real energy in past the current block boundary: the
    // truncated E(0) reference missed that later energy, so the -60dB
    // target was reached (and reported) too early, before the full decay
    // had actually happened. Analyzing once over the complete capture is
    // the only way to guarantee E(0) reflects the true total energy.
    class RT60MeasureThread : public juce::Thread
    {
    public:
        RT60MeasureThread (GrainReverbParams paramsSnapshot, BreakpointCurve cutoff,
                            BreakpointCurve q, BreakpointCurve tail, double sampleRateToUse,
                            std::function<void (double)> onMeasured)
            : juce::Thread ("RT60 Measure"),
              params (std::move (paramsSnapshot)),
              cutoffCurve (std::move (cutoff)), qCurve (std::move (q)), tailCurve (std::move (tail)),
              sampleRate (sampleRateToUse), callback (std::move (onMeasured))
        {
        }

        void run() override
        {
            // A fresh shared state + engine, entirely separate from the
            // live one the processor owns -- this measurement can't affect
            // (or be affected by) whatever's actually being played.
            GrainReverbSharedState localShared;
            localShared.params = params;
            localShared.cutoffCurve = cutoffCurve;
            localShared.qCurve = qCurve;
            localShared.tailCurve = tailCurve;
            localShared.prepare (sampleRate);

            GrainVoiceEngine engine;
            engine.prepare (sampleRate, localShared);

            constexpr double maxSeconds = 30.0;
            const int totalSamples = (int) (maxSeconds * sampleRate);
            std::vector<double> energy ((size_t) totalSamples, 0.0);

            for (int i = 0; i < totalSamples; ++i)
            {
                if ((i & 0xfff) == 0 && threadShouldExit())
                    return;

                double inL = 0.0, inR = 0.0;
                if (i == 0) { inL = inR = 1.0; } // single-sample impulse, then silence

                double outL = 0.0, outR = 0.0;
                engine.processSample (inL, inR, localShared, outL, outR);
                energy[(size_t) i] = 0.5 * (outL * outL + outR * outR);
            }

            std::vector<double> reverseEnergy (energy.size());
            double acc = 0.0;
            for (int i = (int) energy.size() - 1; i >= 0; --i)
            {
                acc += energy[(size_t) i];
                reverseEnergy[(size_t) i] = acc;
            }

            double measured = maxSeconds; // never crossed -60dB within the cap
            const double target = reverseEnergy[0] * 1.0e-6; // -60dB in energy terms
            for (size_t i = 0; i < reverseEnergy.size(); ++i)
            {
                if (reverseEnergy[i] <= target)
                {
                    measured = (double) i / sampleRate;
                    break;
                }
            }

            const auto result = measured;
            const auto cb = callback;
            juce::MessageManager::callAsync ([cb, result] { cb (result); });
        }

    private:
        GrainReverbParams params;
        BreakpointCurve cutoffCurve, qCurve, tailCurve;
        double sampleRate;
        std::function<void (double)> callback;
    };
}

GrainReverb2AudioProcessorEditor::GrainReverb2AudioProcessorEditor (GrainReverb2AudioProcessor& p)
    : AudioProcessorEditor (&p), processorRef (p),
      earlyBufferVisualizer (p, p.getEarlyEngine(), p.getEarlySharedState(), kEarlyPaneColour),
      earlyBreakpointEditor (p, p.getEarlyEngine(), p.getEarlySharedState()),
      lateBufferVisualizer (p, p.getLateEngine(), p.getLateSharedState(), kLatePaneColour),
      lateBreakpointEditor (p, p.getLateEngine(), p.getLateSharedState()),
      dialsPanel (p)
{
    addAndMakeVisible (earlyReflectionsLabel);
    earlyReflectionsLabel.setText ("Early Reflections", juce::dontSendNotification);
    earlyReflectionsLabel.setJustificationType (juce::Justification::centred);
    earlyReflectionsLabel.setFont (juce::FontOptions (16.0f, juce::Font::bold));

    addAndMakeVisible (lateReflectionsLabel);
    lateReflectionsLabel.setText ("Late Reflections", juce::dontSendNotification);
    lateReflectionsLabel.setJustificationType (juce::Justification::centred);
    lateReflectionsLabel.setFont (juce::FontOptions (16.0f, juce::Font::bold));

    // Separate radio groups per side -- early's curve-select buttons must
    // not deselect late's, and vice versa.
    constexpr int earlyRadioGroupId = 2001;
    constexpr int lateRadioGroupId  = 2002;
    auto setup = [] (juce::TextButton& b, BreakpointEditor& editor, int groupId, CurveKind kind, bool initiallyOn)
    {
        b.setClickingTogglesState (true);
        b.setRadioGroupId (groupId);
        b.setToggleState (initiallyOn, juce::dontSendNotification);
        b.onClick = [&editor, kind] { editor.setActiveCurve (kind); };
    };
    for (auto* b : { &earlyCutoffButton, &earlyQButton, &earlyTailButton,
                     &lateCutoffButton, &lateQButton, &lateTailButton })
        addAndMakeVisible (b);
    setup (earlyCutoffButton, earlyBreakpointEditor, earlyRadioGroupId, CurveKind::Cutoff, true);
    setup (earlyQButton,      earlyBreakpointEditor, earlyRadioGroupId, CurveKind::Q,      false);
    setup (earlyTailButton,   earlyBreakpointEditor, earlyRadioGroupId, CurveKind::Tail,   false);
    setup (lateCutoffButton,  lateBreakpointEditor,  lateRadioGroupId,  CurveKind::Cutoff, true);
    setup (lateQButton,       lateBreakpointEditor,  lateRadioGroupId,  CurveKind::Q,      false);
    setup (lateTailButton,    lateBreakpointEditor,  lateRadioGroupId,  CurveKind::Tail,   false);

    // Tied to LATE reflections only -- see the header comment.
    addAndMakeVisible (measureRt60Button);
    measureRt60Button.onClick = [this]
    {
        if (rt60MeasureThread != nullptr && rt60MeasureThread->isThreadRunning())
            return; // measurement already in progress -- ignore repeat clicks

        auto& lateShared = processorRef.getLateSharedState();
        rt60MeasureThread = std::make_unique<RT60MeasureThread> (
            lateShared.params, lateShared.cutoffCurve, lateShared.qCurve, lateShared.tailCurve,
            processorRef.getSampleRate(),
            [this] (double measured)
            {
                rt60Readout.setText ("RT60 meas ~ " + juce::String (measured, 2) + "s", juce::dontSendNotification);
            });

        rt60Readout.setText ("Measuring...", juce::dontSendNotification);
        rt60MeasureThread->startThread();
    };

    addAndMakeVisible (rt60Readout);
    rt60Readout.setJustificationType (juce::Justification::centred);
    rt60Readout.setFont (juce::FontOptions (12.0f));
    rt60Readout.setText ("RT60: --", juce::dontSendNotification);

    addAndMakeVisible (earlyBufferVisualizer);
    addAndMakeVisible (earlyBreakpointEditor); // added after -- frontmost for paint + mouse
    addAndMakeVisible (lateBufferVisualizer);
    addAndMakeVisible (lateBreakpointEditor);

    addAndMakeVisible (dialsPanel);

    // Same overall size as before Balance moved down into ParamDialsPanel --
    // removing its divider strip reservation in resized() (see paneWidth)
    // hands that freed width straight to the two visualizer panes without
    // needing the window itself to grow any further. The dials area below
    // stays a FIXED 735 wide (see resized(), kDialsAreaWidth) rather than
    // stretching to match, so the window's extra width still shows up as a
    // black margin either side of it.
    setSize (875, 619);
}

GrainReverb2AudioProcessorEditor::~GrainReverb2AudioProcessorEditor()
{
    // Give the measurement thread a chance to notice threadShouldExit() at
    // its next check (at most every 4096 samples -- see RT60MeasureThread::
    // run()) and unwind cleanly before this editor (and the callback that
    // captures `this`) goes away.
    if (rt60MeasureThread != nullptr)
        rt60MeasureThread->stopThread (2000);
}

void GrainReverb2AudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    // The only thing marking early/late's boundary now that Balance no
    // longer lives here as its own strip -- dividerLineBounds is set in
    // resized(), so this can't drift out of sync with the actual layout.
    g.setColour (juce::Colours::grey.withAlpha (0.4f));
    g.drawVerticalLine (dividerLineBounds.getCentreX(),
                         (float) dividerLineBounds.getY(), (float) dividerLineBounds.getBottom());
}

void GrainReverb2AudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced (10);

    // Dials area -- 3 rows tall, both sides fully populated 3x3 grids.
    // FIXED width (735 -- 3 columns of 105 either side of ParamDialsPanel's
    // own 105-wide centre column) and horizontally CENTRED, rather than
    // stretched to fill the window's full width: the visualizer panes
    // below now deliberately get more width than this to feel less
    // cramped, which leaves a black margin either side of the dial area
    // as a visible, accepted trade-off.
    constexpr int kDialsAreaWidth = 735;
    auto dialsFullWidthRow = bounds.removeFromBottom (285);
    bounds.removeFromBottom (10); // gap
    dialsPanel.setBounds (dialsFullWidthRow.withSizeKeepingCentre (kDialsAreaWidth, dialsFullWidthRow.getHeight()));

    // No divider strip reserved anymore -- Balance (its one control) moved
    // down into ParamDialsPanel's centre column as a regular knob, so the
    // early/late visualizer panes now split the width between them,
    // minus a tiny black gap (unpainted -- the window's own black
    // fillAll() in paint() shows straight through) so the two panes don't
    // sit flush against each other.
    constexpr int kPaneGap = 6;
    const int paneWidth = (bounds.getWidth() - kPaneGap) / 2;
    auto earlyPane = bounds.removeFromLeft (paneWidth);
    // dividerLineBounds is this gap itself -- its centre X is where the
    // thin separator line in paint() gets drawn.
    dividerLineBounds = bounds.removeFromLeft (kPaneGap);
    auto latePane = bounds; // remainder

    // ---- Early pane (left) ----
    auto earlyButtonRow = earlyPane.removeFromTop (28);
    const int earlyW = earlyButtonRow.getWidth() / 3;
    earlyCutoffButton.setBounds (earlyButtonRow.removeFromLeft (earlyW).reduced (4, 2));
    earlyQButton.setBounds (earlyButtonRow.removeFromLeft (earlyW).reduced (4, 2));
    earlyTailButton.setBounds (earlyButtonRow.reduced (4, 2));
    earlyPane.removeFromTop (10); // breathing room -- see the late pane's identical comment below
    // Label now sits BETWEEN the visualizer and ParamDialsPanel's knobs
    // below, not above the button row -- removed from the bottom of the
    // pane before the visualizer claims whatever's left.
    earlyReflectionsLabel.setBounds (earlyPane.removeFromBottom (24));
    earlyBufferVisualizer.setBounds (earlyPane);
    earlyBreakpointEditor.setBounds (earlyPane); // identical bounds -- lines the overlay up with the waveform

    // ---- Late pane (right) ----
    auto lateButtonRow = latePane.removeFromTop (28);
    rt60Readout.setBounds (lateButtonRow.removeFromRight (110).reduced (4, 2));
    measureRt60Button.setBounds (lateButtonRow.removeFromRight (90).reduced (4, 2));
    const int lateW = lateButtonRow.getWidth() / 3;
    lateCutoffButton.setBounds (lateButtonRow.removeFromLeft (lateW).reduced (4, 2));
    lateQButton.setBounds (lateButtonRow.removeFromLeft (lateW).reduced (4, 2));
    lateTailButton.setBounds (lateButtonRow.reduced (4, 2));
    // Just visual breathing room below the button row -- the ACTUAL fix for
    // max-value points getting clipped is kVisualizerTopMargin, applied
    // inside both CircularBufferVisualizer's and BreakpointEditor's own
    // plot-area math (clipping happens at a component's own bounds, not
    // here, so this gap alone wouldn't have fixed it).
    latePane.removeFromTop (10);
    lateReflectionsLabel.setBounds (latePane.removeFromBottom (24));
    lateBufferVisualizer.setBounds (latePane);
    lateBreakpointEditor.setBounds (latePane);
}
