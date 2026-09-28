#include "GuitarMidiEngine.h"
#include <algorithm>
#include <cmath>

#ifdef GTM_ENGINE_TRACE
 #include <cstdio>
 #define GTM_TRACE(...) do { if (gtm::traceOn) { std::printf("  [%7.3f] ", (double) pos_ / sampleRate_); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)
namespace gtm { bool traceOn = false; }
#else
 #define GTM_TRACE(...) do {} while (0)
#endif

namespace gtm
{
    namespace
    {
        double kGatherSeconds = 0.12;   // how long an attack keeps accepting new chord notes
        double kMaxWindowSeconds = 0.1;  // longest analysis window used during an attack

        struct Timing
        {
            double polyCommitMin, polyCommitMax;
            int confirmAnalyses;
            double monoCommitMin, monoCommitMax;
        };

        // How soon after a pick attack notes may be committed. Notes commit as
        // soon as their estimate is stable once past the minimum, so these are
        // bounds, not fixed delays. polyCommitMax is how long an attack keeps
        // accepting additional (typically lower, slower-to-resolve) chord notes.
        Timing timingFor(int response)
        {
            switch (response)
            {
                case 2:  return { 0.030, kGatherSeconds + 0.01, 4, 0.022, 0.055 }; // accurate
                case 1:  return { 0.022, kGatherSeconds + 0.005, 3, 0.014, 0.040 }; // balanced
                default: return { 0.018, kGatherSeconds, 3, 0.008, 0.025 }; // fast
            }
        }

        double kGatherRelThreshold = 0.4;
        double kMutedRatio = 0.4;
        double kPreOnsetSeconds = 0.02;
        double kLowResolveSeconds = 0.06;
        double kUnexplainedMax = 0.35;
        double kMinPeriods = 3.5;
        double kHoldFloorDb = -24.0;
        double kLegatoSeconds = 0.025;
        double kRepickShare = 0.75;
        double kGhostRatio = 0.8;
        double kSmoothness = 0.8;
        double kRelAmplitude = 0.4;
        double kTrackRelThreshold = 0.1;

        double kPolyEvalMinSeconds = 0.012;
        double kStrumMergeSeconds = 0.035;   // attacks this close = one strum
        double kRefractorySeconds = 0.025;
        double kNoteOffSeconds = 0.040;      // how long a note must be missing
        double kRetriggerRatio = 1.2;        // energy jump that counts as a re-pick
        double kDecayFloorDb = -42.0;        // relative to the note's peak
        double kLateNoteSeconds = 0.060;     // notes that start with no detectable attack
    }

    bool setEngineTuning(const std::string& name, double value)
    {
        if (name == "kGatherRelThreshold") { kGatherRelThreshold = value; return true; }
        if (name == "kMutedRatio") { kMutedRatio = value; return true; }
        if (name == "kPreOnsetSeconds") { kPreOnsetSeconds = value; return true; }
        if (name == "kLowResolveSeconds") { kLowResolveSeconds = value; return true; }
        if (name == "kUnexplainedMax") { kUnexplainedMax = value; return true; }
        if (name == "kMinPeriods") { kMinPeriods = value; return true; }
        if (name == "kHoldFloorDb") { kHoldFloorDb = value; return true; }
        if (name == "kLegatoSeconds") { kLegatoSeconds = value; return true; }
        if (name == "kRepickShare") { kRepickShare = value; return true; }
        if (name == "kGatherSeconds") { kGatherSeconds = value; return true; }
        if (name == "kMaxWindowSeconds") { kMaxWindowSeconds = value; return true; }
        if (name == "kPolyEvalMinSeconds") { kPolyEvalMinSeconds = value; return true; }
        if (name == "kStrumMergeSeconds") { kStrumMergeSeconds = value; return true; }
        if (name == "kRefractorySeconds") { kRefractorySeconds = value; return true; }
        if (name == "kNoteOffSeconds") { kNoteOffSeconds = value; return true; }
        if (name == "kRetriggerRatio") { kRetriggerRatio = value; return true; }
        if (name == "kDecayFloorDb") { kDecayFloorDb = value; return true; }
        if (name == "kLateNoteSeconds") { kLateNoteSeconds = value; return true; }
        if (name == "kGhostRatio") { kGhostRatio = value; return true; }
        if (name == "kSmoothness") { kSmoothness = value; return true; }
        if (name == "kRelAmplitude") { kRelAmplitude = value; return true; }
        if (name == "kTrackRelThreshold") { kTrackRelThreshold = value; return true; }
#ifdef GTM_ENGINE_TRACE
        if (name == "trace") { traceOn = value != 0.0; return true; }
#endif
        return false;
    }

