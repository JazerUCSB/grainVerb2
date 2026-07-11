#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "GrainReverbSharedState.h"
#include "GrainVoiceEngine.h"

// String IDs for every APVTS parameter. Kept as a namespace of constants
// (not scattered string literals) so a typo becomes a compile error at the
// call site instead of a silent "parameter not found" at runtime -- same
// pattern as the ParamID namespace in the original GrainReverb project.
namespace ParamID
{
    // Late reflections (the original del1/del2 engine).
    static constexpr auto fadeSamps     = "fadeSamps";
    static constexpr auto meanWindowMs  = "meanWindowMs";
    static constexpr auto windowRangeMs = "windowRangeMs";
    static constexpr auto bufferLenMs   = "bufferLenMs";
    static constexpr auto feedback      = "feedback";
    static constexpr auto readScatter   = "readScatter";
    static constexpr auto jitter        = "jitter";
    static constexpr auto dispersion    = "dispersion";
    static constexpr auto mix           = "mix";
    static constexpr auto numGrainVoices = "numGrainVoices"; // 25-100, per-bank voice count

    // Early reflections -- a second, smaller/faster-scaled instance of the
    // same GrainVoiceEngine (fixed 1000ms/600ms buffers, no bufferLenMs/
    // readScatter controls -- see PluginProcessor.cpp's syncParams()).
    // Shares late's cutoff/Q/tail curve UI pattern but with its own
    // independent curves, and its own fadeSamps/meanWindowMs/windowRangeMs/
    // feedback/jitter/dispersion.
    static constexpr auto earlyFadeSamps     = "earlyFadeSamps";
    static constexpr auto earlyMeanWindowMs  = "earlyMeanWindowMs";
    static constexpr auto earlyWindowRangeMs = "earlyWindowRangeMs";
    static constexpr auto earlyFeedback      = "earlyFeedback";
    static constexpr auto earlyJitter        = "earlyJitter";
    static constexpr auto earlyDispersion    = "earlyDispersion";
    static constexpr auto earlyNumGrainVoices = "earlyNumGrainVoices"; // 5-30, per-bank voice count

    // Crossfades the early/late wet signals BEFORE the shared dry/wet mix
    // above is applied -- 0 = all late, 1 = all early.
    static constexpr auto balance            = "balance";
}

// GrainReverb2AudioProcessor is the class every plugin format wrapper
// (VST3, Standalone) talks to. JUCE owns its lifecycle:
//   constructor          -> called once when the plugin is instantiated
//   prepareToPlay()      -> called before playback starts / on sample-rate change
//   processBlock()       -> called repeatedly on the audio thread
//   releaseResources()   -> called when playback stops
//
// The processor owns one GrainReverbSharedState (curves/coeff tables,
// scalar params) and ONE GrainVoiceEngine -- stereo width now comes from
// per-grain panning inside that single engine (see GrainVoiceEngine.h),
// not from routing two separate mono engines against each other.
class GrainReverb2AudioProcessor : public juce::AudioProcessor
{
public:
    GrainReverb2AudioProcessor();
    ~GrainReverb2AudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // Message-thread access to the breakpoint curves, for the editor.
    // Editing cutoffCurve/qCurve/tailCurve through this reference is safe
    // -- the audio thread never touches them directly, only the baked
    // tables reached via getLiveCurves()/getLiveCoeffs(). Call rebake()
    // after editing to publish it.
    GrainReverbSharedState& getLateSharedState() { return lateShared; }
    GrainReverbSharedState& getEarlySharedState() { return earlyShared; }

    // Read-only access for the visualizers -- see the comment on
    // GrainVoiceEngine::getDelayBuffer1() for the threading tradeoff.
    const GrainVoiceEngine& getLateEngine() const { return lateEngine; }
    const GrainVoiceEngine& getEarlyEngine() const { return earlyEngine; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    // Copies the current APVTS values into lateShared.params/
    // earlyShared.params. Called once per processBlock --
    // GrainReverbSharedState/GrainVoiceEngine never touch APVTS directly,
    // they just read plain doubles.
    void syncParams();

    GrainReverbSharedState lateShared;
    GrainVoiceEngine lateEngine;

    // Early reflections: same engine/state classes, configured (in
    // prepareToPlay()) with much smaller buffers (1000ms/600ms) and far
    // fewer voices per bank (16 vs late's 200). No bufferLenMs/readScatter
    // controls -- its del1/del2 are always fully active, matching how
    // late's OWN del2 already has no scatter control (see syncParams()).
    GrainReverbSharedState earlyShared;
    GrainVoiceEngine earlyEngine;

    // Raw parameter pointers, cached once in the constructor so syncParams()
    // doesn't do a string-keyed tree lookup every block.
    std::atomic<float>* fadeSampsParam     = nullptr;
    std::atomic<float>* meanWindowMsParam  = nullptr;
    std::atomic<float>* windowRangeMsParam = nullptr;
    std::atomic<float>* bufferLenMsParam   = nullptr;
    std::atomic<float>* feedbackParam      = nullptr;
    std::atomic<float>* readScatterParam   = nullptr;
    std::atomic<float>* jitterParam        = nullptr;
    std::atomic<float>* dispersionParam    = nullptr;
    std::atomic<float>* numGrainVoicesParam = nullptr;

    std::atomic<float>* earlyFadeSampsParam      = nullptr;
    std::atomic<float>* earlyMeanWindowMsParam   = nullptr;
    std::atomic<float>* earlyWindowRangeMsParam  = nullptr;
    std::atomic<float>* earlyFeedbackParam       = nullptr;
    std::atomic<float>* earlyJitterParam         = nullptr;
    std::atomic<float>* earlyDispersionParam     = nullptr;
    std::atomic<float>* earlyNumGrainVoicesParam = nullptr;
    std::atomic<float>* balanceParam             = nullptr;

    // Not routed through GrainReverbParams/syncParams -- mix is a pure
    // wet/dry blend applied in processBlock AFTER the engine runs, so the
    // engine itself never needs to know about it.
    std::atomic<float>* mixParam           = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (GrainReverb2AudioProcessor)
};
