#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <vector>
#include <cstdint>
#include <string>
#include "MusicMath.h"
#include "OnsetDetector.h"
#include "YinPitchDetector.h"
#include "PolyPitchDetector.h"
#include "MidiNoteOutput.h"

namespace gtm
{
    // Offline tuning hook for the evaluation tool (the plugin never calls this).
    bool setEngineTuning(const std::string& name, double value);

    struct EngineSettings
    {
        enum class Mode { poly, mono };

        Mode mode = Mode::poly;
        int response = 0;                  // 0 = fast, 1 = balanced, 2 = accurate
        double gateDb = -55.0;             // input level below which nothing starts
        double onsetSensitivity = 0.5;     // 0..1
        double velocitySensitivity = 0.75; // 0 = fixed velocity, 1 = full dynamics
        int maxPolyphony = 6;
        double tuningA4 = 440.0;
        MidiNoteOutput::Config midi;
    };

    // The whole audio -> MIDI pipeline, independent of the plugin wrapper so it
    // can also be driven offline by the evaluation tool.
    //
    // Design: note-ons are decided at pick attacks. When the onset detector
    // fires, the engine analyses a window that *starts at the attack* and grows
    // hop by hop; as soon as the detected pitch set is stable (or a deadline
    // passes) the notes are committed. Because the analysis never looks at audio
    // from before the attack, the previous chord can't bleed into the new one,
    // and high notes lock in faster than low ones. Between attacks a longer
    // sliding window is used only to decide when sounding notes end (and for
    // per-note bends), plus a slower fallback for notes that start without a
    // detectable attack (hammer-ons, very soft playing).
    class GuitarMidiEngine
    {
    public:
        void prepare(double sampleRate);
        void reset();
        void setSettings(const EngineSettings& s) { pendingSettings_ = s; }

        // mono input; MIDI event offsets are relative to the start of this block.
        void process(const float* input, int numSamples, juce::MidiBuffer& midiOut);
        void allNotesOff(juce::MidiBuffer& midiOut, int sampleOffset);

        float getLevelDb() const { return levelDb_; }
        int getActiveNotes(int* dest, int maxCount) const;
        int getHopSize() const { return hop_; }

    private:
        struct ActiveNote
        {
            bool used = false;
            int midi = 0;
            int voice = -1;
            double f0 = 0.0;
            double amp = 0.0;
            double peakAmp = 0.0;
            double preOnsetAmp = 0.0;
            std::array<double, 3> prePartials {};
            bool preExisting = false; // was sounding before the current attack
            int velocity = 100;
            int miss = 0;
            int64_t start = 0;
        };

        void applySettings(juce::MidiBuffer& out);
        void runHop(juce::MidiBuffer& out, int offset);
        void polyHop(bool onset, int64_t onsetPos, juce::MidiBuffer& out, int offset);
        void monoHop(bool onset, int64_t onsetPos, juce::MidiBuffer& out, int offset);
        void polyTrack(juce::MidiBuffer& out, int offset);
        void commitNote(const PolyPitchDetector::Note& n, int count, int velocity, juce::MidiBuffer& out, int offset);
        bool sharesPartial(double f, int ownMidi, int count) const;
        void releaseMutedNotes(juce::MidiBuffer& out, int offset);
        void finishGather(juce::MidiBuffer& out, int offset);

        int64_t locateOnset() const;
        void readRange(int64_t start, int length, float* dest) const;
        double peakSince(int64_t start) const;
        int velocityFromPeak(double peak) const;

        ActiveNote* findActive(int midi);
        ActiveNote* startNote(int midi, double f0, int velocity, double amp, juce::MidiBuffer& out, int offset);
        void stopNote(ActiveNote& n, juce::MidiBuffer& out, int offset);
        int numActive() const;

        // --- configuration
        double sampleRate_ = 44100.0;
        int hop_ = 256, onsetFrame_ = 1024, trackLen_ = 3000, maxGather_ = 4410, monoTrackLen_ = 1764;
        EngineSettings settings_, pendingSettings_;
        bool firstBlock_ = true;

        // --- audio history
        std::vector<float> ring_;
        int ringMask_ = 0;
        int64_t pos_ = 0;
        std::vector<float> seg_;
        int hopFill_ = 0;
        double hopSumSq_ = 0.0;
        float levelDb_ = -120.0f;
        int gateCount_ = 0;
        int quietHops_ = 0;
        int64_t lastOnset_ = -1000000;
        double hpX1_ = 0, hpX2_ = 0, hpY1_ = 0, hpY2_ = 0;
        double hpB0_ = 1, hpB1_ = 0, hpB2_ = 0, hpA1_ = 0, hpA2_ = 0;

        // --- analysis
        OnsetDetector onset_;
        YinPitchDetector yin_;
        PolyPitchDetector poly_;
        std::array<PolyPitchDetector::Note, PolyPitchDetector::kMaxNotesOut> notes_ {};
        MidiNoteOutput output_;

        // --- note state
        std::array<ActiveNote, 8> active_ {};

        struct Gather
        {
            bool active = false;
            int64_t start = 0;
            bool mutedChecked = false;
            bool repickDecided = false;
            std::array<juce::uint8, 128> streak {};  // consecutive analyses each pitch was seen in
            std::array<bool, 128> handled {};        // pitch already committed in this attack
        } gather_;

        std::array<int, 128> lateCount_ {};

        struct Mono
        {
            bool attack = false;
            int64_t t0 = 0;
            int64_t noteStart = 0;
            int stable = 0;
            double lastEst = -1.0;
            std::array<double, 3> recent { -1.0, -1.0, -1.0 };
            int legatoTarget = -1;
            int legatoCount = 0;
            int lateCount = 0;
            int lateTarget = -1;
        } mono_;
    };
}
