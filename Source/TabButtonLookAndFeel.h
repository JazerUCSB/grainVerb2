#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Cutoff/Q/Tail curve-select buttons styled like web browser tabs: the
// selected tab is filled with the SAME colour as the pane/visualizer it
// controls and has no bottom border, so it reads as part of the same
// surface as the content below it; unselected tabs are plain grey with a
// bottom border line marking them as separate/inactive. Each button's own
// "selected" fill colour is read from a Component property ("paneColour",
// set in PluginEditor to kEarlyPaneColour/kLatePaneColour per side) rather
// than hardcoded here, since one shared instance draws both early's and
// late's buttons.
class TabButtonLookAndFeel : public juce::LookAndFeel_V4
{
public:
    void drawButtonBackground (juce::Graphics& g, juce::Button& button, const juce::Colour&,
                                bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        auto bounds = button.getLocalBounds().toFloat();
        const bool selected = button.getToggleState();

        // Classic tab shape -- only the top two corners rounded, so the
        // selected tab's flat bottom edge reads as continuous with the
        // pane below it rather than a separate rounded box sitting on top.
        constexpr float corner = 6.0f;
        juce::Path path;
        path.addRoundedRectangle (bounds.getX(), bounds.getY(), bounds.getWidth(), bounds.getHeight(),
                                   corner, corner, true, true, false, false);

        if (selected)
        {
            auto& props = button.getProperties();
            auto fill = props.contains ("paneColour")
                            ? juce::Colour ((juce::uint32) (juce::int64) props["paneColour"])
                            : juce::Colours::darkgrey;
            if (shouldDrawButtonAsDown)             fill = fill.brighter (0.1f);
            else if (shouldDrawButtonAsHighlighted) fill = fill.brighter (0.05f);
            g.setColour (fill);
            g.fillPath (path);
        }
        else
        {
            auto fill = juce::Colours::grey.darker (0.5f);
            if (shouldDrawButtonAsDown)             fill = fill.brighter (0.2f);
            else if (shouldDrawButtonAsHighlighted) fill = fill.brighter (0.1f);
            g.setColour (fill);
            g.fillPath (path);

            // Bottom border marking it as NOT the active tab/plane -- the
            // selected tab deliberately has none, so it reads as flush
            // with the pane below instead of a separate box.
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.fillRect (bounds.removeFromBottom (3.0f));
        }
    }
};
