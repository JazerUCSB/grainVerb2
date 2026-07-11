#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cmath>

namespace
{
    // Display-only text conversion for the dial readouts -- the underlying
    // parameter value keeps full float precision, only how it's shown (in
    // our own UI, and any host generic editor) changes. This has to live on
    // the PARAMETER (via AudioParameterFloatAttributes), not the Slider:
    // AudioProcessorValueTreeState::SliderAttachment wires its own
    // Slider::textFromValueFunction to call straight through to
    // RangedAudioParameter::getText(), so anything set directly on the
    // Slider gets silently routed around -- this is the one place that
    // actually sticks.
    juce::AudioParameterFloatAttributes msUnitAttrs()
    {
        return juce::AudioParameterFloatAttributes().withStringFromValueFunction (
            [] (float v, int) { return juce::String (v, 3) + " ms"; });
    }

    juce::AudioParameterFloatAttributes plain3DecimalAttrs()
    {
        return juce::AudioParameterFloatAttributes().withStringFromValueFunction (
            [] (float v, int) { return juce::String (v, 3); });
    }

    juce::AudioParameterFloatAttributes sampsUnitAttrs()
    {
        return juce::AudioParameterFloatAttributes().withStringFromValueFunction (
            [] (float v, int) { return juce::String ((juce::int64) std::round (v)) + " samps"; });
    }

    juce::AudioParameterFloatAttributes grainsUnitAttrs()
    {
        return juce::AudioParameterFloatAttributes().withStringFromValueFunction (
            [] (float v, int) { return juce::String ((juce::int64) std::round (v)) + " grains"; });
    }

    // Early reflections' fixed sizing -- no user-adjustable bufferLenMs/
    // readScatter (see ParamID::earlyFadeSamps and friends' comment), so
    // these are the only place these numbers live.
    constexpr double kEarlyDel1MaxSeconds = 1.0; // 1000ms
    constexpr double kEarlyDel2MaxSeconds = 0.6; // 600ms

    // MAXIMUM voices allocated per bank -- matches each engine's own
    // numGrainVoices dial range's upper bound (see createParameterLayout()).
    // The dial's live value only picks how many of these are ACTIVE each
    // sample (GrainReverbParams::numGrainVoices); the allocation itself
    // never changes after prepare(), so moving the dial can't reallocate on
    // the audio thread.
    constexpr int kLateMaxVoicesPerBank  = 100;
    constexpr int kEarlyMaxVoicesPerBank = 30;
}

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
    numGrainVoicesParam = apvts.getRawParameterValue (ParamID::numGrainVoices);

    earlyFadeSampsParam      = apvts.getRawParameterValue (ParamID::earlyFadeSamps);
    earlyMeanWindowMsParam   = apvts.getRawParameterValue (ParamID::earlyMeanWindowMs);
    earlyWindowRangeMsParam  = apvts.getRawParameterValue (ParamID::earlyWindowRangeMs);
    earlyFeedbackParam       = apvts.getRawParameterValue (ParamID::earlyFeedback);
    earlyJitterParam         = apvts.getRawParameterValue (ParamID::earlyJitter);
    earlyDispersionParam     = apvts.getRawParameterValue (ParamID::earlyDispersion);
    earlyNumGrainVoicesParam = apvts.getRawParameterValue (ParamID::earlyNumGrainVoices);
    balanceParam             = apvts.getRawParameterValue (ParamID::balance);

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
        Range { 3.0f, 4800.0f }, 200.0f, sampsUnitAttrs()));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::meanWindowMs, 1 }, "Grain Size (ms)",
        Range { 1.0f, 3000.0f }, 200.0f, msUnitAttrs()));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::windowRangeMs, 1 }, "Grain Size Variance (ms)",
        Range { 1.0f, 3000.0f }, 50.0f, msUnitAttrs()));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::bufferLenMs, 1 }, "Buffer Length (ms)",
        Range { 500.0f, 6000.0f }, 4000.0f, msUnitAttrs()));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::feedback, 1 }, "Feedback",
        Range { 0.0f, 0.999f }, 0.5f, plain3DecimalAttrs()));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::readScatter, 1 }, "Read Scatter",
        Range { 0.1f, 1.0f }, 1.0f, plain3DecimalAttrs()));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::jitter, 1 }, "Rate Jitter",
        Range { 0.0f, 0.1f }, 0.0f, plain3DecimalAttrs()));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::dispersion, 1 }, "Stereo Dispersion",
        Range { 0.0f, 1.0f }, 1.0f, plain3DecimalAttrs()));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::mix, 1 }, "Mix",
        Range { 0.0f, 1.0f }, 1.0f, plain3DecimalAttrs()));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::numGrainVoices, 1 }, "Number of Grains",
        Range { 25.0f, 100.0f }, 100.0f, grainsUnitAttrs()));

    // Early reflections -- own fadeSamps/grain size/variance/feedback/
    // jitter/dispersion, but no bufferLenMs/readScatter: its del1/del2 are
    // always fully active (fixed 1000ms/600ms, set in prepareToPlay()/
    // syncParams()), the same way late's OWN del2 has no scatter control.
    // Defaults for grain size/variance match the 1000ms buffer's spec
    // (100ms grains, 25ms variance); the 600ms buffer's smaller 50ms/15ms
    // grains aren't separately controllable -- one shared dial set governs
    // both of early's banks, exactly like late's meanWindowMs/windowRangeMs
    // already governs both del1 and del2 today.
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::earlyFadeSamps, 1 }, "Early Fade (samples)",
        Range { 3.0f, 2000.0f }, 100.0f, sampsUnitAttrs()));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::earlyMeanWindowMs, 1 }, "Early Grain Size (ms)",
        Range { 1.0f, 500.0f }, 100.0f, msUnitAttrs()));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::earlyWindowRangeMs, 1 }, "Early Grain Size Variance (ms)",
        Range { 1.0f, 200.0f }, 25.0f, msUnitAttrs()));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::earlyFeedback, 1 }, "Early Feedback",
        Range { 0.0f, 0.999f }, 0.3f, plain3DecimalAttrs()));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::earlyJitter, 1 }, "Early Rate Jitter",
        Range { 0.0f, 0.1f }, 0.0f, plain3DecimalAttrs()));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::earlyDispersion, 1 }, "Early Stereo Dispersion",
        Range { 0.0f, 1.0f }, 1.0f, plain3DecimalAttrs()));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::earlyNumGrainVoices, 1 }, "Early Number of Grains",
        Range { 5.0f, 30.0f }, 16.0f, grainsUnitAttrs()));
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::balance, 1 }, "Early/Late Balance",
        Range { 0.0f, 1.0f }, 0.5f, plain3DecimalAttrs()));

    return { params.begin(), params.end() };
}

