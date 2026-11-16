#include "Analyzer.h"
#include <algorithm>
#include <complex>
#include <numeric>

namespace kr
{
//==============================================================================
juce::String typeName (DrumType t)
{
    switch (t)
    {
        case DrumType::Kick:      return "KICK";
        case DrumType::Snare:     return "SNARE";
        case DrumType::Clap:      return "CLAP";
        case DrumType::HatClosed: return "HAT";
        case DrumType::HatOpen:   return "OPEN HAT";
        case DrumType::Tom:       return "TOM";
        case DrumType::Perc:      return "PERC";
        case DrumType::Cymbal:    return "CRASH";
        case DrumType::NumTypes:
        default:                  return "PERC";
    }
}

juce::uint32 typeColourARGB (DrumType t)
{
    switch (t)
    {
        case DrumType::Kick:      return 0xffe8573f;
        case DrumType::Snare:     return 0xfff2b33d;
        case DrumType::Clap:      return 0xffe98fb4;
        case DrumType::HatClosed: return 0xff6fd3c7;
        case DrumType::HatOpen:   return 0xff4f9fe0;
        case DrumType::Tom:       return 0xffb98a5e;
        case DrumType::Perc:      return 0xff9ccc5a;
        case DrumType::Cymbal:    return 0xffb08be0;
        case DrumType::NumTypes:
        default:                  return 0xff888888;
    }
}

namespace
{
constexpr float eps = 1.0e-12f;

inline bool cancelled (const std::atomic<bool>* c) { return c != nullptr && c->load(); }

//==============================================================================
// FFT reale con scala dell'inversa calibrata (le implementazioni JUCE differiscono per piattaforma)
struct RealFFT
{
    explicit RealFFT (int order)
        : size (1 << order), fft (order), buf ((size_t) size * 2, 0.0f)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        buf[0] = 1.0f;
        fft.performRealOnlyForwardTransform (buf.data(), true);
        fft.performRealOnlyInverseTransform (buf.data());
        invScale = std::abs (buf[0]) > 1.0e-9f ? 1.0f / buf[0] : 1.0f;
    }

    // spettro di potenza (size/2 + 1 valori) di x già finestrato
    void power (const float* x, float* out)
    {
        std::copy (x, x + size, buf.begin());
        std::fill (buf.begin() + size, buf.end(), 0.0f);
        fft.performRealOnlyForwardTransform (buf.data(), true);
        for (int k = 0; k <= size / 2; ++k)
            out[k] = buf[(size_t) (2 * k)] * buf[(size_t) (2 * k)] + buf[(size_t) (2 * k + 1)] * buf[(size_t) (2 * k + 1)];
    }

    void forward (const float* x, std::complex<float>* out)
    {
        std::copy (x, x + size, buf.begin());
        std::fill (buf.begin() + size, buf.end(), 0.0f);
        fft.performRealOnlyForwardTransform (buf.data(), true);
        for (int k = 0; k <= size / 2; ++k)
            out[k] = { buf[(size_t) (2 * k)], buf[(size_t) (2 * k + 1)] };
    }

    void inverse (const std::complex<float>* in, float* out)
    {
        std::fill (buf.begin(), buf.end(), 0.0f);
        for (int k = 0; k <= size / 2; ++k)
        {
            buf[(size_t) (2 * k)]     = in[k].real();
            buf[(size_t) (2 * k + 1)] = in[k].imag();
        }
        fft.performRealOnlyInverseTransform (buf.data());
        for (int n = 0; n < size; ++n)
            out[n] = buf[(size_t) n] * invScale;
    }

    int size;
    juce::dsp::FFT fft;
    std::vector<float> buf;
    float invScale = 1.0f;
};

std::vector<float> periodicHann (int n)
{
    std::vector<float> w ((size_t) n);
    for (int i = 0; i < n; ++i)
        w[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) n);
    return w;
}

std::vector<float> makeMono (const juce::AudioBuffer<float>& b)
{
    const int n = b.getNumSamples(), c = b.getNumChannels();
    std::vector<float> m ((size_t) n, 0.0f);
    for (int ch = 0; ch < c; ++ch)
    {
        auto* p = b.getReadPointer (ch);
        for (int i = 0; i < n; ++i) m[(size_t) i] += p[i];
    }
    if (c > 1)
        for (auto& v : m) v /= (float) c;
    return m;
}

float median (std::vector<float> v)
{
    if (v.empty()) return 0.0f;
    auto mid = v.begin() + (std::ptrdiff_t) (v.size() / 2);
    std::nth_element (v.begin(), mid, v.end());
    return *mid;
}

inline float sampleAt (const std::vector<float>& x, juce::int64 i)
{
    return (i >= 0 && i < (juce::int64) x.size()) ? x[(size_t) i] : 0.0f;
}

float typeMaxSeconds (DrumType t)
{
    switch (t)
    {
        case DrumType::Kick:      return 1.4f;
        case DrumType::Snare:     return 0.9f;
        case DrumType::Clap:      return 0.8f;
        case DrumType::HatClosed: return 0.35f;
        case DrumType::HatOpen:   return 1.2f;
        case DrumType::Tom:       return 1.2f;
        case DrumType::Perc:      return 0.8f;
        case DrumType::Cymbal:    return 3.0f;
        case DrumType::NumTypes:
        default:                  return 1.0f;
    }
}

//==============================================================================
// Vettore di caratteristiche per raggruppare i colpi
struct FeatVec
{
    float v[numBands + 3] {};
};

FeatVec featuresOf (const Hit& h)
{
    FeatVec f;
    for (int b = 0; b < numBands; ++b) f.v[b] = juce::jmax (-30.0f, h.bands[b]);
    f.v[numBands]     = 12.0f * std::log2 (juce::jmax (30.0f, h.centroid));
    f.v[numBands + 1] = 6.0f  * std::log2 (juce::jmax (10.0f, h.decayMs));
    f.v[numBands + 2] = 12.0f * std::log2 (juce::jmax (25.0f, h.lowPeakHz));
    return f;
}

