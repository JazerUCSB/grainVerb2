#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "RadialKnobLookAndFeel.h"

// A grid of small rotary dials, one per APVTS scalar parameter, each bound
// via AudioProcessorValueTreeState::SliderAttachment. Turning a dial writes
// straight to the parameter -- host automation, undo, and preset save/
// restore all keep working exactly as if a DAW automated it directly,
// since the attachment is the same mechanism either way.
//
// Three columns of dials -- early reflections on the left, a narrow
// shared centre column, late reflections on the right -- matching the
// visualizer panes above. Each side is filled with that engine's own pane
// colour (see PaneTheme.h); the centre column gets the neutral shared
// colour. Every dial sits directly on its pane's background and gets a
// rounded OUTLINE border (colour stored per-dial, since the centre column
// mixes early-/late-/shared-tinted borders within one group) grouping its
// title, knob, and readout together.
class ParamDialsPanel : public juce::Component
{
public:
    explicit ParamDialsPanel (GrainReverb2AudioProcessor& processorToUse);
    ~ParamDialsPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Dial
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;

        // Set in resized(), read back in paint() to draw this dial's own
        // chip -- avoids recomputing (or duplicating) the grid math twice.
        juce::Rectangle<int> cellBounds;

        // Set once in addDial() -- per-DIAL rather than per-GROUP, since
        // the centre column mixes early-/late-/shared-tinted borders within
        // one juce::OwnedArray<Dial>.
        juce::Colour borderColour;
    };

    // lowLabelOverride/highLabelOverride, if non-empty, replace the dial's
    // raw min/max value in RadialKnobLookAndFeel's endpoint labels with
    // custom text (e.g. Balance shows "Late"/"Early" instead of 0/1).
    Dial& addDial (juce::OwnedArray<Dial>& group, const juce::String& paramID,
                    const juce::String& labelText, juce::Colour borderColour,
                    const juce::String& lowLabelOverride = {}, const juce::String& highLabelOverride = {});

    void layoutGroup (juce::OwnedArray<Dial>& group, juce::Rectangle<int> bounds, int columns);

    GrainReverb2AudioProcessor& processor;

    // Early/late: 3 columns x 3 rows, both fully populated (9 dials each --
    // Early Gain/Late Gain moved in from the old centre column, completing
    // both grids exactly).
    juce::OwnedArray<Dial> earlyDials;
    juce::OwnedArray<Dial> lateDials;
    static constexpr int groupColumns = 3;

    // Single column between early/late (1 column x 3 rows -- fills the
    // panel's full height exactly the same as early/late's own 3 rows, no
    // special-casing needed) for controls that don't belong to either
    // side's own 3x3 grid: Predelay, Balance (moved down from a dedicated
    // fader in the divider strip above -- now just another knob, with
    // "Late"/"Early" as its low/high labels), and Mix -- all genuinely
    // shared, affecting both engines' combined output.
    juce::OwnedArray<Dial> centerDials;

    // Shared by every dial in all three groups (set in addDial()) -- a
    // plain circle with a radial pointer line and unlabeled tick marks,
    // replacing LookAndFeel_V4's default filled arc.
    RadialKnobLookAndFeel radialKnobLookAndFeel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamDialsPanel)
};
