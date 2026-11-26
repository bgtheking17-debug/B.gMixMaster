#pragma once
// Pure C++17 DSP building blocks (no JUCE dependency, so they can be unit-tested anywhere).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace bg
{
constexpr double kPi = 3.14159265358979323846;

inline double dbToLin (double db) noexcept { return std::exp (db * 0.11512925464970229); }
inline double linToDb (double lin) noexcept { return 8.685889638065035 * std::log (std::max (lin, 1e-12)); }

//==============================================================================
// Double-precision biquad (transposed direct form II)
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;

    double process (double x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }

    void reset() noexcept { z1 = z2 = 0.0; }
    void setIdentity() noexcept { b0 = 1; b1 = b2 = a1 = a2 = 0; }

    void copyCoeffs (const Biquad& o) noexcept
    {
        b0 = o.b0; b1 = o.b1; b2 = o.b2; a1 = o.a1; a2 = o.a2;
    }

    void setCoeffs (double B0, double B1, double B2, double A0, double A1, double A2) noexcept
    {
        b0 = B0 / A0; b1 = B1 / A0; b2 = B2 / A0; a1 = A1 / A0; a2 = A2 / A0;
    }

    void setPeak (double fs, double f, double q, double gainDb) noexcept
    {
        const double A = std::pow (10.0, gainDb / 40.0);
        const double w0 = 2.0 * kPi * f / fs;
        const double alpha = std::sin (w0) / (2.0 * q);
        const double c = std::cos (w0);
        setCoeffs (1 + alpha * A, -2 * c, 1 - alpha * A, 1 + alpha / A, -2 * c, 1 - alpha / A);
    }

    void setLowPass (double fs, double f, double q) noexcept
    {
        const double w0 = 2.0 * kPi * f / fs;
        const double alpha = std::sin (w0) / (2.0 * q);
        const double c = std::cos (w0);
        setCoeffs ((1 - c) / 2, 1 - c, (1 - c) / 2, 1 + alpha, -2 * c, 1 - alpha);
    }

    void setHighPass (double fs, double f, double q) noexcept
    {
        const double w0 = 2.0 * kPi * f / fs;
        const double alpha = std::sin (w0) / (2.0 * q);
        const double c = std::cos (w0);
        setCoeffs ((1 + c) / 2, -(1 + c), (1 + c) / 2, 1 + alpha, -2 * c, 1 - alpha);
    }
};

//==============================================================================
// Linkwitz-Riley 4th order crossover (one channel). lo + hi = all-pass of the input.
struct LR4
{
    Biquad lp1, lp2, hp1, hp2;

    void setFreq (double fs, double f) noexcept
    {
        f = std::min (f, fs * 0.45);
        const double q = 0.70710678118654752;
        lp1.setLowPass (fs, f, q);  lp2.copyCoeffs (lp1);
        hp1.setHighPass (fs, f, q); hp2.copyCoeffs (hp1);
    }

    void reset() noexcept { lp1.reset(); lp2.reset(); hp1.reset(); hp2.reset(); }

    void process (double x, double& lo, double& hi) noexcept
    {
        lo = lp2.process (lp1.process (x));
        hi = hp2.process (hp1.process (x));
    }
};

//==============================================================================
// ITU-R BS.1770 / EBU R128 loudness meter (momentary, short-term, gated integrated)
class LoudnessMeter
{
public:
    void prepare (double fs)
    {
        // Stage 1: high shelf (+4 dB), stage 2: RLB high-pass. Coefficients per the standard's formulas.
        {
            const double f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
            const double K = std::tan (kPi * f0 / fs);
            const double Vh = std::pow (10.0, G / 20.0);
            const double Vb = std::pow (Vh, 0.4996667741545416);
            const double a0 = 1.0 + K / Q + K * K;
            for (auto& s : shelf)
            {
                s.b0 = (Vh + Vb * K / Q + K * K) / a0;
                s.b1 = 2.0 * (K * K - Vh) / a0;
                s.b2 = (Vh - Vb * K / Q + K * K) / a0;
                s.a1 = 2.0 * (K * K - 1.0) / a0;
                s.a2 = (1.0 - K / Q + K * K) / a0;
            }
        }
        {
            const double f0 = 38.13547087602444, Q = 0.5003270373238773;
            const double K = std::tan (kPi * f0 / fs);
            const double a0 = 1.0 + K / Q + K * K;
            for (auto& h : hp)
            {
                h.b0 = 1.0; h.b1 = -2.0; h.b2 = 1.0;
                h.a1 = 2.0 * (K * K - 1.0) / a0;
                h.a2 = (1.0 - K / Q + K * K) / a0;
            }
        }
        subLen = std::max (1, (int) std::lround (0.1 * fs));
        reset();
    }

