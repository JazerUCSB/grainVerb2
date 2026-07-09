#include "PluginEditor.h"

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

    addAndMakeVisible (voiceChannelButton);
    voiceChannelButton.onClick = [this]
    {
        const bool showingLeft = bufferVisualizer.getVoiceChannel() == CircularBufferVisualizer::VoiceChannel::Left;
        const auto newChannel = showingLeft ? CircularBufferVisualizer::VoiceChannel::Right
                                             : CircularBufferVisualizer::VoiceChannel::Left;
        bufferVisualizer.setVoiceChannel (newChannel);
        voiceChannelButton.setButtonText (newChannel == CircularBufferVisualizer::VoiceChannel::Left
                                               ? "Showing: L" : "Showing: R");
    };

    addAndMakeVisible (bufferVisualizer);
    addAndMakeVisible (breakpointEditor); // added after -- frontmost for paint + mouse
    addAndMakeVisible (dialsPanel);

    setSize (700, 640); // dials panel now needs 3 rows (9 dials / 4 columns) instead of 2
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
    voiceChannelButton.setBounds (buttonRow.removeFromRight (100).reduced (4, 2));
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

    auto dialsArea = bounds.removeFromBottom (220);
    bounds.removeFromBottom (10); // gap

    // Identical bounds for both components -- this is what makes the
    // breakpoint overlay line up with the waveform underneath it.
    bufferVisualizer.setBounds (bounds);
    breakpointEditor.setBounds (bounds);

    dialsPanel.setBounds (dialsArea);
}
