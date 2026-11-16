#include "PluginEditor.h"

namespace
{
constexpr int W = 940, H = 620;

juce::Colour colourOf (kr::DrumType t) { return juce::Colour (kr::typeColourARGB (t)); }

juce::String noteName (int pad)
{
    return juce::MidiMessage::getMidiNoteName (kr::baseNote + pad, true, true, 3); // 36 = C1 come in Logic
}
} // namespace

juce::Font krui::font (float h, bool bold)
{
    return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain));
}

//==============================================================================
// LOOK
krui::LookAndFeel::LookAndFeel()
{
    setColour (juce::ComboBox::backgroundColourId, panelHi);
    setColour (juce::ComboBox::textColourId, ink);
    setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
    setColour (juce::ComboBox::arrowColourId, inkDim);
    setColour (juce::PopupMenu::backgroundColourId, panel);
    setColour (juce::PopupMenu::textColourId, ink);
    setColour (juce::PopupMenu::highlightedBackgroundColourId, accent.withAlpha (0.35f));
    setColour (juce::TextButton::textColourOffId, ink);
    setColour (juce::TextButton::textColourOnId, bg);
    setColour (juce::Label::textColourId, ink);
    setColour (juce::Label::textWhenEditingColourId, ink);
    setColour (juce::TextEditor::backgroundColourId, panelHi);
    setColour (juce::TextEditor::textColourId, ink);
    setColour (juce::TextEditor::highlightColourId, accent.withAlpha (0.4f));
    setColour (juce::CaretComponent::caretColourId, accent);
    setColour (juce::Slider::textBoxTextColourId, ink);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::ToggleButton::textColourId, ink);
}

void krui::LookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                                          float startAngle, float endAngle, juce::Slider&)
{
    auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (3.0f);
    const float size = juce::jmin (r.getWidth(), r.getHeight());
    r = r.withSizeKeepingCentre (size, size);
    const auto c = r.getCentre();
    const float rad = size * 0.5f - 2.0f;

    juce::Path track;
    track.addCentredArc (c.x, c.y, rad, rad, 0.0f, startAngle, endAngle, true);
    g.setColour (panelHi.brighter (0.15f));
    g.strokePath (track, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    const float a = startAngle + pos * (endAngle - startAngle);
    juce::Path val;
    val.addCentredArc (c.x, c.y, rad, rad, 0.0f, startAngle, a, true);
    g.setColour (accent);
    g.strokePath (val, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

    auto knob = r.reduced (size * 0.2f);
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3b3c40), knob.getX(), knob.getY(),
                                             juce::Colour (0xff1b1c1e), knob.getRight(), knob.getBottom(), false));
    g.fillEllipse (knob);
    const float rr = knob.getWidth() * 0.5f;
    g.setColour (ink);
    g.drawLine (c.x + rr * 0.2f * std::sin (a), c.y - rr * 0.2f * std::cos (a),
                c.x + rr * 0.85f * std::sin (a), c.y - rr * 0.85f * std::cos (a), 2.0f);
}

void krui::LookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down)
{
    auto r = b.getLocalBounds().toFloat().reduced (1.0f);
    const bool on = b.getToggleState();
    g.setColour (on ? accent : (down ? panelHi.brighter (0.2f) : (over ? panelHi.brighter (0.1f) : panelHi)));
    g.fillRoundedRectangle (r, 5.0f);
}

void krui::LookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool)
{
    auto r = b.getLocalBounds().toFloat();
    auto box = r.removeFromLeft (r.getHeight()).reduced (5.0f);
    g.setColour (over ? panelHi.brighter (0.15f) : panelHi);
    g.fillRoundedRectangle (box, 4.0f);
    if (b.getToggleState())
    {
        g.setColour (accent);
        g.fillRoundedRectangle (box.reduced (4.0f), 2.0f);
    }
    g.setColour (ink);
    g.setFont (font (12.0f, true));
    g.drawText (b.getButtonText(), r.withTrimmedLeft (4.0f), juce::Justification::centredLeft);
}

