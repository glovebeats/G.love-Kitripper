#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
constexpr double maxSourceSeconds = 60.0 * 12.0;   // brani fino a 12 minuti
constexpr double maxPadSampleSeconds = 12.0;

juce::AudioFormatManager& formats()
{
    static juce::AudioFormatManager fm;
    static std::once_flag once;
    std::call_once (once, [] { fm.registerBasicFormats(); });
    return fm;
}

bool readAudio (const juce::File& f, double maxSeconds, juce::AudioBuffer<float>& out, double& sr)
{
    std::unique_ptr<juce::AudioFormatReader> r (formats().createReaderFor (f));
    if (r == nullptr || r->lengthInSamples <= 0) return false;
    sr = r->sampleRate > 0 ? r->sampleRate : 44100.0;
    const int len = (int) juce::jmin (r->lengthInSamples, (juce::int64) (sr * maxSeconds));
    const int ch = (int) juce::jlimit (1u, 2u, r->numChannels);
    out.setSize (ch, len);
    out.clear();
    return r->read (&out, 0, len, 0, true, ch > 1);
}

bool writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, double sr)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    auto stream = std::make_unique<juce::FileOutputStream> (f);
    if (! stream->openedOk()) return false;
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (stream.get(), sr, (unsigned) b.getNumChannels(), 24, {}, 0));
    if (w == nullptr) return false;
    stream.release();
    return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
}

// Scrive in dir/<base>.<ext> senza mai sovrascrivere un file diverso già usato da Logic:
// se esiste con lo stesso contenuto lo riusa, altrimenti prova "<base> 2", "<base> 3", ...
juce::File writeUnique (const juce::File& dir, const juce::String& base, const juce::String& ext,
                        const std::function<bool (const juce::File&)>& writer)
{
    if (! dir.createDirectory()) return {};
    auto tmp = dir.getChildFile (".tmp_" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()) + ext);
    if (! writer (tmp)) { tmp.deleteFile(); return {}; }
    const auto clean = juce::File::createLegalFileName (base);
    for (int i = 1; i < 500; ++i)
    {
        auto target = dir.getChildFile (clean + (i > 1 ? " " + juce::String (i) : juce::String()) + ext);
        if (target.existsAsFile())
        {
            if (target.hasIdenticalContentTo (tmp)) { tmp.deleteFile(); return target; }
            continue;
        }
        if (tmp.moveFileTo (target)) return target;
        break;
    }
    tmp.deleteFile();
    return {};
}
} // namespace

//==============================================================================
KitRipperProcessor::KitRipperProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (*this, nullptr, "PARAMS", createLayout())
{
    for (int p = 0; p < kr::maxPads; ++p)
    {
        pVol[(size_t) p]  = params.getRawParameterValue (pid ("vol", p));
        pTune[(size_t) p] = params.getRawParameterValue (pid ("tune", p));
        pLen[(size_t) p]  = params.getRawParameterValue (pid ("len", p));
        pPan[(size_t) p]  = params.getRawParameterValue (pid ("pan", p));
        padFlash[(size_t) p].store (0);
    }
    std::atomic_store (&kit, std::make_shared<const kr::Kit>());
    startTimer (1000);
}

KitRipperProcessor::~KitRipperProcessor()
{
    stopTimer();
    cancelFlag = true;
    pool.removeAllJobs (true, 20000);
}

