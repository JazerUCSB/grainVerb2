#pragma once

#include <vector>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "VisualizerLayout.h"

// Raw scrolling view of BOTH channels of the engine's del1 circular buffer,
// stacked L-on-top/R-on-bottom -- a diagnostic view of what's literally
// stored in the buffer, not a claim about output timing (see the RT60
// discussion: a dedicated OutputDecayVisualizer, scaled by RT60, is the
// right place for the breakpoint curves to map onto perceived decay
// timing -- that's a separate component, not this one).
//
// Now that stereo width comes from per-grain panning inside a single
// merged engine (not two separate mono engines), showing both del1L and
// del1R at once (rather than toggling between them) is what actually shows
// the stereo picture -- there's no single "which channel" that's more
// informative than the other.
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
    explicit CircularBufferVisualizer (GrainReverb2AudioProcessor& processorToUse);
    ~CircularBufferVisualizer() override;

    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    GrainReverb2AudioProcessor& processor;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CircularBufferVisualizer)
};