float distance (const FeatVec& a, const FeatVec& b, DrumType t)
{
    float spec = 0.0f;
    for (int i = 0; i < numBands; ++i) spec += std::abs (a.v[i] - b.v[i]);
    spec /= (float) numBands;
    float d = spec
            + 0.35f * std::abs (a.v[numBands] - b.v[numBands])
            + 0.2f * std::abs (a.v[numBands + 1] - b.v[numBands + 1]);
    if (t == DrumType::Kick || t == DrumType::Tom)
        d += 0.5f * std::abs (a.v[numBands + 2] - b.v[numBands + 2]);
    return d;
}

constexpr float groupThreshold = 5.0f;

//==============================================================================
// 1. ONSET DETECTION
struct OnsetResult
{
    std::vector<juce::int64> starts;
    std::vector<float> odf;
    double frameRate = 0.0;
};

OnsetResult detectOnsets (const std::vector<float>& mono, double sr, float sensitivity, const std::atomic<bool>* cancel)
{
    OnsetResult res;
    const int order = sr > 60000.0 ? 11 : 10;
    const int N = 1 << order, hop = N / 8;
    const juce::int64 len = (juce::int64) mono.size();
    res.frameRate = sr / hop;

    const int numFrames = (int) juce::jmax ((juce::int64) 0, (len + hop - 1) / hop);
    if (numFrames < 8) return res;

    // 24 bande logaritmiche 40 Hz .. 16 kHz
    constexpr int B = 24;
    const double fMax = juce::jmin (16000.0, sr * 0.45);
    std::vector<int> edge (B + 1);
    for (int b = 0; b <= B; ++b)
    {
        const double f = 40.0 * std::pow (fMax / 40.0, (double) b / B);
        edge[(size_t) b] = juce::jlimit (1, N / 2, (int) std::round (f * N / sr));
        if (b > 0 && edge[(size_t) b] <= edge[(size_t) b - 1]) edge[(size_t) b] = juce::jmin (N / 2, edge[(size_t) b - 1] + 1);
    }

    RealFFT fft (order);
    const auto win = periodicHann (N);
    std::vector<float> frame ((size_t) N), pw ((size_t) N / 2 + 1);
    std::vector<float> level ((size_t) numFrames * B);
    float globalMax = -200.0f;

    for (int t = 0; t < numFrames; ++t)
    {
        if ((t & 255) == 0 && cancelled (cancel)) return res;
        // il frame t è centrato sul campione t*hop
        const juce::int64 off = (juce::int64) t * hop - N / 2;
        for (int i = 0; i < N; ++i) frame[(size_t) i] = sampleAt (mono, off + i) * win[(size_t) i];
        fft.power (frame.data(), pw.data());
        for (int b = 0; b < B; ++b)
        {
            double e = 0.0;
            for (int k = edge[(size_t) b]; k < juce::jmax (edge[(size_t) b] + 1, edge[(size_t) b + 1]); ++k) e += pw[(size_t) k];
            const float db = 10.0f * std::log10 ((float) e + eps);
            level[(size_t) (t * B + b)] = db;
            globalMax = juce::jmax (globalMax, db);
        }
    }

    const float floorDb = globalMax - 70.0f;
    for (auto& l : level) l = juce::jmax (l, floorDb);

    // flusso spettrale positivo con lag (confronto con 2-3 frame prima), pesato "percussivo":
    // un colpo alza tante bande insieme, una voce o un accordo poche
    res.odf.assign ((size_t) numFrames, 0.0f);
    auto levelAt = [&] (int t, int b) { return t >= 0 ? level[(size_t) (t * B + b)] : floorDb; };
    // tre regioni (basse < 200 Hz, medie, alte > 3 kHz) pesate allo stesso modo:
    // così un hat (solo alte) conta quanto un kick
    int region[B]; int regionBands[3] = { 0, 0, 0 };
    for (int b = 0; b < B; ++b)
    {
        const double fc = 40.0 * std::pow (fMax / 40.0, (b + 0.5) / B);
        region[b] = fc < 200.0 ? 0 : (fc < 3000.0 ? 1 : 2);
        ++regionBands[region[b]];
    }
    for (int t = 0; t < numFrames; ++t)
    {
        float sum[3] = { 0, 0, 0 }; int rising[3] = { 0, 0, 0 };
        for (int b = 0; b < B; ++b)
        {
            const float prev = juce::jmax (levelAt (t - 2, b), levelAt (t - 3, b));
            const float d = level[(size_t) (t * B + b)] - prev;
            if (d > 0.0f) { sum[region[b]] += d; if (d > 2.0f) ++rising[region[b]]; }
        }
        float v = 0.0f;
        for (int r = 0; r < 3; ++r)
            if (regionBands[r] > 0)
                v += sum[r] / (float) regionBands[r] * (0.4f + (float) rising[r] / (float) regionBands[r]);
        res.odf[(size_t) t] = v;
    }

    // normalizza al 99.5° percentile
    {
        std::vector<float> sorted (res.odf);
        std::sort (sorted.begin(), sorted.end());
        const float ref = juce::jmax (1.0e-6f, sorted[(size_t) ((sorted.size() - 1) * 995 / 1000)]);
        for (auto& o : res.odf) o = juce::jmin (1.5f, o / ref);
    }

    // peak picking con soglia adattiva
    const int wMax  = juce::jmax (2, (int) std::round (0.025 * res.frameRate));
    const int wPre  = juce::jmax (4, (int) std::round (0.12 * res.frameRate));
    const int wPost = juce::jmax (2, (int) std::round (0.03 * res.frameRate));
    const float delta = juce::jmap (juce::jlimit (0.0f, 1.0f, sensitivity), 0.30f, 0.035f);

    std::vector<int> peaks;
    for (int t = 0; t < numFrames; ++t)
    {
        const float v = res.odf[(size_t) t];
        if (v < 0.02f) continue;
        bool isMax = true;
        for (int j = juce::jmax (0, t - wMax); j <= juce::jmin (numFrames - 1, t + wMax) && isMax; ++j)
            if (res.odf[(size_t) j] > v || (j < t && ! (res.odf[(size_t) j] < v))) isMax = false;
        if (! isMax) continue;
        double s = 0.0; int n = 0;
        for (int j = juce::jmax (0, t - wPre); j <= juce::jmin (numFrames - 1, t + wPost); ++j) { s += res.odf[(size_t) j]; ++n; }
        if (v >= (float) (s / juce::jmax (1, n)) + delta)
            peaks.push_back (t);
    }

    // 2. inizio preciso al campione: picco dell'inviluppo vicino al frame, poi indietro fino all'attacco
    std::vector<float> env (mono.size());
    {
        const float rel = std::exp (-1.0f / (float) (0.004 * sr));
        // pre-enfasi: toglie il basso (che altrimenti "sposta" l'attacco), lascia i transienti
        float e = 0.0f, prev = 0.0f;
        for (size_t i = 0; i < mono.size(); ++i)
        {
            const float hp = mono[i] - 0.95f * prev; prev = mono[i];
            e = juce::jmax (std::abs (hp), e * rel); env[i] = e;
        }
    }
    const juce::int64 maxBack = (juce::int64) (0.02 * sr);
    const juce::int64 preRoll = (juce::int64) (0.0003 * sr);

    std::vector<juce::int64> starts;
    for (int t : peaks)
    {
        const juce::int64 c = (juce::int64) t * hop;
        const juce::int64 a = juce::jlimit ((juce::int64) 0, len - 1, c - N / 2);
        const juce::int64 b = juce::jlimit ((juce::int64) 0, len - 1, c + N / 2);
        juce::int64 p = a;
        for (juce::int64 i = a; i <= b; ++i) if (env[(size_t) i] > env[(size_t) p]) p = i;

        float base = env[(size_t) p];
        for (juce::int64 i = juce::jmax ((juce::int64) 0, p - maxBack); i < p; ++i) base = juce::jmin (base, env[(size_t) i]);
        const float thr = base + 0.15f * (env[(size_t) p] - base);
        juce::int64 s = p;
        while (s > 0 && s > p - maxBack && env[(size_t) s - 1] > thr) --s;
        starts.push_back (juce::jmax ((juce::int64) 0, s - preRoll));
    }

    std::sort (starts.begin(), starts.end());
    const juce::int64 minGap = (juce::int64) (0.02 * sr);
    for (auto s : starts)
        if (res.starts.empty() || s - res.starts.back() >= minGap)
            res.starts.push_back (s);

    return res;
}