juce::AudioProcessorValueTreeState::ParameterLayout KitRipperProcessor::createLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout l;
    using P = juce::AudioParameterFloat;
    using R = juce::NormalisableRange<float>;
    l.add (std::make_unique<P> (juce::ParameterID { "clean", 1 }, "Pulizia", R (0.0f, 100.0f, 1.0f), 100.0f));
    l.add (std::make_unique<P> (juce::ParameterID { "sens", 1 }, "Sensibilita", R (0.0f, 100.0f, 1.0f), 50.0f));
    for (int p = 0; p < kr::maxPads; ++p)
    {
        const auto n = "Pad " + juce::String (p + 1) + " ";
        l.add (std::make_unique<P> (juce::ParameterID { pid ("vol", p), 1 }, n + "Volume", R (-36.0f, 12.0f, 0.1f), 0.0f));
        l.add (std::make_unique<P> (juce::ParameterID { pid ("tune", p), 1 }, n + "Tune", R (-24.0f, 24.0f, 0.01f), 0.0f));
        l.add (std::make_unique<P> (juce::ParameterID { pid ("len", p), 1 }, n + "Lunghezza", R (2.0f, 100.0f, 0.1f), 100.0f));
        l.add (std::make_unique<P> (juce::ParameterID { pid ("pan", p), 1 }, n + "Pan", R (-100.0f, 100.0f, 1.0f), 0.0f));
    }
    return l;
}

bool KitRipperProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto out = layouts.getMainOutputChannelSet();
    return out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono();
}

void KitRipperProcessor::prepareToPlay (double sampleRate, int)
{
    hostRate = sampleRate > 0 ? sampleRate : 44100.0;
    for (auto& v : voices) v.active = false;
    keyboardState.reset();
}

bool KitRipperProcessor::isAudioFile (const juce::File& f)
{
    return f.existsAsFile() && f.hasFileExtension ("wav;wave;aif;aiff;aifc;flac;mp3;m4a;aac;caf;ogg");
}

//==============================================================================
// AUDIO
void KitRipperProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals noDenormals;
    buffer.clear();
    const int n = buffer.getNumSamples();
    keyboardState.processNextMidiBuffer (midi, 0, n, true);

    auto current = std::atomic_load (&kit);
    if (current.get() != audioKit.get())
    {
        for (auto& v : voices) v.active = false;
        audioKit = current;   // il kit precedente è ancora in "retired": non viene liberato qui
    }

    int pos = 0;
    for (const auto meta : midi)
    {
        const int t = juce::jlimit (0, n, meta.samplePosition);
        if (t > pos) { renderVoices (buffer, pos, t - pos); pos = t; }
        const auto m = meta.getMessage();
        if (m.isNoteOn()) noteOn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isAllNotesOff() || m.isAllSoundOff()) for (auto& v : voices) v.active = false;
    }
    if (pos < n) renderVoices (buffer, pos, n - pos);
}

void KitRipperProcessor::noteOn (int note, float velocity)
{
    if (audioKit == nullptr) return;
    const int p = note - kr::baseNote;
    if (p < 0 || p >= (int) audioKit->pads.size() || p >= kr::maxPads) return;
    const auto& pad = audioKit->pads[(size_t) p];
    if (pad.audio.getNumSamples() < 2) return;

    // choke: un hat (chiuso o aperto) chiude gli open hat che stanno suonando
    if (pad.type == kr::DrumType::HatClosed || pad.type == kr::DrumType::HatOpen)
        for (auto& v : voices)
            if (v.active && v.pad >= 0 && v.pad < (int) audioKit->pads.size()
                && audioKit->pads[(size_t) v.pad].type == kr::DrumType::HatOpen && v.fadeStep <= 0.0f)
                v.fadeStep = 1.0f / (float) (0.006 * hostRate);

    Voice* v = nullptr;
    for (auto& cand : voices) if (! cand.active) { v = &cand; break; }
    if (v == nullptr)
    {
        v = &voices[0];
        for (auto& cand : voices) if (cand.age < v->age) v = &cand;
    }

    const float tune = pTune[(size_t) p]->load();
    const float vol  = juce::Decibels::decibelsToGain (pVol[(size_t) p]->load());
    const float pan  = pPan[(size_t) p]->load() / 100.0f;
    const float len  = pLen[(size_t) p]->load() / 100.0f;
    const float vel  = std::pow (juce::jlimit (0.0f, 1.0f, velocity), 1.3f);

    v->active = true;
    v->pad = p;
    v->pos = 0.0;
    v->inc = std::pow (2.0, tune / 12.0) * pad.sampleRate / hostRate;
    v->end = (double) pad.audio.getNumSamples() * len;
    v->fade = 1.0f;
    v->fadeStep = 0.0f;
    v->age = ++voiceCounter;
    if (pad.audio.getNumChannels() == 1)
    {
        const float a = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
        v->gl = std::cos (a) * juce::MathConstants<float>::sqrt2 * vol * vel;
        v->gr = std::sin (a) * juce::MathConstants<float>::sqrt2 * vol * vel;
    }
    else
    {
        v->gl = juce::jmin (1.0f, 1.0f - pan) * vol * vel;
        v->gr = juce::jmin (1.0f, 1.0f + pan) * vol * vel;
    }
    padFlash[(size_t) p].fetch_add (1);
}

