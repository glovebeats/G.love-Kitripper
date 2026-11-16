// Test del processor: carica un loop, aspetta l'analisi, suona i pad, esporta WAV e MIDI.
#include "../Source/PluginEditor.h"
#include <iostream>

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    auto proc = std::make_unique<KitRipperProcessor>();
    proc->setPlayConfigDetails (0, 2, 48000.0, 512);
    proc->prepareToPlay (48000.0, 512);
    proc->loadSource (juce::File (juce::String (argc > 1 ? argv[1] : "/tmp/kr_test_loop.wav")));
    for (int i = 0; i < 300 && proc->isBusy(); ++i) juce::Thread::sleep (20);
    std::cout << "stato: " << proc->getStatus() << "\n";
    auto kit = proc->getKit();
    std::cout << "pad: " << kit->pads.size() << "  bpm " << proc->getEffectiveBpm() << "\n";

    // suona ogni pad e misura il picco
    juce::AudioBuffer<float> buf (2, 512);
    for (int p = 0; p < (int) kit->pads.size(); ++p)
    {
        juce::MidiBuffer mb;
        mb.addEvent (juce::MidiMessage::noteOn (1, kr::baseNote + p, (juce::uint8) 110), 10);
        float peak = 0.0f;
        for (int b = 0; b < 40; ++b) { proc->processBlock (buf, mb); mb.clear(); peak = juce::jmax (peak, buf.getMagnitude (0, 512)); }
        auto wav = proc->exportPadWav (p);
        auto mid = proc->exportPadMidi (p);
        juce::MidiFile mf; juce::FileInputStream in (mid); mf.readFrom (in);
        int notes = 0; double last = 0;
        if (mf.getNumTracks() > 0) for (auto* e : *mf.getTrack (0)) { if (e->message.isNoteOn()) ++notes; last = e->message.getTimeStamp(); }
        std::cout << "  " << kit->pads[(size_t) p].name << " picco " << peak << "  wav " << wav.getFileName() << "  midi " << mid.getFileName()
                  << " note " << notes << " fine " << last / 960.0 << " beat\n";
    }
    auto all = proc->exportPadMidi (-1);
    std::cout << "tutto: " << all.getFullPathName() << "\n";

    // anteprima della finestra
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditor());
        auto* e = dynamic_cast<KitRipperEditor*> (ed.get());
        e->selectPad (0);
        auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 2.0f);
        juce::File out ("/tmp/kr_anteprima.png"); out.deleteFile();
        juce::FileOutputStream os (out); juce::PNGImageFormat png; png.writeImageToStream (img, os);
    }

    // stato salvato → ricaricato
    juce::MemoryBlock mb; proc->getStateInformation (mb);
    auto p2 = std::make_unique<KitRipperProcessor>();
    p2->setStateInformation (mb.getData(), (int) mb.getSize());
    for (int i = 0; i < 300 && p2->isBusy(); ++i) juce::Thread::sleep (20);
    std::cout << "ricaricato: " << p2->getKit()->pads.size() << " pad\n";
    return 0;
}
