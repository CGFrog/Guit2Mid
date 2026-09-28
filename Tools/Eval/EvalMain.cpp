// Offline evaluation / conversion tool for the Guitar to MIDI engine.
//
//   GuitarToMidiEval                      run the synthetic test suite, print scores
//   GuitarToMidiEval --verbose <name>     ...and dump expected vs. detected notes for matching scenarios
//   GuitarToMidiEval --render <dir>       ...and write each scenario's audio as WAV
//   GuitarToMidiEval --mono in.wav out.mid   convert a recording (default poly; --mono for mono mode)
//
// Scoring is note-level: a detected note counts as correct if it has the right
// MIDI pitch and starts within 80 ms of a true note (each true note can be
// matched once). Latency = detected start - true start for matched notes.

#include <juce_audio_formats/juce_audio_formats.h>
#include "../../Source/dsp/GuitarMidiEngine.h"
#include "GuitarSynth.h"
#include <map>
#include <iostream>
#include <iomanip>
#include <sstream>

using namespace gtmtest;

namespace
{
    struct TruthNote
    {
        double onset, offset;
        int midi;
    };

    struct Scenario
    {
        std::string name;
        double duration = 4.0;
        bool mono = false;
        bool bends = false;
        double sampleRate = 44100.0;
        std::vector<SynthNote> synth;
        std::vector<TruthNote> truth;
        double noiseDb = -80.0, humDb = -66.0;
    };

    struct DetectedNote
    {
        double onset, offset;
        int midi, velocity, channel;
    };

    // Standard tuning open strings, low to high.
    const int kOpen[6] = { 40, 45, 50, 55, 59, 64 };

    std::mt19937 gRng(7);
    unsigned gSeed = 0;
    double jitter(double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(gRng); }

    void note(Scenario& s, double onset, double dur, int midi, double vel = 0.8, bool inTruth = true)
    {
        SynthNote n;
        n.onset = onset;
        n.offset = onset + dur;
        n.midi = midi;
        n.velocity = vel;
        n.pickPosition = jitter(0.08, 0.2);
        n.pickupPosition = jitter(0.08, 0.22);
        n.brightness = jitter(0.4, 0.8);
        s.synth.push_back(n);
        if (inTruth)
            s.truth.push_back({ onset, onset + dur, midi });
    }

    // frets: -1 = string not played. Strummed low->high (down) or high->low (up).
    void strum(Scenario& s, double t, const std::array<int, 6>& frets, double dur, double vel = 0.8,
               bool down = true, double spread = 0.012, int maxStrings = 6)
    {
        std::vector<int> strings;
        for (int i = 0; i < 6; ++i)
            if (frets[(size_t) i] >= 0)
                strings.push_back(i);
        if (!down)
            std::reverse(strings.begin(), strings.end());
        if ((int) strings.size() > maxStrings)
            strings.resize((size_t) maxStrings);
        double tt = t;
        for (int i : strings)
        {
            note(s, tt, dur - (tt - t), kOpen[i] + frets[(size_t) i], vel * jitter(0.85, 1.0));
            tt += spread * jitter(0.6, 1.4);
        }
    }

    using Shape = std::array<int, 6>;
    const std::vector<std::pair<std::string, Shape>> kChords = {
        { "E",   { 0, 2, 2, 1, 0, 0 } },   { "A",  { -1, 0, 2, 2, 2, 0 } },
        { "D",   { -1, -1, 0, 2, 3, 2 } }, { "G",  { 3, 2, 0, 0, 0, 3 } },
        { "C",   { -1, 3, 2, 0, 1, 0 } },  { "Am", { -1, 0, 2, 2, 1, 0 } },
        { "Em",  { 0, 2, 2, 0, 0, 0 } },   { "F",  { 1, 3, 3, 2, 1, 1 } },
        { "B7",  { -1, 2, 1, 2, 0, 2 } },  { "Dm", { -1, -1, 0, 2, 3, 1 } },
        { "Bm",  { -1, 2, 4, 4, 3, 2 } },  { "Cmaj7", { -1, 3, 2, 0, 0, 0 } },
        { "E5",  { 0, 2, 2, -1, -1, -1 } },{ "A5", { -1, 0, 2, 2, -1, -1 } },
        { "G5",  { 3, 5, 5, -1, -1, -1 } },{ "C5(3)", { -1, 3, 5, 5, -1, -1 } },
        { "Dtriad", { -1, -1, -1, 7, 7, 5 } }, { "Gtriad(hi)", { -1, -1, -1, 12, 12, 10 } },
        { "A7(5)", { 5, 7, 5, 6, 5, 5 } }, { "Fmaj7", { -1, -1, 3, 2, 1, 0 } },
    };