void KitRipperProcessor::renderVoices (juce::AudioBuffer<float>& out, int start, int num)
{
    if (audioKit == nullptr) return;
    const int outCh = out.getNumChannels();
    float* L = out.getWritePointer (0);
    float* R = outCh > 1 ? out.getWritePointer (1) : nullptr;
    const double fadeLen = 0.004 * hostRate;

    for (auto& v : voices)
    {
        if (! v.active) continue;
        const auto& pad = audioKit->pads[(size_t) v.pad];
        const int len = pad.audio.getNumSamples();
        const float* s0 = pad.audio.getReadPointer (0);
        const float* s1 = pad.audio.getReadPointer (pad.audio.getNumChannels() > 1 ? 1 : 0);
        const double fadeIn = fadeLen * v.inc;

        for (int i = start; i < start + num; ++i)
        {
            if (v.pos >= v.end || v.pos >= (double) (len - 1) || v.fade <= 0.0f) { v.active = false; break; }
            const int idx = (int) v.pos;
            const float fr = (float) (v.pos - idx);
            const float a = s0[idx] + fr * (s0[idx + 1] - s0[idx]);
            const float b = s1[idx] + fr * (s1[idx + 1] - s1[idx]);
            float g = v.fade;
            const double toEnd = v.end - v.pos;
            if (v.end < (double) len && toEnd < fadeIn) g *= (float) (toEnd / fadeIn);   // LUNGHEZZA: chiusura morbida
            if (R != nullptr) { L[i] += a * v.gl * g; R[i] += b * v.gr * g; }
            else L[i] += 0.5f * (a * v.gl + b * v.gr) * g;
            v.pos += v.inc;
            if (v.fadeStep > 0.0f) v.fade -= v.fadeStep;
        }
    }
}

void KitRipperProcessor::triggerPreview (int pad, float velocity)
{
    keyboardState.noteOn (1, kr::baseNote + pad, velocity);
    keyboardState.noteOff (1, kr::baseNote + pad, 0.0f);
}

//==============================================================================
// ANALISI
void KitRipperProcessor::setStatus (const juce::String& s)
{
    { const std::lock_guard<std::mutex> l (statusLock); status = s; }
    sendChangeMessage();
}

void KitRipperProcessor::loadSource (const juce::File& f)
{
    if (! isAudioFile (f)) { setStatus ("Formato non supportato: " + f.getFileName()); return; }
    sourceFile = f;
    bpmOverride = 0.0;
    {
        const std::lock_guard<std::mutex> l (overrideLock);
        typeOverrides.clear();
        fileOverrides.clear();
    }
    startJob (true);
}

void KitRipperProcessor::reanalyse()
{
    if (sourceFile.existsAsFile()) startJob (true);
}

void KitRipperProcessor::rerender()
{
    bool have;
    { const std::lock_guard<std::mutex> l (analysisLock); have = analysis != nullptr; }
    if (have) startJob (false);
    else reanalyse();
}