//==============================================================================
void krui::DragChip::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool over = isMouseOver() && enabled;
    g.setColour (enabled ? colour.withAlpha (over ? 0.32f : 0.18f) : panelHi);
    g.fillRoundedRectangle (r, 6.0f);
    g.setColour (enabled ? colour : inkDim.withAlpha (0.5f));
    g.drawRoundedRectangle (r, 6.0f, 1.2f);
    // icona "trascina"
    auto ic = r.removeFromLeft (r.getHeight()).reduced (r.getHeight() * 0.3f);
    for (int i = 0; i < 3; ++i)
        g.fillRect (ic.getX(), ic.getY() + ic.getHeight() * (0.15f + 0.3f * (float) i), ic.getWidth(), 1.6f);
    g.setFont (font (12.0f, true));
    g.drawText (label, r.withTrimmedRight (6.0f), juce::Justification::centred);
}

void krui::DragChip::mouseDrag (const juce::MouseEvent& e)
{
    if (! enabled || started || e.getDistanceFromDragStart() < 6) return;
    started = true;
    if (onDragOut) onDragOut();
}

krui::KnobWithLabel::KnobWithLabel (juce::String n) : name (std::move (n))
{
    slider.setSliderStyle (juce::Slider::RotaryVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
    slider.setMouseDragSensitivity (200);
    slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    addAndMakeVisible (slider);
}

void krui::KnobWithLabel::resized()
{
    slider.setBounds (getLocalBounds().withTrimmedTop (14));
}

void krui::KnobWithLabel::paint (juce::Graphics& g)
{
    g.setColour (inkDim);
    g.setFont (font (10.5f, true));
    g.drawText (name, getLocalBounds().removeFromTop (14), juce::Justification::centred);
}

//==============================================================================
// PAD
int PadComponent::padIndex() const { return editor.getBank() * kr::padsPerBank + local; }

juce::Rectangle<int> PadComponent::midiChipArea() const
{
    return getLocalBounds().reduced (6).removeFromBottom (18).removeFromRight (40);
}

void PadComponent::paint (juce::Graphics& g)
{
    const int p = padIndex();
    const auto& kit = editor.kit;
    const bool has = kit != nullptr && p < (int) kit->pads.size() && kit->pads[(size_t) p].audio.getNumSamples() > 0;
    auto r = getLocalBounds().toFloat().reduced (3.0f);

    const auto col = has ? colourOf (kit->pads[(size_t) p].type) : krui::inkDim;
    g.setColour (krui::panel.interpolatedWith (col, has ? 0.10f + 0.45f * flash : 0.0f));
    g.fillRoundedRectangle (r, 7.0f);
    if (has)
    {
        g.setColour (col);
        g.fillRoundedRectangle (r.withWidth (5.0f), 2.5f);
    }
    if (editor.getSelectedPad() == p)
    {
        g.setColour (krui::accent);
        g.drawRoundedRectangle (r, 7.0f, 2.0f);
    }
    if (dropHover)
    {
        g.setColour (krui::ink);
        g.drawRoundedRectangle (r.reduced (2.0f), 6.0f, 1.5f);
    }

    auto t = r.reduced (12.0f, 7.0f);
    g.setFont (krui::font (10.0f));
    g.setColour (krui::inkDim);
    g.drawText (noteName (p), t.removeFromTop (12.0f), juce::Justification::topRight);
    t = r.reduced (12.0f, 7.0f);

    if (! has)
    {
        g.setColour (krui::inkDim.withAlpha (0.5f));
        g.setFont (krui::font (11.0f));
        g.drawText (juce::String (p + 1), t, juce::Justification::topLeft);
        return;
    }
    const auto& pad = kit->pads[(size_t) p];
    g.setColour (krui::ink);
    g.setFont (krui::font (14.0f, true));
    g.drawFittedText (pad.name, t.removeFromTop (18.0f).toNearestInt().withTrimmedRight (26), juce::Justification::topLeft, 1);
    g.setColour (krui::inkDim);
    g.setFont (krui::font (11.0f));
    const int hits = (int) pad.hitStarts.size();
    g.drawText (pad.userFile.isNotEmpty() ? "sample tuo" : (juce::String (hits) + (hits == 1 ? " colpo" : " colpi")),
                t.removeFromTop (16.0f), juce::Justification::topLeft);

    if (hits > 0)
    {
        auto chip = midiChipArea().toFloat();
        g.setColour (krui::accent.withAlpha (0.2f));
        g.fillRoundedRectangle (chip, 4.0f);
        g.setColour (krui::accent);
        g.drawRoundedRectangle (chip, 4.0f, 1.0f);
        g.setFont (krui::font (10.5f, true));
        g.drawText ("MIDI", chip, juce::Justification::centred);
    }
}

void PadComponent::mouseDown (const juce::MouseEvent& e)
{
    const int p = padIndex();
    editor.selectPad (p);
    dragStarted = false;
    fromMidiChip = midiChipArea().contains (e.getPosition());
    if (e.mods.isPopupMenu()) { editor.showTypeMenu (p); return; }
    if (! fromMidiChip)
        editor.proc.triggerPreview (p, juce::jlimit (0.3f, 1.0f, 1.0f - 0.5f * (float) e.y / (float) getHeight()));
}

void PadComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (dragStarted || e.mods.isPopupMenu() || e.getDistanceFromDragStart() < 6) return;
    const int p = padIndex();
    if (editor.kit == nullptr || p >= (int) editor.kit->pads.size()) return;
    dragStarted = true;
    auto f = fromMidiChip ? editor.proc.exportPadMidi (p) : editor.proc.exportPadWav (p);
    editor.dragOutFile (f, this);
}