    std::vector<Scenario> buildScenarios()
    {
        std::vector<Scenario> all;

        // --- single notes -----------------------------------------------------
        for (bool mono : { false, true })
        {
            Scenario s;
            s.name = std::string(mono ? "mono" : "poly") + "/single-notes-range";
            s.mono = mono;
            double t = 0.2;
            for (int m = 40; m <= 84; m += 2)
            {
                note(s, t, 0.3, m, jitter(0.5, 1.0));
                t += 0.45;
            }
            s.duration = t + 0.5;
            all.push_back(s);
        }

        for (bool mono : { false, true })
        {
            Scenario s;
            s.name = std::string(mono ? "mono" : "poly") + "/fast-lick-16ths";
            s.mono = mono;
            const int lick[] = { 57, 60, 62, 64, 67, 64, 62, 60, 57, 55, 52, 55, 57, 60, 57, 55,
                                 64, 67, 69, 72, 69, 67, 64, 62, 60, 62, 64, 62, 60, 57, 55, 52 };
            double t = 0.2;
            for (int m : lick)
            {
                note(s, t, 0.118, m, jitter(0.55, 0.9));
                t += 0.125;
            }
            s.duration = t + 0.5;
            all.push_back(s);
        }

        for (bool mono : { false, true })
        {
            Scenario s;
            s.name = std::string(mono ? "mono" : "poly") + "/repeated-same-note";
            s.mono = mono;
            double t = 0.2;
            for (int i = 0; i < 12; ++i)
            {
                note(s, t, 0.1, i < 6 ? 57 : 45, jitter(0.5, 0.9));
                t += 0.11;
            }
            s.duration = t + 0.5;
            all.push_back(s);
        }

        {
            Scenario s;
            s.name = "mono/low-riff-palm-mute";
            s.mono = true;
            double t = 0.2;
            const int riff[] = { 40, 40, 40, 43, 40, 40, 45, 43, 40, 40, 40, 46, 45, 43, 40, 40 };
            for (int m : riff)
            {
                note(s, t, 0.1, m, jitter(0.6, 0.9));
                t += 0.14;
            }
            s.duration = t + 0.5;
            all.push_back(s);
        }

        // Hammer-on / pull-off: the string keeps vibrating, pitch steps.
        for (bool mono : { false, true })
        {
            Scenario s;
            s.name = std::string(mono ? "mono" : "poly") + "/hammer-pull";
            s.mono = mono;
            double t = 0.2;
            const int base[] = { 55, 57, 62, 64, 50 };
            for (int b : base)
            {
                SynthNote n;
                n.onset = t; n.offset = t + 0.6; n.midi = b; n.velocity = 0.8;
                n.pitchCurve = { { 0.0, 0 }, { 0.2, 0 }, { 0.2005, 2 }, { 0.4, 2 }, { 0.4005, 0 } };
                s.synth.push_back(n);
                s.truth.push_back({ t, t + 0.2, b });
                s.truth.push_back({ t + 0.2, t + 0.4, b + 2 });
                s.truth.push_back({ t + 0.4, t + 0.6, b });
                t += 0.9;
            }
            s.duration = t + 0.5;
            all.push_back(s);
        }

        // Bends and vibrato, mono with pitch bend on: each must stay ONE note.
        {
            Scenario s;
            s.name = "mono+bend/bends-vibrato";
            s.mono = true;
            s.bends = true;
            double t = 0.2;
            const int base[] = { 57, 62, 67, 64 };
            for (int b : base)
            {
                SynthNote n;
                n.onset = t; n.offset = t + 1.2; n.midi = b; n.velocity = 0.85;
                n.pitchCurve = { { 0.0, 0 }, { 0.15, 0 }, { 0.35, 2 }, { 0.7, 2 }, { 0.9, 0 } };
                s.synth.push_back(n);
                s.truth.push_back({ t, t + 1.2, b });
                t += 1.5;
            }
            for (int b : { 59, 69 })
            {
                SynthNote n;
                n.onset = t; n.offset = t + 1.2; n.midi = b; n.velocity = 0.8;
                for (int k = 0; k <= 60; ++k)
                    n.pitchCurve.push_back({ k * 0.02, 0.3 * std::sin(2 * 3.14159 * 5.5 * k * 0.02) * std::min(1.0, k * 0.02 / 0.3) });
                s.synth.push_back(n);
                s.truth.push_back({ t, t + 1.2, b });
                t += 1.5;
            }
            s.duration = t + 0.5;
            all.push_back(s);
        }

        // Vibrato with bends OFF must not flicker between notes.
        {
            Scenario s;
            s.name = "poly/vibrato-no-bend";
            double t = 0.2;
            for (int b : { 59, 64, 69, 55 })
            {
                SynthNote n;
                n.onset = t; n.offset = t + 1.0; n.midi = b; n.velocity = 0.8;
                for (int k = 0; k <= 50; ++k)
                    n.pitchCurve.push_back({ k * 0.02, 0.35 * std::sin(2 * 3.14159 * 6 * k * 0.02) });
                s.synth.push_back(n);
                s.truth.push_back({ t, t + 1.0, b });
                t += 1.3;
            }
            s.duration = t + 0.5;
            all.push_back(s);
        }

        // --- chords -----------------------------------------------------------
        {
            Scenario s;
            s.name = "poly/chords-strummed";
            double t = 0.2;
            for (const auto& c : kChords)
            {
                strum(s, t, c.second, 1.1, jitter(0.6, 1.0), true, 0.010);
                t += 1.4;
            }
            s.duration = t + 0.5;
            all.push_back(s);
        }

        {
            Scenario s;
            s.name = "poly/chords-block-plucked";
            double t = 0.2;
            for (const auto& c : kChords)
            {
                strum(s, t, c.second, 1.0, jitter(0.6, 1.0), true, 0.002);
                t += 1.25;
            }
            s.duration = t + 0.5;
            all.push_back(s);
        }

        {
            Scenario s;
            s.name = "poly/rhythm-down-up";
            // 8th-note strumming at 110 bpm, chord change every bar; upstrokes hit 4 strings.
            const std::vector<Shape> prog = { kChords[3].second, kChords[2].second, kChords[6].second, kChords[4].second,
                                              kChords[5].second, kChords[7].second, kChords[1].second, kChords[0].second };
            const double eighth = 60.0 / 110.0 / 2.0;
            double t = 0.2;
            for (const auto& shape : prog)
                for (int i = 0; i < 8; ++i)
                {
                    const bool down = (i % 2) == 0;
                    strum(s, t, shape, eighth - 0.01, down ? jitter(0.7, 0.95) : jitter(0.45, 0.65), down, 0.008, down ? 6 : 4);
                    t += eighth;
                }
            s.duration = t + 0.5;
            all.push_back(s);
        }

        {
            Scenario s;
            s.name = "poly/arpeggio-let-ring";
            // Fingerpicked, each string keeps ringing until the chord changes.
            const std::vector<Shape> prog = { kChords[4].second, kChords[5].second, kChords[3].second, kChords[0].second,
                                              kChords[9].second, kChords[19].second };
            double t = 0.2;
            for (const auto& shape : prog)
            {
                std::vector<int> strings;
                for (int i = 0; i < 6; ++i)
                    if (shape[(size_t) i] >= 0)
                        strings.push_back(i);
                const double barEnd = t + 0.25 * 6;
                std::vector<int> order = { strings[0], strings[2 % strings.size()], strings[strings.size() - 2],
                                           strings.back(), strings[strings.size() - 2], strings[1] };
                std::map<int, bool> played;
                for (size_t k = 0; k < order.size(); ++k)
                {
                    const int str = order[k];
                    if (played[str])
                    {
                        t += 0.25;
                        continue; // string already ringing; the synth would re-pick, keep it simple
                    }
                    played[str] = true;
                    note(s, t, barEnd - t, kOpen[str] + shape[(size_t) str], jitter(0.5, 0.8));
                    t += 0.25;
                }
                t = barEnd;
            }
            s.duration = t + 0.5;
            all.push_back(s);
        }

        {
            Scenario s;
            s.name = "poly/melody-over-chord";
            // Chord stabs with single melody notes in between (typical comping + fills).
            double t = 0.2;
            for (int rep = 0; rep < 4; ++rep)
            {
                strum(s, t, kChords[(size_t) (rep * 2)].second, 0.5, 0.8, true, 0.01);
                t += 0.6;
                for (int m : { 64, 67, 69, 71 })
                {
                    note(s, t, 0.19, m + rep, 0.7);
                    t += 0.2;
                }
                t += 0.2;
            }
            s.duration = t + 0.5;
            all.push_back(s);
        }

        {
            Scenario s;
            s.name = "poly/soft-playing";
            s.noiseDb = -72.0;
            s.humDb = -60.0;
            double t = 0.2;
            for (int m : { 45, 52, 57, 60, 64, 69 })
            {
                note(s, t, 0.4, m, 0.22);
                t += 0.55;
            }
            for (int i : { 0, 5, 6, 3 })
            {
                strum(s, t, kChords[(size_t) i].second, 1.0, 0.25, true, 0.015);
                t += 1.3;
            }
            s.duration = t + 0.5;
            all.push_back(s);
        }

        {
            Scenario s;
            s.name = "poly/silence-hum-noise";
            s.noiseDb = -62.0;
            s.humDb = -52.0;
            s.duration = 5.0;
            all.push_back(s);
        }

        {
            Scenario s;
            s.name = "poly/chords-48k";
            s.sampleRate = 48000.0;
            double t = 0.2;
            for (size_t i = 0; i < 10; ++i)
            {
                strum(s, t, kChords[i].second, 1.0, jitter(0.6, 1.0), (i % 2) == 0, 0.012);
                t += 1.25;
            }
            s.duration = t + 0.5;
            all.push_back(s);
        }

        return all;
    }