void KitRipperProcessor::startJob (bool fullAnalysis)
{
    cancelFlag = true;
    pool.removeAllJobs (true, 10000);
    cancelFlag = false;
    const int gen = ++generation;
    busy = true;
    setStatus (fullAnalysis ? "Analizzo " + sourceFile.getFileName() + "..." : "Ricalcolo i suoni...");

    const auto file  = sourceFile;
    const float sens = params.getRawParameterValue ("sens")->load() / 100.0f;
    const float clean = params.getRawParameterValue ("clean")->load() / 100.0f;

    pool.addJob ([this, gen, fullAnalysis, file, sens, clean]
    {
        std::shared_ptr<kr::Analysis> a;
        if (fullAnalysis)
        {
            juce::AudioBuffer<float> buf;
            double sr = 44100.0;
            if (! readAudio (file, maxSourceSeconds, buf, sr))
            {
                busy = false;
                setStatus ("Non riesco a leggere " + file.getFileName());
                return;
            }
            kr::Settings s;
            s.sensitivity = sens;
            s.maxPads = kr::maxPads;
            a = std::make_shared<kr::Analysis> (kr::analyse (buf, sr, s, &cancelFlag));
            if (cancelFlag || gen != generation) return;
            const std::lock_guard<std::mutex> l (analysisLock);
            analysis = a;
        }
        else
        {
            const std::lock_guard<std::mutex> l (analysisLock);
            a = analysis;
        }
        if (a == nullptr) { busy = false; return; }

        std::vector<juce::AudioBuffer<float>> rendered;
        for (int g = 0; g < (int) a->groups.size() && g < kr::maxPads; ++g)
        {
            if (cancelFlag || gen != generation) return;
            rendered.push_back (kr::renderGroup (*a, g, clean));
        }
        auto k = buildKit (*a, rendered);
        if (cancelFlag || gen != generation) return;

        setKit (k);
        busy = false;
        if (k->pads.empty()) setStatus ("Nessun colpo trovato: alza la SENSIBILITA'");
        else
        {
            int hits = 0;
            for (auto& p : k->pads) hits += (int) p.hitStarts.size();
            setStatus (juce::String ((int) k->pads.size()) + " suoni da " + juce::String (hits) + " colpi  -  " + file.getFileName());
        }
    });
}

kr::KitPtr KitRipperProcessor::buildKit (const kr::Analysis& a, std::vector<juce::AudioBuffer<float>>& rendered) const
{
    auto k = std::make_shared<kr::Kit>();
    k->sourceRate = a.sampleRate;
    k->sourceLength = a.source.getNumSamples();
    k->sourceName = sourceFile.getFileNameWithoutExtension();
    k->bpm = a.bpm;

    for (int g = 0; g < (int) rendered.size(); ++g)
    {
        const auto& grp = a.groups[(size_t) g];
        kr::Pad p;
        p.audio = std::move (rendered[(size_t) g]);
        p.sampleRate = a.sampleRate;
        p.type = grp.type;

        // velocity: forza del colpo rispetto al 90° percentile del gruppo
        std::vector<float> str;
        for (int i : grp.hits) str.push_back (a.hits[(size_t) i].strength);
        auto sorted = str;
        std::sort (sorted.begin(), sorted.end());
        const float ref = juce::jmax (1.0e-6f, sorted[(size_t) ((sorted.size() - 1) * 9 / 10)]);
        for (size_t j = 0; j < grp.hits.size(); ++j)
        {
            p.hitStarts.push_back (a.hits[(size_t) grp.hits[j]].start);
            p.hitVel.push_back (juce::jlimit (0.0f, 1.0f, str[j] / ref));
            k->markers.push_back ({ a.hits[(size_t) grp.hits[j]].start, g });
        }
        k->pads.push_back (std::move (p));
    }

    // forma d'onda per lo schermo
    constexpr int cols = 1200;
    k->overviewMin.assign (cols, 0.0f);
    k->overviewMax.assign (cols, 0.0f);
    const int n = a.source.getNumSamples();
    if (n > 0)
        for (int c = 0; c < cols; ++c)
        {
            const int s0 = (int) ((juce::int64) n * c / cols), s1 = juce::jmax (s0 + 1, (int) ((juce::int64) n * (c + 1) / cols));
            float mn = 0.0f, mx = 0.0f;
            for (int ch = 0; ch < a.source.getNumChannels(); ++ch)
            {
                const auto r = a.source.findMinMax (ch, s0, juce::jmin (n, s1) - s0);
                mn = juce::jmin (mn, r.getStart());
                mx = juce::jmax (mx, r.getEnd());
            }
            k->overviewMin[(size_t) c] = mn;
            k->overviewMax[(size_t) c] = mx;
        }

    applyOverrides (*k);
    nameKit (*k);
    return k;
}

