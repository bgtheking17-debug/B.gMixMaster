#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include "Engine.h"

#ifndef AUTO_IS_MASTER
 #define AUTO_IS_MASTER 1
#endif

// Thin JUCE wrapper around bg::Engine (all DSP lives in Engine.h / Dsp.h and is unit-tested separately).
class AutoProcessor : public juce::AudioProcessor
{
public:
    AutoProcessor();
    ~AutoProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

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

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    // For the GUI (message thread only): copy what "Auto" currently computes into the Manual controls.
    void copyAutoToManual();

    juce::AudioProcessorValueTreeState apvts;
    const bg::Meters& getMeters() const noexcept { return engine.meters(); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();
    bg::EngineParams readParams() const;
    void setParam (const juce::String& id, float realValue);

    bg::Engine engine { AUTO_IS_MASTER != 0 };
    double currentSampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AutoProcessor)
};