    void GuitarMidiEngine::prepare(double sampleRate)
    {
        sampleRate_ = sampleRate;
        hop_ = nextPowerOfTwo((int) std::lround(0.005 * sampleRate));
        onsetFrame_ = nextPowerOfTwo((int) std::lround(0.02 * sampleRate));
        trackLen_ = (int) std::lround(0.07 * sampleRate);
        maxGather_ = (int) std::lround(kMaxWindowSeconds * sampleRate);
        monoTrackLen_ = (int) std::lround(0.04 * sampleRate);

        const int ringSize = nextPowerOfTwo(trackLen_ + maxGather_ + onsetFrame_ + 8 * hop_);
        ring_.assign((size_t) ringSize, 0.0f);
        ringMask_ = ringSize - 1;
        seg_.assign((size_t) std::max({ trackLen_, maxGather_, onsetFrame_, monoTrackLen_ }) + 16, 0.0f);

        settings_ = pendingSettings_;
        onset_.prepare(sampleRate, onsetFrame_);
        poly_.prepare(sampleRate, std::max(trackLen_, maxGather_), settings_.tuningA4);
        yin_.prepare(sampleRate, midiToFreq(kMinGuitarMidi - 1.0), midiToFreq(kMaxGuitarMidi + 1.0),
                     std::max(monoTrackLen_, maxGather_));

        // 2nd-order Butterworth high-pass at 45 Hz: removes DC/rumble below the lowest string.
        const double w0 = 2.0 * 3.14159265358979323846 * 45.0 / sampleRate;
        const double cosw = std::cos(w0), alpha = std::sin(w0) / (2.0 * 0.70710678);
        const double a0 = 1.0 + alpha;
        hpB0_ = (1.0 + cosw) / 2.0 / a0;
        hpB1_ = -(1.0 + cosw) / a0;
        hpB2_ = hpB0_;
        hpA1_ = -2.0 * cosw / a0;
        hpA2_ = (1.0 - alpha) / a0;

        reset();
    }

    void GuitarMidiEngine::reset()
    {
        std::fill(ring_.begin(), ring_.end(), 0.0f);
        pos_ = 0;
        hopFill_ = 0;
        hopSumSq_ = 0.0;
        levelDb_ = -120.0f;
        gateCount_ = 0;
        lastOnset_ = -1000000;
        hpX1_ = hpX2_ = hpY1_ = hpY2_ = 0.0;
        onset_.reset();
        output_.reset();
        output_.setConfig(settings_.midi);
        for (auto& a : active_)
            a = {};
        gather_ = {};
        mono_ = {};
        lateCount_.fill(0);
        firstBlock_ = true;
    }

    void GuitarMidiEngine::allNotesOff(juce::MidiBuffer& out, int offset)
    {
        output_.allNotesOff(out, offset);
        for (auto& a : active_)
            a = {};
        gather_ = {};
        mono_ = {};
        lateCount_.fill(0);
    }

    void GuitarMidiEngine::applySettings(juce::MidiBuffer& out)
    {
        const auto& s = pendingSettings_;
        const bool modeChanged = s.mode != settings_.mode;
        if (output_.setConfig(s.midi) || modeChanged)
            allNotesOff(out, 0);
        if (s.tuningA4 != settings_.tuningA4)
            poly_.setTuning(s.tuningA4);
        settings_ = s;
        output_.emitSetupIfNeeded(out, 0);
    }

    void GuitarMidiEngine::process(const float* input, int numSamples, juce::MidiBuffer& out)
    {
        applySettings(out);

        for (int i = 0; i < numSamples; ++i)
        {
            const double x = input[i];
            const double y = hpB0_ * x + hpB1_ * hpX1_ + hpB2_ * hpX2_ - hpA1_ * hpY1_ - hpA2_ * hpY2_;
            hpX2_ = hpX1_; hpX1_ = x;
            hpY2_ = hpY1_; hpY1_ = y;

            ring_[(size_t) (pos_ & ringMask_)] = (float) y;
            ++pos_;
            hopSumSq_ += y * y;

            if (++hopFill_ >= hop_)
            {
                runHop(out, i);
                hopFill_ = 0;
                hopSumSq_ = 0.0;
            }
        }
    }

