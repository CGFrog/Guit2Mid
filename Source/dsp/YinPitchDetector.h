#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>

namespace gtm
{
    // YIN fundamental-frequency estimator (de Cheveigne & Kawahara, 2002), used
    // for single-note (mono) playing.
    //
    // Works on a variable-length segment so the engine can analyse *only the
    // audio since the last pick attack*: the lag range and integration window
    // both scale with the available length, which lets high notes lock in after
    // a few milliseconds while low notes simply need a little longer. The
    // difference function is computed via FFT cross-correlation, so cost is
    // O(N log N) rather than O(N * maxLag).
    class YinPitchDetector
    {
    public:
        struct Result
        {
            bool found = false;
            double frequencyHz = 0.0;
            double clarity = 0.0; // 1 - CMNDF at the chosen lag: 1 = perfectly periodic
        };

        void prepare(double sampleRate, double minFrequencyHz, double maxFrequencyHz, int maxSegmentLength)
        {
            sampleRate_ = sampleRate;
            tauMaxGlobal_ = (int) std::ceil(sampleRate / minFrequencyHz) + 2;
            tauMin_ = std::max(2, (int) std::floor(sampleRate / maxFrequencyHz));

            maxLength_ = maxSegmentLength;
            int order = 1;
            while ((1 << order) < maxSegmentLength)
                ++order;
            fftSize_ = 1 << order;
            fft_ = std::make_unique<juce::dsp::FFT>(order);

            a_.assign((size_t) fftSize_ * 2, 0.0f);
            b_.assign((size_t) fftSize_ * 2, 0.0f);
            cmndf_.assign((size_t) tauMaxGlobal_ + 2, 1.0);
            prefixEnergy_.assign((size_t) maxSegmentLength + 1, 0.0);
        }

        int minimumUsefulLength() const { return tauMin_ * 4; }

        Result process(const float* x, int length, double threshold = 0.15)
        {
            Result result;
            length = std::min(length, maxLength_);
            const int tauMax = std::min(tauMaxGlobal_, length / 2);
            const int w = length - tauMax;
            if (tauMax <= tauMin_ + 2 || w < 16)
                return result;

            // r(tau) = sum_{j<w} x[j] x[j+tau]  via IFFT(conj(A) * B)
            std::fill(a_.begin(), a_.end(), 0.0f);
            std::fill(b_.begin(), b_.end(), 0.0f);
            std::copy(x, x + w, a_.begin());
            std::copy(x, x + length, b_.begin());
            fft_->performRealOnlyForwardTransform(a_.data());
            fft_->performRealOnlyForwardTransform(b_.data());
            for (int k = 0; k < fftSize_; ++k)
            {
                const float ar = a_[(size_t) 2 * k], ai = a_[(size_t) 2 * k + 1];
                const float br = b_[(size_t) 2 * k], bi = b_[(size_t) 2 * k + 1];
                b_[(size_t) 2 * k] = ar * br + ai * bi;       // conj(a) * b
                b_[(size_t) 2 * k + 1] = ar * bi - ai * br;
            }
            fft_->performRealOnlyInverseTransform(b_.data()); // JUCE scales by 1/N

            prefixEnergy_[0] = 0.0;
            for (int i = 0; i < length; ++i)
                prefixEnergy_[(size_t) i + 1] = prefixEnergy_[(size_t) i] + (double) x[i] * (double) x[i];

            const double e0 = prefixEnergy_[(size_t) w];
            if (e0 <= 1.0e-12)
                return result;

            // Cumulative-mean-normalised difference function.
            cmndf_[0] = 1.0;
            double runningSum = 0.0;
            for (int tau = 1; tau <= tauMax; ++tau)
            {
                const double eTau = prefixEnergy_[(size_t) (tau + w)] - prefixEnergy_[(size_t) tau];
                const double d = std::max(0.0, e0 + eTau - 2.0 * (double) b_[(size_t) tau]);
                runningSum += d;
                cmndf_[(size_t) tau] = runningSum > 0.0 ? d * (double) tau / runningSum : 1.0;
            }

            // First dip below threshold (then descend to its local minimum);
            // fall back to the global minimum if nothing crosses.
            int best = -1;
            for (int tau = tauMin_; tau < tauMax; ++tau)
            {
                if (cmndf_[(size_t) tau] < threshold)
                {
                    while (tau + 1 < tauMax && cmndf_[(size_t) tau + 1] < cmndf_[(size_t) tau])
                        ++tau;
                    best = tau;
                    break;
                }
            }
            if (best < 0)
            {
                double minVal = 1.0e9;
                for (int tau = tauMin_; tau < tauMax; ++tau)
                    if (cmndf_[(size_t) tau] < minVal)
                    {
                        minVal = cmndf_[(size_t) tau];
                        best = tau;
                    }
                if (best < 0)
                    return result;
            }

            // A dip right at the edge of the searchable range is not trustworthy:
            // the real period may simply be longer than we can see yet.
            if (best >= tauMax - 1)
                return result;

            double refined = (double) best;
            {
                const double y0 = cmndf_[(size_t) best - 1], y1 = cmndf_[(size_t) best], y2 = cmndf_[(size_t) best + 1];
                const double denom = y0 - 2.0 * y1 + y2;
                if (denom > 0.0)
                    refined += std::clamp(0.5 * (y0 - y2) / denom, -0.5, 0.5);
            }

            result.found = true;
            result.frequencyHz = sampleRate_ / refined;
            result.clarity = 1.0 - std::min(1.0, cmndf_[(size_t) best]);
            return result;
        }

    private:
        double sampleRate_ = 44100.0;
        int tauMin_ = 16;
        int tauMaxGlobal_ = 700;
        int maxLength_ = 4096;
        int fftSize_ = 4096;
        std::unique_ptr<juce::dsp::FFT> fft_;
        std::vector<float> a_, b_;
        std::vector<double> cmndf_;
        std::vector<double> prefixEnergy_;
    };
}
