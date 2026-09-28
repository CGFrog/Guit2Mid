#pragma once

#include <juce_dsp/juce_dsp.h>
#include <vector>
#include <memory>
#include <algorithm>
#include <cmath>

namespace gtm
{
    // Pick-attack detector: log-compressed "SuperFlux" (Boeck & Widmer, 2013).
    //
    // Each hop takes a short FFT of the newest audio, log-compresses the
    // magnitudes (so a soft pick registers nearly as clearly as a hard one),
    // and sums the positive change against a max-filtered frame from two hops
    // earlier (the max filter stops vibrato/bends from registering as attacks).
    // The detection function is compared to an adaptive threshold.
    class OnsetDetector
    {
    public:
        void prepare(double sampleRate, int frameSize)
        {
            frameSize_ = frameSize;
            int order = 0;
            while ((1 << order) < frameSize)
                ++order;
            fft_ = std::make_unique<juce::dsp::FFT>(order);
            window_.assign((size_t) frameSize, 0.0f);
            juce::dsp::WindowingFunction<float>::fillWindowingTables(
                window_.data(), (size_t) frameSize, juce::dsp::WindowingFunction<float>::hann, false);
            double wsum = 0.0;
            for (float w : window_)
                wsum += w;
            norm_ = (float) (2.0 / wsum);

            fftData_.assign((size_t) frameSize * 2, 0.0f);
            const int numBins = frameSize / 2;
            for (auto& f : logMag_)
                f.assign((size_t) numBins, 0.0f);
            maxFiltered_.assign((size_t) numBins, 0.0f);

            const double binHz = sampleRate / frameSize;
            loBin_ = std::max(1, (int) std::round(60.0 / binHz));
            hiBin_ = std::min(numBins - 2, (int) std::round(10000.0 / binHz));
            reset();
        }

        void reset()
        {
            for (auto& f : logMag_)
                std::fill(f.begin(), f.end(), 0.0f);
            frameIndex_ = 0;
            std::fill(std::begin(history_), std::end(history_), 0.0f);
            historyPos_ = 0;
            lastOdf_ = 0.0f;
        }

        // frame: the newest frameSize samples. sensitivity: 0 (few onsets) .. 1 (many).
        // Returns true when an attack is detected in this hop.
        bool process(const float* frame, double sensitivity, float& odfOut)
        {
            std::fill(fftData_.begin(), fftData_.end(), 0.0f);
            for (int i = 0; i < frameSize_; ++i)
                fftData_[(size_t) i] = frame[i] * window_[(size_t) i];
            fft_->performRealOnlyForwardTransform(fftData_.data(), true);

            auto& cur = logMag_[(size_t) (frameIndex_ % kFrames)];
            const auto& ref = logMag_[(size_t) ((frameIndex_ + kFrames - kLag) % kFrames)];
            const int numBins = frameSize_ / 2;
            for (int k = 0; k < numBins; ++k)
            {
                const float re = fftData_[(size_t) 2 * k], im = fftData_[(size_t) 2 * k + 1];
                const float mag = std::sqrt(re * re + im * im) * norm_;
                cur[(size_t) k] = std::log1p(kLogScale * mag);
            }

            for (int k = 1; k < numBins - 1; ++k)
                maxFiltered_[(size_t) k] = std::max({ ref[(size_t) k - 1], ref[(size_t) k], ref[(size_t) k + 1] });

            float odf = 0.0f;
            for (int k = loBin_; k <= hiBin_; ++k)
                odf += std::max(0.0f, cur[(size_t) k] - maxFiltered_[(size_t) k]);
            odf /= (float) (hiBin_ - loBin_ + 1);

            ++frameIndex_;

            // Adaptive threshold: local mean of recent ODF values plus a fixed
            // offset that the sensitivity control scales.
            float mean = 0.0f;
            for (float h : history_)
                mean += h;
            mean /= (float) kHistory;

            const float delta = (float) (kBaseDelta * std::pow(4.0, 0.5 - std::clamp(sensitivity, 0.0, 1.0)));
            const float threshold = 1.5f * mean + delta;

            history_[(size_t) historyPos_] = odf;
            historyPos_ = (historyPos_ + 1) % kHistory;

            const bool rising = odf > lastOdf_;
            lastOdf_ = odf;
            odfOut = odf;
            return frameIndex_ > kFrames && rising && odf > threshold;
        }

    private:
        static constexpr int kFrames = 4;
        static constexpr int kLag = 2;
        static constexpr int kHistory = 16;
        static constexpr float kLogScale = 1000.0f;
        static constexpr double kBaseDelta = 0.06;

        int frameSize_ = 1024;
        std::unique_ptr<juce::dsp::FFT> fft_;
        std::vector<float> window_, fftData_, maxFiltered_;
        std::vector<float> logMag_[kFrames];
        float norm_ = 1.0f;
        int loBin_ = 1, hiBin_ = 100;
        int frameIndex_ = 0;
        float history_[kHistory] {};
        int historyPos_ = 0;
        float lastOdf_ = 0.0f;
    };
}
