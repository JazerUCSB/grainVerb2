#include "BreakpointEditor.h"
#include <algorithm>
#include <cmath>

BreakpointEditor::BreakpointEditor (GrainReverb2AudioProcessor& processorForSampleRate,
                                     const GrainVoiceEngine& engineToShow,
                                     GrainReverbSharedState& stateToEdit)
    : processor (processorForSampleRate), engine (engineToShow), sharedState (stateToEdit)
{
}

BreakpointCurve& BreakpointEditor::curveFor (CurveKind kind)
{
    switch (kind)
    {
        case CurveKind::Cutoff: return sharedState.cutoffCurve;
        case CurveKind::Q:      return sharedState.qCurve;
        case CurveKind::Tail:   return sharedState.tailCurve;
    }
    jassertfalse;
    return sharedState.cutoffCurve;
}

const BreakpointEditor::CurveStyle& BreakpointEditor::styleFor (CurveKind kind) const
{
    // Ranges match the original gen~ Params these curves subsumed
    // (fcNear/fcFar, filtQ) -- see GrainReverbSharedState.h.
    static const CurveStyle cutoffStyle { juce::Colours::cyan,    20.0, 20000.0, true  };
    // Log scale: the default 0.707 sits at ~1.2% up a linear axis but ~40%
    // up a log one. Capped at 5.0 (not the gen~ patch's original 50 max) --
    // above that the RBJ lowpass starts self-resonating audibly/ringing.
    static const CurveStyle qStyle      { juce::Colours::yellow, 0.1,  5.0,      true  };
    static const CurveStyle tailStyle   { juce::Colours::magenta, 0.0, 1.0,      false };
    switch (kind)
    {
        case CurveKind::Cutoff: return cutoffStyle;
        case CurveKind::Q:      return qStyle;
        case CurveKind::Tail:   return tailStyle;
    }
    jassertfalse;
    return cutoffStyle;
}

juce::Rectangle<float> BreakpointEditor::getPlotArea() const
{
    // Deliberately matches CircularBufferVisualizer's waveformArea exactly
    // (same shared margins, no extra reduction) -- PluginEditor gives both
    // components identical setBounds(), so this overlay lines up pixel for
    // pixel with del1's waveform underneath it. The left margin is where
    // drawGrid() below puts the active curve's value labels.
    auto bounds = getLocalBounds();
    bounds.removeFromBottom (kVisualizerRulerHeight);
    bounds.removeFromLeft (kVisualizerLeftMargin);
    bounds.removeFromTop (kVisualizerTopMargin); // headroom so a max-value point isn't top-clipped
    return bounds.toFloat();
}

std::vector<double> BreakpointEditor::gridValuesFor (CurveKind kind) const
{
    switch (kind)
    {
        case CurveKind::Cutoff: return { 50.0, 100.0, 500.0, 1000.0, 5000.0, 10000.0, 20000.0 };
        // Roughly log-spaced over the capped 0.1-5.0 range, and includes
        // 0.707 explicitly since that's the default value -- the flat
        // default Q curve now sits right on a labeled gridline instead of
        // floating between unrelated ticks.
        case CurveKind::Q:      return { 0.1, 0.707, 1.5, 2.5, 5.0 };
        case CurveKind::Tail:   return { 0.0, 0.25, 0.5, 0.75, 1.0 };
    }
    return {};
}

juce::String BreakpointEditor::formatGridLabel (double value, CurveKind kind) const
{
    if (kind == CurveKind::Cutoff)
        return value >= 1000.0 ? juce::String (value / 1000.0, value >= 10000.0 ? 0 : 1) + "k"
                                : juce::String ((int) value);
    if (kind == CurveKind::Q)
        return juce::String (value, value < 1.0 ? 2 : 1); // e.g. "0.71" vs "20.0"
    return juce::String (value, 2); // tail gain
}

void BreakpointEditor::drawGrid (juce::Graphics& g)
{
    const auto plot = getPlotArea();
    g.setFont (13.0f);
    for (double v : gridValuesFor (activeCurve))
    {
        const float y = valueToY (v, activeCurve, plot);

        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());

        g.setColour (juce::Colours::grey);
        g.drawText (formatGridLabel (v, activeCurve),
                    juce::Rectangle<float> (0.0f, y - 8.0f, plot.getX() - 4.0f, 16.0f),
                    juce::Justification::centredRight);
    }
}