void PadComponent::mouseUp (const juce::MouseEvent&) { dragStarted = false; }

//==============================================================================
// SCHERMO CON LA FORMA D'ONDA
void WaveScreen::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    g.setColour (krui::screen);
    g.fillRoundedRectangle (r, 8.0f);
    g.setColour (dropHover ? krui::accent : krui::panelHi);
    g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, dropHover ? 2.0f : 1.0f);

    auto inner = r.reduced (10.0f, 8.0f);
    auto statusRow = inner.removeFromBottom (16.0f);
    const auto& kit = editor.kit;
    const bool busy = editor.proc.isBusy();

    if (kit != nullptr && ! kit->overviewMax.empty() && kit->sourceLength > 0)
    {
        const int cols = (int) kit->overviewMax.size();
        const float mid = inner.getCentreY(), half = inner.getHeight() * 0.48f;
        g.setColour (krui::inkDim.withAlpha (0.55f));
        for (int x = 0; x < (int) inner.getWidth(); ++x)
        {
            const int c = juce::jlimit (0, cols - 1, (int) ((float) x / inner.getWidth() * (float) cols));
            const float y0 = mid - kit->overviewMax[(size_t) c] * half, y1 = mid - kit->overviewMin[(size_t) c] * half;
            g.fillRect (inner.getX() + (float) x, y0, 1.0f, juce::jmax (1.0f, y1 - y0));
        }
        const int sel = editor.getSelectedPad();
        for (auto& [start, pad] : kit->markers)
        {
            if (pad >= (int) kit->pads.size()) continue;
            const float x = inner.getX() + inner.getWidth() * (float) ((double) start / (double) kit->sourceLength);
            const auto col = colourOf (kit->pads[(size_t) pad].type);
            const bool isSel = pad == sel;
            g.setColour (col.withAlpha (isSel ? 1.0f : 0.45f));
            g.fillRect (x, inner.getY(), isSel ? 2.0f : 1.0f, inner.getHeight());
            if (isSel) g.fillEllipse (x - 3.0f, inner.getY() - 1.0f, 7.0f, 7.0f);
        }
    }
    else if (! busy)
    {
        g.setColour (krui::ink);
        g.setFont (krui::font (17.0f, true));
        g.drawText ("TRASCINA QUI UN LOOP, UN BREAK O UNO STEM DI BATTERIA", inner.withTrimmedBottom (22.0f), juce::Justification::centred);
        g.setColour (krui::inkDim);
        g.setFont (krui::font (12.5f));
        g.drawText ("In Logic: Stem Splitter sulla traccia > tieni la batteria > trascina qui il file (o clicca per sceglierlo)",
                    inner.withTrimmedTop (34.0f), juce::Justification::centred);
    }

    if (busy)
    {
        g.setColour (krui::screen.withAlpha (0.6f));
        g.fillRoundedRectangle (r, 8.0f);
        const int dots = (int) (juce::Time::getMillisecondCounter() / 300) % 4;
        g.setColour (krui::accent);
        g.setFont (krui::font (18.0f, true));
        g.drawText ("ANALISI" + juce::String::repeatedString (".", dots), inner, juce::Justification::centred);
    }

    g.setColour (krui::inkDim);
    g.setFont (krui::font (11.5f));
    g.drawText (editor.proc.getStatus(), statusRow, juce::Justification::centredLeft);
}