//==============================================================================
// 3. CARATTERISTICHE E CLASSIFICAZIONE
void computeFeatures (Hit& h, const std::vector<float>& mono, double sr, RealFFT& fft,
                      const std::vector<float>& hitWin, const std::vector<float>& preWin)
{
    const int Nf = fft.size;
    std::vector<float> seg ((size_t) Nf), pHit ((size_t) Nf / 2 + 1), pPre ((size_t) Nf / 2 + 1);

    for (int i = 0; i < Nf; ++i)
    {
        const juce::int64 idx = h.start + i;
        seg[(size_t) i] = idx < h.nextOnset ? sampleAt (mono, idx) * hitWin[(size_t) i] : 0.0f;
    }
    fft.power (seg.data(), pHit.data());
    for (int i = 0; i < Nf; ++i)
        seg[(size_t) i] = sampleAt (mono, h.start - Nf + i) * preWin[(size_t) i];
    fft.power (seg.data(), pPre.data());

    static constexpr double bandHz[numBands + 1] = { 0, 90, 180, 350, 800, 2000, 5000, 9000, 1.0e9 };
    double E[numBands] {}, Ep[numBands] {}, tot = 0.0, preTot = 0.0, cNum = 0.0;
    const double binHz = sr / Nf;
    float lowPeak = 0.0f; int lowPeakBin = 1;
    double fNum = 0.0, fLogSum = 0.0; int fCount = 0;

    for (int k = 1; k <= Nf / 2; ++k)
    {
        const double f = k * binHz;
        if (f < 20.0) continue;
        const float nw = juce::jmax (0.0f, pHit[(size_t) k] - 2.0f * pPre[(size_t) k]); // margine 3 dB: ignora ciò che era già lì
        preTot += pPre[(size_t) k];
        tot += nw;
        cNum += nw * f;
        int b = 0;
        while (b < numBands - 1 && f >= bandHz[b + 1]) ++b;
        E[b] += nw;
        Ep[b] += pPre[(size_t) k];
        if (f >= 25.0 && f <= 1000.0 && nw > lowPeak) { lowPeak = nw; lowPeakBin = k; }
        if (f >= 1000.0 && f <= juce::jmin (10000.0, sr * 0.45))
        {
            fNum += nw + 1.0e-10; fLogSum += std::log (nw + 1.0e-10); ++fCount;
        }
    }

    h.strength    = (float) std::sqrt (tot);
    h.cleanliness = (float) (10.0 * std::log10 ((tot + 1.0e-12) / (preTot + 1.0e-12)));
    h.centroid    = tot > 0.0 ? (float) (cNum / tot) : 0.0f;
    h.lowPeakHz   = (float) (lowPeakBin * binHz);
    h.flatness    = fCount > 0 ? (float) (std::exp (fLogSum / fCount) / (fNum / fCount)) : 0.0f;
    // conta solo le bande dove il colpo ha davvero portato energia nuova (salto ≥ 4 dB):
    // un cambio di nota del basso sotto un hat non deve farlo sembrare un kick
    double kept[numBands], keptTot = 0.0;
    for (int b = 0; b < numBands; ++b)
    {
        const double jump = 10.0 * std::log10 ((E[b] + 1.0e-12) / (Ep[b] + 1.0e-12));
        kept[b] = jump >= 4.0 ? E[b] : E[b] * 0.05;
        keptTot += kept[b];
    }
    for (int b = 0; b < numBands; ++b)
        h.bands[b] = juce::jmax (-40.0f, (float) (10.0 * std::log10 (kept[b] / juce::jmax (keptTot, 1.0e-12) + 1.0e-4)));

    // decadimento: inviluppo di energia liscio (10 ms) campionato ogni 2 ms.
    // Per i suoni acuti (hat, piatti) si misura sul segnale passa-alto, così il basso sotto non conta.
    // Se c'è un "fondo" si misura la discesa fino a metà strada verso il fondo e si estrapola ai 24 dB.
    {
        double hiShare = 0.0;
        for (int b = 4; b < numBands; ++b) hiShare += kept[b];
        hiShare /= juce::jmax (keptTot, 1.0e-12);
        const bool useHp = hiShare > 0.5;
        auto sig = [&] (juce::int64 i)
        {
            return useHp ? sampleAt (mono, i) - 2.0f * sampleAt (mono, i - 1) + sampleAt (mono, i - 2) : sampleAt (mono, i);
        };

        const double a = std::exp (-1.0 / (0.01 * sr));
        const int step = juce::jmax (8, (int) (0.002 * sr));
        double bg = 0.0; const int bgN = (int) (0.03 * sr);
        for (int i = 1; i <= bgN; ++i) { const float v = sig (h.start - i); bg += v * v; }
        const float bgDb = (float) (10.0 * std::log10 (bg / juce::jmax (1, bgN) + 1.0e-14));

        const juce::int64 end = juce::jmin (h.nextOnset, h.start + (juce::int64) (2.5 * sr), (juce::int64) mono.size());
        std::vector<float> lv;
        double e = bg / juce::jmax (1, bgN);
        for (juce::int64 p = h.start; p < end; ++p)
        {
            const float v = sig (p);
            e = a * e + (1.0 - a) * v * v;
            if ((p - h.start) % step == 0) lv.push_back ((float) (10.0 * std::log10 (e + 1.0e-14)));
        }
        const double stepMs = 1000.0 * step / sr;
        if (lv.size() < 3) h.decayMs = 50.0f;
        else
        {
            const int peakSearch = juce::jmin ((int) lv.size(), juce::jmax (1, (int) (0.04 * sr / step)));
            int pk = 0;
            for (int i = 1; i < peakSearch; ++i) if (lv[(size_t) i] > lv[(size_t) pk]) pk = i;
            const float peak = lv[(size_t) pk];
            float drop = 24.0f;
            if (bgDb > peak - 27.0f) drop = juce::jmax (3.0f, 0.5f * (peak - bgDb));
            int below = -1;
            for (int i = pk + 1; i < (int) lv.size(); ++i) if (lv[(size_t) i] < peak - drop) { below = i; break; }
            if (below >= 0) h.decayMs = (float) ((below - pk) * stepMs * 24.0 / drop);
            else
            {
                const int last = (int) lv.size() - 1;
                const float d = peak - lv[(size_t) last];
                const double ms = juce::jmax (1, last - pk) * stepMs;
                h.decayMs = d >= 2.0f ? (float) (ms * 24.0 / d) : 3000.0f;
            }
            h.decayMs = juce::jlimit (5.0f, 5000.0f, h.decayMs);
        }
    }

    // impulsi ravvicinati (clap): RMS in blocchi da 1 ms nei primi 40 ms
    {
        const int blk = juce::jmax (8, (int) (0.001 * sr));
        const juce::int64 end = juce::jmin (h.nextOnset, h.start + (juce::int64) (0.04 * sr), (juce::int64) mono.size());
        std::vector<float> lv;
        for (juce::int64 p = h.start; p + blk <= end; p += blk)
        {
            double s = 0.0;
            for (int i = 0; i < blk; ++i)
            {
                // passa-alto (differenza seconda): conta i burst di rumore, non le onde del kick
                const float v = mono[(size_t) (p + i)] - 2.0f * sampleAt (mono, p + i - 1) + sampleAt (mono, p + i - 2);
                s += v * v;
            }
            lv.push_back ((float) std::sqrt (s / blk));
        }
        int count = 0;
        if (! lv.empty())
        {
            const float mx = *std::max_element (lv.begin(), lv.end());
            float lastPeak = -1.0f, minSince = 1.0e9f;
            for (size_t i = 0; i < lv.size(); ++i)
            {
                minSince = juce::jmin (minSince, lv[i]);
                const bool localMax = (i == 0 || lv[i] >= lv[i - 1]) && (i + 1 == lv.size() || lv[i] > lv[i + 1]);
                if (localMax && lv[i] > 0.25f * mx)
                {
                    if (lastPeak < 0.0f || minSince < 0.6f * juce::jmin (lastPeak, lv[i]))
                    {
                        ++count; lastPeak = lv[i]; minSince = lv[i];
                    }
                    else lastPeak = juce::jmax (lastPeak, lv[i]);
                }
            }
        }
        h.bursts = juce::jmax (1, count);
    }
}