    // ------------------------------------------------------------------ running

    std::vector<DetectedNote> runEngine(const std::vector<float>& audio, double sampleRate, const gtm::EngineSettings& settings,
                                        int* numBends = nullptr)
    {
        gtm::GuitarMidiEngine engine;
        engine.setSettings(settings);
        engine.prepare(sampleRate);

        const int block = 512;
        std::map<std::pair<int, int>, DetectedNote> open;
        std::vector<DetectedNote> done;
        int bends = 0;
        juce::MidiBuffer midi;

        auto handle = [&](const juce::MidiBuffer& buf, int64_t blockStart)
        {
            for (const auto meta : buf)
            {
                const auto m = meta.getMessage();
                const double t = (double) (blockStart + meta.samplePosition) / sampleRate;
                const auto key = std::make_pair(m.getChannel(), m.getNoteNumber());
                if (m.isNoteOn())
                {
                    if (open.count(key))
                    {
                        auto n = open[key];
                        n.offset = t;
                        done.push_back(n);
                    }
                    open[key] = { t, -1.0, m.getNoteNumber(), m.getVelocity(), m.getChannel() };
                }
                else if (m.isNoteOff())
                {
                    if (open.count(key))
                    {
                        auto n = open[key];
                        n.offset = t;
                        done.push_back(n);
                        open.erase(key);
                    }
                }
                else if (m.isPitchWheel())
                    ++bends;
            }
        };

        for (int64_t pos = 0; pos < (int64_t) audio.size(); pos += block)
        {
            const int n = (int) std::min<int64_t>(block, (int64_t) audio.size() - pos);
            midi.clear();
            engine.process(audio.data() + pos, n, midi);
            handle(midi, pos);
        }
        midi.clear();
        engine.allNotesOff(midi, 0);
        handle(midi, (int64_t) audio.size());

        std::sort(done.begin(), done.end(), [](auto& a, auto& b) { return a.onset < b.onset; });
        if (numBends != nullptr)
            *numBends = bends;
        return done;
    }

