#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <array>
#include <memory>
#include <algorithm>
#include <cmath>
#include "MusicMath.h"

namespace gtm
{
    // Multi-pitch (chord) estimator after Klapuri, "Multiple Fundamental
    // Frequency Estimation by Summing Harmonic Amplitudes" (ISMIR 2006), with
    // the spectral-smoothness subtraction from Klapuri (2003).
    //
    //  1. Window (any length, zero-padded) -> magnitude spectrum X.
    //  2. Band-wise whitening: Y = X * sigma_b^(nu-1) over ~30 critical bands,
    //     so pickup tone / string brightness doesn't bias the search.
    //  3. Salience of each candidate F0 (1/8-semitone grid) = sum over partials of
    //     g(f0,m) * max(Y in a small band around m*f0). g() favours low partials
    //     and suppresses sub-octave candidates; the band absorbs string
    //     inharmonicity.
    //  4. Repeatedly take the best candidate, refine its F0 from the actual
    //     partial peaks, then remove its partials from the residual -- but only
    //     down to a smooth spectral envelope, so a partial that is shared with
    //     another chord note keeps the other note's share.
    class PolyPitchDetector
    {
    public:
        struct Note
        {
            int midi = 0;
            double freqHz = 0.0;
            double salience = 0.0;
            double amplitude = 0.0; // raw partial amplitude (sine-amplitude units)
        };

        struct Params
        {
            int maxNotes = 6;
            double relativeThreshold = 0.25; // vs. strongest note's salience
            double minAmplitude = 0.002;     // absolute partial amplitude floor
            double harmonicGhostRatio = 0.8; // extra evidence needed for a note sitting on another note's partial
            double smoothness = 1.3;         // subtraction cap vs. local partial-envelope mean
            double relativeAmplitude = 0.2;  // raw partial amplitude vs. the strongest note's
        };

        static constexpr int kMaxNotesOut = 10;

        void prepare(double sampleRate, int maxLength, double a4)
        {
            sampleRate_ = sampleRate;
            maxLength_ = maxLength;
            fftSize_ = nextPowerOfTwo(maxLength * 2);
            int order = 0;
            while ((1 << order) < fftSize_)
                ++order;
            fft_ = std::make_unique<juce::dsp::FFT>(order);
            numBins_ = fftSize_ / 2;
            binHz_ = sampleRate / fftSize_;

            fftData_.assign((size_t) fftSize_ * 2, 0.0f);
            window_.assign((size_t) maxLength, 0.0f);
            mag_.assign((size_t) numBins_, 0.0f);
            rawMag_.assign((size_t) numBins_, 0.0f);
            white_.assign((size_t) numBins_, 0.0f);
            residual_.assign((size_t) numBins_, 0.0f);
            peaks_.assign((size_t) numBins_, 0.0f);
            noiseFloor_.assign((size_t) numBins_, 0.0f);
            humPeaks_.clear();
            humPeaks_.reserve(kMaxHumPeaks);
            gamma_.assign((size_t) numBins_, 0.0f);

            numCandidates_ = (int) std::round((kMaxGuitarMidi - kMinGuitarMidi + 1.0) * kStepsPerSemitone) + 1;
            candFreq_.assign((size_t) numCandidates_, 0.0);
            candNumPartials_.assign((size_t) numCandidates_, 0);
            partialLo_.assign((size_t) numCandidates_ * kMaxPartials, 0);
            partialHi_.assign((size_t) numCandidates_ * kMaxPartials, 0);
            partialWeight_.assign((size_t) numCandidates_ * kMaxPartials, 0.0f);
            sal_.assign((size_t) numCandidates_, 0.0);
            banned_.assign((size_t) numCandidates_, false);

            buildBands();
            a4_ = -1.0;
            setTuning(a4);
        }

