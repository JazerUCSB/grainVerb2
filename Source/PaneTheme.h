#pragma once

#include <juce_graphics/juce_graphics.h>

// Shared colour identity for the early (left) and late (right) panes --
// used as the background fill for each side's CircularBufferVisualizer
// and ParamDialsPanel section, so the two engines read as visually
// distinct at a glance without needing to read any label. The "light"
// variants are for the per-dial title/readout chip backgrounds, which
// need to sit visibly ON TOP of the darker pane background beneath them.
inline const juce::Colour kEarlyPaneColour = juce::Colour (0xff3f2c30); // dark rose taupe
inline const juce::Colour kLatePaneColour  = juce::Colour (0xff2c2f40); // dark purple-blue-grey

inline const juce::Colour kEarlyLightColour = kEarlyPaneColour.brighter (0.55f);
inline const juce::Colour kLateLightColour  = kLatePaneColour.brighter (0.55f);