void KitRipperProcessor::applyOverrides (kr::Kit& k) const
{
    const std::lock_guard<std::mutex> l (overrideLock);
    for (auto& [p, t] : typeOverrides)
        if (p < (int) k.pads.size()) k.pads[(size_t) p].type = (kr::DrumType) t;
    for (auto& [p, path] : fileOverrides)
    {
        if (p < 0 || p >= kr::maxPads) continue;
        juce::AudioBuffer<float> b; double sr = 44100.0;
        while ((int) k.pads.size() <= p) k.pads.push_back (kr::Pad {});
        if (readAudio (juce::File (path), maxPadSampleSeconds, b, sr))
        {
            k.pads[(size_t) p].audio = std::move (b);
            k.pads[(size_t) p].sampleRate = sr;
            k.pads[(size_t) p].userFile = path;
        }
    }
}

void KitRipperProcessor::nameKit (kr::Kit& k)
{
    std::map<int, int> total, count;
    for (auto& p : k.pads) if (p.userFile.isEmpty()) total[(int) p.type]++;
    for (auto& p : k.pads)
    {
        if (p.userFile.isNotEmpty()) { p.name = juce::File (p.userFile).getFileNameWithoutExtension(); continue; }
        const int t = (int) p.type;
        p.name = kr::typeName (p.type) + (total[t] > 1 ? " " + juce::String (++count[t]) : juce::String());
    }
}

void KitRipperProcessor::setKit (kr::KitPtr k)
{
    auto old = std::atomic_exchange (&kit, k);
    {
        const std::lock_guard<std::mutex> l (retiredLock);
        if (old != nullptr) retired.push_back (std::move (old));
    }
    sendChangeMessage();
}

void KitRipperProcessor::timerCallback()
{
    const std::lock_guard<std::mutex> l (retiredLock);
    retired.erase (std::remove_if (retired.begin(), retired.end(),
                                   [] (const kr::KitPtr& p) { return p.use_count() <= 1; }),
                   retired.end());
}

void KitRipperProcessor::setPadType (int pad, kr::DrumType t)
{
    auto cur = getKit();
    if (cur == nullptr || pad < 0 || pad >= (int) cur->pads.size()) return;
    { const std::lock_guard<std::mutex> l (overrideLock); typeOverrides[pad] = (int) t; }
    auto k = std::make_shared<kr::Kit> (*cur);
    k->pads[(size_t) pad].type = t;
    nameKit (*k);
    setKit (k);
}

bool KitRipperProcessor::replacePadSample (int pad, const juce::File& f)
{
    auto cur = getKit();
    if (cur == nullptr || pad < 0 || pad >= kr::maxPads || ! isAudioFile (f)) return false;
    juce::AudioBuffer<float> b; double sr = 44100.0;
    if (! readAudio (f, maxPadSampleSeconds, b, sr)) return false;

    auto k = std::make_shared<kr::Kit> (*cur);
    while ((int) k->pads.size() <= pad)   // pad vuoto: si può riempire a mano
    {
        kr::Pad empty;
        empty.type = kr::DrumType::Perc;
        k->pads.push_back (std::move (empty));
    }
    auto& p = k->pads[(size_t) pad];
    p.audio = std::move (b);
    p.sampleRate = sr;
    p.userFile = f.getFullPathName();
    { const std::lock_guard<std::mutex> l (overrideLock); fileOverrides[pad] = p.userFile; }
    nameKit (*k);
    setKit (k);
    return true;
}