    struct Score
    {
        int truth = 0, detected = 0, matched = 0, octave = 0, wrong = 0, chromaMatched = 0, pcCovered = 0, susPoints = 0, susCovered = 0;
        std::vector<double> latencies;
    };

    Score score(const std::vector<TruthNote>& truth, const std::vector<DetectedNote>& det,
                std::vector<int>* matchOfTruth = nullptr, std::vector<int>* detMatched = nullptr)
    {
        constexpr double tol = 0.08;
        Score s;
        s.truth = (int) truth.size();
        s.detected = (int) det.size();
        std::vector<int> used(det.size(), -1);
        std::vector<int> tm(truth.size(), -1);
        for (size_t i = 0; i < truth.size(); ++i)
        {
            int best = -1;
            double bestD = tol;
            for (size_t j = 0; j < det.size(); ++j)
            {
                if (used[j] >= 0 || det[j].midi != truth[i].midi)
                    continue;
                const double d = det[j].onset - truth[i].onset;
                if (d > -0.02 && std::abs(d) <= bestD)
                {
                    bestD = std::abs(d);
                    best = (int) j;
                }
            }
            if (best >= 0)
            {
                used[(size_t) best] = (int) i;
                tm[i] = best;
                ++s.matched;
                s.latencies.push_back(det[(size_t) best].onset - truth[i].onset);
            }
        }
        for (size_t j = 0; j < det.size(); ++j)
        {
            if (used[j] >= 0)
            {
                ++s.chromaMatched;
                continue;
            }
            // Classify the false positive by its relation to a true note sounding at that time.
            bool octave = false, chroma = false;
            for (const auto& t : truth)
                if (det[j].onset >= t.onset - 0.03 && det[j].onset <= t.offset)
                {
                    const int d = std::abs(det[j].midi - t.midi);
                    chroma |= d % 12 == 0;
                    octave |= d == 12 || d == 24;
                }
            if (chroma)
                ++s.chromaMatched;
            if (octave)
                ++s.octave;
            else if (!chroma)
                ++s.wrong;
        }
        // Pitch-class coverage: is each true note's pitch class sounding (in any
        // octave) 100 ms after it starts? Missing octave doublings don't count against this.
        for (const auto& t : truth)
        {
            const double probeT = std::min(t.onset + 0.1, t.offset - 0.001);
            for (const auto& d : det)
                if (d.midi % 12 == t.midi % 12 && d.onset <= probeT && d.offset > probeT)
                {
                    ++s.pcCovered;
                    break;
                }
        }
        // Sustain coverage: for notes >= 300 ms, is the pitch class still sounding
        // mid-note and shortly before the end? Catches chord notes dropped early.
        for (const auto& t : truth)
        {
            if (t.offset - t.onset < 0.3)
                continue;
            for (double probeT : { 0.5 * (t.onset + t.offset), t.offset - 0.06 })
            {
                ++s.susPoints;
                for (const auto& d : det)
                    if (d.midi % 12 == t.midi % 12 && d.onset <= probeT && d.offset > probeT)
                    {
                        ++s.susCovered;
                        break;
                    }
            }
        }
        if (matchOfTruth != nullptr)
            *matchOfTruth = tm;
        if (detMatched != nullptr)
            *detMatched = used;
        return s;
    }

