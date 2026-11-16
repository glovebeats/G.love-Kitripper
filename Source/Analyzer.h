#pragma once

// G.LOVE Kit Ripper — motore di analisi
// Da un file audio (loop, break, stem di batteria o brano intero) ricava:
//  1. i colpi (onset) con l'inizio preciso al campione
//  2. il tipo di ogni colpo (kick, snare, clap, hat, open hat, tom, perc, crash)
//  3. i gruppi di colpi che sono "lo stesso suono"
//  4. per ogni gruppo un one-shot pulito (mediana spettrale dei colpi allineati)
//  5. il BPM stimato
// Nessuna dipendenza dalla GUI: si può testare da riga di comando.

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <vector>

namespace kr
{
enum class DrumType : int { Kick = 0, Snare, Clap, HatClosed, HatOpen, Tom, Perc, Cymbal, NumTypes };

constexpr int numDrumTypes = (int) DrumType::NumTypes;
juce::String typeName (DrumType t);       // "KICK", "SNARE", ...
juce::uint32 typeColourARGB (DrumType t); // colore del tipo (0xAARRGGBB)

constexpr int numBands = 8;

struct Hit
{
    juce::int64 start = 0;        // primo campione del colpo
    juce::int64 nextOnset = 0;    // inizio del colpo successivo (qualsiasi) o fine file
    float strength = 0.0f;        // energia "nuova" (radice) → velocity
    float cleanliness = 0.0f;     // energia nuova / energia già presente prima (dB)
    float bands[numBands] {};     // profilo in dB dell'energia nuova, relativo al totale
    float centroid = 0.0f;        // Hz
    float lowPeakHz = 0.0f;       // picco spettrale sotto 1 kHz
    float flatness = 0.0f;        // 0 = tonale, 1 = rumore
    float decayMs = 0.0f;         // tempo per scendere di 24 dB (stimato)
    int bursts = 1;               // impulsi ravvicinati nei primi 40 ms (clap)
    DrumType type = DrumType::Perc;
    int group = -1;               // indice del gruppo/pad, -1 = scartato
};

struct Group
{
    DrumType type = DrumType::Perc;
    std::vector<int> hits;        // indici in Analysis::hits, in ordine di tempo
    int medoid = -1;              // colpo più rappresentativo
};

struct Settings
{
    float sensitivity = 0.5f;     // 0..1: più alto = trova più colpi
    int maxPads = 32;
};

struct Analysis
{
    juce::AudioBuffer<float> source;   // audio originale (max 2 canali)
    double sampleRate = 44100.0;
    std::vector<Hit> hits;
    std::vector<Group> groups;         // già ordinati per tipo (kick, snare, ...) e numero di colpi
    double bpm = 90.0;
};

// Analisi completa (detect + classify + group + BPM). cancel può essere nullptr.
Analysis analyse (const juce::AudioBuffer<float>& source, double sampleRate,
                  const Settings& settings, const std::atomic<bool>* cancel = nullptr);

// One-shot pulito per un gruppo. clean: 0 = colpo migliore così com'è, 1 = solo ciò che si ripete.
juce::AudioBuffer<float> renderGroup (const Analysis& a, int groupIndex, float clean);

// Stima del BPM da una lista di inizi colpo (usata anche internamente)
double estimateBpm (const std::vector<float>& onsetFunction, double frameRate, double durationSeconds);
} // namespace kr