float BreakpointEditor::dnToX (double dn, juce::Rectangle<float> plot) const
{
    return plot.getX() + (float) dn * plot.getWidth();
}

double BreakpointEditor::xToDn (float x, juce::Rectangle<float> plot) const
{
    return juce::jlimit (0.0, 1.0, (double) (x - plot.getX()) / (double) plot.getWidth());
}

float BreakpointEditor::valueToY (double value, CurveKind kind, juce::Rectangle<float> plot) const
{
    const auto& style = styleFor (kind);
    double t;
    if (style.logScale)
    {
        const double v = juce::jlimit (style.minValue, style.maxValue, value);
        t = (std::log (v) - std::log (style.minValue)) / (std::log (style.maxValue) - std::log (style.minValue));
    }
    else
    {
        t = (value - style.minValue) / (style.maxValue - style.minValue);
    }
    t = juce::jlimit (0.0, 1.0, t);
    return plot.getBottom() - (float) t * plot.getHeight();
}

double BreakpointEditor::yToValue (float y, CurveKind kind, juce::Rectangle<float> plot) const
{
    const auto& style = styleFor (kind);
    double t = (double) (plot.getBottom() - y) / (double) plot.getHeight();
    t = juce::jlimit (0.0, 1.0, t);
    if (style.logScale)
        return std::exp (std::log (style.minValue) + t * (std::log (style.maxValue) - std::log (style.minValue)));
    return style.minValue + t * (style.maxValue - style.minValue);
}

void BreakpointEditor::drawCurve (juce::Graphics& g, CurveKind kind, float opacity, bool drawHandles)
{
    const auto& curve = curveFor (kind);
    const auto plot = getPlotArea();
    const auto& style = styleFor (kind);

    juce::Path path;
    const int steps = juce::jmax (2, (int) plot.getWidth());
    for (int i = 0; i <= steps; ++i)
    {
        const double dn = (double) i / (double) steps;
        const float x = dnToX (dn, plot);
        const float y = valueToY (curve.evaluate (dn), kind, plot);
        if (i == 0)
            path.startNewSubPath (x, y);
        else
            path.lineTo (x, y);
    }

    g.setColour (style.colour.withAlpha (opacity));
    g.strokePath (path, juce::PathStrokeType (drawHandles ? 2.0f : 1.5f));

    if (drawHandles)
    {
        for (size_t i = 0; i < curve.points.size(); ++i)
        {
            const auto& pt = curve.points[i];
            const float x = dnToX (pt.x, plot);
            const float y = valueToY (pt.y, kind, plot);
            const bool isEndpoint = (i == 0 || i == curve.points.size() - 1);

            g.setColour (isEndpoint ? juce::Colours::white : style.colour);
            g.fillEllipse (x - 4.0f, y - 4.0f, 8.0f, 8.0f);
            g.setColour (juce::Colours::black);
            g.drawEllipse (x - 4.0f, y - 4.0f, 8.0f, 8.0f, 1.0f);
        }
    }
}

size_t BreakpointEditor::segmentIndexAt (const BreakpointCurve& curve, double dn) const
{
    size_t segIdx = 0;
    while (segIdx + 1 < curve.points.size() - 1 && dn > curve.points[segIdx + 1].x)
        ++segIdx;
    return juce::jmin (segIdx, curve.points.size() - 2);
}