    void reset()
    {
        for (auto& s : shelf) s.reset();
        for (auto& h : hp) h.reset();
        acc = 0.0; cnt = 0;
        ring.assign (30, 0.0); pos = 0; filled = 0;
        binCount.assign (kBins, 0.0); binEnergy.assign (kBins, 0.0);
        mom = shortT = integ = -120.0;
    }

    void process (const float* l, const float* r, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const double a = hp[0].process (shelf[0].process ((double) l[i]));
            const double b = hp[1].process (shelf[1].process ((double) r[i]));
            acc += a * a + b * b;
            if (++cnt >= subLen)
            {
                finishSubBlock (acc / (double) cnt);
                acc = 0.0; cnt = 0;
            }
        }
    }

    double momentary() const noexcept { return mom; }
    double shortTerm() const noexcept { return shortT; }
    double integrated() const noexcept { return integ; }

private:
    static constexpr int kBins = 751; // -70 .. +5 LUFS in 0.1 steps

    static double toLufs (double ms) noexcept { return std::max (-120.0, -0.691 + 10.0 * std::log10 (ms + 1e-14)); }

    double meanLast (int count) const noexcept
    {
        const int m = std::min (count, filled);
        double s = 0.0;
        for (int k = 0; k < m; ++k) s += ring[(size_t) ((pos - 1 - k + 60) % 30)];
        return s / (double) std::max (1, m);
    }

    void finishSubBlock (double ms) noexcept
    {
        ring[(size_t) pos] = ms;
        pos = (pos + 1) % 30;
        filled = std::min (30, filled + 1);

        const double m400 = meanLast (4);
        mom = toLufs (m400);
        shortT = toLufs (meanLast (30));

        if (filled >= 4)
        {
            const double l = toLufs (m400);
            if (l > -70.0)
            {
                const int bin = std::clamp ((int) ((l + 70.0) / 0.1), 0, kBins - 1);
                binCount[(size_t) bin] += 1.0;
                binEnergy[(size_t) bin] += m400;

                double n = 0.0, e = 0.0;
                for (int b = 0; b < kBins; ++b) { n += binCount[(size_t) b]; e += binEnergy[(size_t) b]; }
                const double relThr = toLufs (e / n) - 10.0;
                double n2 = 0.0, e2 = 0.0;
                for (int b = 0; b < kBins; ++b)
                {
                    const double centre = -70.0 + 0.1 * b + 0.05;
                    if (centre > relThr) { n2 += binCount[(size_t) b]; e2 += binEnergy[(size_t) b]; }
                }
                if (n2 > 0.0) integ = toLufs (e2 / n2);
            }
        }
    }

    Biquad shelf[2], hp[2];
    int subLen = 4800, cnt = 0, pos = 0, filled = 0;
    double acc = 0.0;
    std::vector<double> ring, binCount, binEnergy;
    double mom = -120.0, shortT = -120.0, integ = -120.0;
};

//==============================================================================
// Stereo-linked feed-forward compressor (dB domain, soft knee). Returns a linear gain.
struct LinkedCompressor
{
    double thresholdDb = -18.0, ratio = 2.0, attackMs = 20.0, releaseMs = 150.0, makeupDb = 0.0;
    double kneeDb = 6.0;
    double fs = 44100.0, env = 0.0, aCoef = 0.0, rCoef = 0.0;