void WaveScreen::mouseDown (const juce::MouseEvent& e)
{
    const auto& kit = editor.kit;
    if (kit == nullptr || kit->overviewMax.empty() || kit->sourceLength <= 0)
    {
        editor.openSourceChooser();
        return;
    }
    // click vicino a un colpo: seleziona il suo pad
    auto inner = getLocalBounds().toFloat().reduced (10.0f, 8.0f);
    inner.removeFromBottom (16.0f);
    int best = -1; float bd = 6.0f;
    for (auto& [start, pad] : kit->markers)
    {
        const float x = inner.getX() + inner.getWidth() * (float) ((double) start / (double) kit->sourceLength);
        if (std::abs (x - (float) e.x) < bd) { bd = std::abs (x - (float) e.x); best = pad; }
    }
    if (best >= 0)
    {
        editor.selectPad (best);
        editor.proc.triggerPreview (best);
    }
}

//==============================================================================
// EDITOR
KitRipperEditor::KitRipperEditor (KitRipperProcessor& p)
    : AudioProcessorEditor (&p), proc (p)
{
    setLookAndFeel (&lnf);

    addAndMakeVisible (screen);
    for (auto* k : { &sens, &clean, &vol, &tune, &len, &pan }) addAndMakeVisible (k);
    sensAtt  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.params, "sens", sens.slider);
    cleanAtt = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (proc.params, "clean", clean.slider);
    sens.slider.setTextValueSuffix ("%");
    clean.slider.setTextValueSuffix ("%");
    lastSens = (float) sens.slider.getValue();
    lastClean = (float) clean.slider.getValue();

    bpmLabel.setEditable (false, true, false);
    bpmLabel.setJustificationType (juce::Justification::centred);
    bpmLabel.setFont (krui::font (20.0f, true));
    bpmLabel.setColour (juce::Label::backgroundColourId, krui::panelHi);
    bpmLabel.onTextChange = [this]
    {
        const double v = bpmLabel.getText().retainCharacters ("0123456789.,").replaceCharacter (',', '.').getDoubleValue();
        if (v >= 30.0 && v <= 300.0) proc.setBpmOverride (v);
        updateBpmLabel();
    };
    addAndMakeVisible (bpmLabel);
    bpmAuto.setClickingTogglesState (false);
    bpmAuto.onClick = [this] { proc.setBpmOverride (0.0); updateBpmLabel(); };
    addAndMakeVisible (bpmAuto);

    quant.setToggleState (proc.getQuantize(), juce::dontSendNotification);
    quant.onClick = [this] { proc.setQuantize (quant.getToggleState()); };
    addAndMakeVisible (quant);

    bankA.onClick = [this] { bank = 0; bankA.setToggleState (true, juce::dontSendNotification); bankB.setToggleState (false, juce::dontSendNotification); repaint(); for (auto* pd : pads) pd->repaint(); };
    bankB.onClick = [this] { bank = 1; bankB.setToggleState (true, juce::dontSendNotification); bankA.setToggleState (false, juce::dontSendNotification); repaint(); for (auto* pd : pads) pd->repaint(); };
    bankA.setToggleState (true, juce::dontSendNotification);
    addAndMakeVisible (bankA);
    addAndMakeVisible (bankB);

    exportKit.onClick = [this] { if (proc.exportWholeKit() == juce::File()) juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Kit Ripper", "Non c'e' ancora un kit da esportare."); };
    addAndMakeVisible (exportKit);
    loadBtn.onClick = [this] { openSourceChooser(); };
    addAndMakeVisible (loadBtn);

    allMidi.onDragOut = [this] { dragOutFile (proc.exportPadMidi (-1), &allMidi); };
    padWav.onDragOut  = [this] { dragOutFile (proc.exportPadWav (selected), &padWav); };
    padMidi.onDragOut = [this] { dragOutFile (proc.exportPadMidi (selected), &padMidi); };
    addAndMakeVisible (allMidi);
    addAndMakeVisible (padWav);
    addAndMakeVisible (padMidi);

    for (int t = 0; t < kr::numDrumTypes; ++t) typeBox.addItem (kr::typeName ((kr::DrumType) t), t + 1);
    typeBox.onChange = [this]
    {
        const int id = typeBox.getSelectedId();
        if (id > 0 && kit != nullptr && selected < (int) kit->pads.size() && (int) kit->pads[(size_t) selected].type != id - 1)
            proc.setPadType (selected, (kr::DrumType) (id - 1));
    };
    addAndMakeVisible (typeBox);

    for (int i = 0; i < kr::padsPerBank; ++i) addAndMakeVisible (pads.add (new PadComponent (*this, i)));

    proc.addChangeListener (this);
    setSize (W, H);   // dopo aver creato tutti i componenti (resized li usa)
    refreshFromKit();
    attachPadKnobs();
    startTimerHz (30);
}

