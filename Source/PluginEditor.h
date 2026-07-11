#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "CircularBufferVisualizer.h"
#include "BreakpointEditor.h"
#include "ParamDialsPanel.h"

// The editor is the plugin's GUI. It holds a reference back to the
// processor so it can read/write parameters -- it never owns DSP state.
//
// Layout: early reflections on the LEFT, late reflections (the original
// del1/del2 engine) on the RIGHT, with a thin divider and a vertical
// balance fader in between. Both sides use the exact same
// CircularBufferVisualizer/BreakpointEditor classes, just constructed
// against different engine/state references -- see those classes' headers.
class GrainReverb2AudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit GrainReverb2AudioProcessorEditor (GrainReverb2AudioProcessor&);
    ~GrainReverb2AudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    GrainReverb2AudioProcessor& processorRef;

    juce::TextButton earlyCutoffButton { "Cutoff" }, earlyQButton { "Q" }, earlyTailButton { "Tail" };
    juce::TextButton lateCutoffButton { "Cutoff" }, lateQButton { "Q" }, lateTailButton { "Tail" };

    // Empirical measurement: runs a throwaway GrainVoiceEngine on a
    // background thread with an impulse + silence, faster than real time
    // (processSample() has no wall-clock dependency), and measures the
    // actual decay via Schroeder backward integration. See PluginEditor.cpp
    // for the RT60MeasureThread that does the work. Stored as a
    // juce::Thread base pointer -- the concrete subclass is defined
    // entirely in the .cpp, this only ever needs start/stop/isRunning.
    // Tied to LATE reflections only (its defaults match late's own
    // 200-voice/6s-1s sizing) -- early reflections doesn't get its own
    // measurement button, at least for now.
    juce::TextButton measureRt60Button { "Measure RT60" };
    std::unique_ptr<juce::Thread> rt60MeasureThread;
    juce::Label rt60Readout;

    // Both pairs given IDENTICAL bounds to their own side in resized().
    // The *BreakpointEditor of each pair is added after its own
    // *BufferVisualizer (frontmost, both for painting and mouse
    // hit-testing) and paints transparently, so the curve overlay sits
    // directly on top of the waveform driving it.
    CircularBufferVisualizer earlyBufferVisualizer;
    BreakpointEditor earlyBreakpointEditor;
    CircularBufferVisualizer lateBufferVisualizer;
    BreakpointEditor lateBreakpointEditor;

    // Crossfades the early/late wet signals -- a dedicated vertical fader
    // sitting in the divider between the two panes (not just another dial
    // in the grid below), bound directly to the "balance" APVTS parameter.
    juce::Slider balanceSlider { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::Label balanceLabel;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> balanceAttachment;

    ParamDialsPanel dialsPanel;

    // The divider strip's full vertical span (set in resized(), used in
    // paint() to draw the border line) -- stored rather than recomputed
    // with magic numbers in paint(), so it can never drift out of sync
    // with the actual layout.
    juce::Rectangle<int> dividerLineBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrainReverb2AudioProcessorEditor)
};