    void GuitarMidiEngine::readRange(int64_t start, int length, float* dest) const
    {
        for (int i = 0; i < length; ++i)
        {
            const int64_t p = start + i;
            dest[i] = p >= 0 ? ring_[(size_t) (p & ringMask_)] : 0.0f;
        }
    }

    double GuitarMidiEngine::peakSince(int64_t start) const
    {
        start = std::max(start, pos_ - (int64_t) ring_.size() + 1);
        float peak = 0.0f;
        for (int64_t p = std::max<int64_t>(0, start); p < pos_; ++p)
            peak = std::max(peak, std::abs(ring_[(size_t) (p & ringMask_)]));
        return peak;
    }

    int GuitarMidiEngine::velocityFromPeak(double peak) const
    {
        // -48 dBFS .. -8 dBFS peak maps onto velocity 20..127.
        const double x = std::clamp((gainToDb(peak) + 48.0) / 40.0, 0.0, 1.0);
        const double dynamic = 20.0 + 107.0 * std::pow(x, 0.8);
        const double v = 100.0 + (dynamic - 100.0) * std::clamp(settings_.velocitySensitivity, 0.0, 1.0);
        return std::clamp((int) std::lround(v), 1, 127);
    }

    // Refine the attack time to the 64-sample block with the sharpest energy
    // rise inside the onset frame, so post-attack analysis starts at the pick
    // and not a hop or two early/late.
    int64_t GuitarMidiEngine::locateOnset() const
    {
        const int block = std::max(32, hop_ / 4);
        const int numBlocks = onsetFrame_ / block;
        const int64_t base = pos_ - (int64_t) numBlocks * block;
        std::array<double, 64> e {};
        const int nb = std::min(numBlocks, 64);
        for (int b = 0; b < nb; ++b)
        {
            double sum = 0.0;
            for (int i = 0; i < block; ++i)
            {
                const int64_t p = base + (int64_t) b * block + i;
                const float v = p >= 0 ? ring_[(size_t) (p & ringMask_)] : 0.0f;
                sum += (double) v * v;
            }
            e[(size_t) b] = sum + 1.0e-12;
        }
        int best = -1;
        double bestRatio = 2.0;
        for (int b = 2; b < nb; ++b)
        {
            const double prev = 0.5 * (e[(size_t) b - 1] + e[(size_t) b - 2]);
            const double ratio = e[(size_t) b] / prev;
            if (ratio > bestRatio)
            {
                bestRatio = ratio;
                best = b;
            }
        }
        if (best < 0)
            return pos_ - onsetFrame_ / 2;
        return base + (int64_t) best * block;
    }

    GuitarMidiEngine::ActiveNote* GuitarMidiEngine::findActive(int midi)
    {
        for (auto& a : active_)
            if (a.used && a.midi == midi)
                return &a;
        return nullptr;
    }

    int GuitarMidiEngine::numActive() const
    {
        int n = 0;
        for (auto& a : active_)
            n += a.used ? 1 : 0;
        return n;
    }

    GuitarMidiEngine::ActiveNote* GuitarMidiEngine::startNote(int midi, double f0, int velocity, double amp,
                                                              juce::MidiBuffer& out, int offset)
    {
        ActiveNote* slot = nullptr;
        for (auto& a : active_)
            if (!a.used) { slot = &a; break; }
        if (slot == nullptr)
            return nullptr;

        GTM_TRACE("NOTE ON %d vel %d", midi, velocity);
        const int voice = output_.noteOn(midi, velocity, out, offset);
        if (voice < 0)
            return nullptr;
        *slot = {};
        slot->used = true;
        slot->midi = midi;
        slot->voice = voice;
        slot->f0 = f0;
        slot->amp = slot->peakAmp = slot->preOnsetAmp = amp;
        slot->velocity = velocity;
        slot->start = pos_;
        return slot;
    }

    void GuitarMidiEngine::stopNote(ActiveNote& n, juce::MidiBuffer& out, int offset)
    {
        if (!n.used)
            return;
        GTM_TRACE("note off %d", n.midi);
        output_.noteOff(n.voice, out, offset);
        n = {};
    }

    int GuitarMidiEngine::getActiveNotes(int* dest, int maxCount) const
    {
        int n = 0;
        for (auto& a : active_)
            if (a.used && n < maxCount)
                dest[n++] = a.midi;
        std::sort(dest, dest + n);
        return n;
    }

