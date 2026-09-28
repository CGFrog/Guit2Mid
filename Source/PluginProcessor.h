#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <vector>

#include "PluginParameters.h"
#include "dsp/GuitarMidiEngine.h"

class GuitarToMidiAudioProcessor : public juce::AudioProcessor
{
public:
    GuitarToMidiAudioProcessor();
    ~GuitarToMidiAudioProcessor() override = default;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;

    // Written on the audio thread, read by the editor's timer. Plain atomics:
    // slight staleness is harmless for a display.
    struct DisplayState
    {
        std::atomic<float> inputLevelDb { -100.0f };
        std::atomic<int> numNotes { 0 };
        std::array<std::atomic<int>, 6> notes {};
    };
    DisplayState displayState;

private:
    gtm::EngineSettings readSettings() const;

    gtm::GuitarMidiEngine engine_;
    std::vector<float> monoBuffer_;
    juce::MidiBuffer chunkMidi_;
    bool wasBypassed_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GuitarToMidiAudioProcessor)
};