    void prepare (double sampleRate) noexcept { fs = sampleRate; env = 0.0; updateCoefs(); }

    void setParams (double thr, double rat, double atk, double rel, double makeup) noexcept
    {
        thresholdDb = thr;
        ratio = std::max (1.0, rat);
        attackMs = atk; releaseMs = rel; makeupDb = makeup;
        updateCoefs();
    }

    void updateCoefs() noexcept
    {
        aCoef = std::exp (-1.0 / (std::max (0.01, attackMs) * 0.001 * fs));
        rCoef = std::exp (-1.0 / (std::max (0.01, releaseMs) * 0.001 * fs));
    }

    double process (double l, double r) noexcept
    {
        const double lvl = std::max (std::abs (l), std::abs (r));
        const double db = linToDb (lvl);
        const double over = db - thresholdDb;
        const double slope = 1.0 / ratio - 1.0;
        double gc;
        if (2.0 * over < -kneeDb)               gc = 0.0;
        else if (2.0 * std::abs (over) <= kneeDb) { const double x = over + kneeDb * 0.5; gc = slope * x * x / (2.0 * kneeDb); }
        else                                    gc = slope * over;

        env = (gc < env) ? aCoef * env + (1.0 - aCoef) * gc
                         : rCoef * env + (1.0 - rCoef) * gc;
        return dbToLin (env + makeupDb);
    }

    double reductionDb() const noexcept { return -env; }
};

//==============================================================================
// Look-ahead brickwall limiter with a 4x inter-sample (true-peak) estimate (24-tap windowed sinc)
// and a 0.2 dB detection margin, so the true peak of real (band-limited) material stays under the ceiling.
// Gain = boxcar(release-smoothed sliding-minimum of the required gain) -> never exceeds the ceiling.
class LookaheadLimiter
{
public:
    void prepare (double fs)
    {
        sr = fs;
        N = std::max (16, (int) std::lround (0.0015 * fs));
        D = N + kLead - 1;
        ringLen = D + 1;
        audio[0].assign ((size_t) ringLen, 0.f);
        audio[1].assign ((size_t) ringLen, 0.f);
        qbuf.assign ((size_t) N, 1.0);
        sbuf.assign ((size_t) N, 1.0);

        // interpolation taps for the segment between hist[kLead-1] and hist[kLead] at 1/4, 1/2, 3/4
        for (int ph = 0; ph < 3; ++ph)
        {
            const double frac = 0.25 * (ph + 1);
            double total = 0.0;
            for (int k = 0; k < kTaps; ++k)
            {
                const double t = (double) (k - (kLead - 1)) - frac;
                const double sinc = std::sin (kPi * t) / (kPi * t);
                const double w = std::abs (t) < kLead ? 0.5 + 0.5 * std::cos (kPi * t / kLead) : 0.0;
                taps[ph][k] = sinc * w;
                total += taps[ph][k];
            }
            for (int k = 0; k < kTaps; ++k) taps[ph][k] /= total;
        }

        setReleaseMs (100.0);
        setCeilingDb (-1.0);
        reset();
    }

    int latency() const noexcept { return D; }

    void reset()
    {
        std::fill (qbuf.begin(), qbuf.end(), 1.0);
        std::fill (sbuf.begin(), sbuf.end(), 1.0);
        std::fill (audio[0].begin(), audio[0].end(), 0.f);
        std::fill (audio[1].begin(), audio[1].end(), 0.f);
        sum = (double) N; relState = 1.0; prevReq = 1.0;
        for (auto& c : h) for (auto& v : c) v = 0.0;
        wp = qp = sp = 0;
        minGain = 1.0;
    }

    void setReleaseMs (double ms) noexcept
    {
        relCoef = std::exp (-1.0 / (std::max (1.0, ms) * 0.001 * sr));
    }

    void setCeilingDb (double db) noexcept { ceiling = dbToLin (db); }

    // returns the smallest gain applied since the previous call, then resets it
    double takeMinGain() noexcept { const double g = minGain; minGain = 1.0; return g; }

