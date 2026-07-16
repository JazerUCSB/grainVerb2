#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "CircularBufferVisualizer.h"
#include "BreakpointEditor.h"
#include "ParamDialsPanel.h"
#include "TabButtonLookAndFeel.h"
#include "Rt60ButtonLookAndFeel.h"

// The editor is the plugin's GUI. It holds a reference back to the
// processor so it can read/write parameters -- it never owns DSP state.
//
// Layout: early reflections on the LEFT, late reflections (the original
// del1/del2 engine) on the RIGHT, with just a thin dividing line between
// them -- Balance used to live here as a dedicated vertical fader, but is
// now just another knob down in ParamDialsPanel's centre column, so both
// visualizer panes get the full width to themselves instead. Both sides
// use the exact same CircularBufferVisualizer/BreakpointEditor classes,
// just constructed against different engine/state references -- see those
// classes' headers.
class GrainReverb2AudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit GrainReverb2AudioProcessorEditor (GrainReverb2AudioProcessor&);
    ~GrainReverb2AudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    GrainReverb2AudioProcessor& processorRef;

    // Section headers -- one strip reserved between each pane's own
    // visualizer and ParamDialsPanel's knobs below it (see resized()), so
    // it's unambiguous which side is which even before reading any
    // individual dial.
    juce::Label earlyReflectionsLabel;
    juce::Label lateReflectionsLabel;

    // Styled like browser tabs (see TabButtonLookAndFeel) -- the selected
    // one fills with its own side's pane colour and has no bottom border,
    // reading as flush with the visualizer below; unselected ones are grey
    // with a bottom border marking them inactive. Set on all 6 (not
    // app-wide), reset to nullptr in the destructor before
    // tabButtonLookAndFeel itself is destroyed.
    juce::TextButton earlyCutoffButton { "Cutoff" }, earlyQButton { "Q" }, earlyTailButton { "Tail" };
    juce::TextButton lateCutoffButton { "Cutoff" }, lateQButton { "Q" }, lateTailButton { "Tail" };
    TabButtonLookAndFeel tabButtonLookAndFeel;

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
    // Text has an explicit line break -- this now lives in the narrow
    // black margin beside the dial grid (see resized()), not the wide
    // button row it used to share with the Cutoff/Q/Tail tabs.
    juce::TextButton measureRt60Button { "Measure\nRT60" };
    Rt60ButtonLookAndFeel rt60ButtonLookAndFeel;
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

    ParamDialsPanel dialsPanel;

    // A single thin line marking the boundary between the early (left) and
    // late (right) visualizer panes -- set in resized(), read back in
    // paint() so it can never drift out of sync with the actual layout.
    // All that's left of the old divider strip now that Balance (its one
    // control) moved down into ParamDialsPanel's centre column.
    juce::Rectangle<int> dividerLineBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrainReverb2AudioProcessorEditor)
};