void BreakpointEditor::drawSegmentTypeIcon (juce::Graphics& g)
{
    const auto plot = getPlotArea();
    if (! plot.contains (lastMousePos))
        return;

    const auto& curve = curveFor (activeCurve);
    const double dn = xToDn (lastMousePos.x, plot);
    const auto segIdx = segmentIndexAt (curve, dn);
    const bool isExp = curve.points[segIdx].segmentType == CurveInterpolation::Exponential;

    juce::Rectangle<float> iconBounds (lastMousePos.x + 10.0f, lastMousePos.y - 22.0f, 30.0f, 18.0f);
    g.setColour (juce::Colours::black.withAlpha (0.8f));
    g.fillRoundedRectangle (iconBounds, 3.0f);
    g.setColour (juce::Colours::white);
    g.drawRoundedRectangle (iconBounds, 3.0f, 1.0f);

    juce::Path icon;
    if (isExp)
    {
        icon.startNewSubPath (iconBounds.getX() + 4.0f, iconBounds.getBottom() - 4.0f);
        icon.quadraticTo (iconBounds.getX() + 4.0f, iconBounds.getY() + 3.0f,
                           iconBounds.getRight() - 4.0f, iconBounds.getY() + 3.0f);
    }
    else
    {
        icon.startNewSubPath (iconBounds.getX() + 4.0f, iconBounds.getBottom() - 4.0f);
        icon.lineTo (iconBounds.getRight() - 4.0f, iconBounds.getY() + 3.0f);
    }
    g.strokePath (icon, juce::PathStrokeType (1.5f));
}

void BreakpointEditor::drawCurvatureIcon (juce::Graphics& g)
{
    const auto plot = getPlotArea();
    if (! plot.contains (lastMousePos))
        return;

    const auto& curve = curveFor (activeCurve);
    const double dn = xToDn (lastMousePos.x, plot);
    const auto segIdx = segmentIndexAt (curve, dn);
    const auto& seg = curve.points[segIdx];

    juce::Rectangle<float> iconBounds (lastMousePos.x + 10.0f, lastMousePos.y - 22.0f, 30.0f, 18.0f);
    g.setColour (juce::Colours::black.withAlpha (0.8f));
    g.fillRoundedRectangle (iconBounds, 3.0f);
    g.setColour (juce::Colours::white);
    g.drawRoundedRectangle (iconBounds, 3.0f, 1.0f);

    juce::Path icon;
    const float x0 = iconBounds.getX() + 4.0f, x1 = iconBounds.getRight() - 4.0f;
    const float yBottom = iconBounds.getBottom() - 4.0f, yTop = iconBounds.getY() + 3.0f;

    if (seg.segmentType != CurveInterpolation::Exponential)
    {
        // Linear segments ignore Ctrl-drag -- show a plain straight line so
        // it's clear there's nothing to bend until Option toggles it first.
        icon.startNewSubPath (x0, yBottom);
        icon.lineTo (x1, yTop);
    }
    else
    {
        // Bow the preview line using a quadratic control point offset by
        // curvature's sign/magnitude, so the icon itself previews the
        // actual current bend direction and roughly how strong it is.
        const float bend = (float) juce::jlimit (-1.0, 1.0, seg.curvature / maxCurvature);
        const float midX = (x0 + x1) * 0.5f, midY = (yTop + yBottom) * 0.5f;
        const float controlX = midX + bend * (iconBounds.getWidth() * 0.35f);
        icon.startNewSubPath (x0, yBottom);
        icon.quadraticTo (controlX, midY, x1, yTop);
    }
    g.strokePath (icon, juce::PathStrokeType (1.5f));
}