DrumType classify (const Hit& h)
{
    float r[numBands];
    for (int b = 0; b < numBands; ++b) r[b] = juce::jmax (0.0f, std::pow (10.0f, h.bands[b] / 10.0f) - 1.0e-4f);
    const float low = r[0] + r[1], body = r[2] + r[3], mid = r[4] + r[5], high = r[6] + r[7];

    if (h.bursts >= 3 && mid + high >= 0.3f) return DrumType::Clap;
    if (high >= 0.45f && low < 0.2f && body < 0.3f)
    {
        if (h.decayMs <= 150.0f) return DrumType::HatClosed;
        if (h.decayMs <= 700.0f) return DrumType::HatOpen;
        return DrumType::Cymbal;
    }
    if (low >= 0.5f && h.lowPeakHz < 160.0f)           return DrumType::Kick;
    if (mid + high >= 0.08f && h.flatness >= 0.1f)
    {
        if (h.bursts >= 3 || (h.bursts >= 2 && body < 0.25f && h.decayMs < 400.0f)) return DrumType::Clap;
        return DrumType::Snare;
    }
    if (low + body >= 0.75f)                           return (low > body && h.lowPeakHz < 100.0f) ? DrumType::Kick : DrumType::Tom;
    if (low >= 0.4f) return DrumType::Kick;
    return DrumType::Perc;
}

