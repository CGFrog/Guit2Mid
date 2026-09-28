#pragma once

#include <cmath>
#include <algorithm>

namespace gtm
{
    constexpr double kReferenceA4Freq = 440.0;
    constexpr int kReferenceA4Midi = 69;

    // Guitar-relevant pitch range: C2 (drop-C low string) .. D6 (beyond the
    // 24th fret of the high E string). Kept deliberately tight: every extra
    // candidate below the real range is another chance for a sub-octave error.
    constexpr double kMinGuitarMidi = 36.0;
    constexpr double kMaxGuitarMidi = 86.0;

    inline double midiToFreq(double midiNote, double a4 = kReferenceA4Freq) noexcept
    {
        return a4 * std::pow(2.0, (midiNote - kReferenceA4Midi) / 12.0);
    }

    inline double freqToMidi(double freqHz, double a4 = kReferenceA4Freq) noexcept
    {
        if (freqHz <= 0.0)
            return 0.0;
        return kReferenceA4Midi + 12.0 * std::log2(freqHz / a4);
    }

    inline int freqToNearestMidi(double freqHz, double a4 = kReferenceA4Freq) noexcept
    {
        return (int) std::lround(freqToMidi(freqHz, a4));
    }

    inline double gainToDb(double gain) noexcept
    {
        return 20.0 * std::log10(std::max(gain, 1.0e-9));
    }

    inline double dbToGain(double db) noexcept
    {
        return std::pow(10.0, db / 20.0);
    }

    // 14-bit MIDI pitch-bend value for a semitone offset, given the receiver's
    // bend range in semitones.
    inline int semitonesToPitchBend14Bit(double semitones, double bendRangeSemitones) noexcept
    {
        const double normalized = std::clamp(semitones / bendRangeSemitones, -1.0, 1.0);
        return std::clamp(8192 + (int) std::lround(normalized * 8191.0), 0, 16383);
    }

    inline int nextPowerOfTwo(int x) noexcept
    {
        int p = 1;
        while (p < x)
            p <<= 1;
        return p;
    }
}
