#include "ParamDialsPanel.h"
#include "PaneTheme.h"

namespace
{
    // Centre column width -- matches PluginEditor's own kDividerWidth so
    // the divider strip above lines up with this column below, AND matches
    // one early/late column's own width (see resized()), so Predelay/Mix
    // render at the same size as every other dial. Scaled to 75% of an
    // earlier, bigger pass at this (140 -> 105) alongside the same 75%
    // shrink applied to PluginEditor's dialsArea height -- both dimensions
    // shrink together so the whole dial (box + knob) scales down uniformly.
    constexpr int kCenterColumnWidth = 105;
}

ParamDialsPanel::ParamDialsPanel (GrainReverb2AudioProcessor& processorToUse)
    : processor (processorToUse)
{
    // Early (left, 9 dials, fully populated 3x3 -- Early Gain moved in
    // from the old centre column, completing the grid): value-readout
    // formatting (units, decimal places) lives on the parameter itself
    // (see PluginProcessor.cpp's createParameterLayout()), not here.
    addDial (earlyDials, ParamID::earlyBufferLenMs,      "Read Range",    kEarlyBorderColour);
    addDial (earlyDials, ParamID::earlyFeedback,         "Feedback",        kEarlyBorderColour);
    addDial (earlyDials, ParamID::earlyNumGrainVoices,   "Num Grains",      kEarlyBorderColour);
    addDial (earlyDials, ParamID::earlyMeanWindowMs,     "Grain Size",      kEarlyBorderColour);
    addDial (earlyDials, ParamID::earlyWindowRangeMs,    "Grain Variance",  kEarlyBorderColour);
    addDial (earlyDials, ParamID::earlyFadeSamps,        "Grain Fade",      kEarlyBorderColour);
    addDial (earlyDials, ParamID::earlyJitter,           "Jitter",          kEarlyBorderColour);
    addDial (earlyDials, ParamID::earlyDispersion,       "Dispersion",      kEarlyBorderColour);
    addDial (earlyDials, ParamID::earlyGainDb,           "Early Gain",      kEarlyBorderColour);

    // Late (right, 9 dials, fully populated 3x3 -- Late Gain moved in from
    // the old centre column, completing the grid the same way early's did).
    addDial (lateDials, ParamID::bufferLenMs,      "Read Range",   kLateBorderColour);
    addDial (lateDials, ParamID::feedback,         "Feedback",       kLateBorderColour);
    addDial (lateDials, ParamID::numGrainVoices,   "Num Grains",     kLateBorderColour);
    addDial (lateDials, ParamID::meanWindowMs,     "Grain Size",     kLateBorderColour);
    addDial (lateDials, ParamID::windowRangeMs,    "Grain Variance", kLateBorderColour);
    addDial (lateDials, ParamID::fadeSamps,        "Grain Fade",     kLateBorderColour);
    addDial (lateDials, ParamID::jitter,           "Jitter",         kLateBorderColour);
    addDial (lateDials, ParamID::dispersion,       "Dispersion",     kLateBorderColour);
    addDial (lateDials, ParamID::lateGainDb,       "Late Gain",      kLateBorderColour);

    // Centre column (1 x 3, top to bottom): Predelay, Balance, Mix -- all
    // genuinely shared (neutral border colour), affecting both engines'
    // combined output. Balance used to be a dedicated vertical fader in
    // the divider strip above; it's now just another knob here, with
    // "Early"/"Late" overriding its raw 0/1 low/high labels -- matches
    // PluginProcessor::processBlock()'s wetL/wetR crossfade exactly
    // (0 = all early, 1 = all late).
    addDial (centerDials, ParamID::predelayMs, "Predelay", kSharedBorderColour);
    addDial (centerDials, ParamID::balance,    "Balance",  kSharedBorderColour, "Early", "Late");
    addDial (centerDials, ParamID::mix,        "Mix",      kSharedBorderColour);
}

ParamDialsPanel::~ParamDialsPanel()
{
    // Every dial's slider was given &radialKnobLookAndFeel in addDial();
    // all must be cleared before radialKnobLookAndFeel is destroyed, or
    // the Slider would hold a dangling raw pointer past its lifetime.
    auto clearLookAndFeel = [] (juce::OwnedArray<Dial>& group)
    {
        for (auto* d : group)
            d->slider.setLookAndFeel (nullptr);
    };
    clearLookAndFeel (earlyDials);
    clearLookAndFeel (lateDials);
    clearLookAndFeel (centerDials);
}

