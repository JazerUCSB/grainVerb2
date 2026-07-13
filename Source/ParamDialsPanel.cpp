#include "ParamDialsPanel.h"
#include "PaneTheme.h"

ParamDialsPanel::ParamDialsPanel (GrainReverb2AudioProcessor& processorToUse)
    : processor (processorToUse)
{
    // Early (left, 8 dials -- row 3 only has 2, the 3rd cell is left
    // empty): value-readout formatting (units, decimal places) lives on
    // the parameter itself (see PluginProcessor.cpp's
    // createParameterLayout()), not here.
    addDial (earlyDials, ParamID::earlyBufferLenMs,      "Buffer Length",    true);
    addDial (earlyDials, ParamID::earlyFeedback,         "Feedback",         true);
    addDial (earlyDials, ParamID::earlyNumGrainVoices,   "Num Grains",       true);
    addDial (earlyDials, ParamID::earlyMeanWindowMs,     "Grain Size",       true);
    addDial (earlyDials, ParamID::earlyWindowRangeMs,    "Grain Variance",   true);
    addDial (earlyDials, ParamID::earlyFadeSamps,        "Grain Fade",       true);
    addDial (earlyDials, ParamID::earlyJitter,           "Jitter",           true);
    addDial (earlyDials, ParamID::earlyDispersion,       "Dispersion",       true);

    // Late (right, 9 dials -- fully populated 3x3 grid).
    addDial (lateDials, ParamID::bufferLenMs,      "Buffer Length",   false);
    addDial (lateDials, ParamID::feedback,         "Feedback",        false);
    addDial (lateDials, ParamID::numGrainVoices,   "Num Grains",      false);
    addDial (lateDials, ParamID::meanWindowMs,     "Grain Size",      false);
    addDial (lateDials, ParamID::windowRangeMs,    "Grain Variance",  false);
    addDial (lateDials, ParamID::fadeSamps,        "Grain Fade",      false);
    addDial (lateDials, ParamID::jitter,           "Jitter",          false);
    addDial (lateDials, ParamID::dispersion,       "Dispersion",      false);
    addDial (lateDials, ParamID::mix,              "Mix",             false);
}

ParamDialsPanel::Dial& ParamDialsPanel::addDial (juce::OwnedArray<Dial>& group, const juce::String& paramID,
                                                  const juce::String& labelText, bool isEarly)
{
    auto* d = group.add (new Dial());

    d->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    // Wide enough to fit a unit suffix like "200.000 ms" without truncating.
    d->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 18);

    // The default LookAndFeel_V4 text box renders as a flat white-ish
    // panel regardless of what sits behind it -- clashing with the
    // per-dial chip drawn in paint() below. Make the box itself
    // transparent so the chip shows straight through, and tint the
    // readout text/caret to read clearly against that chip colour instead.
    d->slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    d->slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    d->slider.setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
    d->slider.setColour (juce::Slider::rotarySliderFillColourId,
                          isEarly ? kEarlyLightColour : kLateLightColour);
    addAndMakeVisible (d->slider);

    d->label.setText (labelText, juce::dontSendNotification);
    d->label.setJustificationType (juce::Justification::centred);
    d->label.setFont (juce::FontOptions (12.0f));
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
        auto labelArea = cell.removeFromTop (16);
        d->label.setBounds (labelArea);
        d->slider.setBounds (cell);
    }
}

void ParamDialsPanel::resized()
{
    auto bounds = getLocalBounds();
    const auto earlyArea = bounds.removeFromLeft (bounds.getWidth() / 2);
    const auto lateArea = bounds;

    layoutGroup (earlyDials, earlyArea, groupColumns);
    layoutGroup (lateDials, lateArea, groupColumns);
}

void ParamDialsPanel::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    auto earlyArea = bounds.removeFromLeft (bounds.getWidth() / 2);
    auto lateArea = bounds;

    g.setColour (kEarlyPaneColour);
    g.fillRect (earlyArea);
    g.setColour (kLatePaneColour);
    g.fillRect (lateArea);

    // A lighter-tinted rounded chip behind each dial's title+readout, so
    // every control reads as its own labeled unit against the darker pane
    // fill above, and so the slider's own (now-transparent) text box has
    // something other than the raw pane colour behind it.
    auto drawChips = [&] (juce::OwnedArray<Dial>& group, juce::Colour chipColour)
    {
        g.setColour (chipColour);
        for (auto* d : group)
            g.fillRoundedRectangle (d->cellBounds.toFloat(), 6.0f);
    };

    drawChips (earlyDials, kEarlyLightColour);
    drawChips (lateDials, kLateLightColour);
}
