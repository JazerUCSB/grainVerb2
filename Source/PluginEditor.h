#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "CircularBufferVisualizer.h"
#include "BreakpointEditor.h"
#include "ParamDialsPanel.h"

// The editor is the plugin's GUI. It holds a reference back to the
// processor so it can read/write parameters -- it never owns DSP state.
class GrainReverb2AudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit GrainReverb2AudioProcessorEditor (GrainReverb2AudioProcessor&);
    ~GrainReverb2AudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    GrainReverb2AudioProcessor& processorRef;

    juce::TextButton cutoffButton { "Cutoff" }, qButton { "Q" }, tailButton { "Tail" };

    // On-demand RT60 estimate (cheap closed-form, PDF Section 2.6) -- not a
    // live/continuous measurement, just a snapshot computed from the
    // current params when clicked, shown in rt60Readout.
    juce::TextButton rt60Button { "Calc RT60" };
    juce::Label rt60Readout;

    // bufferVisualizer and breakpointEditor are given IDENTICAL bounds in
    // resized(). breakpointEditor is added second (frontmost, both for
    // painting and mouse hit-testing) and paints transparently, so the
    // curve overlay sits directly on top of the waveform driving it.
    CircularBufferVisualizer bufferVisualizer;
    BreakpointEditor breakpointEditor;
    ParamDialsPanel dialsPanel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrainReverb2AudioProcessorEditor)
};
