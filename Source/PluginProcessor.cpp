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

    // Explicit "+" for positive values (0 itself gets none) -- these are
    // bipolar trims centred on unity, so "3.00 dB" alone reads ambiguously
    // next to "-3.00 dB" without it.
    juce::AudioParameterFloatAttributes dbUnitAttrs()
    {
        return juce::AudioParameterFloatAttributes().withStringFromValueFunction (
            [] (float v, int) { return (v > 0.0f ? juce::String ("+") : juce::String()) + juce::String (v, 2) + " dB"; });
    }

    // MAXIMUM capacity for each engine's del1 -- earlyBufferLenMs/
    // bufferLenMs dials pick the ACTIVE portion within this (see
    // GrainVoiceEngine::seedGrains()'s del1Len formula), never resized
    // after prepare(). Late still has a fixed-capacity del2 with no dial
    // of its own; early has no del2 at all any more (singleBufferDualFeedback
    // -- see GrainVoiceEngine::prepare()), so there's no equivalent early
    // constant here. Shrunk from 1.0s to match earlyBufferLenMs's dial
    // range dropping to 25-400ms -- no point allocating capacity the dial
    // can never reach.
    constexpr double kEarlyDel1MaxSeconds = 0.4; // 400ms

    // MAXIMUM voices allocated per bank -- matches each engine's own
    // numGrainVoices dial range's upper bound (see createParameterLayout()).
    // The dial's live value only picks how many of these are ACTIVE each
    // sample (GrainReverbParams::numGrainVoices); the allocation itself
    // never changes after prepare(), so moving the dial can't reallocate on
    // the audio thread.
    constexpr int kLateMaxVoicesPerBank  = 200;
    constexpr int kEarlyMaxVoicesPerBank = 100;
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
    jitterParam        = apvts.getRawParameterValue (ParamID::jitter);
    dispersionParam    = apvts.getRawParameterValue (ParamID::dispersion);
    mixParam           = apvts.getRawParameterValue (ParamID::mix);
    numGrainVoicesParam = apvts.getRawParameterValue (ParamID::numGrainVoices);

    earlyBufferLenMsParam    = apvts.getRawParameterValue (ParamID::earlyBufferLenMs);
    earlyFadeSampsParam      = apvts.getRawParameterValue (ParamID::earlyFadeSamps);
    earlyMeanWindowMsParam   = apvts.getRawParameterValue (ParamID::earlyMeanWindowMs);
    earlyWindowRangeMsParam  = apvts.getRawParameterValue (ParamID::earlyWindowRangeMs);
    earlyFeedbackParam       = apvts.getRawParameterValue (ParamID::earlyFeedback);
    earlyJitterParam         = apvts.getRawParameterValue (ParamID::earlyJitter);
    earlyDispersionParam     = apvts.getRawParameterValue (ParamID::earlyDispersion);
    earlyNumGrainVoicesParam = apvts.getRawParameterValue (ParamID::earlyNumGrainVoices);
    balanceParam             = apvts.getRawParameterValue (ParamID::balance);
    earlyGainDbParam         = apvts.getRawParameterValue (ParamID::earlyGainDb);
    lateGainDbParam          = apvts.getRawParameterValue (ParamID::lateGainDb);
    predelayMsParam          = apvts.getRawParameterValue (ParamID::predelayMs);

    // No allocation here otherwise -- sample rate isn't known yet. Real
    // DSP setup happens in prepareToPlay().
}