        void setTuning(double a4)
        {
            if (a4 == a4_)
                return;
            a4_ = a4;
            const double fMax = std::min(kMaxPartialHz, 0.45 * sampleRate_);
            for (int c = 0; c < numCandidates_; ++c)
            {
                const double midi = kMinGuitarMidi - 0.5 + (double) c / kStepsPerSemitone;
                const double f0 = midiToFreq(midi, a4);
                candFreq_[(size_t) c] = f0;
                int n = 0;
                for (int m = 1; m <= kMaxPartials && m * f0 < fMax; ++m, ++n)
                {
                    const double fm = m * f0;
                    const double tol = fm * (kGridTolerance + kInharmonicity * m * m) + 0.5 * binHz_;
                    const size_t idx = (size_t) c * kMaxPartials + (size_t) (m - 1);
                    partialLo_[idx] = std::max(1, (int) std::floor((fm - tol) / binHz_));
                    partialHi_[idx] = std::min(numBins_ - 1, (int) std::ceil((fm + tol) / binHz_));
                    partialWeight_[idx] = (float) ((f0 + kAlpha) / (fm + kBeta));
                }
                candNumPartials_[(size_t) c] = n;
            }
        }

        // Analyse `length` samples (any length up to the prepared maximum;
        // zero-padded to a fixed FFT size so all analyses share one bin grid).
        void analyse(const float* x, int length)
        {
            length = std::min(length, maxLength_);
            analysedLength_ = length;
            buildWindow(length);

            double wsum = 0.0;
            std::fill(fftData_.begin(), fftData_.end(), 0.0f);
            for (int i = 0; i < length; ++i)
            {
                fftData_[(size_t) i] = x[i] * window_[(size_t) i];
                wsum += window_[(size_t) i];
            }
            fft_->performRealOnlyForwardTransform(fftData_.data(), true);

            const float norm = (float) (2.0 / std::max(1.0, wsum));
            for (int k = 0; k < numBins_; ++k)
            {
                const float re = fftData_[(size_t) 2 * k], im = fftData_[(size_t) 2 * k + 1];
                mag_[(size_t) k] = std::sqrt(re * re + im * im) * norm;
            }
            removeHum(length);
            whiten();
            buildPeakSpectrum();
            std::copy(peaks_.begin(), peaks_.end(), residual_.begin());
        }

        // Iterative detection on the last analysed spectrum. Returns count written to out.
        int detect(const Params& p, Note* out)
        {
            std::copy(peaks_.begin(), peaks_.end(), residual_.begin());
            int count = 0;
            double firstSalience = 0.0, firstAmplitude = 0.0;
            const int maxIterations = std::min(kMaxNotesOut, p.maxNotes + 4);
            float peakMax = 0.0f;
            for (float v : residual_)
                peakMax = std::max(peakMax, v);
            const float presenceFloor = std::max(1.0e-9f, peakMax * (float) kPresenceFraction);
            std::fill(banned_.begin(), banned_.end(), false);

            for (int it = 0; it < maxIterations && count < p.maxNotes; ++it)
            {
                double maxS = 0.0;
                for (int c = 0; c < numCandidates_; ++c)
                {
                    sal_[(size_t) c] = salience(residual_, c, presenceFloor);
                    maxS = std::max(maxS, sal_[(size_t) c]);
                }
                if (maxS <= 0.0)
                    break;
                if (it == 0)
                    firstSalience = maxS;
                if (maxS < p.relativeThreshold * firstSalience)
                    break;

                // Bottom-up: take the LOWEST clear salience peak rather than the
                // global maximum. In guitar voicings the upper notes are mostly
                // overtones of the lower ones (E2 -> E3, B3, E4, G#4...), so a
                // top-down greedy search lets high notes steal the low strings'
                // partials. Lower notes first explains overtones before they can
                // masquerade as extra notes.
                int best = -1;
                for (int c = 0; c < numCandidates_ && best < 0; ++c)
                {
                    const double s = sal_[(size_t) c];
                    if (banned_[(size_t) c] || s < kBottomUpFraction * maxS)
                        continue;
                    bool isPeak = true;
                    for (int d = -kPeakHalfWidth; d <= kPeakHalfWidth && isPeak; ++d)
                    {
                        const int j = c + d;
                        if (d != 0 && j >= 0 && j < numCandidates_ && sal_[(size_t) j] > s)
                            isPeak = false;
                    }
                    if (isPeak)
                        best = c;
                }
                if (best < 0)
                    break;
                const double bestS = sal_[(size_t) best];

                const double f0 = refineF0(candFreq_[(size_t) best]);
                const int midi = (int) std::lround(freqToMidi(f0, a4_));
                const double amp = harmonicAmplitude(f0);

                // The fundamental must be really there in the raw spectrum, not
                // just in the whitened one (whitening lifts the near-empty bass
                // band, so hum/noise can pose as the fundamental of a
                // sub-octave "ghost" whose even partials are a real note's).
                if (!hasRealFundamental(f0))
                {
                    for (int d = -kPeakHalfWidth; d <= kPeakHalfWidth; ++d)
                        if (best + d >= 0 && best + d < numCandidates_)
                            banned_[(size_t) (best + d)] = true;
                    continue;
                }

                firstAmplitude = std::max(firstAmplitude, amp);
                bool accept = amp >= p.minAmplitude && amp >= p.relativeAmplitude * firstAmplitude
                              && midi >= (int) kMinGuitarMidi && midi <= (int) kMaxGuitarMidi;

                for (int i = 0; i < count && accept; ++i)
                {
                    if (std::abs(out[i].midi - midi) <= 1)
                        accept = false; // duplicate / semitone neighbour of a stronger note
                    else if (out[i].freqHz < f0)
                    {
                        // Does this candidate sit exactly on a partial of an
                        // already-found lower note? Then it needs real evidence
                        // of its own to count as a separate string.
                        const double ratio = f0 / out[i].freqHz;
                        const double h = std::round(ratio);
                        if (h >= 2.0 && h <= 12.0 && std::abs(ratio - h) / h < 0.012 + 0.0004 * h
                            && bestS < p.harmonicGhostRatio * out[i].salience)
                            accept = false;
                    }
                }

                if (accept)
                {
                    Note& n = out[count++];
                    n.midi = midi;
                    n.freqHz = f0;
                    n.salience = bestS;
                    n.amplitude = amp;
                }

                subtract(f0, p.smoothness);
            }
            return count;
        }

