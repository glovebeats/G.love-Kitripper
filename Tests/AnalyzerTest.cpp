// Test da riga di comando del motore di analisi.
// Uso: AnalyzerTest            → loop sintetico con risultati attesi
//      AnalyzerTest file.wav   → analizza un file vero e salva i pad in /tmp/kr_pads
#include "../Source/Analyzer.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <iostream>
#include <map>
#include <random>

namespace
{
constexpr double SR = 44100.0;
std::mt19937 rng (1234);
float noise() { static std::uniform_real_distribution<float> d (-1.0f, 1.0f); return d (rng); }

std::vector<float> kick (float f0, float f1, float dec)
{
    std::vector<float> v ((size_t) (SR * 0.8));
    double ph = 0.0;
    for (size_t i = 0; i < v.size(); ++i)
    {
        const double t = i / SR;
        const double f = f1 + (f0 - f1) * std::exp (-t / 0.03);
        ph += 2.0 * juce::MathConstants<double>::pi * f / SR;
        v[i] = (float) (std::sin (ph) * std::exp (-t / dec) + (i < 60 ? 0.3 * noise() : 0.0));
    }
    return v;
}
std::vector<float> snare()
{
    std::vector<float> v ((size_t) (SR * 0.5));
    float lp = 0.0f, hpPrev = 0.0f, hp = 0.0f;
    for (size_t i = 0; i < v.size(); ++i)
    {
        const double t = i / SR;
        const float n = noise();
        hp = 0.7f * (hp + n - hpPrev); hpPrev = n; // passa-alto leggero
        lp = lp + 0.5f * (hp - lp);
        v[i] = (float) (0.6 * lp * std::exp (-t / 0.09) + 0.5 * std::sin (2 * juce::MathConstants<double>::pi * 190 * t) * std::exp (-t / 0.05));
    }
    return v;
}
std::vector<float> hat (float dec)
{
    std::vector<float> v ((size_t) (SR * (dec * 6 + 0.02)));
    float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (size_t i = 0; i < v.size(); ++i)
    {
        const float n = noise();
        // passa-alto di 2° ordine ~7 kHz (biquad semplice)
        const float y = 0.48f * (n - 2 * x1 + x2) - (-0.55f * y1 + 0.2f * y2);
        x2 = x1; x1 = n; y2 = y1; y1 = y;
        v[i] = 0.5f * y * (float) std::exp (-(i / SR) / dec);
    }
    return v;
}
std::vector<float> clap()
{
    std::vector<float> v ((size_t) (SR * 0.4));
    float lp = 0, hpPrev = 0, hp = 0;
    for (size_t i = 0; i < v.size(); ++i)
    {
        const double t = i / SR;
        const float n = noise();
        hp = 0.8f * (hp + n - hpPrev); hpPrev = n;
        lp = lp + 0.6f * (hp - lp);
        double envl = 0.0;
        for (double b : { 0.0, 0.011, 0.022 }) if (t >= b) envl = juce::jmax (envl, std::exp (-(t - b) / 0.004));
        if (t >= 0.033) envl = juce::jmax (envl, 0.8 * std::exp (-(t - 0.033) / 0.12));
        v[i] = (float) (0.7 * lp * envl);
    }
    return v;
}

void add (std::vector<float>& mix, const std::vector<float>& s, double sec, float gain)
{
    const size_t o = (size_t) (sec * SR);
    for (size_t i = 0; i < s.size() && o + i < mix.size(); ++i) mix[o + i] += s[i] * gain;
}

float bestCorr (const juce::AudioBuffer<float>& pad, const std::vector<float>& truth)
{
    const int n = juce::jmin (pad.getNumSamples(), (int) truth.size(), (int) (0.15 * SR));
    float best = 0.0f;
    for (int lag = -40; lag <= 40; ++lag)
    {
        double xy = 0, xx = 0, yy = 0;
        for (int i = 0; i < n; ++i)
        {
            const int j = i + lag;
            const float x = pad.getSample (0, i), y = (j >= 0 && j < (int) truth.size()) ? truth[(size_t) j] : 0.0f;
            xy += x * y; xx += x * x; yy += y * y;
        }
        best = juce::jmax (best, (float) (xy / std::sqrt (xx * yy + 1e-20)));
    }
    return best;
}

void writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, double sr)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), sr, (unsigned) b.getNumChannels(), 24, {}, 0));
    if (w) w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