juce::AudioProcessorValueTreeState::ParameterLayout GrainReverb2AudioProcessor::createParameterLayout()
{
    using Param = juce::AudioParameterFloat;
    using Range = juce::NormalisableRange<float>;

    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    // Takes the prefix ONCE per engine instead of retyping "Late "/"Early "
    // into every display-name string -- guarantees consistent formatting
    // and lets matching early/late pairs be written side by side below.
    auto addParam = [&params] (const char* id, const juce::String& prefix, const juce::String& baseName,
                                Range range, float def, juce::AudioParameterFloatAttributes attrs)
    {
        params.push_back (std::make_unique<Param> (juce::ParameterID { id, 1 }, prefix + baseName, range, def, attrs));
    };

    // No readScatter for EITHER engine any more -- it's pinned to 1.0 in
    // syncParams(), so bufferLenMs alone controls how much of del1's fixed
    // capacity is active (see kEarlyDel1MaxSeconds's comment above). del2
    // still has no dial for either engine -- always fully active at its
    // own fixed capacity.
    // Renamed from "Buffer Length" to "Read Range" (display text only --
    // ParamID::bufferLenMs/earlyBufferLenMs themselves are unchanged, so
    // existing saved state/automation still resolves correctly): a grain
    // reading position is a fixed delay TAP somewhere within this many ms
    // behind the write head, not a container a grain has to "fit inside"
    // -- a grain (Grain Size) can be, and often should be, longer than
    // this range without anything breaking (see GrainVoiceEngine's
    // read-anchor invariant). "Buffer Length" implied the latter and was
    // genuinely confusing once Grain Size could exceed it.
    addParam (ParamID::bufferLenMs,      "Late ",  "Read Range (ms)",       Range { 200.0f, 6000.0f }, 3500.0f, msUnitAttrs());
    // Range dropped to 25-400ms (was 50-1000ms) -- see kEarlyDel1MaxSeconds
    // below, shrunk to match. Default stays 50ms + Grain Size default
    // 200ms below: grain size 4x the buffer range collapses each grain
    // into a static short-delay tap (see GrainVoiceEngine's read-anchor
    // invariant: at rate=1, read stays a FIXED distance behind the write
    // head for the grain's whole life), landing much closer to a classic
    // multi-tap early-reflections generator than a churning granular
    // cloud -- this was found by ear to be the best match against a
    // reference ER plugin.
    addParam (ParamID::earlyBufferLenMs, "Early ", "Read Range (ms)",       Range { 25.0f, 400.0f },  50.0f,   msUnitAttrs());
    addParam (ParamID::feedback,         "Late ",  "Feedback",                 Range { 0.0f, 0.999f },    0.5f,    plain3DecimalAttrs());
    // Early used to split this into two dials (Feedback 1/2 -- write-only
    // bank vs. listen bank, singleBufferDualFeedback's two loops into the
    // shared buffer, see GrainVoiceEngine::prepare()), but they were
    // audibly indistinguishable from one another -- one dial now drives
    // both (see syncParams(), which mirrors this into GrainReverbParams::
    // fb AND fb2). Lower default (0.1 vs. late's 0.5) since early's shared-
    // buffer topology builds up energy faster per unit of feedback.
    addParam (ParamID::earlyFeedback,    "Early ", "Feedback",                 Range { 0.0f, 0.999f },    0.1f,    plain3DecimalAttrs());
    addParam (ParamID::numGrainVoices,      "Late ",  "Number of Grains",      Range { 50.0f, 200.0f },   100.0f,  grainsUnitAttrs());
    addParam (ParamID::earlyNumGrainVoices, "Early ", "Number of Grains",      Range { 30.0f, 100.0f },   50.0f,   grainsUnitAttrs());
    addParam (ParamID::meanWindowMs,      "Late ",  "Grain Size (ms)",         Range { 50.0f, 2000.0f },  200.0f,  msUnitAttrs());
    addParam (ParamID::earlyMeanWindowMs, "Early ", "Grain Size (ms)",         Range { 50.0f, 500.0f },   200.0f,  msUnitAttrs());
    addParam (ParamID::windowRangeMs,      "Late ",  "Grain Size Variance (ms)", Range { 0.0f, 500.0f },  50.0f,   msUnitAttrs());
    addParam (ParamID::earlyWindowRangeMs, "Early ", "Grain Size Variance (ms)", Range { 0.0f, 500.0f },  25.0f,   msUnitAttrs());
    addParam (ParamID::fadeSamps,      "Late ",  "Fade (samples)",             Range { 5.0f, 1000.0f },   200.0f,  sampsUnitAttrs());
    addParam (ParamID::earlyFadeSamps, "Early ", "Fade (samples)",             Range { 5.0f, 1000.0f },   200.0f,  sampsUnitAttrs());
    addParam (ParamID::jitter,      "Late ",  "Rate Jitter", Range { 0.0f, 0.1f }, 0.0f, plain3DecimalAttrs());
    addParam (ParamID::earlyJitter, "Early ", "Rate Jitter", Range { 0.0f, 0.1f }, 0.0f, plain3DecimalAttrs());
    addParam (ParamID::dispersion,      "Late ",  "Stereo Dispersion", Range { 0.0f, 1.0f }, 1.0f, plain3DecimalAttrs());
    addParam (ParamID::earlyDispersion, "Early ", "Stereo Dispersion", Range { 0.0f, 1.0f }, 1.0f, plain3DecimalAttrs());
    // Renamed from "Late Mix" -- it's a post-balance blend of BOTH
    // engines' combined output (applied in processBlock() after early/late
    // are already crossfaded together), not a late-only control; the old
    // "Late " prefix was a leftover from when it lived in late's
    // ParamDialsPanel grid, which it no longer does (see PluginEditor's
    // shared middle strip). ParamID::mix itself is unchanged, so saved
    // state/automation still resolves correctly.
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::mix, 1 }, "Mix", Range { 0.0f, 1.0f }, 1.0f, plain3DecimalAttrs()));

    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::balance, 1 }, "Early/Late Balance",
        Range { 0.0f, 1.0f }, 0.5f, plain3DecimalAttrs()));

    // Independent output trims, alongside (not instead of) Balance --
    // unity (0dB) sits exactly at the dial's centre/12-o'clock since the
    // range is symmetric and 0 is the default.
    addParam (ParamID::earlyGainDb, "Early ", "Gain", Range { -24.0f, 24.0f }, 0.0f, dbUnitAttrs());
    addParam (ParamID::lateGainDb,  "Late ",  "Gain", Range { -24.0f, 24.0f }, 0.0f, dbUnitAttrs());

    // Shared -- one control, written into BOTH engines' params.predelayMs
    // in syncParams(). No prefix (it's neither early- nor late-specific).
    params.push_back (std::make_unique<Param> (
        juce::ParameterID { ParamID::predelayMs, 1 }, "Predelay", Range { 0.0f, 100.0f }, 0.0f, msUnitAttrs()));

    return { params.begin(), params.end() };
}

