#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

// A grid of small rotary dials, one per APVTS scalar parameter, each bound
// via AudioProcessorValueTreeState::SliderAttachment. Turning a dial writes
// straight to the parameter -- host automation, undo, and preset save/
// restore all keep working exactly as if a DAW automated it directly,
// since the attachment is the same mechanism either way.
//
// Split into two independent 3-column grids -- early reflections on the
// left, late on the right -- matching the visualizer panes above. Each
// half is filled with that engine's own pane colour (see PaneTheme.h), and
// every dial gets its own lighter-tinted rounded "chip" behind its title
// and readout, replacing the default plain text-box look.
class ParamDialsPanel : public juce::Component
{
public:
    explicit ParamDialsPanel (GrainReverb2AudioProcessor& processorToUse);

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
    };

    // isEarly picks which pane colour this dial's chip uses.
    Dial& addDial (juce::OwnedArray<Dial>& group, const juce::String& paramID,
                    const juce::String& labelText, bool isEarly);

    void layoutGroup (juce::OwnedArray<Dial>& group, juce::Rectangle<int> bounds, int columns);

    GrainReverb2AudioProcessor& processor;

    // Early: 3 columns x 3 rows (row 3 only has 2 dials -- Jitter,
    // Dispersion -- the 3rd cell is simply left empty). Late: 3 columns x
    // 3 rows, fully populated (row 3 adds Mix, which early has no
    // equivalent of -- see PluginProcessor's Balance control instead).
    juce::OwnedArray<Dial> earlyDials;
    juce::OwnedArray<Dial> lateDials;
    static constexpr int groupColumns = 3;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamDialsPanel)
};
