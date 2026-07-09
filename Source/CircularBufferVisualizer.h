#pragma once

#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "VisualizerLayout.h"

// Raw scrolling view of one channel of the engine's del1 circular buffer --
// a diagnostic view of what's literally stored in the buffer, not a claim
// about output timing (see the RT60 discussion: a dedicated
// OutputDecayVisualizer, scaled by RT60, is the right place for the
// breakpoint curves to map onto perceived decay timing -- that's a
// separate component, not this one).
//
// Which channel (L/R) is chosen by setVoiceChannel() (defaults to Left).
// Now that stereo width comes from per-grain panning inside a single
// merged engine (not two separate mono engines), "channel" here just means
// "which of del1L/del1R to look at."
//
// Orientation matches the PDF's spec exactly: newest material (the write
// head) is at the left edge; the right edge is the oldest material a grain
// can reach (readSpan = readScatter * del1Len samples behind the write
// head). x maps to dn in [0, 1] -- the same coordinate cutoffCurve/qCurve/
// tailCurve are indexed by -- labeled in seconds via t = dn * readSpan /
// sampleRate, which is exact for "age of this buffer slot since it was
// last written" (see GrainVoiceEngine::pokeWrite) -- though NOT the same
// thing as "when you'll hear this in the output," which is what the RT60
// view will address separately.
class CircularBufferVisualizer : public juce::Component,
                                  private juce::Timer
{
public:
    enum class VoiceChannel { Left, Right };

    explicit CircularBufferVisualizer (GrainReverb2AudioProcessor& processorToUse);
    ~CircularBufferVisualizer() override;

    void setVoiceChannel (VoiceChannel ch) { voiceChannel = ch; repaint(); }
    VoiceChannel getVoiceChannel() const { return voiceChannel; }

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    GrainReverb2AudioProcessor& processor;
    VoiceChannel voiceChannel = VoiceChannel::Left;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CircularBufferVisualizer)
};