void BreakpointEditor::drawDragValuePopup (juce::Graphics& g)
{
    if (draggedPointIndex < 0)
        return;

    auto& curve = curveFor (activeCurve);
    if (draggedPointIndex >= (int) curve.points.size())
        return;

    const auto& pt = curve.points[(size_t) draggedPointIndex];
    const auto plot = getPlotArea();
    const float px = dnToX (pt.x, plot);
    const float py = valueToY (pt.y, activeCurve, plot);

    // Exact dn -> seconds-since-written conversion, same as the ruler.
    const auto& params = sharedState.params;
    const double sampleRate = processor.getSampleRate();
    const double capacity = (double) engine.getDelayBuffer1 (0).size();
    const double del1Len = std::floor ((params.bufferLenMs / (engine.getDel1MaxSeconds() * 1000.0)) * capacity);
    const double readSpan = juce::jmax (1.0, params.readScatter * del1Len);
    const double seconds = sampleRate > 0.0 ? pt.x * readSpan / sampleRate : 0.0;

    juce::String valueText = formatGridLabel (pt.y, activeCurve);
    if (activeCurve == CurveKind::Cutoff)
        valueText += " Hz";

    constexpr float popupW = 76.0f, popupH = 34.0f;
    float popupX = juce::jlimit (0.0f, (float) getWidth() - popupW, px + 10.0f);
    float popupY = juce::jlimit (0.0f, (float) getHeight() - popupH, py - popupH - 6.0f);
    juce::Rectangle<float> popupBounds (popupX, popupY, popupW, popupH);

    g.setColour (juce::Colours::black.withAlpha (0.85f));
    g.fillRoundedRectangle (popupBounds, 4.0f);
    g.setColour (juce::Colours::white);
    g.drawRoundedRectangle (popupBounds, 4.0f, 1.0f);

    g.setFont (14.0f);
    g.drawText (valueText, popupBounds.removeFromTop (20.0f).reduced (2.0f, 0.0f),
                juce::Justification::centred);
    g.setFont (11.0f);
    g.setColour (juce::Colours::grey);
    g.drawText (juce::String (seconds, 2) + "s", popupBounds.reduced (2.0f, 0.0f),
                juce::Justification::centred);
}

int BreakpointEditor::findNearbyPoint (const BreakpointCurve& curve, juce::Point<float> pos,
                                        juce::Rectangle<float> plot, CurveKind kind) const
{
    for (int i = 0; i < (int) curve.points.size(); ++i)
    {
        const auto& pt = curve.points[(size_t) i];
        const juce::Point<float> pixel { dnToX (pt.x, plot), valueToY (pt.y, kind, plot) };
        if (pixel.getDistanceFrom (pos) <= hitRadius)
            return i;
    }
    return -1;
}

void BreakpointEditor::paint (juce::Graphics& g)
{
    // No background fill -- this is a transparent overlay sitting on top of
    // CircularBufferVisualizer (see the class comment in the header). That
    // component already paints the black background and the seconds ruler
    // at this exact position, so we don't duplicate either here.
    drawGrid (g); // light gridlines + value labels for the active curve, behind the curves themselves

    for (auto kind : { CurveKind::Cutoff, CurveKind::Q, CurveKind::Tail })
        if (kind != activeCurve)
            drawCurve (g, kind, 0.25f, false);
    drawCurve (g, activeCurve, 1.0f, true);

    if (altHeld)
        drawSegmentTypeIcon (g);
    if (ctrlHeld)
        drawCurvatureIcon (g);

    drawDragValuePopup (g); // no-op unless a point is actively being dragged
}

void BreakpointEditor::mouseDown (const juce::MouseEvent& e)
{
    auto& curve = curveFor (activeCurve);
    const auto plot = getPlotArea();
    const int idx = findNearbyPoint (curve, e.position, plot, activeCurve);
    const bool isInteriorPoint = idx > 0 && idx < (int) curve.points.size() - 1;

    if (e.mods.isRightButtonDown() || (e.mods.isCtrlDown() && isInteriorPoint))
    {
        // Delete -- endpoints (index 0 and back()) are protected.
        if (isInteriorPoint)
        {
            curve.points.erase (curve.points.begin() + idx);
            sharedState.rebake();
            repaint();
        }
        return;
    }

    if (e.mods.isCtrlDown() && idx < 0 && plot.contains (e.position))
    {
        // Ctrl-drag a segment (not a point): start a curvature-bend drag,
        // but only if that segment is already Exponential -- Ctrl only
        // adjusts HOW a curved segment bends, not whether it's curved at
        // all (that's Option's job, below).
        const double dn = xToDn (e.position.x, plot);
        const auto segIdx = segmentIndexAt (curve, dn);
        if (curve.points[segIdx].segmentType == CurveInterpolation::Exponential)
        {
            curvatureDragging = true;
            curvatureDragSegment = segIdx;
            curvatureDragStartY = e.position.y;
            curvatureDragStartValue = curve.points[segIdx].curvature;
        }
        return;
    }

    if (e.mods.isAltDown() && idx < 0 && plot.contains (e.position))
    {
        // Option/Alt-click a segment (not a point): toggle its interpolation.
        const double dn = xToDn (e.position.x, plot);
        const auto segIdx = segmentIndexAt (curve, dn);
        curve.points[segIdx].segmentType =
            (curve.points[segIdx].segmentType == CurveInterpolation::Linear)
                ? CurveInterpolation::Exponential
                : CurveInterpolation::Linear;
        sharedState.rebake();
        repaint();
        return;
    }

    // idx may be -1 (no nearby point) -- mouseDrag() no-ops in that case.
    // Deliberately NOT gated on plot.contains(e.position): the two default
    // endpoints sit exactly on the plot's left/right edge, and JUCE
    // rectangles are right/bottom-exclusive, so a strict bounds check here
    // made the right endpoint (and any click a hair outside the plot rect)
    // unselectable. findNearbyPoint()'s radius test is the only gate we
    // need for starting a drag.
    draggedPointIndex = idx;
}

void BreakpointEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (curvatureDragging)
    {
        auto& curve = curveFor (activeCurve);
        if (curvatureDragSegment >= curve.points.size())
        {
            curvatureDragging = false;
            return;
        }

        // Dragging UP increases curvature (screen y decreases upward, so
        // startY - currentY is positive when dragging up).
        const float deltaPixels = curvatureDragStartY - e.position.y;
        const double newCurvature = curvatureDragStartValue + (double) deltaPixels * curvatureSensitivity;
        curve.points[curvatureDragSegment].curvature = juce::jlimit (-maxCurvature, maxCurvature, newCurvature);

        sharedState.rebake();
        repaint();
        return;
    }

    if (draggedPointIndex < 0)
        return;

    auto& curve = curveFor (activeCurve);
    if (draggedPointIndex >= (int) curve.points.size())
    {
        draggedPointIndex = -1;
        return;
    }

    const auto plot = getPlotArea();
    auto& pt = curve.points[(size_t) draggedPointIndex];
    const bool isFirst = (draggedPointIndex == 0);
    const bool isLast  = (draggedPointIndex == (int) curve.points.size() - 1);

    if (! isFirst && ! isLast)
    {
        // Clamp x between this point's neighbors so points can't cross and
        // silently reorder the curve.
        const double lo = curve.points[(size_t) draggedPointIndex - 1].x;
        const double hi = curve.points[(size_t) draggedPointIndex + 1].x;
        pt.x = juce::jlimit (lo + 1.0e-4, hi - 1.0e-4, xToDn (e.position.x, plot));
    }
    // First/last points: x stays pinned at 0/1, only y (below) changes --
    // your keymapping spec: "except for the two default points, those
    // should stay at the beginning and ending of the display."

    pt.y = yToValue (e.position.y, activeCurve, plot);

    sharedState.rebake();
    repaint();
}

void BreakpointEditor::mouseUp (const juce::MouseEvent&)
{
    draggedPointIndex = -1;
    curvatureDragging = false;
}

void BreakpointEditor::mouseMove (const juce::MouseEvent& e)
{
    const bool wasAlt = altHeld;
    const bool wasCtrl = ctrlHeld;
    altHeld = e.mods.isAltDown();
    ctrlHeld = e.mods.isCtrlDown();
    lastMousePos = e.position;
    if (altHeld || wasAlt || ctrlHeld || wasCtrl)
        repaint();
}

void BreakpointEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto plot = getPlotArea();
    if (! plot.contains (e.position))
        return;

    auto& curve = curveFor (activeCurve);
    if (findNearbyPoint (curve, e.position, plot, activeCurve) >= 0)
        return; // double-clicked an existing point -- not an "add" gesture

    const Breakpoint newPoint { xToDn (e.position.x, plot),
                                 yToValue (e.position.y, activeCurve, plot),
                                 CurveInterpolation::Linear };
    const auto insertPos = std::lower_bound (curve.points.begin(), curve.points.end(), newPoint,
                                              [] (const Breakpoint& a, const Breakpoint& b) { return a.x < b.x; });
    curve.points.insert (insertPos, newPoint);

    sharedState.rebake();
    repaint();
}