    void process (float* L, float* R, int n) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            double pk = 0.0;
            for (int ch = 0; ch < 2; ++ch)
            {
                double* hh = h[ch];
                for (int k = 0; k < kTaps - 1; ++k) hh[k] = hh[k + 1];
                hh[kTaps - 1] = (double) (ch == 0 ? L[i] : R[i]);

                pk = std::max (pk, std::max (std::abs (hh[kLead - 1]), std::abs (hh[kLead])));
                for (int ph = 0; ph < 3; ++ph)
                {
                    double v = 0.0;
                    for (int k = 0; k < kTaps; ++k) v += taps[ph][k] * hh[k];
                    pk = std::max (pk, std::abs (v));
                }
            }

            const double det = ceiling * kMargin;
            const double reqC = pk > det ? det / pk : 1.0;
            const double q = std::min (reqC, prevReq);
            prevReq = reqC;

            qbuf[(size_t) qp] = q;
            if (++qp >= N) qp = 0;
            double mf = 1.0;
            for (int k = 0; k < N; ++k) mf = std::min (mf, qbuf[(size_t) k]);

            if (mf < relState) relState = mf;
            else               relState = mf + (relState - mf) * relCoef;

            sum += relState - sbuf[(size_t) sp];
            sbuf[(size_t) sp] = relState;
            if (++sp >= N) sp = 0;
            const double g = std::clamp (sum / (double) N, 0.0, 1.0);

            audio[0][(size_t) wp] = L[i];
            audio[1][(size_t) wp] = R[i];
            const int rp = (wp + 1) % ringLen; // oldest sample = D samples ago
            L[i] = (float) ((double) audio[0][(size_t) rp] * g);
            R[i] = (float) ((double) audio[1][(size_t) rp] * g);
            wp = rp;

            minGain = std::min (minGain, g);
        }
    }

private:
    static constexpr int kTaps = 24;
    static constexpr int kLead = kTaps / 2;
    static constexpr double kMargin = 0.9772; // -0.2 dB

    double sr = 44100.0;
    int N = 66, D = 71, ringLen = 72;
    std::vector<float> audio[2];
    std::vector<double> qbuf, sbuf;
    double h[2][kTaps] = {};
    double taps[3][kTaps] = {};
    double sum = 0.0, relState = 1.0, prevReq = 1.0, relCoef = 0.999, ceiling = 0.89, minGain = 1.0;
    int wp = 0, qp = 0, sp = 0;
};

//==============================================================================
// Radix-2 FFT (in place, forward)
class Fft
{
public:
    explicit Fft (int order) : n (1 << order)
    {
        cosT.resize ((size_t) n / 2);
        sinT.resize ((size_t) n / 2);
        for (int k = 0; k < n / 2; ++k)
        {
            cosT[(size_t) k] = std::cos (2.0 * kPi * k / n);
            sinT[(size_t) k] = std::sin (2.0 * kPi * k / n);
        }
        rev.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < order; ++b) if (i & (1 << b)) r |= 1 << (order - 1 - b);
            rev[(size_t) i] = r;
        }
    }

    int size() const noexcept { return n; }

    void forward (double* re, double* im) const noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const int j = rev[(size_t) i];
            if (j > i) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
        }
        for (int size = 2; size <= n; size <<= 1)
        {
            const int half = size / 2, step = n / size;
            for (int i = 0; i < n; i += size)
                for (int j = 0; j < half; ++j)
                {
                    const double wr = cosT[(size_t) (j * step)];
                    const double wi = -sinT[(size_t) (j * step)];
                    const int a = i + j, b = i + j + half;
                    const double tr = wr * re[b] - wi * im[b];
                    const double ti = wr * im[b] + wi * re[b];
                    re[b] = re[a] - tr; im[b] = im[a] - ti;
                    re[a] += tr;        im[a] += ti;
                }
        }
    }

private:
    int n;
    std::vector<double> cosT, sinT;
    std::vector<int> rev;
};

} // namespace bg
