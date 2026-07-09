#pragma once

// Layout constants shared between CircularBufferVisualizer and
// BreakpointEditor so their plot areas line up pixel-for-pixel when
// BreakpointEditor is overlaid on top of the waveform (Step 7/8). Keeping
// these in one place instead of duplicating literals in both files avoids
// the two silently drifting out of alignment if either changes.
inline constexpr int kVisualizerLeftMargin = 40; // reserved for y-axis value labels
inline constexpr int kVisualizerRulerHeight = 20; // reserved for the x-axis (seconds) ruler

// A point/handle sitting exactly at the plot's maximum value (e.g.
// cutoff's 20kHz ceiling) is drawn as a circle CENTERED on that pixel --
// with zero top margin, half the circle falls above y=0 and gets clipped
// by the component's own bounds (JUCE clips child painting to its local
// rect regardless of how much empty space exists further out in the
// parent's layout). This reserves real headroom inside the plot area
// itself so that doesn't happen; both components must apply it identically
// or their coordinate mapping drifts out of alignment.
inline constexpr int kVisualizerTopMargin = 8;