void GrainReverb2AudioProcessor::syncParams()
{
    auto& p = lateShared.params;
    p.fadeSamps     = (double) fadeSampsParam->load();
    p.meanWindowMs  = (double) meanWindowMsParam->load();
    p.windowRangeMs = (double) windowRangeMsParam->load();
    p.bufferLenMs   = (double) bufferLenMsParam->load();
    p.fb            = (double) feedbackParam->load();
    p.readScatter   = (double) readScatterParam->load();
    p.jitter        = (double) jitterParam->load();
    p.dispersion    = (double) dispersionParam->load();
    p.numGrainVoices = (double) numGrainVoicesParam->load();

    auto& ep = earlyShared.params;
    ep.fadeSamps     = (double) earlyFadeSampsParam->load();
    ep.meanWindowMs  = (double) earlyMeanWindowMsParam->load();
    ep.windowRangeMs = (double) earlyWindowRangeMsParam->load();
    ep.fb            = (double) earlyFeedbackParam->load();
    ep.jitter        = (double) earlyJitterParam->load();
    ep.dispersion    = (double) earlyDispersionParam->load();
    ep.numGrainVoices = (double) earlyNumGrainVoicesParam->load();
    // No dial for these -- early's del1/del2 are always fully active (see
    // the ParamID::earlyFadeSamps comment), matching how del1Len/readSpan
    // are computed: bufferLenMs at its own max (del1MaxSeconds*1000) and
    // readScatter at 1.0 mean del1Len == del1L.size() and readSpan ==
    // del1Len, i.e. no scatter-limited subset, ever.
    ep.bufferLenMs   = kEarlyDel1MaxSeconds * 1000.0;
    ep.readScatter   = 1.0;
}

GrainReverb2AudioProcessor::~GrainReverb2AudioProcessor() {}

void GrainReverb2AudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    syncParams(); // pick up current APVTS values (e.g. a loaded preset) before seeding grains

    lateShared.prepare (sampleRate);
    lateEngine.prepare (sampleRate, lateShared, kLateMaxVoicesPerBank, kLateMaxVoicesPerBank); // 6s/1s buffers (defaults)

    earlyShared.prepare (sampleRate);
    earlyEngine.prepare (sampleRate, earlyShared, kEarlyMaxVoicesPerBank, kEarlyMaxVoicesPerBank,
                          kEarlyDel1MaxSeconds, kEarlyDel2MaxSeconds);
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
    const double balance = (double) balanceParam->load();

    for (int i = 0; i < numSamples; ++i)
    {
        const double dryL = (double) left[i];
        const double dryR = (double) right[i];

        // Parallel, not series: both engines process the SAME dry input
        // independently, each with its own del1/del2 feedback loop -- two
        // self-contained reverb tanks running side by side, crossfaded by
        // balance rather than one feeding the other.
        double lateWetL = 0.0, lateWetR = 0.0;
        lateEngine.processSample (dryL, dryR, lateShared, lateWetL, lateWetR);

        double earlyWetL = 0.0, earlyWetR = 0.0;
        earlyEngine.processSample (dryL, dryR, earlyShared, earlyWetL, earlyWetR);

        const double wetL = lateWetL * (1.0 - balance) + earlyWetL * balance;
        const double wetR = lateWetR * (1.0 - balance) + earlyWetR * balance;

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
