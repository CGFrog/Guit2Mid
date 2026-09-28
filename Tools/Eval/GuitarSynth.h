#pragma once

// Physically-inspired electric-guitar DI synthesiser used to generate test
// material with exact ground truth. Extended Karplus-Strong string with:
//  - exact (fractional, Lagrange-interpolated) tuning and a linear-phase loss
//    filter, so the pitch is right and the partials are harmonic,
//  - pick-position comb on the excitation and pickup-position comb on the output,
//  - frequency-dependent decay (bright attack, darker sustain),
//  - time-varying pitch for bends/vibrato/hammer-ons (the string keeps
//    vibrating through the change, like the real thing),
//  - quick damping at note end (fretting hand release / palm mute),
//  - pickup resonance low-pass, mains hum and noise floor.

#include <vector>
#include <random>
#include <cmath>
#include <algorithm>

namespace gtmtest
{
    struct SynthNote
    {
        double onset = 0.0;       // seconds
        double offset = 1.0;      // seconds: string damped from here
        double midi = 40.0;
        double velocity = 0.8;    // 0..1
        // Piecewise-linear pitch offset in semitones vs time since onset.
        std::vector<std::pair<double, double>> pitchCurve;
        double pickPosition = 0.13;   // fraction of string length
        double pickupPosition = 0.12;
        double brightness = 0.6;      // 0..1
        double excitationScale = 1.0; // < 1 for soft/legato starts
    };

    inline double midiToHz(double m) { return 440.0 * std::pow(2.0, (m - 69.0) / 12.0); }

    class GuitarSynth
    {
    public:
        explicit GuitarSynth(double sampleRate, unsigned seed = 1234) : fs_(sampleRate), rng_(seed) {}

        void addNote(const SynthNote& n) { notes_.push_back(n); }

        std::vector<float> render(double durationSeconds, double noiseDb = -80.0, double humDb = -66.0)
        {
            const int total = (int) std::ceil(durationSeconds * fs_);
            std::vector<double> mix((size_t) total, 0.0);
            for (const auto& n : notes_)
                renderString(n, mix);

            // Pickup resonance: 2nd-order low-pass, ~4.5 kHz, Q 1.4.
            {
                const double f = 4500.0, q = 1.4;
                const double w0 = 2.0 * M_PI_ * f / fs_, alpha = std::sin(w0) / (2.0 * q), c = std::cos(w0);
                const double a0 = 1.0 + alpha;
                const double b0 = (1.0 - c) / 2.0 / a0, b1 = (1.0 - c) / a0, b2 = b0;
                const double a1 = -2.0 * c / a0, a2 = (1.0 - alpha) / a0;
                double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
                for (auto& s : mix)
                {
                    const double y = b0 * s + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
                    x2 = x1; x1 = s; y2 = y1; y1 = y;
                    s = y;
                }
            }

            std::normal_distribution<double> gauss(0.0, 1.0);
            const double noiseAmp = std::pow(10.0, noiseDb / 20.0);
            const double humAmp = std::pow(10.0, humDb / 20.0);
            std::vector<float> out((size_t) total);
            for (int i = 0; i < total; ++i)
            {
                const double t = i / fs_;
                const double hum = humAmp * (std::sin(2 * M_PI_ * 60 * t) + 0.5 * std::sin(2 * M_PI_ * 180 * t + 0.3)
                                             + 0.25 * std::sin(2 * M_PI_ * 300 * t + 1.1));
                out[(size_t) i] = (float) (mix[(size_t) i] + hum + noiseAmp * gauss(rng_));
            }
            return out;
        }

    private:
        static constexpr double M_PI_ = 3.14159265358979323846;

        double pitchOffsetAt(const SynthNote& n, double t) const
        {
            const auto& c = n.pitchCurve;
            if (c.empty())
                return 0.0;
            if (t <= c.front().first)
                return c.front().second;
            for (size_t i = 1; i < c.size(); ++i)
                if (t <= c[i].first)
                {
                    const double span = c[i].first - c[i - 1].first;
                    const double u = span > 0 ? (t - c[i - 1].first) / span : 1.0;
                    return c[i - 1].second + u * (c[i].second - c[i - 1].second);
                }
            return c.back().second;
        }

