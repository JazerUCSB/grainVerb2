#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

// A grid of small rotary dials, one per APVTS scalar parameter, each bound
// via AudioProcessorValueTreeState::SliderAttachment. Turning a dial writes
// straight to the parameter -- host automation, undo, and preset save/
// restore all keep working exactly as if a DAW automated it directly,
// since the attachment is the same mechanism either way. Deliberately
// plain: default JUCE rotary style, a text box for the numeric value, a
// short label underneath. No custom look-and-feel yet.
class ParamDialsPanel : public juce::Component
{
public:
    explicit ParamDialsPanel (GrainReverb2AudioProcessor& processorToUse);

    void resized() override;

private:
    struct Dial
    {
        juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    Dial& addDial (const juce::String& paramID, const juce::String& labelText);

    GrainReverb2AudioProcessor& processor;
    juce::OwnedArray<Dial> dials;

    // 3x3 for 9 dials -- fewer, wider columns than before so each dial is
    // noticeably bigger (a rotary slider's diameter is bounded by the
    // smaller of its cell's width/height).
    static constexpr int columns = 3;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ParamDialsPanel)
};
