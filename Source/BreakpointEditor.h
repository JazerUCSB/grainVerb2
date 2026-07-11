#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "GrainVoiceEngine.h"
#include "VisualizerLayout.h"

// Which curve is currently active/editable. Declared at namespace scope
// (not nested in BreakpointEditor) so PluginEditor's curve-select buttons
// can reference it too.
enum class CurveKind { Cutoff, Q, Tail };

// Interactive editor for the three user-facing breakpoint curves (cutoff,
// Q, tail), drawn as a TRANSPARENT OVERLAY directly on top of
// CircularBufferVisualizer's del1 waveform. Both share the exact same
// dn/seconds x-axis, so overlaying them lets you see a curve edit against
// the actual buffer content it's shaping (though not a claim about
// perceived output timing -- see the RT60 discussion; a dedicated,
// RT60-scaled output view is where that mapping belongs). PluginEditor
// gives both components identical bounds and adds this one second, making
// it frontmost for both painting and mouse hit-testing.
//
// Keymapping:
//   - Click and drag within a small radius of an existing point to move it.
//     The first and last points are pinned to dn=0/dn=1 and only move
//     vertically; every other point moves freely, clamped between its
//     neighbors so points can't cross and reorder.
//   - Double-click empty curve area to add a new point there.
//   - Right-click (or ctrl-click on a POINT) an existing interior point to
//     delete it. Endpoints can't be deleted.
//   - Option/Alt-click a SEGMENT (not a point) to toggle it between linear
//     and exponential. A small straight/curved icon follows the cursor
//     while Option is held, previewing the segment currently under it.
//   - Ctrl-drag a SEGMENT that's already Exponential to bend its shape
//     continuously from concave-up through to concave-down (the
//     Breakpoint::curvature field) -- vertical drag distance controls the
//     bend amount. A small bowed-line icon follows the cursor while Ctrl is
//     held over a curved segment, tilting to preview the current bend.
//     No-op on Linear segments -- toggle to Exponential with Option first.
// Every edit calls GrainReverbSharedState::rebake() immediately -- audible
// in real time via the Step 5 atomic double-buffer swap.
class BreakpointEditor : public juce::Component
{
public:
    // processorForSampleRate is only used for getSampleRate() (shared by
    // every engine in the plugin); engineToShow/stateToEdit point at
    // whichever specific engine/curve-set this instance edits -- late or
    // early reflections both use this same class, just constructed with
    // different references. stateToEdit is non-const (points/mouse edits
    // mutate its curves directly and call rebake()).
    BreakpointEditor (GrainReverb2AudioProcessor& processorForSampleRate,
                       const GrainVoiceEngine& engineToShow,
                       GrainReverbSharedState& stateToEdit);

    void setActiveCurve (CurveKind kind) { activeCurve = kind; repaint(); }
    CurveKind getActiveCurve() const { return activeCurve; }

    void paint (juce::Graphics&) override;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

private:
    struct CurveStyle
    {
        juce::Colour colour;
        double minValue, maxValue;
        bool logScale;
    };

    BreakpointCurve& curveFor (CurveKind kind);
    const CurveStyle& styleFor (CurveKind kind) const;

    juce::Rectangle<float> getPlotArea() const;
    float dnToX (double dn, juce::Rectangle<float> plot) const;
    double xToDn (float x, juce::Rectangle<float> plot) const;
    float valueToY (double value, CurveKind kind, juce::Rectangle<float> plot) const;
    double yToValue (float y, CurveKind kind, juce::Rectangle<float> plot) const;

    void drawCurve (juce::Graphics& g, CurveKind kind, float opacity, bool drawHandles);
    void drawSegmentTypeIcon (juce::Graphics& g);
    void drawCurvatureIcon (juce::Graphics& g);
    void drawDragValuePopup (juce::Graphics& g);
    void drawGrid (juce::Graphics& g);
    std::vector<double> gridValuesFor (CurveKind kind) const;
    juce::String formatGridLabel (double value, CurveKind kind) const;
    int findNearbyPoint (const BreakpointCurve& curve, juce::Point<float> pos,
                          juce::Rectangle<float> plot, CurveKind kind) const;
    size_t segmentIndexAt (const BreakpointCurve& curve, double dn) const;

    GrainReverb2AudioProcessor& processor;
    const GrainVoiceEngine& engine;
    GrainReverbSharedState& sharedState;

    CurveKind activeCurve = CurveKind::Cutoff;
    int draggedPointIndex = -1;
    bool altHeld = false;
    bool ctrlHeld = false;
    juce::Point<float> lastMousePos;

    // Ctrl-drag curvature adjustment state, active between mouseDown and
    // mouseUp when a drag starts on an Exponential segment with Ctrl held.
    bool curvatureDragging = false;
    size_t curvatureDragSegment = 0;
    float curvatureDragStartY = 0.0f;
    double curvatureDragStartValue = 0.0;

    static constexpr float hitRadius = 8.0f;
    static constexpr double maxCurvature = 8.0;
    static constexpr float curvatureSensitivity = 0.05f; // curvature units per pixel dragged

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BreakpointEditor)
};
