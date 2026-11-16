#pragma once

#include "PluginProcessor.h"

namespace krui
{
const juce::Colour bg       { 0xff151617 };
const juce::Colour panel    { 0xff212225 };
const juce::Colour panelHi  { 0xff2c2d31 };
const juce::Colour screen   { 0xff0d0e0f };
const juce::Colour ink      { 0xffece6da };
const juce::Colour inkDim   { 0xff8d8a83 };
const juce::Colour accent   { 0xffff8a3d };

juce::Font font (float h, bool bold = false);

class LookAndFeel : public juce::LookAndFeel_V4
{
public:
    LookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool over, bool down) override;
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return font (12.0f, true); }
    void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool over, bool down) override;
};

// Etichetta che si trascina verso Logic (WAV o MIDI)
class DragChip : public juce::Component
{
public:
    DragChip (juce::String text, juce::Colour c) : label (std::move (text)), colour (c) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override { started = false; }
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseEnter (const juce::MouseEvent&) override { repaint(); }
    void mouseExit (const juce::MouseEvent&) override { repaint(); }
    std::function<void()> onDragOut;
    juce::String label;
    juce::Colour colour;
    bool enabled = true;
private:
    bool started = false;
};

class KnobWithLabel : public juce::Component
{
public:
    explicit KnobWithLabel (juce::String name);
    void resized() override;
    void paint (juce::Graphics&) override;
    juce::Slider slider;
    juce::String name;
};
} // namespace krui

//==============================================================================
class KitRipperEditor;

class PadComponent : public juce::Component
{
public:
    PadComponent (KitRipperEditor& e, int localIndex) : editor (e), local (localIndex) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    int padIndex() const;
    juce::Rectangle<int> midiChipArea() const;

    bool dropHover = false;
    float flash = 0.0f;
private:
    KitRipperEditor& editor;
    int local;
    bool dragStarted = false, fromMidiChip = false;
};

//==============================================================================
class WaveScreen : public juce::Component
{
public:
    explicit WaveScreen (KitRipperEditor& e) : editor (e) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    bool dropHover = false;
private:
    KitRipperEditor& editor;
};

//==============================================================================
class KitRipperEditor : public juce::AudioProcessorEditor,
                        public juce::FileDragAndDropTarget,
                        private juce::ChangeListener,
                        private juce::Timer
{
public:
    explicit KitRipperEditor (KitRipperProcessor&);
    ~KitRipperEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int x, int y) override { fileDragMove ({}, x, y); }
    void fileDragMove (const juce::StringArray&, int x, int y) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

    // usati dai pad
    void selectPad (int pad);
    int getSelectedPad() const { return selected; }
    int getBank() const { return bank; }
    void dragOutFile (const juce::File& f, juce::Component* source);
    void showTypeMenu (int pad);
    void openSourceChooser();

    KitRipperProcessor& proc;
    kr::KitPtr kit;

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    void refreshFromKit();
    void attachPadKnobs();
    void updateBpmLabel();
    PadComponent* padAt (juce::Point<int> p);

    krui::LookAndFeel lnf;
    WaveScreen screen { *this };
    krui::KnobWithLabel sens { "SENSIBILITA'" }, clean { "PULIZIA" };
    krui::KnobWithLabel vol { "VOLUME" }, tune { "TUNE" }, len { "LUNGHEZZA" }, pan { "PAN" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> sensAtt, cleanAtt, volAtt, tuneAtt, lenAtt, panAtt;
    juce::Label bpmLabel;
    juce::TextButton bpmAuto { "AUTO" }, bankA { "A" }, bankB { "B" }, exportKit { "ESPORTA KIT" }, loadBtn { "CARICA..." };
    juce::ToggleButton quant { "QUANTIZZA 1/16" };
    krui::DragChip allMidi { "MIDI TUTTO", krui::accent };
    krui::DragChip padWav { "TRASCINA WAV", krui::ink }, padMidi { "TRASCINA MIDI", krui::accent };
    juce::ComboBox typeBox;
    juce::OwnedArray<PadComponent> pads;
    std::unique_ptr<juce::FileChooser> chooser;

    int selected = 0, bank = 0;
    bool draggingOut = false;
    int pendingAnalysis = 0;      // debounce manopole (tick del timer)
    int pendingRender = 0;
    float lastSens = -1.0f, lastClean = -1.0f;
    std::array<int, kr::maxPads> lastFlash {};

    juce::Rectangle<int> rightPanel, padArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KitRipperEditor)
};
