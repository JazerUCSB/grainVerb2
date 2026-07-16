#pragma once

#include <juce_graphics/juce_graphics.h>

// Shared colour identity for the early (left) and late (right) panes --
// used as the background fill for each side's CircularBufferVisualizer
// and ParamDialsPanel section, so the two engines read as visually
// distinct at a glance without needing to read any label.
inline const juce::Colour kEarlyPaneColour = juce::Colour (0xff3f2c30); // dark rose taupe
inline const juce::Colour kLatePaneColour  = juce::Colour (0xff2c2f40); // dark purple-blue-grey

// Neutral identity for shared controls (affect BOTH engines equally --
// Mix, Predelay) -- deliberately dark/desaturated rather than early- or
// late-tinted, so they read as "belongs to neither side" at a glance, the
// same way kEarly/kLate do for their own panes.
inline const juce::Colour kSharedPaneColour = juce::Colour (0xff121214); // near-black neutral grey

// Border-only accent colours for ParamDialsPanel's dial cells -- replaced
// the earlier filled light-colour chip look: dials now sit directly on
// their pane's own background (kEarly/kLate/kSharedPaneColour, no extra
// fill layer on top), with just an outlined border grouping each dial's
// title/knob/readout together. Deliberately a DIFFERENT hue family from
// kEarly/kLatePaneColour (rose taupe / blue-grey) so the border reads as
// its own distinct accent rather than a lighter shade of the pane itself.
inline const juce::Colour kEarlyBorderColour  = juce::Colour (0xff1f5c38); // dark green
inline const juce::Colour kLateBorderColour   = juce::Colour (0xff1c3f66); // dark blue
inline const juce::Colour kSharedBorderColour = juce::Colour (0xff555555); // neutral dark grey
