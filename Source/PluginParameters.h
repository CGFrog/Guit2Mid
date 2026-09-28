#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace gtm::param
{
    inline constexpr auto modeId             = "mode";
    inline constexpr auto responseId         = "response";
    inline constexpr auto gateId             = "gate";
    inline constexpr auto pickSensitivityId  = "pickSensitivity";
    inline constexpr auto velocityDynamicsId = "velocityDynamics";
    inline constexpr auto maxPolyphonyId     = "maxPolyphony";
    inline constexpr auto midiModeId         = "midiMode";
    inline constexpr auto pitchBendId        = "pitchBend";
    inline constexpr auto bendRangeId        = "bendRange";
    inline constexpr auto outputChannelId    = "outputChannel";
    inline constexpr auto transposeId        = "transpose";
    inline constexpr auto tuningId           = "tuning";
    inline constexpr auto monitorInputId     = "monitorInput";

    // Parameter version 2: the whole layout changed with the engine rewrite.
    inline constexpr int kVersion = 2;

    inline juce::AudioProcessorValueTreeState::ParameterLayout createLayout()
    {
        using namespace juce;
        std::vector<std::unique_ptr<RangedAudioParameter>> params;

        params.push_back(std::make_unique<AudioParameterChoice>(
            ParameterID { modeId, kVersion }, "Playing Mode",
            StringArray { "Chords & Notes", "Single Notes (lead)" }, 0));

        params.push_back(std::make_unique<AudioParameterChoice>(
            ParameterID { responseId, kVersion }, "Response",
            StringArray { "Fast", "Balanced", "Accurate" }, 0));

        params.push_back(std::make_unique<AudioParameterFloat>(
            ParameterID { gateId, kVersion }, "Noise Gate",
            NormalisableRange<float> { -80.0f, -20.0f, 0.5f }, -55.0f,
            AudioParameterFloatAttributes{}.withLabel("dB")));

        params.push_back(std::make_unique<AudioParameterFloat>(
            ParameterID { pickSensitivityId, kVersion }, "Pick Sensitivity",
            NormalisableRange<float> { 0.0f, 1.0f, 0.01f }, 0.5f));

        params.push_back(std::make_unique<AudioParameterFloat>(
            ParameterID { velocityDynamicsId, kVersion }, "Velocity Dynamics",
            NormalisableRange<float> { 0.0f, 1.0f, 0.01f }, 0.75f));

        params.push_back(std::make_unique<AudioParameterInt>(
            ParameterID { maxPolyphonyId, kVersion }, "Max Notes", 1, 6, 6));

        params.push_back(std::make_unique<AudioParameterChoice>(
            ParameterID { midiModeId, kVersion }, "MIDI Output",
            StringArray { "Single Channel", "MPE (per-note bend)" }, 0));

        params.push_back(std::make_unique<AudioParameterBool>(
            ParameterID { pitchBendId, kVersion }, "Pitch Bend", false));

        params.push_back(std::make_unique<AudioParameterInt>(
            ParameterID { bendRangeId, kVersion }, "Bend Range", 1, 24, 2,
            AudioParameterIntAttributes{}.withLabel("st")));

        params.push_back(std::make_unique<AudioParameterInt>(
            ParameterID { outputChannelId, kVersion }, "MIDI Channel", 1, 16, 1));

        params.push_back(std::make_unique<AudioParameterInt>(
            ParameterID { transposeId, kVersion }, "Transpose", -24, 24, 0,
            AudioParameterIntAttributes{}.withLabel("st")));

        params.push_back(std::make_unique<AudioParameterFloat>(
            ParameterID { tuningId, kVersion }, "Tuning (A4)",
            NormalisableRange<float> { 430.0f, 450.0f, 0.1f }, 440.0f,
            AudioParameterFloatAttributes{}.withLabel("Hz")));

        params.push_back(std::make_unique<AudioParameterBool>(
            ParameterID { monitorInputId, kVersion }, "Monitor Input", false));

        return { params.begin(), params.end() };
    }
}