KitRipperEditor::~KitRipperEditor()
{
    proc.removeChangeListener (this);
    setLookAndFeel (nullptr);
}

void KitRipperEditor::paint (juce::Graphics& g)
{
    g.fillAll (krui::bg);

    // titolo
    auto head = getLocalBounds().removeFromTop (46).reduced (18, 0);
    g.setColour (krui::accent);
    g.setFont (krui::font (22.0f, true));
    g.drawText ("G.LOVE", head, juce::Justification::centredLeft);
    g.setColour (krui::ink);
    g.drawText ("KIT RIPPER", head.withTrimmedLeft (86), juce::Justification::centredLeft);
    g.setColour (krui::inkDim);
    g.setFont (krui::font (11.0f));
    g.drawText ("loop > kit di batteria + MIDI", head, juce::Justification::centredRight);

    // etichette riga controlli
    g.setFont (krui::font (10.5f, true));
    g.setColour (krui::inkDim);
    g.drawText ("BPM", bpmLabel.getBounds().translated (0, -15).withHeight (14), juce::Justification::centred);
    g.drawText ("BANCO", bankA.getBounds().getUnion (bankB.getBounds()).translated (0, -15).withHeight (14), juce::Justification::centred);

    // pannello del pad selezionato
    g.setColour (krui::panel);
    g.fillRoundedRectangle (rightPanel.toFloat(), 8.0f);
    auto rp = rightPanel.reduced (12);
    auto top = rp.removeFromTop (30);
    g.setColour (krui::ink);
    g.setFont (krui::font (11.0f, true));
    g.drawText ("PAD " + juce::String (selected + 1) + "  " + noteName (selected), top.removeFromLeft (110), juce::Justification::centredLeft);

    rp.removeFromTop (6);
    auto wave = rp.removeFromTop (70).toFloat();
    g.setColour (krui::screen);
    g.fillRoundedRectangle (wave, 6.0f);
    if (kit != nullptr && selected < (int) kit->pads.size() && kit->pads[(size_t) selected].audio.getNumSamples() > 0)
    {
        const auto& pd = kit->pads[(size_t) selected];
        const auto& b = pd.audio;
        auto wr = wave.reduced (6.0f, 6.0f);
        const float lenFrac = proc.params.getRawParameterValue (KitRipperProcessor::pid ("len", selected))->load() / 100.0f;
        const auto col = colourOf (pd.type);
        const int n = b.getNumSamples();
        for (int x = 0; x < (int) wr.getWidth(); ++x)
        {
            const int s0 = (int) ((juce::int64) n * x / (int) wr.getWidth());
            const int s1 = juce::jmax (s0 + 1, (int) ((juce::int64) n * (x + 1) / (int) wr.getWidth()));
            const auto mm = b.findMinMax (0, s0, juce::jmin (n, s1) - s0);
            const bool inside = (float) x / wr.getWidth() <= lenFrac;
            g.setColour (inside ? col : krui::inkDim.withAlpha (0.3f));
            const float y0 = wr.getCentreY() - mm.getEnd() * wr.getHeight() * 0.5f;
            const float y1 = wr.getCentreY() - mm.getStart() * wr.getHeight() * 0.5f;
            g.fillRect (wr.getX() + (float) x, y0, 1.0f, juce::jmax (1.0f, y1 - y0));
        }
        g.setColour (krui::inkDim);
        g.setFont (krui::font (10.0f));
        g.drawText (juce::String (juce::roundToInt ((double) n / pd.sampleRate * 1000.0)) + " ms", wave.reduced (6.0f, 3.0f), juce::Justification::bottomRight);
    }
}