//==============================================================================
// 4. RAGGRUPPAMENTO
void buildGroups (Analysis& a, int maxPads)
{
    std::vector<FeatVec> fv;
    fv.reserve (a.hits.size());
    for (auto& h : a.hits) fv.push_back (featuresOf (h));

    struct Cluster { DrumType type; FeatVec centre; std::vector<int> hits; float maxStrength = 0.0f; };
    std::vector<Cluster> clusters;

    for (int t = 0; t < numDrumTypes; ++t)
    {
        std::vector<int> idx;
        for (int i = 0; i < (int) a.hits.size(); ++i) if ((int) a.hits[(size_t) i].type == t) idx.push_back (i);
        if (idx.empty()) continue;
        // prima i colpi più forti: fanno da "leader" dei gruppi
        std::sort (idx.begin(), idx.end(), [&] (int x, int y) { return a.hits[(size_t) x].strength > a.hits[(size_t) y].strength; });

        std::vector<Cluster> cl;
        for (int i : idx)
        {
            int best = -1; float bd = 1.0e9f;
            for (int c = 0; c < (int) cl.size(); ++c)
            {
                const float d = distance (fv[(size_t) i], cl[(size_t) c].centre, (DrumType) t);
                if (d < bd) { bd = d; best = c; }
            }
            if (best >= 0 && bd < groupThreshold)
            {
                auto& c = cl[(size_t) best];
                const float n = (float) c.hits.size();
                for (int k = 0; k < numBands + 3; ++k) c.centre.v[k] = (c.centre.v[k] * n + fv[(size_t) i].v[k]) / (n + 1.0f);
                c.hits.push_back (i);
                c.maxStrength = juce::jmax (c.maxStrength, a.hits[(size_t) i].strength);
            }
            else
            {
                Cluster c { (DrumType) t, fv[(size_t) i], { i }, a.hits[(size_t) i].strength };
                cl.push_back (c);
            }
        }

        // unisce gruppi troppo vicini
        bool merged = true;
        while (merged && cl.size() > 1)
        {
            merged = false;
            for (size_t x = 0; x < cl.size() && ! merged; ++x)
                for (size_t y = x + 1; y < cl.size() && ! merged; ++y)
                    if (distance (cl[x].centre, cl[y].centre, (DrumType) t) < groupThreshold * 0.8f)
                    {
                        const float nx = (float) cl[x].hits.size(), ny = (float) cl[y].hits.size();
                        for (int k = 0; k < numBands + 3; ++k) cl[x].centre.v[k] = (cl[x].centre.v[k] * nx + cl[y].centre.v[k] * ny) / (nx + ny);
                        cl[x].hits.insert (cl[x].hits.end(), cl[y].hits.begin(), cl[y].hits.end());
                        cl[x].maxStrength = juce::jmax (cl[x].maxStrength, cl[y].maxStrength);
                        cl.erase (cl.begin() + (std::ptrdiff_t) y);
                        merged = true;
                    }
        }

        // colpi isolati: si uniscono al gruppo più vicino, oppure restano solo se forti
        float typeMax = 0.0f;
        for (auto& c : cl) typeMax = juce::jmax (typeMax, c.maxStrength);
        for (size_t x = 0; x < cl.size();)
        {
            if (cl[x].hits.size() > 1 || cl.size() == 1) { ++x; continue; }
            int best = -1; float bd = 1.0e9f;
            for (size_t y = 0; y < cl.size(); ++y)
            {
                if (y == x || cl[y].hits.size() < 2) continue;
                const float d = distance (cl[x].centre, cl[y].centre, (DrumType) t);
                if (d < bd) { bd = d; best = (int) y; }
            }
            if (best >= 0 && bd < groupThreshold * 2.5f)
            {
                cl[(size_t) best].hits.push_back (cl[x].hits[0]);
                cl.erase (cl.begin() + (std::ptrdiff_t) x);
            }
            else if (cl[x].maxStrength < 0.35f * typeMax)
                cl.erase (cl.begin() + (std::ptrdiff_t) x);
            else
                ++x;
        }

        for (auto& c : cl) clusters.push_back (std::move (c));
    }

    // troppi gruppi: unisce i più piccoli al più vicino dello stesso tipo, altrimenti li scarta
    while ((int) clusters.size() > maxPads)
    {
        size_t smallest = 0;
        for (size_t i = 1; i < clusters.size(); ++i)
            if (clusters[i].hits.size() < clusters[smallest].hits.size()
                || (clusters[i].hits.size() == clusters[smallest].hits.size() && clusters[i].maxStrength < clusters[smallest].maxStrength))
                smallest = i;
        int best = -1; float bd = 1.0e9f;
        for (size_t y = 0; y < clusters.size(); ++y)
        {
            if (y == smallest || clusters[y].type != clusters[smallest].type) continue;
            const float d = distance (clusters[smallest].centre, clusters[y].centre, clusters[y].type);
            if (d < bd) { bd = d; best = (int) y; }
        }
        if (best >= 0)
            clusters[(size_t) best].hits.insert (clusters[(size_t) best].hits.end(), clusters[smallest].hits.begin(), clusters[smallest].hits.end());
        clusters.erase (clusters.begin() + (std::ptrdiff_t) smallest);
    }

    std::stable_sort (clusters.begin(), clusters.end(), [] (const Cluster& x, const Cluster& y)
    {
        if (x.type != y.type) return (int) x.type < (int) y.type;
        return x.hits.size() > y.hits.size();
    });

    for (auto& h : a.hits) h.group = -1;
    a.groups.clear();
    for (auto& c : clusters)
    {
        Group g;
        g.type = c.type;
        g.hits = c.hits;
        std::sort (g.hits.begin(), g.hits.end());
        const int gi = (int) a.groups.size();
        for (int i : g.hits) a.hits[(size_t) i].group = gi;

        // medoid: tra i colpi più "puliti" (metà migliore), il più vicino al centro del gruppo
        std::vector<int> cand (g.hits);
        std::sort (cand.begin(), cand.end(), [&] (int x, int y) { return a.hits[(size_t) x].cleanliness > a.hits[(size_t) y].cleanliness; });
        cand.resize (juce::jmax ((size_t) 1, (cand.size() + 1) / 2));
        float bd = 1.0e9f;
        for (int i : cand)
        {
            const float d = distance (fv[(size_t) i], c.centre, c.type) - 0.05f * a.hits[(size_t) i].cleanliness;
            if (d < bd) { bd = d; g.medoid = i; }
        }
        a.groups.push_back (std::move (g));
    }
}
} // namespace

