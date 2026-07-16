#pragma once

// Layout constants shared between CircularBufferVisualizer and
// BreakpointEditor so their plot areas line up pixel-for-pixel when
// BreakpointEditor is overlaid on top of the waveform (Step 7/8). Keeping
// these in one place instead of duplicating literals in both files avoids
// the two silently drifting out of alignment if either changes.
// Widened (40->72) to fit the "Read Range" axis-caption badge
// (CircularBufferVisualizer's bottom-left corner) at a legible size --
// this margin also holds BreakpointEditor's own value labels, which
// benefit from the extra room too.
inline constexpr int kVisualizerLeftMargin = 72; // reserved for y-axis value labels
// Heightened (20->26->34) alongside the margin widening above -- the
// second bump (26->34) is for the badge's text wrapping to two lines
// ("Buffer"/"Range" via drawFittedText) instead of one, so both words
// stay legible at this width rather than the second getting squeezed out.
inline constexpr int kVisualizerRulerHeight = 34; // reserved for the x-axis (seconds) ruler

// A point/handle sitting exactly at the plot's maximum value (e.g.
// cutoff's 20kHz ceiling) is drawn as a circle CENTERED on that pixel --
// with zero top margin, half the circle falls above y=0 and gets clipped
// by the component's own bounds (JUCE clips child painting to its local
// rect regardless of how much empty space exists further out in the
// parent's layout). This reserves real headroom inside the plot area
// itself so that doesn't happen; both components must apply it identically
// or their coordinate mapping drifts out of alignment.
inline constexpr int kVisualizerTopMargin = 8;

// Same clipping bug as kVisualizerTopMargin above, but on the RIGHT edge:
// a breakpoint sitting at dn = 1 (the far right of its curve) is drawn as
// a circle centered exactly on the component's right edge, so half of it
// got clipped with zero margin here. Both CircularBufferVisualizer and
// BreakpointEditor must reserve this identically, same as every other
// margin in this file.
inline constexpr int kVisualizerRightMargin = 10;
