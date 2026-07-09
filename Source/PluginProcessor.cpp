#include "PluginProcessor.h"
#include "PluginEditor.h"

GrainReverb2AudioProcessor::GrainReverb2AudioProcessor()
    : AudioProcessor (BusesProperties()
                           .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    // Cache raw parameter pointers once here rather than doing a
    // string-keyed apvts.getRawParameterValue() lookup every block.
    fadeSampsParam     = apvts.getRawParameterValue (ParamID::fadeSamps);
    meanWindowMsParam  = apvts.getRawParameterValue (ParamID::meanWindowMs);
    windowRangeMsParam = apvts.getRawParameterValue (ParamID::windowRangeMs);
    bufferLenMsParam   = apvts.getRawParameterValue (ParamID::bufferLenMs);
    feedbackParam      = apvts.getRawParameterValue (ParamID::feedback);
    readScatterParam   = apvts.getRawParameterValue (ParamID::readScatter);
    jitterParam        = apvts.getRawParameterValue (ParamID::jitter);
    dispersionParam    = apvts.getRawParameterValue (ParamID::dispersion);
    mixParam           = apvts.getRawParameterValue (ParamID::mix);

    // No allocation here otherwise -- sample rate isn't known yet. Real
    // DSP setup happens in prepareToPlay().
}

juce::AudioProcessorValueTreeState::ParameterLayout GrainReverb2AudioProcessor::createParameterLayout()
{
    using Param = juce::AudioParameterFloat;
    using Range = juce::NormalisableRange<float>;

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Ranges/defaults match the PDF's Section 4.3 table (gen~'s Param
    // declarations), except dispersion and mix, which don't exist in the
    // gen~ patch -- dispersion is our per-grain stereo pan-spread control
    // (see GrainVoiceEngine), mix is a plain wet/dry blend applied in
    // processBlock.
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::fadeSamps, 1 }, "Fade (samples)",
        Range { 3.0f, 4800.0f }, 200.0f));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::meanWindowMs, 1 }, "Grain Size (ms)",
        Range { 1.0f, 3000.0f }, 200.0f));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::windowRangeMs, 1 }, "Grain Size Variance (ms)",
        Range { 1.0f, 3000.0f }, 50.0f));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::bufferLenMs, 1 }, "Buffer Length (ms)",
        Range { 500.0f, 6000.0f }, 4000.0f));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::feedback, 1 }, "Feedback",
        Range { 0.0f, 0.999f }, 0.5f));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::readScatter, 1 }, "Read Scatter",
        Range { 0.1f, 1.0f }, 0.9f));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::jitter, 1 }, "Rate Jitter",
        Range { 0.0f, 0.75f }, 0.0f));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::dispersion, 1 }, "Stereo Dispersion",
        Range { 0.0f, 1.0f }, 1.0f));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::mix, 1 }, "Mix",
        Range { 0.0f, 1.0f }, 1.0f));

    return { params.begin(), params.end() };
}

void GrainReverb2AudioProcessor::syncParams()
{
    auto& p = shared.params;
    p.fadeSamps     = (double) fadeSampsParam->load();
    p.meanWindowMs  = (double) meanWindowMsParam->load();
    p.windowRangeMs = (double) windowRangeMsParam->load();
    p.bufferLenMs   = (double) bufferLenMsParam->load();
    p.fb            = (double) feedbackParam->load();
    p.readScatter   = (double) readScatterParam->load();
    p.jitter        = (double) jitterParam->load();
    p.dispersion    = (double) dispersionParam->load();
}

GrainReverb2AudioProcessor::~GrainReverb2AudioProcessor() {}

void GrainReverb2AudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    syncParams(); // pick up current APVTS values (e.g. a loaded preset) before seeding grains
    shared.prepare (sampleRate);
    engine.prepare (sampleRate, shared);
}

void GrainReverb2AudioProcessor::releaseResources() {}

bool GrainReverb2AudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainInputChannelSet()  == juce::AudioChannelSet::stereo();
}

void GrainReverb2AudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    syncParams();

    // isBusesLayoutSupported() locks us to stereo in and out, so channels
    // 0/1 are always valid here.
    auto* left  = buffer.getWritePointer (0);
    auto* right = buffer.getWritePointer (1);
    const int numSamples = buffer.getNumSamples();

    const double mix = (double) mixParam->load();

    for (int i = 0; i < numSamples; ++i)
    {
        const double dryL = (double) left[i];
        const double dryR = (double) right[i];

        double wetL = 0.0, wetR = 0.0;
        engine.processSample (dryL, dryR, shared, wetL, wetR);

        left[i]  = (float) (dryL * (1.0 - mix) + wetL * mix);
        right[i] = (float) (dryR * (1.0 - mix) + wetR * mix);
    }
}

juce::AudioProcessorEditor* GrainReverb2AudioProcessor::createEditor()
{
    return new GrainReverb2AudioProcessorEditor (*this);
}

void GrainReverb2AudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void GrainReverb2AudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

// This creates new instances of the plugin — required by every JUCE plugin format wrapper.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new GrainReverb2AudioProcessor();
}
