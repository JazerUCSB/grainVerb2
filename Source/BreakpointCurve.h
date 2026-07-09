#pragma once

#include <array>
#include <cmath>
#include <vector>
#include <juce_core/juce_core.h>

// Curve resolution shared by every baked table (cutoff/Q/tail) and by the
// visualizer's x-axis, so pixel position, table slot, and breakpoint x all
// agree on what dn means. Declared here (not in GrainReverbEngine.h) since
// the breakpoint editor component (Step 7) needs it too, without needing to
// know anything about grains or delay buffers.
inline constexpr int kNumTableSlots = 512;

enum class CurveInterpolation { Linear, Exponential };

struct Breakpoint
{
    double x = 0.0; // normalized read distance, dn in [0, 1] (0 = write head, 1 = oldest)
    double y = 0.0; // curve value: Hz for cutoff, ratio for Q, gain for tail

    // Interpolation used for the segment FROM this point TO the next one.
    // Meaningless/unused on the curve's last point (no "next" to blend
    // toward). Per-segment rather than per-curve so a curve can mix a
    // linear stretch and an exponential stretch -- e.g. Option-click a
    // segment in the editor to toggle it (Step 7).
    CurveInterpolation segmentType = CurveInterpolation::Linear;

    // Bend amount for this segment when segmentType == Exponential; ignored
    // for Linear segments. 0 = the segment would be a straight line (same
    // as Linear); positive/negative bends it concave-down/concave-up (or
    // vice versa -- see evaluate() below), with magnitude controlling how
    // sharp the bend is. Ctrl-drag on the segment adjusts this continuously
    // (Step 7 follow-up); Option-click only toggles segmentType itself.
    double curvature = 4.0;
};

// A user-editable curve: an ordered list of breakpoints, each carrying the
// interpolation type for the segment that follows it. This struct is the
// SOURCE OF TRUTH -- bakeInto() renders it down to a dense 512-slot table,
// and that dense table (not this struct) is what the audio thread and the
// coefficient bake ever read from.
//
// Invariant: points must stay sorted by x, and must always include x=0 and
// x=1 (the two fixed endpoints -- see the editor's keymapping in Step 7,
// where the first/last point can move vertically but never horizontally).
struct BreakpointCurve
{
    std::vector<Breakpoint> points;

    double evaluate (double dn) const
    {
        jassert (points.size() >= 2);
        dn = juce::jlimit (0.0, 1.0, dn);

        // Only a handful of points ever exist (hand-placed breakpoints), so
        // a linear scan for the bracketing segment is simpler than a binary
        // search and plenty fast -- this runs during a bake, never per
        // audio sample.
        size_t i = 0;
        while (i + 1 < points.size() && dn > points[i + 1].x)
            ++i;

        const auto& p0 = points[i];
        const auto& p1 = points[juce::jmin (i + 1, points.size() - 1)];

        if (p1.x <= p0.x)
            return p0.y;

        const double t = (dn - p0.x) / (p1.x - p0.x);

        if (p0.segmentType == CurveInterpolation::Exponential)
        {
            // Exponential-ease bend: reshape t itself before blending
            // linearly between y0 and y1, rather than blending in log
            // space. tShaped(0)=0, tShaped(1)=1 always (endpoints never
            // move), and the curvature parameter k continuously bends the
            // shape from concave-up (k<0) through linear (k->0) to
            // concave-down (k>0) -- this is what Ctrl-drag adjusts in the
            // editor. Same family of curve DAW envelope "curve" handles use.
            const double k = p0.curvature;
            double tShaped;
            if (std::abs (k) < 1.0e-6)
                tShaped = t; // limiting case as k -> 0 is plain linear
            else
                tShaped = (std::exp (k * t) - 1.0) / (std::exp (k) - 1.0);
            return p0.y + tShaped * (p1.y - p0.y);
        }

        return p0.y + t * (p1.y - p0.y); // linear
    }

    void bakeInto (std::array<double, kNumTableSlots>& table) const
    {
        for (int j = 0; j < kNumTableSlots; ++j)
        {
            const double dn = (double) j / (double) (kNumTableSlots - 1);
            table[(size_t) j] = evaluate (dn);
        }
    }
};