    void GuitarMidiEngine::runHop(juce::MidiBuffer& out, int offset)
    {
        levelDb_ = (float) gainToDb(std::sqrt(hopSumSq_ / hop_));

        readRange(pos_ - onsetFrame_, onsetFrame_, seg_.data());
        float odf = 0.0f;
        const bool rawOnset = onset_.process(seg_.data(), settings_.onsetSensitivity, odf);
        const bool onset = rawOnset && levelDb_ > settings_.gateDb
                           && (pos_ - lastOnset_) > (int64_t) (kRefractorySeconds * sampleRate_);
        int64_t onsetPos = pos_;
        if (onset)
        {
            onsetPos = locateOnset();
            lastOnset_ = pos_;
            GTM_TRACE("onset at %.3f (odf %.3f, level %.1f dB)", (double) onsetPos / sampleRate_, odf, levelDb_);
        }

        if (settings_.mode == EngineSettings::Mode::mono)
            monoHop(onset, onsetPos, out, offset);
        else
            polyHop(onset, onsetPos, out, offset);
    }

    // ------------------------------------------------------------------ poly

    void GuitarMidiEngine::polyHop(bool onset, int64_t onsetPos, juce::MidiBuffer& out, int offset)
    {
        const auto timing = timingFor(settings_.response);

        if (onset)
        {
            const bool mergeIntoStrum = gather_.active
                                        && (onsetPos - gather_.start) < (int64_t) (kStrumMergeSeconds * sampleRate_);
            if (!mergeIntoStrum)
            {
                GTM_TRACE("new attack window");
                if (gather_.active)
                    finishGather(out, offset);
                gather_ = {};
                gather_.active = true;
                gather_.start = onsetPos;

                // Level of each sounding note just before the attack, so a
                // re-pick of the same pitch can be told apart from a note that
                // simply keeps ringing.
                if (numActive() > 0)
                {
                    const int preLen = (int) (kPreOnsetSeconds * sampleRate_);
                    readRange(onsetPos - preLen, preLen, seg_.data());
                    poly_.analyse(seg_.data(), preLen);
                }
                for (auto& a : active_)
                    if (a.used)
                    {
                        a.preOnsetAmp = poly_.harmonicAmplitude(a.f0);
                        poly_.partialAmplitudes(a.f0, a.prePartials.data(), 3);
                        a.preExisting = true;
                    }
            }
        }

        if (!gather_.active)
        {
            polyTrack(out, offset);
            return;
        }

        const int len = (int) std::min<int64_t>(pos_ - gather_.start, maxGather_);
        if (len < (int) (kPolyEvalMinSeconds * sampleRate_))
            return;

        readRange(pos_ - len, len, seg_.data());
        poly_.analyse(seg_.data(), len);

        PolyPitchDetector::Params p;
        p.maxNotes = settings_.maxPolyphony;
        p.relativeThreshold = kGatherRelThreshold;
        p.minAmplitude = dbToGain(settings_.gateDb);
        p.harmonicGhostRatio = kGhostRatio;
        p.smoothness = kSmoothness;
        p.relativeAmplitude = kRelAmplitude;
        const int count = poly_.detect(p, notes_.data());

        // A pitch must be seen in several consecutive (growing) analysis
        // windows before it is committed: short-window artefacts flicker from
        // hop to hop, real notes persist. Clear single notes commit within a
        // few hops; the low strings of a chord fill in as the window grows.
        std::array<bool, 128> seen {};
        for (int i = 0; i < count; ++i)
            seen[(size_t) notes_[(size_t) i].midi] = true;
#ifdef GTM_ENGINE_TRACE
        if (traceOn)
        {
            std::printf("  [%7.3f] gather %4.0fms:", (double) pos_ / sampleRate_, 1000.0 * len / sampleRate_);
            for (int i = 0; i < count; ++i)
                std::printf(" %d(%.2f,u%.2f)", notes_[(size_t) i].midi, notes_[(size_t) i].salience / notes_[0].salience,
                            poly_.unexplainedBelow(notes_[(size_t) i].freqHz));
            std::printf("\n");
        }
#endif
        for (int m = 0; m < 128; ++m)
            gather_.streak[(size_t) m] = seen[(size_t) m] ? (juce::uint8) std::min(255, gather_.streak[(size_t) m] + 1) : 0;

        const double seconds = (double) len / sampleRate_;
        if (seconds >= timing.polyCommitMin)
        {
            const int velocity = velocityFromPeak(peakSince(gather_.start));
            bool committed = false;

            // An attack that brought no new pitch must have re-struck notes that
            // were already sounding (re-picked note, re-strummed chord). Retrigger
            // the ones whose level rose the most relative to just before.
            if (!gather_.repickDecided && count > 0)
            {
                bool allSounding = true, anyConfirmed = false;
                double maxRatio = 0.0;
                std::array<double, PolyPitchDetector::kMaxNotesOut> ratio {};
                for (int i = 0; i < count; ++i)
                {
                    const auto& n = notes_[(size_t) i];
                    const auto* a = findActive(n.midi);
                    if (a == nullptr || !a->preExisting)
                    {
                        allSounding = false;
                        break;
                    }
                    anyConfirmed |= gather_.streak[(size_t) n.midi] >= timing.confirmAnalyses;
                    ratio[(size_t) i] = n.amplitude / std::max(1.0e-9, a->preOnsetAmp);
                    maxRatio = std::max(maxRatio, ratio[(size_t) i]);
                }
                if (allSounding && anyConfirmed)
                {
                    gather_.repickDecided = true;
                    for (int i = 0; i < count; ++i)
                    {
                        const auto& n = notes_[(size_t) i];
                        if (ratio[(size_t) i] < kRepickShare * maxRatio || gather_.handled[(size_t) n.midi])
                            continue;
                        gather_.handled[(size_t) n.midi] = true;
                        if (auto* a = findActive(n.midi))
                        {
                            stopNote(*a, out, offset);
                            startNote(n.midi, n.freqHz, velocity, n.amplitude, out, offset);
                            committed = true;
                        }
                    }
                }
                else if (!allSounding)
                    gather_.repickDecided = true;
            }

            for (int i = 0; i < count; ++i)
            {
                const auto& n = notes_[(size_t) i];
                if (gather_.handled[(size_t) n.midi] || gather_.streak[(size_t) n.midi] < timing.confirmAnalyses)
                    continue;
                // Too few periods in the window to separate this note from a
                // neighbouring string (unresolved partials merge into a phantom
                // in-between peak)? Wait for more audio.
                if (seconds * n.freqHz < kMinPeriods)
                    continue;
                // Lower strings still unresolved? Then this may be one of their
                // partials; wait until the window is long enough to tell.
                if (seconds < kLowResolveSeconds && poly_.unexplainedBelow(n.freqHz) > kUnexplainedMax)
                    continue;
                gather_.handled[(size_t) n.midi] = true;
                commitNote(n, count, velocity, out, offset);
                committed = true;
            }
            if (committed && !gather_.mutedChecked)
            {
                releaseMutedNotes(out, offset);
                gather_.mutedChecked = true;
            }
        }

        if ((double) (pos_ - gather_.start) / sampleRate_ >= timing.polyCommitMax)
            finishGather(out, offset);
    }

