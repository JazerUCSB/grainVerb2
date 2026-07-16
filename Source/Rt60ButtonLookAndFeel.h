#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Just the Measure RT60 button: less rounded corners than LookAndFeel_V4's
// default TextButton, a dedicated dark-purple fill (not the shared grey
// the Cutoff/Q/Tail tabs use), and a brighter purple outline -- a whole
// separate colourway, not just a border, so it doesn't read as a fourth
// tab but as a distinct one-shot action.
class Rt60ButtonLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
        constexpr float corner = 3.0f;

        auto fill = juce::Colour (0xff3a1f4a); // dark purple/plum, distinct from the tabs' grey
        if (shouldDrawButtonAsDown)             fill = fill.darker (0.2f);
        else if (shouldDrawButtonAsHighlighted) fill = fill.brighter (0.15f);
        g.setColour (fill);
        g.fillRoundedRectangle (bounds, corner);

        g.setColour (juce::Colours::purple.brighter (0.4f));
        g.drawRoundedRectangle (bounds, corner, 1.5f);
    }

    // Explicit 2-line wrap ("Measure\nRT60") rather than one long line --
    // this button now lives in the narrow black margin beside the dial
    // grid (see PluginEditor::resized()), not the wide button row it used
    // to share with the Cutoff/Q/Tail tabs.
    void drawButtonText (juce::Graphics& g, juce::TextButton& button, bool, bool) override
    {
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (11.0f));
        g.drawFittedText (button.getButtonText(), button.getLocalBounds().reduced (2),
                           juce::Justification::centred, 2);
    }
};
