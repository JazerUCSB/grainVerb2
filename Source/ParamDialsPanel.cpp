#include "ParamDialsPanel.h"

ParamDialsPanel::ParamDialsPanel (GrainReverb2AudioProcessor& processorToUse)
    : processor (processorToUse)
{
    // Late reflections (10 dials), then early reflections (7 dials) --
    // value-readout formatting (units, decimal places) lives on the
    // parameter itself now (see PluginProcessor.cpp's
    // createParameterLayout()) -- not here.
    addDial (ParamID::bufferLenMs, "Buffer Length");
    addDial (ParamID::feedback, "Feedback");
    addDial (ParamID::readScatter, "Scatter");
    addDial (ParamID::meanWindowMs, "Grain Size");
    addDial (ParamID::windowRangeMs, "Grain Variance");
    addDial (ParamID::fadeSamps, "Grain Fade");
    addDial (ParamID::jitter, "Jitter");
    addDial (ParamID::dispersion, "Dispersion");
    addDial (ParamID::mix, "Mix");
    addDial (ParamID::numGrainVoices, "Num Grains");

    addDial (ParamID::earlyFeedback, "Early Feedback");
    addDial (ParamID::earlyMeanWindowMs, "Early Grain Size");
    addDial (ParamID::earlyWindowRangeMs, "Early Grain Var");
    addDial (ParamID::earlyFadeSamps, "Early Grain Fade");
    addDial (ParamID::earlyJitter, "Early Jitter");
    addDial (ParamID::earlyDispersion, "Early Dispersion");
    addDial (ParamID::earlyNumGrainVoices, "Early Num Grains");
}

ParamDialsPanel::Dial& ParamDialsPanel::addDial (const juce::String& paramID, const juce::String& labelText)
{
    auto* d = dials.add (new Dial());

    d->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    // Wide enough to fit a unit suffix like "200.000 ms" without truncating.
    d->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 80, 18);
    addAndMakeVisible (d->slider);

    d->label.setText (labelText, juce::dontSendNotification);
    d->label.setJustificationType (juce::Justification::centred);
    d->label.setFont (juce::FontOptions (12.0f));
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

void ParamDialsPanel::resized()
{
    auto bounds = getLocalBounds();
    if (dials.isEmpty())
        return;

    const int rows = (dials.size() + columns - 1) / columns;
    const int cellW = bounds.getWidth() / columns;
    const int cellH = bounds.getHeight() / juce::jmax (1, rows);

    for (int i = 0; i < dials.size(); ++i)
    {
        const int col = i % columns;
        const int row = i / columns;
        auto cell = juce::Rectangle<int> (bounds.getX() + col * cellW, bounds.getY() + row * cellH, cellW, cellH)
                        .reduced (4, 2);
        auto labelArea = cell.removeFromTop (16);
        dials[i]->label.setBounds (labelArea);
        dials[i]->slider.setBounds (cell);
    }
}