    // Is frequency f (a partial of `ownMidi`) also a partial of another note
    // detected in the current analysis?
    bool GuitarMidiEngine::sharesPartial(double f, int ownMidi, int count) const
    {
        for (int i = 0; i < count; ++i)
        {
            const auto& o = notes_[(size_t) i];
            if (o.midi == ownMidi)
                continue;
            const double h = std::round(f / o.freqHz);
            if (h >= 1.0 && std::abs(f - h * o.freqHz) < 0.015 * f)
                return true;
        }
        return false;
    }

    void GuitarMidiEngine::commitNote(const PolyPitchDetector::Note& n, int count, int velocity, juce::MidiBuffer& out, int offset)
    {
        if (auto* a = findActive(n.midi))
        {
            // Same pitch already sounding: a re-pick only if EVERY one of its
            // low partials jumped. A newly plucked string that shares some of
            // its partials (octave, fifth) inflates those, but not all of them.
            bool repicked = a->preExisting;
            if (repicked)
            {
                std::array<double, 3> now {};
                poly_.partialAmplitudes(n.freqHz, now.data(), 3);
                int compared = 0;
                for (size_t m = 0; m < now.size(); ++m)
                {
                    if (a->prePartials[m] <= 0.02 * a->preOnsetAmp || sharesPartial(n.freqHz * (double) (m + 1), n.midi, count))
                        continue;
                    ++compared;
                    if (now[m] < kRetriggerRatio * a->prePartials[m])
                        repicked = false;
                }
                if (compared == 0)
                    repicked = n.amplitude > kRetriggerRatio * a->preOnsetAmp;
            }
            if (repicked)
            {
                stopNote(*a, out, offset);
                startNote(n.midi, n.freqHz, velocity, n.amplitude, out, offset);
            }
            else
            {
                a->f0 = n.freqHz;
                a->miss = 0;
            }
            return;
        }

        if (numActive() >= settings_.maxPolyphony)
        {
            // Make room: drop the oldest note that predates this attack.
            ActiveNote* oldest = nullptr;
            for (auto& a : active_)
                if (a.used && a.preExisting && (oldest == nullptr || a.start < oldest->start))
                    oldest = &a;
            if (oldest == nullptr)
                return;
            stopNote(*oldest, out, offset);
        }
        startNote(n.midi, n.freqHz, velocity, n.amplitude, out, offset);
    }