void printAnalysis (const kr::Analysis& a)
{
    std::cout << "BPM " << a.bpm << "  colpi " << a.hits.size() << "  gruppi " << a.groups.size() << "\n";
    for (size_t g = 0; g < a.groups.size(); ++g)
    {
        const auto& m = a.hits[(size_t) a.groups[g].medoid];
        std::cout << "  pad " << g + 1 << " " << kr::typeName (a.groups[g].type) << " x" << a.groups[g].hits.size()
                  << "  centroid " << (int) m.centroid << "Hz lowPk " << (int) m.lowPeakHz << " decay " << (int) m.decayMs
                  << "ms flat " << m.flatness << " bursts " << m.bursts << "\n";
    }
}
} // namespace

int main (int argc, char** argv)
{

    if (argc > 1)
    {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (juce::File (juce::String (argv[1]))));
        if (! r) { std::cout << "file non leggibile\n"; return 1; }
        juce::AudioBuffer<float> b ((int) juce::jmin (2u, r->numChannels), (int) r->lengthInSamples);
        r->read (&b, 0, b.getNumSamples(), 0, true, true);
        kr::Settings s; s.sensitivity = argc > 2 ? juce::String (argv[2]).getFloatValue() : 0.5f;
        auto a = kr::analyse (b, r->sampleRate, s);
        printAnalysis (a);
        auto dir = juce::File ("/tmp/kr_pads"); dir.deleteRecursively(); dir.createDirectory();
        for (int g = 0; g < (int) a.groups.size(); ++g)
            writeWav (dir.getChildFile (juce::String (g + 1).paddedLeft ('0', 2) + " " + kr::typeName (a.groups[(size_t) g].type) + ".wav"), kr::renderGroup (a, g, 1.0f), a.sampleRate);
        return 0;
    }

    // loop sintetico: 92 BPM, 4 battute, con basso e accordi sopra
    const double bpm = 92.0, beat = 60.0 / bpm;
    const int bars = 4;
    std::vector<float> mix ((size_t) (bars * 4 * beat * SR), 0.0f);
    const auto kA = kick (160, 48, 0.25f), kB = kick (260, 85, 0.12f), sn = snare(), hc = hat (0.02f), ho = hat (0.12f), cl = clap();

    std::map<std::string, int> expected;
    for (int bar = 0; bar < bars; ++bar)
    {
        const double b0 = bar * 4 * beat;
        add (mix, kA, b0, 0.9f);                     expected["KICK"]++;
        add (mix, kA, b0 + 2.5 * beat, 0.8f);        expected["KICK"]++;
        add (mix, kB, b0 + 1.75 * beat, 0.6f);       expected["KICK"]++;
        add (mix, sn, b0 + 1 * beat, 0.8f);          expected["SNARE"]++;
        add (mix, (bar % 2 == 1) ? cl : sn, b0 + 3 * beat, 0.8f); expected[(bar % 2 == 1) ? "CLAP" : "SNARE"]++;
        for (int e = 0; e < 8; ++e)
        {
            if (e == 7) { add (mix, ho, b0 + e * 0.5 * beat, 0.5f); expected["OPEN HAT"]++; }
            else        { add (mix, hc, b0 + e * 0.5 * beat, 0.4f); expected["HAT"]++; }
        }
    }
    // basso e accordo di "sample" sopra
    for (size_t i = 0; i < mix.size(); ++i)
    {
        const double t = i / SR;
        const double f = (std::fmod (t, 4 * beat) < 2 * beat) ? 55.0 : 49.0;
        mix[i] += (float) (0.25 * std::sin (2 * juce::MathConstants<double>::pi * f * t));
        double chord = 0.0;
        for (double cf : { 261.6, 311.1, 392.0, 466.2 }) chord += std::sin (2 * juce::MathConstants<double>::pi * cf * t + cf);
        mix[i] += (float) (0.04 * chord);
    }

    juce::AudioBuffer<float> buf (1, (int) mix.size());
    std::copy (mix.begin(), mix.end(), buf.getWritePointer (0));
    writeWav (juce::File ("/tmp/kr_test_loop.wav"), buf, SR);

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    kr::Settings s;
    auto a = kr::analyse (buf, SR, s);
    const auto t1 = juce::Time::getMillisecondCounterHiRes();
    printAnalysis (a);
    if (std::getenv ("KR_DEBUG"))
        for (auto& h : a.hits)
            std::cout << "  t=" << (h.start / SR) / beat << "beat " << kr::typeName (h.type) << " str " << h.strength << " clean " << h.cleanliness
                      << " b=[" << h.bands[0] << "," << h.bands[1] << "," << h.bands[2] << "," << h.bands[3] << "," << h.bands[4] << "," << h.bands[5] << "," << h.bands[6] << "," << h.bands[7]
                      << "] dec " << h.decayMs << " flat " << h.flatness << " bur " << h.bursts << " lowPk " << h.lowPeakHz << "\n";

    std::map<std::string, int> found;
    for (auto& h : a.hits) found[kr::typeName (h.type).toStdString()]++;
    std::cout << "\ntipo       atteso  trovato\n";
    for (auto& [k, v] : expected) std::cout << "  " << k << "\t" << v << "\t" << found[k] << "\n";
    for (auto& [k, v] : found) if (! expected.count (k)) std::cout << "  " << k << "\t0\t" << v << "  (in più)\n";

    // posizione: ogni colpo atteso dovrebbe avere un onset entro 3 ms
    std::cout << "\nanalisi " << (int) (t1 - t0) << " ms\n";
    std::cout << "BPM atteso 92 → " << a.bpm << "\n\n";

    auto dir = juce::File ("/tmp/kr_pads"); dir.deleteRecursively(); dir.createDirectory();
    for (int g = 0; g < (int) a.groups.size(); ++g)
    {
        auto pad = kr::renderGroup (a, g, 1.0f);
        auto raw = kr::renderGroup (a, g, 0.0f);
        const std::vector<float>* truth = nullptr;
        switch (a.groups[(size_t) g].type)
        {
            case kr::DrumType::Kick: truth = &kA; break;
            case kr::DrumType::Snare: truth = &sn; break;
            case kr::DrumType::HatClosed: truth = &hc; break;
            case kr::DrumType::HatOpen: truth = &ho; break;
            case kr::DrumType::Clap: truth = &cl; break;
            default: break;
        }
        float cPad = 0, cRaw = 0, cB = 0;
        if (truth) { cPad = bestCorr (pad, *truth); cRaw = bestCorr (raw, *truth); }
        if (a.groups[(size_t) g].type == kr::DrumType::Kick) { cB = bestCorr (pad, kB); }
        std::cout << "pad " << g + 1 << " " << kr::typeName (a.groups[(size_t) g].type) << " len " << (int) (1000.0 * pad.getNumSamples() / SR)
                  << "ms  somiglianza pulito " << cPad << "  grezzo " << cRaw << (cB > 0 ? "  (vs kick B " + std::to_string (cB) + ")" : "") << "\n";
        writeWav (dir.getChildFile (juce::String (g + 1) + " " + kr::typeName (a.groups[(size_t) g].type) + ".wav"), pad, SR);
    }
    return 0;
}
