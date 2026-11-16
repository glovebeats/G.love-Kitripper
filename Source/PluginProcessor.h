#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "Analyzer.h"
#include <array>
#include <map>
#include <memory>
#include <mutex>

namespace kr
{
constexpr int maxPads = 32;          // 2 banchi da 16
constexpr int baseNote = 36;         // C1 in Logic = pad 1
constexpr int padsPerBank = 16;

//==============================================================================
struct Pad
{
    juce::AudioBuffer<float> audio;
    double sampleRate = 44100.0;
    DrumType type = DrumType::Perc;
    juce::String name;
    juce::String userFile;                 // se il suono è stato sostituito trascinando un file sul pad
    std::vector<juce::int64> hitStarts;    // posizioni nel file sorgente (campioni)
    std::vector<float> hitVel;             // 0..1
};

struct Kit
{
    std::vector<Pad> pads;
    double bpm = 90.0;
    double sourceRate = 44100.0;
    juce::int64 sourceLength = 0;
    juce::String sourceName;
    std::vector<float> overviewMin, overviewMax;            // forma d'onda per lo schermo
    std::vector<std::pair<juce::int64, int>> markers;       // inizio colpo → pad
};
using KitPtr = std::shared_ptr<const Kit>;
} // namespace kr

//==============================================================================
class KitRipperProcessor : public juce::AudioProcessor,
                           public juce::ChangeBroadcaster,
                           private juce::Timer
{
public:
    KitRipperProcessor();
    ~KitRipperProcessor() override;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "G.LOVE Kit Ripper"; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    //==========================================================================
    // usati dall'editor (message thread)
    void loadSource (const juce::File& f);           // nuova sorgente: analisi completa
    void reanalyse();                                // dopo aver cambiato la sensibilità
    void rerender();                                 // dopo aver cambiato la pulizia
    void setPadType (int pad, kr::DrumType t);
    bool replacePadSample (int pad, const juce::File& f);

    juce::File exportPadWav (int pad);               // per il drag verso Logic
    juce::File exportPadMidi (int pad);              // pad = -1 → tutto il pattern
    juce::File exportWholeKit();                     // tutti i WAV + MIDI in una cartella

    kr::KitPtr getKit() const { return std::atomic_load (&kit); }
    juce::File getSourceFile() const { return sourceFile; }
    bool isBusy() const { return busy.load(); }
    juce::String getStatus() const { const std::lock_guard<std::mutex> l (statusLock); return status; }

    double getEffectiveBpm() const;
    double getBpmOverride() const { return bpmOverride; }
    void setBpmOverride (double b) { bpmOverride = b; sendChangeMessage(); }
    bool getQuantize() const { return quantize; }
    void setQuantize (bool q) { quantize = q; }

    void triggerPreview (int pad, float velocity = 0.85f);
    int getPadFlash (int pad) const { return padFlash[(size_t) juce::jlimit (0, kr::maxPads - 1, pad)].load(); }

    static bool isAudioFile (const juce::File& f);

    juce::AudioProcessorValueTreeState params;
    juce::MidiKeyboardState keyboardState;

    static juce::String pid (const char* what, int pad) { return juce::String (what) + juce::String (pad + 1); }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createLayout();

    void timerCallback() override;
    void startJob (bool fullAnalysis);
    void setKit (kr::KitPtr k);
    void setStatus (const juce::String& s);
    kr::KitPtr buildKit (const kr::Analysis& a, std::vector<juce::AudioBuffer<float>>& rendered) const;
    void applyOverrides (kr::Kit& k) const;
    static void nameKit (kr::Kit& k);
    juce::File exportDir() const;

    // ---- voci del campionatore (thread audio)
    struct Voice
    {
        bool active = false;
        int pad = -1;
        double pos = 0.0, inc = 1.0, end = 0.0;
        float gl = 1.0f, gr = 1.0f;
        float fade = 1.0f, fadeStep = 0.0f;     // fadeStep > 0 = chiusura (choke)
        juce::uint32 age = 0;
    };
    void noteOn (int note, float velocity);
    void renderVoices (juce::AudioBuffer<float>& out, int start, int num);

    std::array<Voice, 48> voices;
    juce::uint32 voiceCounter = 0;
    double hostRate = 44100.0;
    kr::KitPtr kit, audioKit;
    std::vector<kr::KitPtr> retired;                 // liberati qui, mai dal thread audio
    std::mutex retiredLock;

    std::array<std::atomic<float>*, kr::maxPads> pVol {}, pTune {}, pLen {}, pPan {};
    std::array<std::atomic<int>, kr::maxPads> padFlash {};

    // ---- analisi in background
    juce::ThreadPool pool { 1 };
    std::atomic<bool> cancelFlag { false }, busy { false };
    std::atomic<int> generation { 0 };
    std::shared_ptr<kr::Analysis> analysis;          // protetto da analysisLock
    mutable std::mutex analysisLock;

    juce::File sourceFile;
    double bpmOverride = 0.0;                        // 0 = automatico
    bool quantize = false;
    std::map<int, int> typeOverrides;                // pad → DrumType scelto a mano
    std::map<int, juce::String> fileOverrides;       // pad → file trascinato sul pad
    mutable std::mutex overrideLock;

    mutable std::mutex statusLock;
    juce::String status { "Trascina qui un loop, un break o lo stem di batteria di Logic" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KitRipperProcessor)
};