    // Notes that were sounding before this attack and haven't been re-detected:
    // keep them only if still ringing at close to their previous level (let-ring
    // arpeggios); otherwise the player muted them to change chord.
    void GuitarMidiEngine::releaseMutedNotes(juce::MidiBuffer& out, int offset)
    {
        for (auto& a : active_)
        {
            if (!a.used || !a.preExisting || gather_.handled[(size_t) a.midi])
                continue;
            if (poly_.harmonicAmplitude(a.f0) < kMutedRatio * a.preOnsetAmp)
                stopNote(a, out, offset);
        }
    }

    void GuitarMidiEngine::finishGather(juce::MidiBuffer& out, int offset)
    {
        releaseMutedNotes(out, offset);
        for (auto& a : active_)
            a.preExisting = false;
        gather_.active = false;
        lateCount_.fill(0);
    }

    void GuitarMidiEngine::polyTrack(juce::MidiBuffer& out, int offset)
    {
        // Gate: everything off once the input has been quiet for a moment.
        if (levelDb_ < settings_.gateDb - 6.0)
        {
            if (++gateCount_ * hop_ >= (int) (kNoteOffSeconds * sampleRate_))
            {
                for (auto& a : active_)
                    stopNote(a, out, offset);
                lateCount_.fill(0);
                return;
            }
        }
        else
            gateCount_ = 0;

        const int len = (int) std::min<int64_t>(pos_, trackLen_);
        const bool quiet = numActive() == 0 && levelDb_ < settings_.gateDb;
        if (quiet && pos_ - lastOnset_ < (int64_t) (0.5 * sampleRate_))
            return; // too close to recent playing to be sure it's just background

        readRange(pos_ - len, len, seg_.data());
        poly_.analyse(seg_.data(), len);
        if (quiet)
        {
            // Nothing is being played: learn the hum/buzz spectrum instead.
            if (++quietHops_ % 4 == 0)
                poly_.learnNoiseFloor();
            return;
        }

        PolyPitchDetector::Params p;
        p.maxNotes = std::min(PolyPitchDetector::kMaxNotesOut - 2, settings_.maxPolyphony + 2);
        p.relativeThreshold = kTrackRelThreshold;
        p.minAmplitude = dbToGain(settings_.gateDb - 12.0);
        p.harmonicGhostRatio = kGhostRatio;
        p.smoothness = kSmoothness;
        p.relativeAmplitude = kRelAmplitude * 0.5;
        const int count = poly_.detect(p, notes_.data());

        const int offHops = std::max(2, (int) std::ceil(kNoteOffSeconds * sampleRate_ / hop_));
        const int64_t grace = trackLen_ / 2;
        const double decayFloor = dbToGain(kDecayFloorDb);
        const bool bends = settings_.midi.bendEnabled && settings_.midi.mpe;
        bool anyActiveMissing = false;

        for (auto& a : active_)
        {
            if (!a.used)
                continue;
            const PolyPitchDetector::Note* match = nullptr;
            for (int i = 0; i < count; ++i)
                if (std::abs(freqToMidi(notes_[(size_t) i].freqHz, settings_.tuningA4) - a.midi) < 0.6)
                {
                    match = &notes_[(size_t) i];
                    break;
                }

            if (match != nullptr)
            {
                a.miss = 0;
                a.f0 = match->freqHz;
                a.amp = match->amplitude;
            }
            else
            {
                anyActiveMissing = true;
                // Not picked out by the detector -- often because it's an octave
                // of a lower sounding note whose partials it shares. Keep it as
                // long as its own fundamental is still clearly there.
                a.amp = poly_.harmonicAmplitude(a.f0);
                const bool stillThere = poly_.hasRealFundamental(a.f0) && a.amp > a.peakAmp * dbToGain(kHoldFloorDb);
                if (stillThere)
                    a.miss = 0;
                else if (pos_ - a.start > grace)
                    ++a.miss;
            }
            a.peakAmp = std::max(a.peakAmp, a.amp);

            if (a.miss >= offHops || (pos_ - a.start > grace && a.amp < a.peakAmp * decayFloor))
            {
                stopNote(a, out, offset);
                continue;
            }
            if (bends)
                output_.pitchBend(a.voice, freqToMidi(a.f0, settings_.tuningA4) - a.midi, out, offset);
        }

        // Fallback for notes that begin without a detectable pick attack
        // (hammer-ons, slides, very soft playing): must persist for a while
        // and not be an overtone of something already sounding.
        const int lateHops = std::max(3, (int) std::ceil(kLateNoteSeconds * sampleRate_ / hop_));
        const int legatoHops = std::max(2, (int) std::ceil(kLegatoSeconds * sampleRate_ / hop_));
        std::array<bool, 128> seen {};
        for (int i = 0; i < count; ++i)
        {
            const auto& n = notes_[(size_t) i];
            if (findActive(n.midi) != nullptr || n.salience < 0.6 * notes_[0].salience
                || n.amplitude < dbToGain(settings_.gateDb + 6.0))
                continue;
            bool overtone = false;
            for (auto& a : active_)
                if (a.used)
                {
                    // Overtone of a sounding note, or a sub-harmonic "ghost" below one.
                    const double r = n.freqHz > a.f0 ? n.freqHz / a.f0 : a.f0 / n.freqHz;
                    const double h = std::round(r);
                    overtone |= h >= 2.0 && std::abs(r - h) / h < 0.015;
                }
            if (overtone)
                continue;
            seen[(size_t) n.midi] = true;

            // The strongest pitch changed while a sounding note faded: a legato
            // move on one string (hammer-on, pull-off, slide). Switch quickly.
            const bool legato = anyActiveMissing && i == 0 && numActive() == 1;
            if (++lateCount_[(size_t) n.midi] >= (legato ? legatoHops : lateHops))
            {
                int vel = std::max(1, (int) std::lround(velocityFromPeak(peakSince(pos_ - hop_)) * 0.8));
                if (legato)
                    for (auto& a : active_)
                        if (a.used)
                        {
                            vel = std::max(1, (int) std::lround(a.velocity * 0.85));
                            stopNote(a, out, offset);
                        }
                if (numActive() < settings_.maxPolyphony)
                    startNote(n.midi, n.freqHz, vel, n.amplitude, out, offset);
                lateCount_[(size_t) n.midi] = 0;
            }
        }
        for (int m = 0; m < 128; ++m)
            if (!seen[(size_t) m])
                lateCount_[(size_t) m] = 0;
    }