        // Raw amplitudes of partials 1..n of f0 (0 where there is no peak).
        void partialAmplitudes(double f0, double* dest, int n) const
        {
            for (int m = 1; m <= n; ++m)
            {
                const int k = peakBin(mag_, m * f0, 0.012);
                dest[m - 1] = k > 0 ? mag_[(size_t) k] : 0.0;
            }
        }

        // Call after analyse() on audio known to contain no playing (the input is
        // below the gate): learns the tonal background -- mains hum and its
        // harmonics, buzz -- so it can be removed from later analyses.
        void learnNoiseFloor()
        {
            for (int k = 0; k < numBins_; ++k)
                noiseFloor_[(size_t) k] = std::max(noiseFloor_[(size_t) k] * kNoiseFloorDecay, rawMag_[(size_t) k]);

            humPeaks_.clear();
            float mx = 0.0f;
            for (float v : noiseFloor_)
                mx = std::max(mx, v);
            if (mx <= 0.0f)
                return;
            for (int k = 2; k < numBins_ - 2 && (int) humPeaks_.size() < kMaxHumPeaks; ++k)
            {
                const float v = noiseFloor_[(size_t) k];
                if (v > noiseFloor_[(size_t) k - 1] && v >= noiseFloor_[(size_t) k + 1] && v > 0.05f * mx
                    && k * binHz_ < kMaxPartialHz)
                    humPeaks_.push_back({ k, v });
            }
        }

        void resetNoiseFloor()
        {
            std::fill(noiseFloor_.begin(), noiseFloor_.end(), 0.0f);
            humPeaks_.clear();
        }

        // Summed amplitude of the first few partials of f0 in the raw spectrum.
        double harmonicAmplitude(double f0) const
        {
            double sum = 0.0;
            for (int m = 1; m <= 4; ++m)
            {
                const int k = peakBin(mag_, m * f0, 0.012);
                if (k > 0)
                    sum += mag_[(size_t) k];
            }
            return sum;
        }

        // Strongest spectral peak below `freqHz` that the last detect() call did
        // NOT attribute to a detected note, relative to the strongest peak
        // overall. A high value means lower strings are sounding that haven't
        // been resolved yet, so a higher "note" may just be one of their partials.
        double unexplainedBelow(double freqHz) const
        {
            float total = 0.0f, below = 0.0f;
            const int limit = std::min(numBins_ - 1, (int) (freqHz * 0.94 / binHz_));
            const int start = (int) (kLowestAnalysisHz / binHz_);
            for (int k = start; k < numBins_; ++k)
            {
                total = std::max(total, peaks_[(size_t) k]);
                if (k <= limit)
                    below = std::max(below, residual_[(size_t) k]);
            }
            return total > 0.0f ? below / total : 0.0;
        }