//==============================================================================
// EXPORT (drag verso Logic e cartella del kit)
// ~/Music/G.LOVE Kit Ripper/<nome sorgente>/  — cartella fissa: Logic può riferirsi ai file senza copiarli
juce::File KitRipperProcessor::exportDir() const
{
    auto k = getKit();
    auto name = k != nullptr && k->sourceName.isNotEmpty() ? k->sourceName : juce::String ("Kit");
    return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
               .getChildFile ("G.LOVE Kit Ripper").getChildFile (juce::File::createLegalFileName (name));
}

double KitRipperProcessor::getEffectiveBpm() const
{
    if (bpmOverride > 0.0) return bpmOverride;
    auto k = getKit();
    return k != nullptr ? k->bpm : 90.0;
}

juce::File KitRipperProcessor::exportPadWav (int pad)
{
    auto k = getKit();
    if (k == nullptr || pad < 0 || pad >= (int) k->pads.size()) return {};
    const auto& p = k->pads[(size_t) pad];
    if (p.audio.getNumSamples() == 0) return {};

    // il WAV esce come lo senti: tune e lunghezza applicati
    juce::AudioBuffer<float> out;
    const float tune = pTune[(size_t) pad]->load();
    const float len  = pLen[(size_t) pad]->load() / 100.0f;
    const float vol  = juce::Decibels::decibelsToGain (pVol[(size_t) pad]->load());
    const double ratio = std::pow (2.0, tune / 12.0);
    const int srcLen = juce::jmax (2, (int) ((float) p.audio.getNumSamples() * len));
    const int outLen = juce::jmax (1, (int) std::floor ((double) srcLen / ratio));
    out.setSize (p.audio.getNumChannels(), outLen);
    for (int ch = 0; ch < p.audio.getNumChannels(); ++ch)
    {
        if (std::abs (tune) < 0.005f) out.copyFrom (ch, 0, p.audio, ch, 0, juce::jmin (outLen, srcLen));
        else
        {
            juce::LagrangeInterpolator interp;
            interp.process (ratio, p.audio.getReadPointer (ch), out.getWritePointer (ch), outLen, srcLen, 0);
        }
    }
    out.applyGain (juce::jmin (vol, 1.12f));
    if (len < 0.999f)
    {
        const int fo = juce::jmin (outLen / 2, (int) (0.004 * p.sampleRate));
        for (int ch = 0; ch < out.getNumChannels(); ++ch) out.applyGainRamp (ch, outLen - fo, fo, 1.0f, 0.0f);
    }

    const double sr = p.sampleRate;
    return writeUnique (exportDir(), p.name, ".wav", [&] (const juce::File& f) { return writeWav (f, out, sr); });
}

juce::File KitRipperProcessor::exportPadMidi (int pad)
{
    auto k = getKit();
    if (k == nullptr || k->pads.empty() || pad >= (int) k->pads.size()) return {};

    const double bpm = getEffectiveBpm();
    const double sr = k->sourceRate;
    constexpr int ppq = 960;
    auto toTicks = [&] (juce::int64 sample)
    {
        double t = (double) sample / sr * bpm / 60.0 * ppq;
        if (quantize) t = std::round (t / (ppq / 4.0)) * (ppq / 4.0);
        return t;
    };

    juce::MidiMessageSequence seq;
    const juce::String trackName = pad < 0 ? k->sourceName + " PATTERN" : k->pads[(size_t) pad].name;
    seq.addEvent (juce::MidiMessage::textMetaEvent (3, trackName), 0.0);
    seq.addEvent (juce::MidiMessage::tempoMetaEvent ((int) std::round (60000000.0 / bpm)), 0.0);
    seq.addEvent (juce::MidiMessage::timeSignatureMetaEvent (4, 4), 0.0);

    int notes = 0;
    for (int p = 0; p < (int) k->pads.size(); ++p)
    {
        if (pad >= 0 && p != pad) continue;
        const auto& pd = k->pads[(size_t) p];
        const int note = kr::baseNote + p;
        for (size_t i = 0; i < pd.hitStarts.size(); ++i)
        {
            const double t = toTicks (pd.hitStarts[i]);
            const int vel = juce::jlimit (1, 127, (int) std::round (40.0f + 87.0f * pd.hitVel[i]));
            seq.addEvent (juce::MidiMessage::noteOn (1, note, (juce::uint8) vel), t);
            seq.addEvent (juce::MidiMessage::noteOff (1, note), t + ppq / 8.0);
            ++notes;
        }
    }
    if (notes == 0) return {};

    // fine traccia = fine della sorgente, così la region è lunga quanto il loop
    const double endTick = juce::jmax (seq.getEndTime() + 1.0, (double) k->sourceLength / sr * bpm / 60.0 * ppq);
    seq.addEvent (juce::MidiMessage::endOfTrack(), endTick);
    seq.sort();
    seq.updateMatchedPairs();

    juce::MidiFile mf;
    mf.setTicksPerQuarterNote (ppq);
    mf.addTrack (seq);

    auto dir = exportDir().getChildFile ("MIDI");
    return writeUnique (dir, trackName, ".mid", [&] (const juce::File& f)
    {
        f.deleteFile();
        juce::FileOutputStream os (f);
        return os.openedOk() && mf.writeTo (os, 1);
    });
}

