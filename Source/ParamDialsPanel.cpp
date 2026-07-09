#include "ParamDialsPanel.h"

ParamDialsPanel::ParamDialsPanel (GrainReverb2AudioProcessor& processorToUse)
    : processor (processorToUse)
{
    addDial (ParamID::fadeSamps, "Fade");
    addDial (ParamID::meanWindowMs, "Grain Size");
    addDial (ParamID::windowRangeMs, "Grain Var");
    addDial (ParamID::bufferLenMs, "Buf Len");
    addDial (ParamID::feedback, "Feedback");
    addDial (ParamID::readScatter, "Scatter");
    addDial (ParamID::jitter, "Jitter");
    addDial (ParamID::dispersion, "Dispersion");
    addDial (ParamID::mix, "Mix");
}

ParamDialsPanel::Dial& ParamDialsPanel::addDial (const juce::String& paramID, const juce::String& labelText)
{
    auto* d = dials.add (new Dial());

    d->slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    d->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 56, 16);
    addAndMakeVisible (d->slider);

    d->label.setText (labelText, juce::dontSendNotification);
    d->label.setJustificationType (juce::Justification::centred);
    d->label.setFont (juce::FontOptions (12.0f));
    addAndMakeVisible (d->label);

    // Binds the slider to the APVTS parameter bidirectionally: turning the
    // dial writes the parameter (host automation/undo/state-save all keep
    // working through APVTS same as if a DAW automated it), and automation
    // moves the dial. Range/default come from the RangedAudioParameter
    // itself -- no manual setRange() needed.
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
                        .reduced (6, 4);
        auto labelArea = cell.removeFromTop (16);
        dials[i]->label.setBounds (labelArea);
        dials[i]->slider.setBounds (cell);
    }
}