void KitRipperEditor::resized()
{
    screen.setBounds (16, 52, W - 32, 140);

    int x = 16;
    const int y = 204;
    sens.setBounds (x, y, 84, 80); x += 90;
    clean.setBounds (x, y, 84, 80); x += 100;
    bpmLabel.setBounds (x, y + 16, 100, 32);
    bpmAuto.setBounds (x, y + 52, 100, 24); x += 116;
    quant.setBounds (x, y + 18, 150, 28); x += 160;
    bankA.setBounds (x, y + 16, 34, 32);
    bankB.setBounds (x + 38, y + 16, 34, 32); x += 92;
    allMidi.setBounds (x, y + 16, 122, 32);
    exportKit.setBounds (x + 130, y + 16, 112, 32);
    loadBtn.setBounds (W - 16 - 90, y + 16, 90, 32);

    padArea = { 16, 296, 572, H - 296 - 16 };
    const int pw = padArea.getWidth() / 4, ph = padArea.getHeight() / 4;
    for (int i = 0; i < kr::padsPerBank; ++i)
    {
        // come una MPC: il pad 1 è in basso a sinistra
        const int col = i % 4, row = 3 - i / 4;
        pads[i]->setBounds (padArea.getX() + col * pw, padArea.getY() + row * ph, pw, ph);
    }

    rightPanel = { padArea.getRight() + 16, padArea.getY(), W - 16 - (padArea.getRight() + 16), padArea.getHeight() };
    auto rp = rightPanel.reduced (12);
    typeBox.setBounds (rp.removeFromTop (30).removeFromRight (150).reduced (0, 2));
    rp.removeFromTop (6 + 70 + 10);
    auto knobs = rp.removeFromTop (86);
    const int kw = knobs.getWidth() / 4;
    vol.setBounds (knobs.removeFromLeft (kw));
    tune.setBounds (knobs.removeFromLeft (kw));
    len.setBounds (knobs.removeFromLeft (kw));
    pan.setBounds (knobs);
    rp.removeFromTop (12);
    auto chips = rp.removeFromTop (38);
    padWav.setBounds (chips.removeFromLeft (chips.getWidth() / 2).reduced (0, 0).withTrimmedRight (5));
    padMidi.setBounds (chips.withTrimmedLeft (5));
}