    // ------------------------------------------------------------------ mono

    void GuitarMidiEngine::monoHop(bool onset, int64_t onsetPos, juce::MidiBuffer& out, int offset)
    {
        const auto timing = timingFor(settings_.response);
        const double a4 = settings_.tuningA4;
        ActiveNote* current = nullptr;
        for (auto& a : active_)
            if (a.used) { current = &a; break; }

        if (onset)
        {
            mono_.attack = true;
            mono_.t0 = onsetPos;
            mono_.stable = 0;
            mono_.lastEst = -1.0;
        }

        if (mono_.attack)
        {
            const int len = (int) std::min<int64_t>(pos_ - mono_.t0, maxGather_);
            const double seconds = (double) (pos_ - mono_.t0) / sampleRate_;
            if (len >= yin_.minimumUsefulLength())
            {
                readRange(pos_ - len, len, seg_.data());
                const auto r = yin_.process(seg_.data(), len, 0.15);
                if (r.found && r.clarity >= 0.7)
                {
                    const double est = freqToMidi(r.frequencyHz, a4);
                    mono_.stable = (mono_.lastEst > 0.0 && std::abs(est - mono_.lastEst) < 0.4) ? mono_.stable + 1 : 0;
                    mono_.lastEst = est;
                }
                else
                    mono_.stable = 0;
            }

            const bool ready = (seconds >= timing.monoCommitMin && mono_.stable >= 1)
                               || (seconds >= timing.monoCommitMax && mono_.lastEst > 0.0);
            if (ready)
            {
                const int midi = (int) std::lround(mono_.lastEst);
                if (current != nullptr)
                    stopNote(*current, out, offset);
                if (midi >= (int) kMinGuitarMidi - 1 && midi <= (int) kMaxGuitarMidi + 1)
                {
                    startNote(midi, midiToFreq(mono_.lastEst, a4), velocityFromPeak(peakSince(mono_.t0)), 0.0, out, offset);
                    mono_.noteStart = mono_.t0;
                }
                mono_.attack = false;
                mono_.recent = { mono_.lastEst, mono_.lastEst, mono_.lastEst };
                mono_.legatoCount = 0;
                mono_.lateCount = 0;
            }
            else if (seconds >= timing.monoCommitMax * 2.0)
            {
                // Attack with no pitch (muted pick / scratch): it still silences the string.
                if (current != nullptr)
                    stopNote(*current, out, offset);
                mono_.attack = false;
            }
            return;
        }

        const int len = (int) std::min<int64_t>(current != nullptr ? pos_ - mono_.noteStart : pos_, monoTrackLen_);
        YinPitchDetector::Result r;
        if (len >= yin_.minimumUsefulLength() && levelDb_ > settings_.gateDb - 6.0)
        {
            readRange(pos_ - len, len, seg_.data());
            r = yin_.process(seg_.data(), len, 0.15);
        }

        if (current == nullptr)
        {
            // Note that started with no detectable attack.
            if (r.found && r.clarity >= 0.85 && levelDb_ > settings_.gateDb)
            {
                const int target = freqToNearestMidi(r.frequencyHz, a4);
                mono_.lateCount = target == mono_.lateTarget ? mono_.lateCount + 1 : 1;
                mono_.lateTarget = target;
                if (mono_.lateCount >= 4)
                {
                    startNote(target, r.frequencyHz, velocityFromPeak(peakSince(pos_ - hop_)), 0.0, out, offset);
                    mono_.noteStart = pos_ - len;
                    mono_.recent.fill(freqToMidi(r.frequencyHz, a4));
                    mono_.lateCount = 0;
                }
            }
            else
                mono_.lateCount = 0;
            return;
        }

        const int offHops = std::max(2, (int) std::ceil(kNoteOffSeconds * sampleRate_ / hop_));
        if (!(r.found && r.clarity >= 0.55))
        {
            if (++current->miss >= offHops)
                stopNote(*current, out, offset);
            return;
        }
        current->miss = 0;

        // Median of the last three estimates removes one-hop octave blips.
        const double raw = freqToMidi(r.frequencyHz, a4);
        const double prev = mono_.recent[2];
        mono_.recent = { mono_.recent[1], mono_.recent[2], raw };
        auto sorted = mono_.recent;
        std::sort(sorted.begin(), sorted.end());
        const double est = sorted[1];
        const double dev = est - current->midi;
        const bool bends = settings_.midi.bendEnabled;

        // Pitch moved to a different note: a jump (hammer-on / pull-off / slide
        // arrival) rather than a smooth bend, or bends are disabled.
        const bool jumped = std::abs(raw - prev) > 0.45;
        bool wantsNewNote;
        if (bends)
            wantsNewNote = mono_.legatoCount > 0 ? std::abs(est - mono_.legatoTarget) < 0.5
                                                 : (jumped && std::abs(dev) >= 0.6) || std::abs(dev) > settings_.midi.bendRange + 0.5;
        else
            wantsNewNote = std::abs(dev) >= 0.6;

        if (wantsNewNote)
        {
            const int target = (int) std::lround(est);
            if (target != mono_.legatoTarget)
            {
                mono_.legatoTarget = target;
                mono_.legatoCount = 0;
            }
            ++mono_.legatoCount;
            const int needed = std::abs(target - current->midi) == 12 ? 6 : (bends ? 2 : 3);
            if (mono_.legatoCount >= needed && target != current->midi)
            {
                const int vel = std::max(1, (int) std::lround(current->velocity * 0.85));
                stopNote(*current, out, offset);
                startNote(target, midiToFreq(est, a4), vel, 0.0, out, offset);
                mono_.legatoCount = 0;
                mono_.legatoTarget = -1;
            }
            return;
        }

        mono_.legatoCount = 0;
        mono_.legatoTarget = -1;
        current->f0 = midiToFreq(est, a4);
        if (bends)
            output_.pitchBend(current->voice, dev, out, offset);
    }
}