    std::string noteName(int m)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return std::string(names[m % 12]) + std::to_string(m / 12 - 1);
    }

    gtm::EngineSettings settingsFor(const Scenario& sc, int response)
    {
        gtm::EngineSettings s;
        s.mode = sc.mono ? gtm::EngineSettings::Mode::mono : gtm::EngineSettings::Mode::poly;
        s.response = response;
        s.midi.bendEnabled = sc.bends;
        return s;
    }

    void writeWav(const juce::File& file, const std::vector<float>& audio, double sr)
    {
        file.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os(file.createOutputStream());
        if (os == nullptr)
            return;
        auto opts = juce::AudioFormatWriterOptions{}.withSampleRate(sr).withNumChannels(1).withBitsPerSample(24);
        if (auto writer = wav.createWriterFor(os, opts))
        {
            const float* chans[] = { audio.data() };
            writer->writeFromFloatArrays(chans, 1, (int) audio.size());
        }
    }

    // --bench: CPU cost of the engine on a dense 60 s chord/lead passage.
    int bench(const juce::StringArray& args)
    {
        const double fs = args.contains("--48k") ? 48000.0 : 44100.0;
        Scenario s;
        double t = 0.1;
        while (t < 60.0)
        {
            strum(s, t, kChords[(size_t) ((int) (t * 3.0) % (int) kChords.size())].second, 0.25, 0.8, true, 0.01);
            t += 0.27;
        }
        GuitarSynth synth(fs, 3);
        for (auto& n : s.synth)
            synth.addNote(n);
        const auto audio = synth.render(60.0);
        for (auto mode : { gtm::EngineSettings::Mode::poly, gtm::EngineSettings::Mode::mono })
        {
            gtm::EngineSettings st;
            st.mode = mode;
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            runEngine(audio, fs, st);
            const double ms = juce::Time::getMillisecondCounterHiRes() - t0;
            std::cout << (mode == gtm::EngineSettings::Mode::poly ? "poly" : "mono") << ": " << std::fixed << std::setprecision(2)
                      << ms / 1000.0 << " s CPU for 60 s of audio = " << ms / 600.0 << "% of one core\n";
        }
        return 0;
    }

    int runSuite(const juce::StringArray& args)
    {
        const int verboseIdx = args.indexOf("--verbose");
        const juce::String verbose = verboseIdx >= 0 ? args[verboseIdx + 1] : juce::String();
        const int renderIdx = args.indexOf("--render");
        const juce::File renderDir = renderIdx >= 0 ? juce::File::getCurrentWorkingDirectory().getChildFile(args[renderIdx + 1]) : juce::File();
        const int respIdx = args.indexOf("--response");
        const int response = respIdx >= 0 ? args[respIdx + 1].getIntValue() : 0;

        const int seedIdx = args.indexOf("--seed");
        gSeed = seedIdx >= 0 ? (unsigned) args[seedIdx + 1].getIntValue() : 0u;
        gRng.seed(7 + gSeed);
        auto scenarios = buildScenarios();
        Score total;
        int totalBendMsgs = 0;

        std::cout << std::left << std::setw(30) << "scenario" << std::right
                  << std::setw(6) << "true" << std::setw(6) << "det" << std::setw(7) << "prec" << std::setw(7) << "recall"
                  << std::setw(7) << "F1" << std::setw(7) << "oct" << std::setw(7) << "wrong"
                  << std::setw(9) << "lat-med" << std::setw(9) << "lat-p90" << std::setw(7) << "pcR" << std::setw(7) << "susR" << "\n";

        const int onlyIdx = args.indexOf("--only");
        for (auto& sc : scenarios)
        {
            if (onlyIdx >= 0 && !juce::String(sc.name).contains(args[onlyIdx + 1]))
                continue;
            GuitarSynth synth(sc.sampleRate, 99 + gSeed * 1000);
            for (auto& n : sc.synth)
                synth.addNote(n);
            const auto audio = synth.render(sc.duration, sc.noiseDb, sc.humDb);

            if (renderDir != juce::File())
            {
                renderDir.createDirectory();
                writeWav(renderDir.getChildFile(juce::String(sc.name).replaceCharacter('/', '_') + ".wav"), audio, sc.sampleRate);
            }

            int bends = 0;
            const auto det = runEngine(audio, sc.sampleRate, settingsFor(sc, response), &bends);
            std::vector<int> tm, dm;
            const auto s = score(sc.truth, det, &tm, &dm);

            auto lat = s.latencies;
            std::sort(lat.begin(), lat.end());
            const double med = lat.empty() ? 0.0 : lat[lat.size() / 2] * 1000.0;
            const double p90 = lat.empty() ? 0.0 : lat[(size_t) std::min(lat.size() - 1, (size_t) (lat.size() * 0.9))] * 1000.0;
            const double prec = s.detected > 0 ? (double) s.matched / s.detected : (s.truth == 0 ? 1.0 : 0.0);
            const double rec = s.truth > 0 ? (double) s.matched / s.truth : 1.0;
            const double f1 = prec + rec > 0 ? 2 * prec * rec / (prec + rec) : 0.0;

            std::cout << std::left << std::setw(30) << sc.name << std::right << std::fixed << std::setprecision(2)
                      << std::setw(6) << s.truth << std::setw(6) << s.detected << std::setw(7) << prec << std::setw(7) << rec
                      << std::setw(7) << f1 << std::setw(7) << s.octave << std::setw(7) << s.wrong
                      << std::setprecision(0) << std::setw(9) << med << std::setw(9) << p90
                      << std::setprecision(2) << std::setw(7) << (s.truth > 0 ? (double) s.pcCovered / s.truth : 1.0)
                      << std::setw(7) << (s.susPoints > 0 ? (double) s.susCovered / s.susPoints : 1.0);
            if (sc.bends)
                std::cout << "   (bend msgs: " << bends << ")";
            std::cout << "\n";

            total.truth += s.truth;
            total.detected += s.detected;
            total.matched += s.matched;
            total.octave += s.octave;
            total.wrong += s.wrong;
            total.pcCovered += s.pcCovered;
            total.susPoints += s.susPoints;
            total.susCovered += s.susCovered;
            total.latencies.insert(total.latencies.end(), s.latencies.begin(), s.latencies.end());
            totalBendMsgs += bends;

            if (verbose.isNotEmpty() && juce::String(sc.name).contains(verbose))
            {
                std::cout << "   truth:\n";
                for (size_t i = 0; i < sc.truth.size(); ++i)
                {
                    const auto& t = sc.truth[i];
                    std::cout << "     " << std::setprecision(3) << t.onset << " " << noteName(t.midi) << " (" << t.midi << ")";
                    if (tm[i] >= 0)
                        std::cout << "  ok +" << std::setprecision(0) << (det[(size_t) tm[i]].onset - t.onset) * 1000 << "ms";
                    else
                        std::cout << "  MISSED";
                    std::cout << "\n";
                }
                std::cout << "   detected:\n";
                for (size_t j = 0; j < det.size(); ++j)
                    std::cout << "     " << std::setprecision(3) << det[j].onset << "-" << det[j].offset << " " << noteName(det[j].midi)
                              << " (" << det[j].midi << ") v" << det[j].velocity << (dm[j] >= 0 ? "" : "  EXTRA") << "\n";
            }
        }

        auto lat = total.latencies;
        std::sort(lat.begin(), lat.end());
        const double prec = total.detected > 0 ? (double) total.matched / total.detected : 0.0;
        const double rec = total.truth > 0 ? (double) total.matched / total.truth : 0.0;
        std::cout << std::string(95, '-') << "\n"
                  << std::left << std::setw(30) << "TOTAL" << std::right << std::fixed << std::setprecision(2)
                  << std::setw(6) << total.truth << std::setw(6) << total.detected << std::setw(7) << prec << std::setw(7) << rec
                  << std::setw(7) << (prec + rec > 0 ? 2 * prec * rec / (prec + rec) : 0.0) << std::setw(7) << total.octave
                  << std::setw(7) << total.wrong << std::setprecision(0)
                  << std::setw(9) << (lat.empty() ? 0.0 : lat[lat.size() / 2] * 1000)
                  << std::setw(9) << (lat.empty() ? 0.0 : lat[(size_t) (lat.size() * 0.9)] * 1000)
                  << std::setprecision(2) << std::setw(7) << (double) total.pcCovered / std::max(1, total.truth)
                  << std::setw(7) << (double) total.susCovered / std::max(1, total.susPoints) << "\n";
        return 0;
    }

    // --probe <midi...>: render a single pluck (or chord of the given notes) and
    // print what the poly detector sees for growing post-attack window lengths.
    int probe(const juce::StringArray& args)
    {
        const double fs = 44100.0;
        GuitarSynth synth(fs, 5);
        std::vector<int> midis;
        for (int i = args.indexOf("--probe") + 1; i < args.size() && !args[i].startsWith("--"); ++i)
            midis.push_back(args[i].getIntValue());
        const double onset = 0.3;
        for (size_t i = 0; i < midis.size(); ++i)
        {
            SynthNote n;
            n.onset = onset + 0.008 * (double) i;
            n.offset = onset + 1.5;
            n.midi = midis[i];
            synth.addNote(n);
        }
        const auto audio = synth.render(2.0);
        gtm::PolyPitchDetector det;
        det.prepare(fs, 4410, 440.0);
        std::array<gtm::PolyPitchDetector::Note, gtm::PolyPitchDetector::kMaxNotesOut> notes {};
        gtm::PolyPitchDetector::Params p;
        const int ri = args.indexOf("--rel");
        if (ri >= 0) p.relativeThreshold = args[ri + 1].getDoubleValue();
        const int start = (int) (onset * fs);
        for (double ms : { 10.0, 15.0, 20.0, 25.0, 30.0, 40.0, 50.0, 70.0, 100.0 })
        {
            const int len = (int) (ms * fs / 1000.0);
            det.analyse(audio.data() + start, len);
            if (args.contains("--spectrum") && ms == 100.0)
            {
                const auto& mg = det.getMagnitude();
                const auto& wh = det.getWhitened();
                for (int m = 1; m <= (midis.size() > 1 ? 0 : 12); ++m)
                {
                    const double f = midiToHz(midis[0]) * m;
                    const int k = (int) std::round(f / det.getBinHz());
                    float pm = 0, pw = 0;
                    for (int j = k - 3; j <= k + 3; ++j) { pm = std::max(pm, mg[(size_t) j]); pw = std::max(pw, wh[(size_t) j]); }
                    std::cout << "  partial " << m << " f=" << f << " mag=" << pm << " white=" << pw << "\n";
                }
                if (args.contains("--peaks"))
                {
                    const auto& wh = det.getWhitened();
                    const auto& mg = det.getMagnitude();
                    float mx = 0;
                    for (float v : wh) mx = std::max(mx, v);
                    for (size_t k = 2; k + 2 < mg.size() && k * det.getBinHz() < 2000; ++k)
                        if (mg[k] > mg[k - 1] && mg[k] >= mg[k + 1] && wh[k] > 0.05f * mx)
                        {
                            const double f = k * det.getBinHz();
                            std::string who;
                            for (int md : midis)
                                for (int h = 1; h <= 12; ++h)
                                    if (std::abs(f - midiToHz(md) * h) < det.getBinHz() * 1.5)
                                        who += noteName(md) + "x" + std::to_string(h) + " ";
                            std::cout << "    peak " << std::setw(7) << std::setprecision(1) << f << " w=" << std::setprecision(2) << wh[k] / mx << "  " << who << "\n";
                        }
                }
                for (int mm = 36; mm <= 80; ++mm)
                    std::cout << "  " << noteName(mm) << (std::find(midis.begin(), midis.end(), mm) != midis.end() ? "*" : " ") << " sal=" << std::setprecision(3) << det.residualSalience(midiToHz(mm)) << (mm % 6 == 5 ? "\n" : "");
                for (double m2 : { (double) midis[0], midis[0] + 12.0, 81.0, 68.0 })
                    std::cout << "  salience(" << m2 << ") = " << det.residualSalience(midiToHz(m2)) << "\n";
            }
            const int c = det.detect(p, notes.data());
            std::cout << std::setw(5) << ms << "ms:";
            for (int i = 0; i < c; ++i)
                std::cout << "  " << noteName(notes[(size_t) i].midi) << "(" << std::fixed << std::setprecision(2)
                          << notes[(size_t) i].salience / notes[0].salience << ",a=" << std::setprecision(4) << notes[(size_t) i].amplitude << ")";
            std::cout << "\n";
        }
        return 0;
    }

    int convertFile(const juce::StringArray& args)
    {
        const bool mono = args.contains("--mono");
        juce::StringArray files;
        for (auto& a : args)
            if (!a.startsWith("--"))
                files.add(a);
        const juce::File in = juce::File::getCurrentWorkingDirectory().getChildFile(files[0]);
        const juce::File outFile = files.size() > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile(files[1])
                                                    : in.withFileExtension("mid");

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(in));
        if (reader == nullptr)
        {
            std::cerr << "Can't read " << in.getFullPathName() << "\n";
            return 1;
        }
        juce::AudioBuffer<float> buf((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read(&buf, 0, (int) reader->lengthInSamples, 0, true, true);
        std::vector<float> monoAudio((size_t) buf.getNumSamples(), 0.0f);
        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
            for (int i = 0; i < buf.getNumSamples(); ++i)
                monoAudio[(size_t) i] += buf.getSample(ch, i) / (float) buf.getNumChannels();

        gtm::EngineSettings s;
        s.mode = mono ? gtm::EngineSettings::Mode::mono : gtm::EngineSettings::Mode::poly;
        const auto det = runEngine(monoAudio, reader->sampleRate, s);

        juce::MidiMessageSequence seq;
        const double ticksPerSecond = 960.0 * 2.0; // 120 bpm
        for (const auto& n : det)
        {
            seq.addEvent(juce::MidiMessage::noteOn(1, n.midi, (juce::uint8) n.velocity), n.onset * ticksPerSecond);
            seq.addEvent(juce::MidiMessage::noteOff(1, n.midi), n.offset * ticksPerSecond);
        }
        seq.updateMatchedPairs();
        juce::MidiFile mf;
        mf.setTicksPerQuarterNote(960);
        mf.addTrack(seq);
        outFile.deleteFile();
        juce::FileOutputStream os(outFile);
        mf.writeTo(os);
        std::cout << "Wrote " << det.size() << " notes to " << outFile.getFullPathName() << "\n";
        for (const auto& n : det)
            std::cout << std::fixed << std::setprecision(3) << n.onset << "-" << n.offset << "  " << noteName(n.midi) << "  v" << n.velocity << "\n";
        return 0;
    }
}

int main(int argc, char* argv[])
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add(argv[i]);

    for (int i = 0; i < args.size(); ++i)
        if (args[i] == "--set" && i + 1 < args.size())
        {
            const auto kv = juce::StringArray::fromTokens(args[i + 1], "=", "");
            if (!gtm::setEngineTuning(kv[0].toStdString(), kv[1].getDoubleValue()))
                std::cerr << "unknown tuning " << kv[0] << std::endl;
        }
    if (args.contains("--bench"))
        return bench(args);
    if (args.contains("--probe"))
        return probe(args);
    for (auto& a : args)
        if (a.endsWithIgnoreCase(".wav") || a.endsWithIgnoreCase(".aif") || a.endsWithIgnoreCase(".aiff") || a.endsWithIgnoreCase(".flac"))
            return convertFile(args);
    return runSuite(args);
}