//==============================================================================
void KitRipperEditor::selectPad (int p)
{
    p = juce::jlimit (0, kr::maxPads - 1, p);
    if (p / kr::padsPerBank != bank)
    {
        if (p < kr::padsPerBank) bankA.onClick(); else bankB.onClick();
    }
    if (p == selected) return;
    selected = p;
    attachPadKnobs();
    refreshFromKit();
}

void KitRipperEditor::attachPadKnobs()
{
    volAtt.reset(); tuneAtt.reset(); lenAtt.reset(); panAtt.reset();
    using A = juce::AudioProcessorValueTreeState::SliderAttachment;
    volAtt  = std::make_unique<A> (proc.params, KitRipperProcessor::pid ("vol", selected), vol.slider);
    tuneAtt = std::make_unique<A> (proc.params, KitRipperProcessor::pid ("tune", selected), tune.slider);
    lenAtt  = std::make_unique<A> (proc.params, KitRipperProcessor::pid ("len", selected), len.slider);
    panAtt  = std::make_unique<A> (proc.params, KitRipperProcessor::pid ("pan", selected), pan.slider);
    vol.slider.setTextValueSuffix (" dB");
    tune.slider.setTextValueSuffix (" st");
    len.slider.setTextValueSuffix ("%");
    len.slider.onValueChange = [this] { repaint (rightPanel); };
}

void KitRipperEditor::refreshFromKit()
{
    kit = proc.getKit();
    const bool hasPad = kit != nullptr && selected < (int) kit->pads.size() && kit->pads[(size_t) selected].audio.getNumSamples() > 0;
    const bool hasHits = hasPad && ! kit->pads[(size_t) selected].hitStarts.empty();
    typeBox.setSelectedId (hasPad ? (int) kit->pads[(size_t) selected].type + 1 : 0, juce::dontSendNotification);
    typeBox.setEnabled (hasPad);
    padWav.enabled = hasPad;
    padMidi.enabled = hasHits;
    allMidi.enabled = kit != nullptr && ! kit->markers.empty();
    padWav.repaint(); padMidi.repaint(); allMidi.repaint();
    updateBpmLabel();
    repaint();
    for (auto* pd : pads) pd->repaint();
}

void KitRipperEditor::updateBpmLabel()
{
    bpmLabel.setText (kit != nullptr && ! kit->pads.empty() ? juce::String (proc.getEffectiveBpm(), 2) : juce::String ("--"), juce::dontSendNotification);
    bpmAuto.setToggleState (proc.getBpmOverride() <= 0.0, juce::dontSendNotification);
}

void KitRipperEditor::changeListenerCallback (juce::ChangeBroadcaster*) { refreshFromKit(); }

void KitRipperEditor::timerCallback()
{
    // lampeggio dei pad quando suonano
    for (int i = 0; i < kr::padsPerBank; ++i)
    {
        auto* pd = pads[i];
        const int p = pd->padIndex();
        const int f = proc.getPadFlash (p);
        if (f != lastFlash[(size_t) p]) { lastFlash[(size_t) p] = f; pd->flash = 1.0f; pd->repaint(); }
        else if (pd->flash > 0.0f) { pd->flash = juce::jmax (0.0f, pd->flash - 0.12f); pd->repaint(); }
    }
    if (proc.isBusy()) screen.repaint();

    // le manopole SENSIBILITA'/PULIZIA ricalcolano quando smetti di girarle
    const float s = (float) sens.slider.getValue(), c = (float) clean.slider.getValue();
    if (std::abs (s - lastSens) > 0.01f) { lastSens = s; pendingAnalysis = 12; }
    if (std::abs (c - lastClean) > 0.01f) { lastClean = c; pendingRender = 12; }
    const bool dragging = sens.slider.isMouseButtonDown() || clean.slider.isMouseButtonDown();
    if (! dragging)
    {
        if (pendingAnalysis > 0 && --pendingAnalysis == 0) { pendingRender = 0; proc.reanalyse(); }
        if (pendingRender > 0 && --pendingRender == 0) proc.rerender();
    }
}