juce::File KitRipperProcessor::exportWholeKit()
{
    auto k = getKit();
    if (k == nullptr || k->pads.empty()) return {};
    for (int p = 0; p < (int) k->pads.size(); ++p)
    {
        exportPadWav (p);
        exportPadMidi (p);
    }
    exportPadMidi (-1);
    auto dir = exportDir();
    dir.revealToUser();
    return dir;
}

//==============================================================================
// STATO (salvato nel progetto di Logic)
void KitRipperProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    juce::XmlElement root ("KitRipper");
    root.setAttribute ("version", 1);
    root.setAttribute ("source", sourceFile.getFullPathName());
    root.setAttribute ("bpmOverride", bpmOverride);
    root.setAttribute ("quantize", quantize);
    if (auto x = params.copyState().createXml()) root.addChildElement (x.release());
    auto* ov = root.createNewChildElement ("Overrides");
    {
        const std::lock_guard<std::mutex> l (overrideLock);
        for (auto& [p, t] : typeOverrides)
        {
            auto* e = ov->createNewChildElement ("Type");
            e->setAttribute ("pad", p); e->setAttribute ("type", t);
        }
        for (auto& [p, path] : fileOverrides)
        {
            auto* e = ov->createNewChildElement ("File");
            e->setAttribute ("pad", p); e->setAttribute ("path", path);
        }
    }
    copyXmlToBinary (root, dest);
}

void KitRipperProcessor::setStateInformation (const void* data, int size)
{
    auto root = getXmlFromBinary (data, size);
    if (root == nullptr || ! root->hasTagName ("KitRipper")) return;
    if (auto* x = root->getChildByName (params.state.getType()))
        params.replaceState (juce::ValueTree::fromXml (*x));
    bpmOverride = root->getDoubleAttribute ("bpmOverride", 0.0);
    quantize = root->getBoolAttribute ("quantize", false);
    {
        const std::lock_guard<std::mutex> l (overrideLock);
        typeOverrides.clear();
        fileOverrides.clear();
        if (auto* ov = root->getChildByName ("Overrides"))
        {
            for (auto* e : ov->getChildWithTagNameIterator ("Type"))
                typeOverrides[e->getIntAttribute ("pad")] = juce::jlimit (0, kr::numDrumTypes - 1, e->getIntAttribute ("type"));
            for (auto* e : ov->getChildWithTagNameIterator ("File"))
                fileOverrides[e->getIntAttribute ("pad")] = e->getStringAttribute ("path");
        }
    }
    const juce::File f (root->getStringAttribute ("source"));
    if (isAudioFile (f))
    {
        sourceFile = f;
        startJob (true);
    }
    else if (f != juce::File())
        setStatus ("File sorgente non trovato: " + f.getFileName());
}

//==============================================================================
juce::AudioProcessorEditor* KitRipperProcessor::createEditor() { return new KitRipperEditor (*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new KitRipperProcessor(); }
