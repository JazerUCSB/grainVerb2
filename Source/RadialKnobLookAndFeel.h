#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Replaces LookAndFeel_V4's default filled-arc rotary look, used across
// every ParamDialsPanel dial (early/late grids and the shared centre
// column): a plain circle with a single radial pointer line marking the
// turned angle, unlabeled tick marks tracing the rotary sweep, and the
// range's low/high endpoints printed at the two ends of that sweep. The
// knob circle itself is drawn smaller than the sweep's full radius to
// leave room for the ticks/endpoint labels ringing it. The precise current
// value is left entirely to the Slider's own attached text box below
// (set up in ParamDialsPanel::addDial) -- this only draws the knob graphic
// above it.
class RadialKnobLookAndFeel : public juce::LookAndFeel_V4
{
public:
    // The Slider's own text box (the live numeric readout below the knob)
    // is a Label created by the base class with its own auto-sized font --
    // shave 2pt off whatever that default comes out to, rather than
    // hardcoding an absolute size that would drift out of sync with it.
    juce::Label *createSliderTextBox(juce::Slider &slider) override
    {
        auto *l = LookAndFeel_V4::createSliderTextBox(slider);
        l->setFont(l->getFont().withHeight(juce::jmax(1.0f, l->getFont().getHeight() - 2.0f)));
        return l;
    }

