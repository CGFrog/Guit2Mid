#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <algorithm>
#include <cmath>
#include "MusicMath.h"

namespace gtm
{
    // Owns everything about how detected notes become MIDI bytes: channel
    // allocation (single channel or MPE lower zone), transpose, velocity, pitch
    // bend (with a small dead-zone so ordinary finger-pressure intonation
    // wobble doesn't make the synth sound out of tune), and making sure a
    // channel's bend is reset before a new note starts on it.
    class MidiNoteOutput
    {
    public:
        struct Config
        {
            bool mpe = false;
            int channel = 1;          // single-channel mode
            bool bendEnabled = false;
            int bendRange = 2;        // semitones; sent to the receiver as RPN 0
            int transpose = 0;

            bool operator==(const Config& o) const
            {
                return mpe == o.mpe && channel == o.channel && bendEnabled == o.bendEnabled
                       && bendRange == o.bendRange && transpose == o.transpose;
            }
            bool operator!=(const Config& o) const { return !(*this == o); }
        };

        static constexpr int kMaxVoices = 16;

        MidiNoteOutput() { reset(); }

        void reset()
        {
            for (auto& v : voices_)
                v = {};
            lastBend_.fill(8192);
            channelReleaseTime_.fill(0);
            clock_ = 0;
            needsSetup_ = true;
        }

        const Config& getConfig() const { return config_; }

        // Returns true if the change requires all sounding notes to be released
        // first (caller must call allNotesOff before the next noteOn).
        bool setConfig(const Config& c)
        {
            if (c == config_)
                return false;
            const bool structural = c.mpe != config_.mpe || c.channel != config_.channel
                                    || c.transpose != config_.transpose;
            if (c.bendRange != config_.bendRange || c.mpe != config_.mpe || c.bendEnabled != config_.bendEnabled)
                needsSetup_ = true;
            config_ = c;
            return structural;
        }

        void emitSetupIfNeeded(juce::MidiBuffer& out, int offset)
        {
            if (!needsSetup_)
                return;
            needsSetup_ = false;
            if (config_.mpe)
            {
                auto setup = juce::MPEMessages::setLowerZone(15, config_.bendRange, 2);
                for (const auto meta : setup)
                    out.addEvent(meta.getMessage(), offset);
                lastBend_.fill(8192);
            }
            else if (config_.bendEnabled)
            {
                const auto rpn = juce::MidiRPNGenerator::generate(config_.channel, 0, config_.bendRange << 7, false, true);
                for (const auto meta : rpn)
                    out.addEvent(meta.getMessage(), offset);
            }
        }

        int noteOn(int midi, int velocity, juce::MidiBuffer& out, int offset)
        {
            const int outNote = std::clamp(midi + config_.transpose, 0, 127);
            int slot = -1;
            for (int i = 0; i < kMaxVoices; ++i)
                if (!voices_[(size_t) i].on) { slot = i; break; }
            if (slot < 0)
                return -1;

            const int channel = config_.mpe ? allocateMpeChannel(out, offset) : config_.channel;
            if (lastBend_[(size_t) channel - 1] != 8192)
            {
                out.addEvent(juce::MidiMessage::pitchWheel(channel, 8192), offset);
                lastBend_[(size_t) channel - 1] = 8192;
            }
            out.addEvent(juce::MidiMessage::noteOn(channel, outNote, (juce::uint8) std::clamp(velocity, 1, 127)), offset);
            voices_[(size_t) slot] = { true, channel, outNote };
            return slot;
        }

        void noteOff(int voice, juce::MidiBuffer& out, int offset)
        {
            if (voice < 0 || voice >= kMaxVoices || !voices_[(size_t) voice].on)
                return;
            auto& v = voices_[(size_t) voice];
            out.addEvent(juce::MidiMessage::noteOff(v.channel, v.note, (juce::uint8) 64), offset);
            v.on = false;
            channelReleaseTime_[(size_t) v.channel - 1] = ++clock_;
        }

        // semitones: deviation of the played pitch from the note's nominal pitch.
        void pitchBend(int voice, double semitones, juce::MidiBuffer& out, int offset)
        {
            if (!config_.bendEnabled || voice < 0 || voice >= kMaxVoices || !voices_[(size_t) voice].on)
                return;
            const auto& v = voices_[(size_t) voice];
            const int value = semitonesToPitchBend14Bit(applyDeadZone(semitones), config_.bendRange);
            int& last = lastBend_[(size_t) v.channel - 1];
            if (std::abs(value - last) < 24 && value != 8192)
                return; // below audibility; don't flood the MIDI stream
            if (value == last)
                return;
            last = value;
            out.addEvent(juce::MidiMessage::pitchWheel(v.channel, value), offset);
        }

        void allNotesOff(juce::MidiBuffer& out, int offset)
        {
            for (int i = 0; i < kMaxVoices; ++i)
                noteOff(i, out, offset);
            for (int ch = 1; ch <= 16; ++ch)
                if (lastBend_[(size_t) ch - 1] != 8192)
                {
                    out.addEvent(juce::MidiMessage::pitchWheel(ch, 8192), offset);
                    lastBend_[(size_t) ch - 1] = 8192;
                }
        }

        // Pitch deviations smaller than ~12 cents are guitar intonation noise,
        // not intentional bends; fade them out smoothly rather than hard-gating.
        static double applyDeadZone(double semitones)
        {
            constexpr double dz = 0.12;
            const double a = std::abs(semitones);
            if (a >= 2.0 * dz)
                return semitones;
            const double t = a / (2.0 * dz);
            return semitones * t * t * (3.0 - 2.0 * t);
        }

    private:
        int allocateMpeChannel(juce::MidiBuffer& out, int offset)
        {
            // Least-recently-released free member channel, so a new note never
            // lands on a channel whose previous note is still in its release tail.
            int best = -1;
            for (int ch = 2; ch <= 16; ++ch)
            {
                const bool busy = std::any_of(voices_.begin(), voices_.end(),
                                              [ch](const Voice& v) { return v.on && v.channel == ch; });
                if (busy)
                    continue;
                if (best < 0 || channelReleaseTime_[(size_t) ch - 1] < channelReleaseTime_[(size_t) best - 1])
                    best = ch;
            }
            if (best >= 0)
                return best;
            // All 15 busy (can't happen with <= 6 guitar notes, but be safe): steal voice 0's channel.
            const int ch = voices_[0].channel;
            noteOff(0, out, offset);
            return ch;
        }

        struct Voice
        {
            bool on = false;
            int channel = 1;
            int note = 0;
        };

        Config config_;
        std::array<Voice, kMaxVoices> voices_ {};
        std::array<int, 16> lastBend_ {};
        std::array<juce::uint32, 16> channelReleaseTime_ {};
        juce::uint32 clock_ = 0;
        bool needsSetup_ = true;
    };
}