ParamDialsPanel::Dial& ParamDialsPanel::addDial (juce::OwnedArray<Dial>& group, const juce::String& paramID,
                                                  const juce::String& labelText, juce::Colour borderColour,
                                                  const juce::String& lowLabelOverride, const juce::String& highLabelOverride)
{
    auto* d = group.add (new Dial());
    d->borderColour = borderColour;

    // Read by RadialKnobLookAndFeel::drawRotarySlider() to override the
    // low/high endpoint labels it would otherwise compute from the
    // slider's own min/max -- only set when the caller actually wants
    // custom text (e.g. Balance's "Late"/"Early"), so every other dial's
    // properties stay empty and the LookAndFeel falls back to numeric labels.
    if (lowLabelOverride.isNotEmpty())
        d->slider.getProperties().set ("lowLabel", lowLabelOverride);
    if (highLabelOverride.isNotEmpty())
        d->slider.getProperties().set ("highLabel", highLabelOverride);

    d->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    // Wide enough to fit a unit suffix like "200.000 ms" without truncating;
    // sized up alongside the dial's own ~5x growth so the readout doesn't
    // look tiny under the now much bigger knob.
    d->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 110, 26);

    // The default LookAndFeel_V4 text box renders as a flat white-ish
    // panel regardless of what sits behind it -- clashing with the pane
    // background showing through behind this dial. Make the box itself
    // transparent so the pane colour shows straight through, and tint the
    // readout text/caret to read clearly against that background instead.
    d->slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    d->slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    d->slider.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
    // rotarySliderFillColourId is what RadialKnobLookAndFeel uses for the
    // knob's radial pointer line (and a faint fill behind it) -- matched
    // to the LookAndFeel's own default thumb colour rather than guessing a
    // hex value, so it reads as the same accent colour throughout.
    d->slider.setColour (juce::Slider::rotarySliderFillColourId,
                          d->slider.findColour (juce::Slider::thumbColourId));
    d->slider.setLookAndFeel (&radialKnobLookAndFeel);
    addAndMakeVisible (d->slider);

    d->label.setText (labelText, juce::dontSendNotification);
    d->label.setJustificationType (juce::Justification::centred);
    d->label.setFont (juce::FontOptions (13.0f)); // 2pt down from an earlier pass
    d->label.setColour (juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible (d->label);

    // Binds the slider to the APVTS parameter bidirectionally: turning the
    // dial writes the parameter (host automation/undo/state-save all keep
    // working through APVTS same as if a DAW automated it), and automation
    // moves the dial. Range/default/display-text come from the
    // RangedAudioParameter itself -- no manual setRange() or
    // textFromValueFunction needed (the latter would just get silently
    // overridden by this attachment's own wiring to the parameter's
    // getText() anyway).
    d->attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.apvts, paramID, d->slider);

    return *d;
}

void ParamDialsPanel::layoutGroup (juce::OwnedArray<Dial>& group, juce::Rectangle<int> bounds, int columns)
{
    if (group.isEmpty())
        return;

    const int rows = (group.size() + columns - 1) / columns;
    const int cellW = bounds.getWidth() / columns;
    const int cellH = bounds.getHeight() / juce::jmax (1, rows);

    for (int i = 0; i < group.size(); ++i)
    {
        const int col = i % columns;
        const int row = i / columns;
        auto* d = group[i];

        d->cellBounds = juce::Rectangle<int> (bounds.getX() + col * cellW, bounds.getY() + row * cellH, cellW, cellH)
                            .reduced (4, 2);

        auto cell = d->cellBounds.reduced (6, 4);
        auto labelArea = cell.removeFromTop (20);
        d->label.setBounds (labelArea);
        d->slider.setBounds (cell);
    }
}

void ParamDialsPanel::resized()
{
    auto bounds = getLocalBounds();
    const int halfWidth = (bounds.getWidth() - kCenterColumnWidth) / 2;
    const auto earlyArea = bounds.removeFromLeft (halfWidth);
    const auto centerArea = bounds.removeFromLeft (kCenterColumnWidth);
    const auto lateArea = bounds; // remainder

    layoutGroup (earlyDials, earlyArea, groupColumns);
    layoutGroup (lateDials, lateArea, groupColumns);
    layoutGroup (centerDials, centerArea, 1); // single column -- Predelay, Balance, Mix top to bottom -- 3 rows, same as early/late
}

void ParamDialsPanel::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    const int halfWidth = (bounds.getWidth() - kCenterColumnWidth) / 2;
    auto earlyArea = bounds.removeFromLeft (halfWidth);
    auto centerArea = bounds.removeFromLeft (kCenterColumnWidth);
    auto lateArea = bounds; // remainder

    g.setColour (kEarlyPaneColour);
    g.fillRect (earlyArea);
    g.setColour (kSharedPaneColour);
    g.fillRect (centerArea);
    g.setColour (kLatePaneColour);
    g.fillRect (lateArea);

    // Each dial sits directly on its pane's own plain background (filled
    // above) -- no extra fill layer on top anymore. Instead, a rounded
    // OUTLINE groups each dial's title+knob+readout together as one unit.
    // Colour is stored PER DIAL (not per group) since centerDials mixes
    // early-/late-/shared-tinted borders within one array.
    auto drawBorders = [&] (juce::OwnedArray<Dial>& group)
    {
        for (auto* d : group)
        {
            g.setColour (d->borderColour);
            g.drawRoundedRectangle (d->cellBounds.toFloat(), 6.0f, 2.0f);
        }
    };

    drawBorders (earlyDials);
    drawBorders (lateDials);
    drawBorders (centerDials);
}