//==============================================================================
// BPM
double estimateBpm (const std::vector<float>& odf, double frameRate, double durationSeconds)
{
    if (odf.size() < 64 || frameRate <= 0.0) return 90.0;
    std::vector<double> o (odf.begin(), odf.end());
    const double mean = std::accumulate (o.begin(), o.end(), 0.0) / (double) o.size();
    for (auto& v : o) v -= mean;

    const int lagMin = juce::jmax (1, (int) std::floor (frameRate * 60.0 / 200.0));
    const int lagMax = juce::jmin ((int) o.size() / 2, (int) std::ceil (frameRate * 60.0 / 50.0));
    if (lagMax <= lagMin + 2) return 90.0;

    auto R = [&] (int lag)
    {
        double s = 0.0;
        for (size_t t = 0; t + (size_t) lag < o.size(); ++t) s += o[t] * o[t + (size_t) lag];
        return s / (double) (o.size() - (size_t) lag);
    };
    std::vector<double> r ((size_t) lagMax + 2, 0.0);
    for (int l = lagMin - 1; l <= lagMax + 1; ++l) if (l > 0 && l < (int) o.size()) r[(size_t) l] = R (l);

    int best = lagMin; double bestScore = -1.0e30;
    for (int l = lagMin; l <= lagMax; ++l)
    {
        const double bpm = 60.0 * frameRate / l;
        const double w = std::exp (-0.5 * std::pow (std::log2 (bpm / 95.0) / 0.9, 2.0)); // preferenza hip hop
        const double s = r[(size_t) l] * w;
        if (s > bestScore) { bestScore = s; best = l; }
    }
    double lag = best;
    if (best > lagMin && best < lagMax)
    {
        const double y0 = r[(size_t) best - 1], y1 = r[(size_t) best], y2 = r[(size_t) best + 1];
        const double den = y0 - 2.0 * y1 + y2;
        if (std::abs (den) > 1.0e-12) lag += juce::jlimit (-0.5, 0.5, 0.5 * (y0 - y2) / den);
    }
    double bpm = 60.0 * frameRate / lag;
    while (bpm < 70.0) bpm *= 2.0;
    while (bpm >= 140.0) bpm *= 0.5;

    // loop: se la durata è quasi un numero intero di battute, aggancia il BPM esatto
    if (durationSeconds > 1.0 && durationSeconds < 60.0)
    {
        for (double b : { bpm, bpm * 2.0, bpm * 0.5 })
        {
            const double beats = durationSeconds * b / 60.0;
            const double nb = std::round (beats / 4.0) * 4.0;
            if (nb >= 4.0 && std::abs (beats - nb) / nb < 0.03)
            {
                bpm = nb * 60.0 / durationSeconds;
                while (bpm < 70.0) bpm *= 2.0;
                while (bpm >= 140.0) bpm *= 0.5;
                break;
            }
        }
    }
    return std::round (bpm * 100.0) / 100.0;
}

//==============================================================================
Analysis analyse (const juce::AudioBuffer<float>& source, double sr, const Settings& settings, const std::atomic<bool>* cancel)
{
    Analysis a;
    a.sampleRate = sr;
    const int chans = juce::jlimit (1, 2, source.getNumChannels());
    a.source.setSize (chans, source.getNumSamples());
    for (int ch = 0; ch < chans; ++ch) a.source.copyFrom (ch, 0, source, ch, 0, source.getNumSamples());

    const auto mono = makeMono (a.source);
    if (mono.size() < 2048) return a;

    auto on = detectOnsets (mono, sr, settings.sensitivity, cancel);
    if (cancelled (cancel)) return a;

    a.bpm = estimateBpm (on.odf, on.frameRate, (double) mono.size() / sr);

    const int order = sr > 60000.0 ? 12 : 11;
    RealFFT fft (order);
    const int Nf = fft.size;
    std::vector<float> hitWin ((size_t) Nf), preWin ((size_t) Nf);
    for (int i = 0; i < Nf; ++i)
    {
        hitWin[(size_t) i] = 0.5f * (1.0f + std::cos (juce::MathConstants<float>::pi * (float) i / (float) Nf));
        preWin[(size_t) (Nf - 1 - i)] = hitWin[(size_t) i];
    }

    for (size_t i = 0; i < on.starts.size(); ++i)
    {
        if ((i & 63) == 0 && cancelled (cancel)) return a;
        Hit h;
        h.start = on.starts[i];
        h.nextOnset = i + 1 < on.starts.size() ? on.starts[i + 1] : (juce::int64) mono.size();
        computeFeatures (h, mono, sr, fft, hitWin, preWin);
        if (h.strength <= 1.0e-5f) continue; // niente di nuovo: falso positivo
        h.type = classify (h);
        a.hits.push_back (h);
    }

    // colpi molto più deboli degli altri dello stesso tipo: residui, non colpi veri
    {
        float maxStr[numDrumTypes] {};
        for (auto& h : a.hits) maxStr[(int) h.type] = juce::jmax (maxStr[(int) h.type], h.strength);
        a.hits.erase (std::remove_if (a.hits.begin(), a.hits.end(), [&] (const Hit& h)
                      { return h.strength < 0.12f * maxStr[(int) h.type]; }), a.hits.end());
    }

    buildGroups (a, juce::jmax (1, settings.maxPads));
    return a;
}