void GrainReverb2AudioProcessor::syncParams()
{
    // Shared -- same value goes into both engines' params below.
    const double predelayMs = (double) predelayMsParam->load();

    auto& p = lateShared.params;
    p.fadeSamps     = (double) fadeSampsParam->load();
    p.meanWindowMs  = (double) meanWindowMsParam->load();
    p.windowRangeMs = (double) windowRangeMsParam->load();
    p.bufferLenMs   = (double) bufferLenMsParam->load();
    p.fb            = (double) feedbackParam->load();
    p.jitter        = (double) jitterParam->load();
    p.dispersion    = (double) dispersionParam->load();
    p.numGrainVoices = (double) numGrainVoicesParam->load();
    // No dial any more -- pinned at 1.0 for both engines. There's no
    // longer a meaningful difference between "how much of the buffer is
    // active" (bufferLenMs) and "how far across the active buffer can
    // grains scatter" (readScatter) once grain read positions are safely
    // clamped against the write head (clampReadAgainstWriteHead) -- the two
    // dials were redundant, so bufferLenMs alone now controls del1's
    // active/scatter range.
    p.readScatter   = 1.0;
    p.predelayMs    = predelayMs;

    auto& ep = earlyShared.params;
    ep.bufferLenMs   = (double) earlyBufferLenMsParam->load();
    ep.fadeSamps     = (double) earlyFadeSampsParam->load();
    ep.meanWindowMs  = (double) earlyMeanWindowMsParam->load();
    ep.windowRangeMs = (double) earlyWindowRangeMsParam->load();
    ep.fb            = (double) earlyFeedbackParam->load();
    ep.fb2           = ep.fb; // one dial now drives both feedback loops equally
    ep.jitter        = (double) earlyJitterParam->load();
    ep.dispersion    = (double) earlyDispersionParam->load();
    ep.numGrainVoices = (double) earlyNumGrainVoicesParam->load();
    ep.readScatter   = 1.0;
    ep.predelayMs    = predelayMs;
}