    void drawRotarySlider(juce::Graphics &g, int x, int y, int width, int height,
                          float sliderPos, float rotaryStartAngle, float rotaryEndAngle,
                          juce::Slider &slider) override
    {
        auto bounds = juce::Rectangle<int>(x, y, width, height).toFloat();
        auto centre = bounds.getCentre();

        // Proportional (not fixed-pixel) margins -- at the panel's old,
        // much smaller dial size, fixed-pixel gaps here ate almost the
        // whole available radius, leaving a barely-visible knob. Fractions
        // of outerRadius instead keep the knob a consistent, generously
        // sized majority of the sweep at ANY cell size.
        const float outerRadius = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.5f;
        const float labelRadius = outerRadius * 0.99f;
        const float knobRadius = outerRadius * 0.68f;

        // Hatch band sits just outside the knob body, with an absolute
        // pixel floor on both the gap and the tick length -- at the
        // panel's current tiny cell size, outerRadius itself is small
        // enough that a pure fraction of it (as used everywhere else here)
        // rounds down to an imperceptible sliver. The floor keeps the
        // ticks an actually-visible length regardless of how small the
        // dial gets, clamped back under outerRadius so they still can't
        // spill past the slider's own bounds.
        const float hatchInnerRadius = knobRadius + juce::jmax(2.0f, outerRadius * 0.08f);
        const float hatchOuterRadius = juce::jmin(outerRadius * 0.98f,
                                                  hatchInnerRadius + juce::jmax(3.0f, outerRadius * 0.18f));

        auto pointOnCircle = [&](float angle, float radius)
        {
            return centre.getPointOnCircumference(radius, angle);
        };

        // Stroke widths and font/label size all scale off outerRadius too,
        // so they stay in proportion rather than looking hairline-thin (or
        // comically thick) once the dial itself is much bigger.
        const float hatchThickness = juce::jmax(1.5f, outerRadius * 0.06f);
        const float knobOutlineThickness = juce::jmax(1.0f, outerRadius * 0.045f);
        const float pointerThickness = juce::jmax(1.5f, outerRadius * 0.06f);
        const float labelFontSize = juce::jmax(9.0f, outerRadius * 0.22f) + 1.0f; // back down 1pt from the previous pass
        const float labelBoxSize = juce::jmax(68.0f, outerRadius * 2.0f);         // wider still, cumulative over three passes
        // Small extra downward nudge on top of labelRadius's own outward
        // push -- "a tiny bit lower" specifically, separate from the
        // radial (outward) positioning below. Cumulative over two passes.
        const float labelYOffset = juce::jmax(4.0f, outerRadius * 0.15f);

        // Unlabeled hatch marks, stretching the FULL sweep from the low end
        // (rotaryStartAngle) to the high end (rotaryEndAngle) -- same span
        // as the low/high endpoint labels below, not just the top of the
        // knob. Same colour as those endpoint labels (white, 0.7 alpha --
        // not the outline colour) so they read as part of the same visual
        // element and stay visible at a glance instead of only when zoomed
        // in. numHatches=9 divides the sweep into 8 equal eighths, so
        // t=0.25/0.5/0.75 land exactly on hatches 2/4/6 -- those three get
        // a longer, slightly thicker mark as quarter references.
        constexpr int numHatches = 9;
        const float hatchQuarterExtra = juce::jmax(2.0f, outerRadius * 0.10f);
        g.setColour(juce::Colours::white.withAlpha(0.7f));
        for (int i = 0; i < numHatches; ++i)
        {
            const bool isQuarterMark = (i == 2 || i == 4 || i == 6); // t = 0.25 / 0.5 / 0.75
            const float t = (float)i / (float)(numHatches - 1);
            const float angle = rotaryStartAngle + t * (rotaryEndAngle - rotaryStartAngle);
            const float outerR = hatchOuterRadius + (isQuarterMark ? hatchQuarterExtra : 0.0f);
            g.drawLine(juce::Line<float>(pointOnCircle(angle, hatchInnerRadius),
                                         pointOnCircle(angle, outerR)),
                       isQuarterMark ? hatchThickness * 1.4f : hatchThickness);
        }

        // Knob body -- a simple circle, no fill-arc.
        auto knobBounds = juce::Rectangle<float>(knobRadius * 2.0f, knobRadius * 2.0f).withCentre(centre);
        g.setColour(slider.findColour(juce::Slider::rotarySliderFillColourId).withAlpha(0.25f));
        g.fillEllipse(knobBounds);
        g.setColour(slider.findColour(juce::Slider::rotarySliderOutlineColourId));
        g.drawEllipse(knobBounds, knobOutlineThickness);

        // Single radial pointer line marking the current turned angle.
        const float pointerAngle = rotaryStartAngle + sliderPos * (rotaryEndAngle - rotaryStartAngle);
        g.setColour(slider.findColour(juce::Slider::rotarySliderFillColourId));
        g.drawLine(juce::Line<float>(centre, pointOnCircle(pointerAngle, knobRadius * 0.9f)), pointerThickness);

        // Low/high endpoint labels at the sweep's two ends (start = low,
        // end = high) -- fixed range bounds, not the live value, which
        // the Slider's own text box below already shows precisely. A dial
        // can override either side with custom text (e.g. Balance uses
        // "Late"/"Early" instead of its raw 0/1 range) via Component
        // properties set in ParamDialsPanel::addDial -- see lowLabel/
        // highLabel there.
        auto formatEndpoint = [](double v)
        {
            return juce::approximatelyEqual(v, std::round(v))
                       ? juce::String((int)std::round(v))
                       : juce::String(v, 1);
        };
        const auto &props = slider.getProperties();
        const auto lowText = props.contains("lowLabel") ? props["lowLabel"].toString() : formatEndpoint(slider.getMinimum());
        const auto highText = props.contains("highLabel") ? props["highLabel"].toString() : formatEndpoint(slider.getMaximum());

        g.setColour(juce::Colours::white.withAlpha(0.7f));
        g.setFont(juce::FontOptions(labelFontSize));
        g.drawText(lowText,
                   juce::Rectangle<float>(labelBoxSize, labelFontSize + 4.0f)
                       .withCentre(pointOnCircle(rotaryStartAngle, labelRadius).translated(0.0f, labelYOffset)),
                   juce::Justification::centred);
        g.drawText(highText,
                   juce::Rectangle<float>(labelBoxSize, labelFontSize + 4.0f)
                       .withCentre(pointOnCircle(rotaryEndAngle, labelRadius).translated(0.0f, labelYOffset)),
                   juce::Justification::centred);
    }
};