        // Salience of f0 in what is left after the last detect() call.
        double residualSalience(double f0) const
        {
            const double midi = freqToMidi(f0, a4_);
            const int c = std::clamp((int) std::lround((midi - (kMinGuitarMidi - 0.5)) * kStepsPerSemitone), 0, numCandidates_ - 1);
            return salience(residual_, c);
        }

        int getAnalysedLength() const { return analysedLength_; }
        const std::vector<float>& getMagnitude() const { return mag_; }
        const std::vector<float>& getWhitened() const { return white_; }
        double getBinHz() const { return binHz_; }

    private:
        static constexpr int kMaxPartials = 12;
        static constexpr double kStepsPerSemitone = 8.0;
        static constexpr double kMaxPartialHz = 5000.0;
        static constexpr double kGridTolerance = 0.0045;
        static constexpr double kInharmonicity = 0.00003;
        static constexpr double kAlpha = 52.0;
        static constexpr double kBeta = 320.0;
        static constexpr int kNumBands = 30;
        static constexpr double kNu = 0.33;
        static constexpr double kPeakSpread = 0.12; // in native (un-padded) bins
        static constexpr double kPresenceFraction = 0.06;
        static constexpr double kLowestAnalysisHz = 55.0;
        static constexpr float kNoiseFloorDecay = 0.998f;
        static constexpr float kHumOverSubtract = 1.5f;
        static constexpr int kMaxHumPeaks = 64;
        static constexpr double kMinFundamentalRatio = 0.06; // -24 dB vs strongest low partial
        static constexpr double kFundamentalFraction = 0.03;
        static constexpr double kBottomUpFraction = 0.55;
        static constexpr int kPeakHalfWidth = 4;

        // presenceFloor > 0 enables the low-partial gate: a real guitar note
        // always shows at least two of its first three partials, whereas a
        // phantom candidate assembled from other notes' upper partials doesn't.
        double salience(const std::vector<float>& spec, int c, float presenceFloor = 0.0f) const
        {
            const size_t base = (size_t) c * kMaxPartials;
            const int n = candNumPartials_[(size_t) c];
            double s = 0.0;
            int lowPresent = 0;
            bool fundamental = false;
            for (int m = 0; m < n; ++m)
            {
                const int lo = partialLo_[base + (size_t) m], hi = partialHi_[base + (size_t) m];
                float mx = 0.0f;
                for (int k = lo; k <= hi; ++k)
                    mx = std::max(mx, spec[(size_t) k]);
                if (m == 0)
                    fundamental = mx > presenceFloor * (float) (kFundamentalFraction / kPresenceFraction);
                else if (m < 5 && mx > presenceFloor)
                    ++lowPresent;
                s += partialWeight_[base + (size_t) m] * mx;
            }
            if (presenceFloor > 0.0f && !(fundamental && lowPresent >= std::min(2, n - 1)))
                return 0.0;
            return s;
        }

        int peakBin(const std::vector<float>& spec, double freq, double relTol) const
        {
            const int lo = std::max(1, (int) std::floor(freq * (1.0 - relTol) / binHz_ - 1.0));
            const int hi = std::min(numBins_ - 2, (int) std::ceil(freq * (1.0 + relTol) / binHz_ + 1.0));
            if (lo > hi)
                return -1;
            int best = lo;
            for (int k = lo + 1; k <= hi; ++k)
                if (spec[(size_t) k] > spec[(size_t) best])
                    best = k;
            return best;
        }

    public:
        bool hasRealFundamental(double f0) const
        {
            double a[4] {};
            partialAmplitudes(f0, a, 4);
            const double strongest = std::max({ a[0], a[1], a[2], a[3] });
            return a[0] >= kMinFundamentalRatio * strongest;
        }

    private:

        // Precise F0 from the interpolated peaks of the lowest few partials.
        double refineF0(double coarse) const
        {
            double num = 0.0, den = 0.0;
            for (int m = 1; m <= 6; ++m)
            {
                const double fm = coarse * m;
                if (fm > 0.45 * sampleRate_)
                    break;
                const int k = peakBin(mag_, fm, 0.02);
                if (k <= 1 || k >= numBins_ - 2)
                    continue;
                const double y0 = std::log(mag_[(size_t) k - 1] + 1e-12);
                const double y1 = std::log(mag_[(size_t) k] + 1e-12);
                const double y2 = std::log(mag_[(size_t) k + 1] + 1e-12);
                if (!(y1 >= y0 && y1 >= y2))
                    continue;
                const double denom = y0 - 2.0 * y1 + y2;
                const double delta = denom < 0.0 ? std::clamp(0.5 * (y0 - y2) / denom, -0.5, 0.5) : 0.0;
                const double est = (k + delta) * binHz_ / m;
                if (std::abs(est - coarse) / coarse > 0.02)
                    continue;
                const double w = mag_[(size_t) k] * std::sqrt((double) m);
                num += w * est;
                den += w;
            }
            return den > 0.0 ? num / den : coarse;
        }

        void subtract(double f0, double smoothness)
        {
            const double fMax = std::min(kMaxPartialHz, 0.45 * sampleRate_);
            std::array<int, kMaxPartials> bins {};
            std::array<float, kMaxPartials> amps {};
            int n = 0;
            for (int m = 1; m <= kMaxPartials && m * f0 < fMax; ++m, ++n)
            {
                const double tol = kGridTolerance + kInharmonicity * m * m;
                bins[(size_t) n] = peakBin(residual_, m * f0, tol);
                amps[(size_t) n] = bins[(size_t) n] > 0 ? residual_[(size_t) bins[(size_t) n]] : 0.0f;
            }

            const int lobe = peakSpread_ + 1;

            for (int i = 0; i < n; ++i)
            {
                const float a = amps[(size_t) i];
                if (a <= 0.0f || bins[(size_t) i] <= 0)
                    continue;
                float localMean = a;
                int cnt = 1;
                if (i > 0) { localMean += amps[(size_t) i - 1]; ++cnt; }
                if (i + 1 < n) { localMean += amps[(size_t) i + 1]; ++cnt; }
                localMean /= (float) cnt;
                const float removed = std::min(a, (float) smoothness * localMean);
                const float keep = 1.0f - removed / a;
                const int c = bins[(size_t) i];
                for (int k = std::max(1, c - lobe); k <= std::min(numBins_ - 1, c + lobe); ++k)
                    residual_[(size_t) k] *= keep;
            }
        }

        // Salience is computed from genuine spectral peaks only, each placed at
        // its interpolated frequency. In short post-attack windows the main
        // lobes are wide, and summing raw bins lets a candidate collect the
        // skirt of a neighbouring partial; a peak list doesn't have that problem.
        void buildPeakSpectrum()
        {
            std::fill(peaks_.begin(), peaks_.end(), 0.0f);
            float maxMag = 0.0f;
            for (float m : mag_)
                maxMag = std::max(maxMag, m);
            const float floorMag = maxMag * 0.003f; // -50 dB
            const double zeroPad = (double) fftSize_ / std::max(1, analysedLength_);
            peakSpread_ = std::max(0, (int) std::lround(kPeakSpread * zeroPad));
            for (int k = 2; k < numBins_ - 2; ++k)
            {
                const float m = mag_[(size_t) k];
                if (m <= floorMag || m <= mag_[(size_t) k - 1] || m < mag_[(size_t) k + 1])
                    continue;
                const double y0 = std::log(mag_[(size_t) k - 1] + 1e-12), y1 = std::log(m + 1e-12), y2 = std::log(mag_[(size_t) k + 1] + 1e-12);
                const double denom = y0 - 2.0 * y1 + y2;
                const double delta = denom < 0.0 ? std::clamp(0.5 * (y0 - y2) / denom, -0.5, 0.5) : 0.0;
                const int centre = (int) std::lround(k + delta);
                const float w = white_[(size_t) k];
                for (int j = std::max(1, centre - peakSpread_); j <= std::min(numBins_ - 1, centre + peakSpread_); ++j)
                    peaks_[(size_t) j] = std::max(peaks_[(size_t) j], w);
            }
        }

        void removeHum(int length)
        {
            std::copy(mag_.begin(), mag_.end(), rawMag_.begin());
            if (humPeaks_.empty())
                return;
            const int lobe = (int) std::ceil(2.0 * fftSize_ / std::max(1, length));
            for (const auto& h : humPeaks_)
            {
                // A stationary sinusoid keeps the same (normalised) peak height
                // for any window length; only its lobe width changes.
                const float sub = kHumOverSubtract * h.second;
                for (int k = std::max(0, h.first - lobe); k <= std::min(numBins_ - 1, h.first + lobe); ++k)
                    mag_[(size_t) k] = std::max(0.0f, mag_[(size_t) k] - sub);
            }
        }