//==============================================================================
// 5. ONE-SHOT PULITO: allinea i colpi del gruppo e tiene la mediana spettrale
// (ciò che si ripete in tutti i colpi resta, voci/basso/altri strumenti spariscono).
// La fase e lo stereo vengono dal colpo migliore (medoid), quindi il suono resta naturale.
juce::AudioBuffer<float> renderGroup (const Analysis& a, int groupIndex, float clean)
{
    juce::AudioBuffer<float> out;
    if (groupIndex < 0 || groupIndex >= (int) a.groups.size()) return out;
    const auto& g = a.groups[(size_t) groupIndex];
    if (g.medoid < 0) return out;

    const double sr = a.sampleRate;
    const auto& src = a.source;
    const int C = src.getNumChannels();
    const juce::int64 total = src.getNumSamples();
    const auto mono = makeMono (src);
    const Hit& m = a.hits[(size_t) g.medoid];

    const int L = (int) juce::jmin ((juce::int64) (typeMaxSeconds (g.type) * sr), total - m.start);
    if (L < 64) return out;

    // colpi da usare: i più puliti, massimo 40
    std::vector<int> use (g.hits);
    std::sort (use.begin(), use.end(), [&] (int x, int y) { return a.hits[(size_t) x].cleanliness > a.hits[(size_t) y].cleanliness; });
    if (use.size() > 40) use.resize (40);
    if (std::find (use.begin(), use.end(), g.medoid) == use.end()) use.back() = g.medoid;

    // allineamento fine (±1.5 ms) e livello rispetto al medoid
    const int A = juce::jmin (L, (int) (0.012 * sr));
    const int maxLag = (int) (0.0015 * sr);
    std::vector<juce::int64> starts;
    std::vector<float> gains;
    auto energy = [&] (juce::int64 s, int n) { double e = 0.0; for (int i = 0; i < n; ++i) { const float v = sampleAt (mono, s + i); e += v * v; } return e; };
    const double eRef = energy (m.start, (int) (0.03 * sr));
    for (int i : use)
    {
        const juce::int64 s0 = a.hits[(size_t) i].start;
        juce::int64 bestS = s0;
        if (i != g.medoid)
        {
            double best = -1.0e30;
            for (int lag = -maxLag; lag <= maxLag; ++lag)
            {
                double c = 0.0;
                for (int k = 0; k < A; k += 2) c += (double) sampleAt (mono, m.start + k) * sampleAt (mono, s0 + lag + k);
                if (c > best) { best = c; bestS = s0 + lag; }
            }
        }
        starts.push_back (bestS);
        const double e = energy (bestS, (int) (0.03 * sr));
        gains.push_back ((float) juce::jlimit (0.25, 4.0, std::sqrt ((eRef + 1.0e-12) / (e + 1.0e-12))));
    }
    const int medoidSlot = (int) (std::find (use.begin(), use.end(), g.medoid) - use.begin());

    const int order = sr > 60000.0 ? 11 : 10;
    RealFFT fft (order);
    const int N = fft.size, hop = N / 4, K = N / 2 + 1;
    const auto win = periodicHann (N);
    const int F = (L + N - hop) / hop + 1;
    const int H = (int) starts.size();

    auto segSample = [&] (const float* data, juce::int64 start, int n) -> float
    {
        if (n < 0 || n >= L) return 0.0f;
        const juce::int64 idx = start + n;
        return (idx >= 0 && idx < total) ? data[idx] : 0.0f;
    };

    // magnitudini (mono) di tutti i colpi
    std::vector<float> mags ((size_t) H * (size_t) F * (size_t) K);
    std::vector<float> frame ((size_t) N);
    std::vector<std::complex<float>> spec ((size_t) K);
    for (int h = 0; h < H; ++h)
        for (int f = 0; f < F; ++f)
        {
            const int off = f * hop - (N - hop);
            for (int n = 0; n < N; ++n) frame[(size_t) n] = segSample (mono.data(), starts[(size_t) h], off + n) * gains[(size_t) h] * win[(size_t) n];
            fft.forward (frame.data(), spec.data());
            float* dst = &mags[((size_t) h * (size_t) F + (size_t) f) * (size_t) K];
            for (int k = 0; k < K; ++k) dst[k] = std::abs (spec[(size_t) k]);
        }

    // fondo: spettro medio di ciò che suona appena prima di ogni colpo (basso, pad, coda del colpo prima)
    std::vector<float> preMed ((size_t) K, 0.0f);
    {
        std::vector<float> pre ((size_t) H * (size_t) K, 0.0f);
        const auto* md = mono.data();
        for (int h = 0; h < H; ++h)
            for (int j = 0; j < 4; ++j)
            {
                const juce::int64 off = starts[(size_t) h] - N - (juce::int64) (0.002 * sr) - (juce::int64) j * hop;
                for (int n = 0; n < N; ++n)
                {
                    const juce::int64 idx = off + n;
                    frame[(size_t) n] = (idx >= 0 && idx < total ? md[idx] : 0.0f) * gains[(size_t) h] * win[(size_t) n];
                }
                fft.forward (frame.data(), spec.data());
                for (int k = 0; k < K; ++k) pre[(size_t) h * (size_t) K + (size_t) k] += std::norm (spec[(size_t) k]) * 0.25f;
            }
        std::vector<float> col ((size_t) H);
        for (int k = 0; k < K; ++k)
        {
            for (int h = 0; h < H; ++h) col[(size_t) h] = pre[(size_t) h * (size_t) K + (size_t) k];
            preMed[(size_t) k] = median (col);
        }
    }

    // maschera = mediana / medoid (≤ 1) × sottrazione del fondo, ammorbidita dalla manopola PULIZIA
    std::vector<float> mask ((size_t) F * (size_t) K, 1.0f);
    if (clean > 0.001f)
    {
        for (int f = 0; f < F; ++f)
            for (int k = 0; k < K; ++k)
            {
                const float own = mags[((size_t) medoidSlot * (size_t) F + (size_t) f) * (size_t) K + (size_t) k];
                const float sub = 1.0f - 1.5f * preMed[(size_t) k] / (own * own + 1.0e-12f);
                mask[(size_t) f * (size_t) K + (size_t) k] = std::sqrt (juce::jmax (0.03f, sub));
            }
    }
    if (H >= 2 && clean > 0.001f)
    {
        std::vector<float> col ((size_t) H);
        for (int f = 0; f < F; ++f)
            for (int k = 0; k < K; ++k)
            {
                for (int h = 0; h < H; ++h) col[(size_t) h] = mags[((size_t) h * (size_t) F + (size_t) f) * (size_t) K + (size_t) k];
                const float own = col[(size_t) medoidSlot];
                float med;
                if (H == 2) med = juce::jmin (col[0], col[1]);
                else
                {
                    auto mid = col.begin() + (std::ptrdiff_t) ((H - 1) / 2);
                    std::nth_element (col.begin(), mid, col.end());
                    med = *mid;
                }
                const float gm = juce::jmin (1.0f, med / (own + 1.0e-9f));
                mask[(size_t) f * (size_t) K + (size_t) k] *= gm;
            }
    }

    if (clean > 0.001f && clean < 0.999f)
        for (auto& v : mask) v = std::pow (v, clean);

    // ricostruzione per canale dal medoid
    out.setSize (C, L);
    out.clear();
    std::vector<float> acc ((size_t) (L + 2 * N), 0.0f), tmp ((size_t) N);
    for (int ch = 0; ch < C; ++ch)
    {
        std::fill (acc.begin(), acc.end(), 0.0f);
        const float* data = src.getReadPointer (ch);
        for (int f = 0; f < F; ++f)
        {
            const int off = f * hop - (N - hop);
            for (int n = 0; n < N; ++n) frame[(size_t) n] = segSample (data, starts[(size_t) medoidSlot], off + n) * win[(size_t) n];
            fft.forward (frame.data(), spec.data());
            for (int k = 0; k < K; ++k) spec[(size_t) k] *= mask[(size_t) f * (size_t) K + (size_t) k];
            fft.inverse (spec.data(), tmp.data());
            for (int n = 0; n < N; ++n)
            {
                const int pos = off + n + N; // acc è spostato di N
                if (pos >= 0 && pos < (int) acc.size()) acc[(size_t) pos] += tmp[(size_t) n] * win[(size_t) n];
            }
        }
        auto* o = out.getWritePointer (ch);
        for (int n = 0; n < L; ++n) o[n] = acc[(size_t) (n + N)] / 1.5f; // somma di hann² con hop N/4
    }

    // coda: se il colpo successivo arriva sempre alla stessa distanza, sfuma da lì in poi
    // seguendo la pendenza del decadimento (così non si porta dietro hat/snare che seguono)
    {
        std::vector<float> gaps;
        for (size_t i = 0; i < use.size(); ++i)
            gaps.push_back ((float) (a.hits[(size_t) use[i]].nextOnset - a.hits[(size_t) use[i]].start));
        const float gap = median (gaps);
        const int blk = juce::jmax (8, (int) (0.005 * sr));

        auto blockDb = [&] (int p)
        {
            double s = 0.0; int n = 0;
            for (int ch = 0; ch < C; ++ch)
                for (int i = p; i < juce::jmin (L, p + blk); ++i) { const float v = out.getSample (ch, i); s += v * v; ++n; }
            return (float) (10.0 * std::log10 (s / juce::jmax (1, n) + 1.0e-14));
        };

        if (gap > 0.04f * (float) sr && gap < (float) L)
        {
            const int gp = (int) gap;
            int pkPos = 0; float pk = -200.0f;
            for (int p = 0; p < juce::jmin (gp, (int) (0.05 * sr)); p += blk) { const float d = blockDb (p); if (d > pk) { pk = d; pkPos = p; } }
            const float atGap = blockDb (juce::jmax (0, gp - blk));
            const float sec = (float) juce::jmax (1, gp - pkPos) / (float) sr;
            float slope = (pk - atGap) / sec;                         // dB al secondo
            if (pk - atGap > 18.0f) slope = juce::jmax (slope, 400.0f); // già quasi finito: chiudi in fretta
            slope = juce::jmax (slope, 40.0f);
            for (int ch = 0; ch < C; ++ch)
            {
                auto* o = out.getWritePointer (ch);
                for (int n = gp; n < L; ++n)
                    o[n] *= std::pow (10.0f, -slope * (float) (n - gp) / (float) sr / 20.0f);
            }
        }

        // taglio dove scende sotto -50 dB dal picco
        float pk = -200.0f;
        for (int p = 0; p < L; p += blk) pk = juce::jmax (pk, blockDb (p));
        int lastLoud = 0;
        for (int p = 0; p < L; p += blk) if (blockDb (p) > pk - 50.0f) lastLoud = p + blk;
        const int newLen = juce::jlimit (juce::jmin (L, 64), L, lastLoud + (int) (0.005 * sr));
        out.setSize (C, newLen, true, true, false);
    }

    // micro fade in/out e normalizzazione a -1 dBFS
    {
        const int n = out.getNumSamples();
        const int fi = juce::jmin (8, n / 4);
        const int fo = juce::jmin (n / 3, (int) ((g.type == DrumType::HatClosed ? 0.004 : 0.01) * sr));
        for (int ch = 0; ch < C; ++ch)
        {
            out.applyGainRamp (ch, 0, fi, 0.0f, 1.0f);
            out.applyGainRamp (ch, n - fo, fo, 1.0f, 0.0f);
        }
        const float mag = out.getMagnitude (0, n);
        if (mag > 1.0e-6f) out.applyGain (0.891f / mag);
    }
    return out;
}
} // namespace kr