        static double lagrange(const std::vector<double>& buf, int mask, long long writePos, double delay)
        {
            // Read buf at (writePos - delay) with 3rd-order Lagrange interpolation.
            const double readPos = (double) writePos - delay;
            const long long i = (long long) std::floor(readPos);
            const double d = readPos - (double) i;
            auto at = [&](long long k) { return buf[(size_t) (k & mask)]; };
            const double xm1 = at(i - 1), x0 = at(i), x1 = at(i + 1), x2 = at(i + 2);
            const double c0 = -d * (d - 1) * (d - 2) / 6.0;
            const double c1 = (d + 1) * (d - 1) * (d - 2) / 2.0;
            const double c2 = -(d + 1) * d * (d - 2) / 2.0;
            const double c3 = (d + 1) * d * (d - 1) / 6.0;
            return c0 * xm1 + c1 * x0 + c2 * x1 + c3 * x2;
        }

        void renderString(const SynthNote& n, std::vector<double>& mix)
        {
            const int size = 16384, mask = size - 1;
            std::vector<double> line((size_t) size, 0.0);
            std::vector<double> outHist((size_t) size, 0.0);

            const long long start = (long long) std::llround(n.onset * fs_);
            const long long stop = (long long) std::llround(n.offset * fs_);
            const long long end = std::min<long long>((long long) mix.size(), stop + (long long) (0.25 * fs_));

            const double f0 = midiToHz(n.midi);
            const double period0 = fs_ / f0;

            // Excitation: the plucked string's initial displacement -- a triangle
            // with its apex at the pick position (partials ~ sin(pi m beta) / m^2),
            // sharpened by pick hardness, plus a little pick noise.
            std::uniform_real_distribution<double> uni(-1.0, 1.0);
            const int exLen = (int) std::round(period0);
            std::vector<double> ex((size_t) exLen, 0.0);
            const double apex = std::clamp(n.pickPosition, 0.05, 0.5) * exLen;
            for (int i = 0; i < exLen; ++i)
            {
                const double tri = i < apex ? i / apex : (exLen - i) / (exLen - apex);
                ex[(size_t) i] = tri + 0.08 * uni(rng_);
            }
            // Hardness: add a scaled first difference (boosts upper partials ~ m).
            const double hard = 0.3 + 0.9 * n.brightness * n.velocity;
            std::vector<double> shaped((size_t) exLen, 0.0);
            for (int i = 0; i < exLen; ++i)
                shaped[(size_t) i] = ex[(size_t) i] + hard * 0.15 * exLen / 8.0 * (ex[(size_t) i] - ex[(size_t) (i > 0 ? i - 1 : exLen - 1)]);
            double mean = 0.0;
            for (double s : shaped) mean += s;
            mean /= exLen;
            double peak = 1e-9;
            for (auto& s : shaped) { s -= mean; peak = std::max(peak, std::abs(s)); }
            const double gain = 0.22 * std::pow(n.velocity, 1.3) * n.excitationScale / peak;

            // Decay: low strings ring longer.
            const double t60 = std::clamp(6.0 * std::pow(82.4 / f0, 0.6), 1.2, 7.0);
            double prevRead = 0.0;

            for (long long s = start, k = 0; s < end; ++s, ++k)
            {
                const double t = (double) k / fs_;
                const double freq = midiToHz(n.midi + pitchOffsetAt(n, t));
                const double period = fs_ / freq;
                const bool damped = s >= stop;
                const double decay = damped ? 0.05 : t60;
                const double loopGain = std::pow(10.0, -3.0 / (freq * decay));

                // Averaging filter adds half a sample of delay: read at period - 0.5.
                const double rd = lagrange(line, mask, k, period - 0.5);
                const double y = (k < exLen ? gain * shaped[(size_t) k] : 0.0) + loopGain * 0.5 * (rd + prevRead);
                prevRead = rd;
                line[(size_t) (k & mask)] = y;

                const double pickupDelay = std::max(2.0, n.pickupPosition * period);
                outHist[(size_t) (k & mask)] = y;
                const double outSample = y - 0.8 * lagrange(outHist, mask, k, pickupDelay);
                if (s >= 0 && s < (long long) mix.size())
                    mix[(size_t) s] += outSample;
            }
        }

        double fs_;
        std::mt19937 rng_;
        std::vector<SynthNote> notes_;
    };
}