        // Plain Hann over the whole segment. (A fast-rise window that keeps the
        // attack was tried; its sidelobes create phantom peaks between partials.)
        void buildWindow(int length)
        {
            const double pi = 3.14159265358979323846;
            for (int i = 0; i < length; ++i)
                window_[(size_t) i] = (float) (0.5 - 0.5 * std::cos(2.0 * pi * (i + 0.5) / length));
        }

        void buildBands()
        {
            for (int b = 0; b < kNumBands + 2; ++b)
                bandCentre_[(size_t) b] = 229.0 * (std::pow(10.0, (b + 1) / 21.4) - 1.0);
        }

        void whiten()
        {
            std::array<double, kNumBands + 2> gammaB {};
            double maxSigma = 0.0;
            std::array<double, kNumBands + 2> sigma {};
            for (int b = 1; b <= kNumBands; ++b)
            {
                const double lo = bandCentre_[(size_t) b - 1], c = bandCentre_[(size_t) b], hi = bandCentre_[(size_t) b + 1];
                double sum = 0.0, wsum = 0.0;
                const int k0 = std::max(1, (int) std::floor(lo / binHz_));
                const int k1 = std::min(numBins_ - 1, (int) std::ceil(hi / binHz_));
                for (int k = k0; k <= k1; ++k)
                {
                    const double f = k * binHz_;
                    const double h = f < c ? (f - lo) / (c - lo) : (hi - f) / (hi - c);
                    if (h <= 0.0)
                        continue;
                    sum += h * (double) mag_[(size_t) k] * (double) mag_[(size_t) k];
                    wsum += h;
                }
                sigma[(size_t) b] = wsum > 0.0 ? std::sqrt(sum / wsum) : 0.0;
                maxSigma = std::max(maxSigma, sigma[(size_t) b]);
            }
            // Floor: never boost a near-empty band by more than ~30 dB relative
            // to the loudest band, or noise gets whitened into fake partials.
            const double floorSigma = std::max(1.0e-7, maxSigma * 0.03);
            for (int b = 1; b <= kNumBands; ++b)
                gammaB[(size_t) b] = std::pow(std::max(sigma[(size_t) b], floorSigma), kNu - 1.0);

            int b = 1;
            for (int k = 0; k < numBins_; ++k)
            {
                const double f = k * binHz_;
                while (b < kNumBands && f > bandCentre_[(size_t) b + 1])
                    ++b;
                double g;
                if (f <= bandCentre_[1])
                    g = gammaB[1];
                else if (f >= bandCentre_[(size_t) kNumBands])
                    g = gammaB[(size_t) kNumBands];
                else
                {
                    const int b0 = f < bandCentre_[(size_t) b] ? b - 1 : b;
                    const double c0 = bandCentre_[(size_t) b0], c1 = bandCentre_[(size_t) b0 + 1];
                    const double t = (f - c0) / (c1 - c0);
                    g = gammaB[(size_t) b0] * (1.0 - t) + gammaB[(size_t) std::min(kNumBands, b0 + 1)] * t;
                }
                gamma_[(size_t) k] = (float) g;
                white_[(size_t) k] = mag_[(size_t) k] * (float) g;
            }
        }

        double sampleRate_ = 44100.0, binHz_ = 5.0, a4_ = 440.0;
        int maxLength_ = 4096, fftSize_ = 8192, numBins_ = 4096, analysedLength_ = 1, peakSpread_ = 1;
        std::unique_ptr<juce::dsp::FFT> fft_;
        std::vector<float> fftData_, window_, mag_, white_, residual_, gamma_, peaks_;
        std::vector<float> rawMag_, noiseFloor_;
        std::vector<std::pair<int, float>> humPeaks_;
        int numCandidates_ = 0;
        std::vector<double> candFreq_;
        std::vector<int> candNumPartials_, partialLo_, partialHi_;
        std::vector<float> partialWeight_;
        std::vector<double> sal_;
        std::vector<bool> banned_;
        std::array<double, kNumBands + 2> bandCentre_ {};
    };
}
