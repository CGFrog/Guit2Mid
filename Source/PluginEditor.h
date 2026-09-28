#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"

class GuitarToMidiAudioProcessorEditor : public juce::AudioProcessorEditor,
                                          private juce::Timer
{
public:
    explicit GuitarToMidiAudioProcessorEditor(GuitarToMidiAudioProcessor&);
    ~GuitarToMidiAudioProcessorEditor() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    struct LabelledSlider
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };
    struct LabelledCombo
    {
        juce::ComboBox box;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;
    };
    struct Toggle
    {
        juce::ToggleButton button;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;
    };

    void setup(LabelledSlider&, const char* paramId, const juce::String& text, const juce::String& tooltip);
    void setup(LabelledCombo&, const char* paramId, const juce::String& text, const juce::String& tooltip);
    void setup(Toggle&, const char* paramId, const juce::String& text, const juce::String& tooltip);
    void updateEnablement();

    GuitarToMidiAudioProcessor& processor;
    juce::TooltipWindow tooltips { this, 600 };

    LabelledCombo mode, response, midiMode;
    LabelledSlider gate, pickSensitivity, maxNotes, velocityDynamics, channel, bendRange, transpose, tuning;
    Toggle pitchBend, monitor;

    juce::Rectangle<int> notesArea, meterArea, detectionHeader, outputHeader;
    float displayedLevelDb = -100.0f;
    juce::String notesText;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GuitarToMidiAudioProcessorEditor)
};