GrainReverb2AudioProcessor::~GrainReverb2AudioProcessor() {}

void GrainReverb2AudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    syncParams(); // pick up current APVTS values (e.g. a loaded preset) before seeding grains

    lateShared.prepare (sampleRate);
    lateEngine.prepare (sampleRate, lateShared, kLateMaxVoicesPerBank, kLateMaxVoicesPerBank); // 6s/1s buffers (defaults)

    earlyShared.prepare (sampleRate);
    earlyEngine.prepare (sampleRate, earlyShared, kEarlyMaxVoicesPerBank, kEarlyMaxVoicesPerBank,
                          kEarlyDel1MaxSeconds, 0.0, /*singleBufferDualFeedback*/ true);
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

    // Independent trims on top of balance -- read once per block, same as
    // mix/balance above (no additional smoothing beyond what those two
    // already get away without): a plain dB-to-linear conversion applied
    // to each side's wet signal.
    const double earlyGainLin = std::pow (10.0, (double) earlyGainDbParam->load() / 20.0);
    const double lateGainLin  = std::pow (10.0, (double) lateGainDbParam->load() / 20.0);

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

        // Late's audible bank (Bank 2, reading del2) never receives the
        // dry input directly -- only fb*(aud1+aud2), feedback that's
        // already been through Bank 1's own normalization once. Early's
        // audible bank shares del1 with the write bank, so it reads the
        // raw dry input at full strength in the very buffer it's reading
        // -- one fewer feedback/normalization stage between input and
        // output. That structural gap, not anything wrong with either
        // engine's settings, is why late reads quieter than early at
        // matched feedback values. Boost late's wet signal here (not
        // inside GrainVoiceEngine, which both engines share) to close it.
        constexpr double kLateOutputBoost = 2.0;
        lateWetL *= kLateOutputBoost;
        lateWetR *= kLateOutputBoost;
        lateWetL *= lateGainLin;
        lateWetR *= lateGainLin;

        // Early's audible bank shares del1 with the write bank, so it
        // reads the raw dry input directly into the very buffer it's
        // reading -- the exact structural gap that makes hot input drive
        // coherent-overshoot-based distortion in GrainVoiceEngine's tanh
        // safety net (see its comment). Scaling the input down before it
        // ever reaches early's engine reduces how much energy builds up
        // in that shared buffer/feedback loop in the first place --
        // fixing the overshoot at its source instead of trying to patch
        // it after the fact downstream.
        constexpr double kEarlyInputScale = 0.25;
        double earlyWetL = 0.0, earlyWetR = 0.0;
        earlyEngine.processSample (dryL * kEarlyInputScale, dryR * kEarlyInputScale, earlyShared, earlyWetL, earlyWetR);
        earlyWetL *= earlyGainLin;
        earlyWetR *= earlyGainLin;

        // 0 = all early, 1 = all late (matches the Balance dial's "Early"/
        // "Late" low/high labels in ParamDialsPanel).
        const double wetL = earlyWetL * (1.0 - balance) + lateWetL * balance;
        const double wetR = earlyWetR * (1.0 - balance) + lateWetR * balance;

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