//==============================================================================
// DRAG & DROP
void KitRipperEditor::dragOutFile (const juce::File& f, juce::Component* source)
{
    if (! f.existsAsFile()) return;
    draggingOut = true;
    juce::Component::SafePointer<KitRipperEditor> safe (this);
    juce::DragAndDropContainer::performExternalDragDropOfFiles ({ f.getFullPathName() }, false, source,
        [safe] { if (safe != nullptr) safe->draggingOut = false; });
}

PadComponent* KitRipperEditor::padAt (juce::Point<int> p)
{
    for (auto* pd : pads) if (pd->getBounds().contains (p)) return pd;
    return nullptr;
}

bool KitRipperEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    if (draggingOut) return false;   // è il nostro stesso drag verso Logic
    for (auto& f : files) if (KitRipperProcessor::isAudioFile (juce::File (f))) return true;
    return false;
}

void KitRipperEditor::fileDragMove (const juce::StringArray&, int x, int y)
{
    auto* over = padAt ({ x, y });
    for (auto* pd : pads) { const bool h = pd == over; if (pd->dropHover != h) { pd->dropHover = h; pd->repaint(); } }
    const bool sh = over == nullptr;
    if (screen.dropHover != sh) { screen.dropHover = sh; screen.repaint(); }
}

void KitRipperEditor::fileDragExit (const juce::StringArray&)
{
    for (auto* pd : pads) { pd->dropHover = false; pd->repaint(); }
    screen.dropHover = false;
    screen.repaint();
}

void KitRipperEditor::filesDropped (const juce::StringArray& files, int x, int y)
{
    fileDragExit (files);
    if (draggingOut) return;
    juce::File f;
    for (auto& s : files) if (KitRipperProcessor::isAudioFile (juce::File (s))) { f = juce::File (s); break; }
    if (f == juce::File()) return;

    if (auto* pd = padAt ({ x, y }))
    {
        // file su un pad: sostituisce il suono, il pattern MIDI resta
        const int p = pd->padIndex();
        selectPad (p);
        if (proc.replacePadSample (p, f)) proc.triggerPreview (p);
        return;
    }
    proc.loadSource (f);
}

void KitRipperEditor::openSourceChooser()
{
    chooser = std::make_unique<juce::FileChooser> ("Scegli un loop o uno stem di batteria",
                                                   proc.getSourceFile().existsAsFile() ? proc.getSourceFile().getParentDirectory()
                                                                                       : juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.aac;*.caf;*.ogg");
    juce::Component::SafePointer<KitRipperEditor> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [safe] (const juce::FileChooser& fc)
                          {
                              if (safe == nullptr) return;
                              const auto f = fc.getResult();
                              if (f.existsAsFile()) safe->proc.loadSource (f);
                          });
}

void KitRipperEditor::showTypeMenu (int pad)
{
    if (kit == nullptr || pad >= (int) kit->pads.size()) return;
    juce::PopupMenu m;
    m.addSectionHeader ("Tipo del pad " + juce::String (pad + 1));
    const int cur = (int) kit->pads[(size_t) pad].type;
    for (int t = 0; t < kr::numDrumTypes; ++t)
        m.addItem (t + 1, kr::typeName ((kr::DrumType) t), true, t == cur);
    juce::Component::SafePointer<KitRipperEditor> safe (this);
    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (pads[pad % kr::padsPerBank]),
                     [safe, pad] (int r)
                     {
                         if (safe != nullptr && r > 0) safe->proc.setPadType (pad, (kr::DrumType) (r - 1));
                     });
}
