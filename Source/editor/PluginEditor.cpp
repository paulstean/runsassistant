#include "PluginEditor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

// --------------------------------------------------------------------------
// Settings dialog content (S9): engine source type + event numbers,
// per-parameter CC bindings with conflict prevention, tuning constants and
// Reset to defaults. All edits write processor.settings (chunk state, S7)
// and mark the host state dirty. The panel is a plain component; the dialog
// window lives in RunsEditor::settingsDialog (modal, Esc closes).

namespace
{
constexpr int kCcNone = 255; // same sentinel as PluginSettings (S4)

juce::String ccText (int cc)
{
    return cc == kCcNone ? "none" : juce::String (cc);
}

// "0..127" or "none"; -1 = invalid
int parseCc (const juce::String& s)
{
    const juce::String t = s.trim();
    if (t.isEmpty() || t.equalsIgnoreCase ("none"))
        return kCcNone;
    if (! t.containsOnly ("0123456789"))
        return -1;
    const int v = t.getIntValue();
    return v >= 0 && v <= 127 ? v : -1;
}

// -1 = invalid; accepts only clean integers
int parseNum (const juce::String& s, int lo, int hi)
{
    const juce::String t = s.trim();
    if (! t.containsOnly ("0123456789"))
        return -1;
    const int v = t.getIntValue();
    return v >= lo && v <= hi ? v : -1;
}

float parseFloat (const juce::String& s, float lo, float hi, bool* ok)
{
    const float v = s.trim().getFloatValue();
    const bool valid = s.trim().containsOnly ("0123456789.eE-+")
                       && v >= lo && v <= hi;
    if (ok != nullptr) *ok = valid;
    return v;
}
} // namespace

class SettingsPanel : public juce::Component,
                      private juce::ComboBox::Listener,
                      private juce::TextEditor::Listener,
                      private juce::Button::Listener
{
public:
    explicit SettingsPanel (RunsProcessor& p)
        : processor (p), settings (p.settings)
    {
        addAndMakeVisible (sourceLabel);
        sourceLabel.setText ("Engine switch source", juce::dontSendNotification);
        sourceLabel.setColour (juce::Label::textColourId, runui::text());
        sourceType.addItemList ({ "Notes", "CC", "PC" }, 1);
        sourceType.setSelectedItemIndex (
            juce::jlimit (0, 2, settings.engineSourceType),
            juce::dontSendNotification);
        sourceType.addListener (this);
        sourceType.setName ("Engine switch source type");
        addAndMakeVisible (sourceType);

        addAndMakeVisible (ccNumLabel);
        ccNumLabel.setText ("Engine CC number", juce::dontSendNotification);
        addAndMakeVisible (ccNumber);
        ccNumber.addListener (this);
        ccNumber.setName ("Engine CC number");

        addAndMakeVisible (notesLabel);
        notesLabel.setText ("Keyswitch notes (Off / Up / Down)",
                            juce::dontSendNotification);
        addAndMakeVisible (pcsLabel);
        pcsLabel.setText ("Program changes (Off / Up / Down)",
                          juce::dontSendNotification);
        for (int i = 0; i < 3; ++i)
        {
            addAndMakeVisible (noteNumbers[i]);
            noteNumbers[i].addListener (this);
            noteNumbers[i].setName ("Keyswitch note " + juce::String (i));
            addAndMakeVisible (pcNumbers[i]);
            pcNumbers[i].addListener (this);
            pcNumbers[i].setName ("Program change " + juce::String (i));
        }

        addAndMakeVisible (bindHeader);
        bindHeader.setText ("Per-parameter CC bindings (0..127 or 'none')",
                            juce::dontSendNotification);
        bindHeader.setColour (juce::Label::textColourId, runui::text());
        const char* names[8] = { "Beats", "Density", "Curve", "Accent",
                                 "Arc", "Tonic", "Mode", "Walk" };
        for (int b = 0; b < 8; ++b)
        {
            bindNames[b].setColour (juce::Label::textColourId, runui::text());
            bindNames[b].setText (names[b], juce::dontSendNotification);
            addAndMakeVisible (bindNames[b]);
            addAndMakeVisible (bindEditors[b]);
            bindEditors[b].addListener (this);
            bindEditors[b].setName (juce::String (names[b]) + " CC");
            addAndMakeVisible (bindErrors[b]);
            bindErrors[b].setColour (juce::Label::textColourId,
                                     runui::warn());
        }

        addAndMakeVisible (tuneHeader);
        tuneHeader.setText ("Tuning (raw values)",
                            juce::dontSendNotification);
        tuneHeader.setColour (juce::Label::textColourId, runui::text());
        const char* tuneNames[4] = { "Alignment epsilon (ms)", "Gate fraction",
                                     "Downbeat weight", "Mid-bar beat weight" };
        for (int i = 0; i < 4; ++i)
        {
            tuneNamesL[i].setText (tuneNames[i], juce::dontSendNotification);
            tuneNamesL[i].setColour (juce::Label::textColourId,
                                     runui::text());
            addAndMakeVisible (tuneNamesL[i]);
            addAndMakeVisible (tuneEditors[i]);
            tuneEditors[i].addListener (this);
            tuneEditors[i].setName (juce::String (tuneNames[i]));
        }

        resetButton.setTooltip ("Restore factory parameter values, CC "
                                "bindings, tuning constants and engine "
                                "source settings (not undoable, S7).");
        addAndMakeVisible (resetButton);

        loadFromSettings();
        refreshEngineFieldVisibility();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (runui::panel());
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        auto row = area.removeFromTop (24);
        sourceLabel.setBounds (row.removeFromLeft (170));
        sourceType.setBounds (row.removeFromLeft (100));
        area.removeFromTop (8);

        row = area.removeFromTop (24);
        ccNumLabel.setBounds (row.removeFromLeft (170));
        ccNumber.setBounds (row.removeFromLeft (60));
        row = area.removeFromTop (24);
        notesLabel.setBounds (row.removeFromLeft (170));
        for (auto& e : noteNumbers)
            e.setBounds (row.removeFromLeft (60));
        row = area.removeFromTop (24);
        pcsLabel.setBounds (row.removeFromLeft (170));
        for (auto& e : pcNumbers)
            e.setBounds (row.removeFromLeft (60));
        area.removeFromTop (10);

        bindHeader.setBounds (area.removeFromTop (22));
        for (int b = 0; b < 8; ++b)
        {
            row = area.removeFromTop (24);
            bindNames[b].setBounds (row.removeFromLeft (110));
            bindEditors[b].setBounds (row.removeFromLeft (70));
            bindErrors[b].setBounds (row.removeFromLeft (row.getWidth()));
        }
        area.removeFromTop (10);

        tuneHeader.setBounds (area.removeFromTop (22));
        for (int i = 0; i < 4; ++i)
        {
            row = area.removeFromTop (24);
            tuneNamesL[i].setBounds (row.removeFromLeft (180));
            tuneEditors[i].setBounds (row.removeFromLeft (70));
        }
        area.removeFromTop (10);
        resetButton.setBounds (area.removeFromTop (26).removeFromLeft (180));
    }

    // Re-sync the fields from processor.settings after external changes.
    void refresh() { loadFromSettings(); }

private:
    void comboBoxChanged (juce::ComboBox* c) override
    {
        if (c == &sourceType)
        {
            settings.engineSourceType = c->getSelectedItemIndex();
            markDirty();
            refreshEngineFieldVisibility();
        }
    }

    void textEditorReturnKeyPressed (juce::TextEditor& e) override
    {
        commitByPointer (&e);
        e.giveAwayKeyboardFocus();
    }

    void textEditorEscapeKeyPressed (juce::TextEditor& e) override
    {
        // Esc inside a field rolls the text back; Esc at window level closes
        // the dialog (SettingsDialog::closeButtonPressed).
        loadFromSettings();
        e.giveAwayKeyboardFocus();
    }

    void textEditorFocusLost (juce::TextEditor& e) override
    {
        commitByPointer (&e);
    }

    void buttonClicked (juce::Button* b) override
    {
        if (b == &resetButton)
            resetToDefaults();
    }

    bool isEngineField (const juce::TextEditor* e) const
    {
        if (e == &ccNumber)
            return true;
        for (const auto& n : noteNumbers)
            if (e == &n)
                return true;
        for (const auto& p : pcNumbers)
            if (e == &p)
                return true;
        return false;
    }

    void commitByPointer (juce::TextEditor* e)
    {
        // only visible fields commit (hidden source-mode fields hold stale
        // text from another source type)
        if (e == nullptr || ! e->isVisible())
            return;
        if (isEngineField (e)) { commitEngineFields(); return; }
        for (int b = 0; b < 8; ++b)
            if (e == &bindEditors[b]) { commitBinding (b); return; }
        for (int i = 0; i < 4; ++i)
            if (e == &tuneEditors[i]) { commitTuning (i); return; }
    }

    void commitEngineFields()
    {
        const int src = juce::jlimit (0, 2, settings.engineSourceType);
        bool ok = false;
        if (src == 1)
        {
            const int v = parseNum (ccNumber.getText(), 0, 127);
            ok = v >= 0;
            if (ok) settings.engineNumber = v;
        }
        else
        {
            juce::TextEditor* fields[3] = { nullptr, nullptr, nullptr };
            for (int i = 0; i < 3; ++i)
                fields[i] = src == 0 ? &noteNumbers[i] : &pcNumbers[i];
            int vals[3] = { 0, 0, 0 };
            ok = true;
            for (int i = 0; i < 3; ++i)
            {
                vals[i] = parseNum (fields[i]->getText(), 0, 127);
                ok = ok && vals[i] >= 0;
            }
            if (ok)
                for (int i = 0; i < 3; ++i)
                    (src == 0 ? settings.engineNoteNumbers[i]
                              : settings.enginePcNumbers[i]) = vals[i];
        }
        if (ok) markDirty();
        loadFromSettings();
    }

    // Rows Beats/Density/Curve/Accent/Arc/Tonic/Mode/Walk: two rows may not
    // share a CC number, and the engine CC number (CC source mode) may not
    // be bound elsewhere. Violations show a red warning on the row and the
    // assignment is refused (reverted), per S4/S9.
    bool ccConflicts (int row, int value) const
    {
        if (value == kCcNone)
            return false;
        for (int b = 0; b < 8; ++b)
            if (b != row && settings.boundCC[b] == value)
                return true;
        return settings.engineSourceType == 1
               && settings.engineNumber == value;
    }

    void commitBinding (int row)
    {
        const int v = parseCc (bindEditors[row].getText());
        auto& error = bindErrors[row];
        error.setText ({}, juce::dontSendNotification);
        if (v < 0)
        {
            error.setText ("invalid: 0..127 or none",
                           juce::dontSendNotification);
            bindEditors[row].setText (ccText (settings.boundCC[row]),
                                      juce::dontSendNotification);
            return;
        }
        if (ccConflicts (row, v))
        {
            const juce::String owner = findConflictingOwner (row, v);
            error.setText ("conflict: CC " + juce::String (v) + " " + owner,
                           juce::dontSendNotification);
            bindEditors[row].setText (ccText (settings.boundCC[row]),
                                      juce::dontSendNotification);
            return; // refused; text reverts to the stored binding
        }
        settings.boundCC[row] = v;
        markDirty();
        clearResolvedErrors();
    }

    juce::String findConflictingOwner (int row, int value) const
    {
        static const char* names[8] = { "Beats", "Density", "Curve", "Accent",
                                        "Arc", "Tonic", "Mode", "Walk" };
        for (int b = 0; b < 8; ++b)
            if (b != row && settings.boundCC[b] == value)
                return "already bound to " + juce::String (names[b]);
        if (settings.engineSourceType == 1
            && settings.engineNumber == value)
            return "is the engine CC";
        return {};
    }

    void clearResolvedErrors()
    {
        for (int b = 0; b < 8; ++b)
        {
            if (bindErrors[b].getText().isEmpty())
                continue;
            const int v = settings.boundCC[b];
            if (v == kCcNone || ! ccConflicts (b, v))
                bindErrors[b].setText ({}, juce::dontSendNotification);
        }
    }

    void commitTuning (int i)
    {
        static constexpr float los[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        static constexpr float his[4] = { 100.0f, 1.0f, 2.0f, 2.0f };
        float* dst[4] = { &settings.epsilonMs, &settings.gateFraction,
                          &settings.downWeight, &settings.midBarWeight };
        bool ok = false;
        const float v = parseFloat (tuneEditors[i].getText(), los[i], his[i],
                                    &ok);
        if (ok)
        {
            *dst[i] = v;
    markDirty();
}
        // on invalid input the display reverts to the stored value
        tuneEditors[i].setText (juce::String (*dst[i], 4),
                                juce::dontSendNotification);
    }

    void resetToDefaults()
    {
        // Factory parameter values (S7) through the normal notify path so
        // attachments and host learn of every change, exactly like GUI edits.
        static const char* ids[RunsProcessor::kNumParams] = {
            "engine", "beats", "density", "curve", "accent", "arc",
            "tonic", "mode", "walk"
        };
        for (int i = 0; i < RunsProcessor::kNumParams; ++i)
        {
            auto* r = dynamic_cast<juce::RangedAudioParameter*> (
                processor.apvts.getParameter (ids[i]));
            if (r != nullptr)
                r->setValueNotifyingHost (r->getDefaultValue());
        }
        // Factory settings + CC bindings + tuning + engine source (S7).
        settings = runsp::PluginSettings {};
        markDirty();
        loadFromSettings();
        refreshEngineFieldVisibility();
    }

    void markDirty()
    {
        // S7: settings edits mark the host state dirty (VST3 dirty flag /
        // CLAP stateMarkDirty); the values round-trip through the chunk.
        // The non-parameter-changed path is what reaches the host's dirty
        // flags; the bare updateHostDisplay() defaulted flags do not set it,
        // so plain settings edits never dirtied the project (REAPER report).
        processor.markStateDirty();
    }

    void loadFromSettings()
    {
        sourceType.setSelectedItemIndex (
            juce::jlimit (0, 2, settings.engineSourceType),
            juce::dontSendNotification);
        ccNumber.setText (ccText (settings.engineNumber),
                          juce::dontSendNotification);
        for (int i = 0; i < 3; ++i)
        {
            noteNumbers[i].setText (
                juce::String (settings.engineNoteNumbers[i]),
                juce::dontSendNotification);
            pcNumbers[i].setText (
                juce::String (settings.enginePcNumbers[i]),
                juce::dontSendNotification);
        }
        for (int b = 0; b < 8; ++b)
            bindEditors[b].setText (ccText (settings.boundCC[b]),
                                    juce::dontSendNotification);
        tuneEditors[0].setText (juce::String (settings.epsilonMs, 4),
                                juce::dontSendNotification);
        tuneEditors[1].setText (juce::String (settings.gateFraction, 4),
                                juce::dontSendNotification);
        tuneEditors[2].setText (juce::String (settings.downWeight, 4),
                                juce::dontSendNotification);
        tuneEditors[3].setText (juce::String (settings.midBarWeight, 4),
                                juce::dontSendNotification);
        for (auto& e : bindErrors)
            e.setText ({}, juce::dontSendNotification);
    }

    void refreshEngineFieldVisibility()
    {
        const int t = juce::jlimit (0, 2, settings.engineSourceType);
        ccNumLabel.setVisible (t == 1);
        ccNumber.setVisible (t == 1);
        notesLabel.setVisible (t == 0);
        for (auto& e : noteNumbers)
            e.setVisible (t == 0);
        pcsLabel.setVisible (t == 2);
        for (auto& e : pcNumbers)
            e.setVisible (t == 2);
    }

    RunsProcessor& processor;
    runsp::PluginSettings& settings;

    juce::Label sourceLabel;
    juce::ComboBox sourceType;
    juce::Label ccNumLabel;
    juce::TextEditor ccNumber;
    juce::Label notesLabel, pcsLabel;
    juce::TextEditor noteNumbers[3];
    juce::TextEditor pcNumbers[3];

    juce::Label bindHeader;
    juce::Label bindNames[8];
    juce::TextEditor bindEditors[8];
    juce::Label bindErrors[8];

    juce::Label tuneHeader;
    juce::Label tuneNamesL[4];
    juce::TextEditor tuneEditors[4];

    juce::TextButton resetButton { "Reset to defaults" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsPanel)
};

// Dark modal dialog; Esc key triggers closeButtonPressed (DialogWindow
// ctor flag) and hides (not deletes: the editor owns it for reuse).
class SettingsDialog : public juce::DialogWindow
{
public:
    SettingsDialog()
        : DialogWindow ("Settings", runui::panel(), nullptr, true)
    {
        setUsingNativeTitleBar (false);
    }

    void closeButtonPressed() override
    {
        exitModalState (0);
        setVisible (false);
    }
};

// --------------------------------------------------------------------------
// Debug overlay (S9 bottom row): engine state, pending note, active run
// progress (notes emitted / n, last pitch/velocity), counters, playhead
// beat + bpm. Polls the P2 lock-free FIFO on a Timer (S6.1 step 6 path).

class DebugOverlayPanel : public juce::Component, private juce::Timer
{
public:
    explicit DebugOverlayPanel (RunsProcessor& p)
        : processor (p)
    {
        spikeLogPath = juce::SystemStats::getEnvironmentVariable (
            "RUNSASSISTANT_SPIKE_LOG", ""); // kept from the P0 spike
        for (auto& l : labels)
        {
            l.setColour (juce::Label::textColourId, runui::text());
            l.setJustificationType (juce::Justification::centredLeft);
            addAndMakeVisible (l);
        }
        startTimerHz (10);
    }

    ~DebugOverlayPanel() override { stopTimer(); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (runui::panel());
        g.setColour (runui::edge());
        g.drawRect (getLocalBounds(), 1);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8);
        for (auto& l : labels)
            l.setBounds (area.removeFromTop (18));
    }

private:
    void timerCallback() override
    {
        const runsp::UiMessage s = processor.latestUiState();
        const char* engName[3] = { "Off", "Up", "Down" };
        labels[0].setText ("engine: " + juce::String (engName[engNameIndex (s)])
                               + "   pending note: "
                               + (s.pendingPitch >= 0
                                      ? juce::String (s.pendingPitch)
                                      : "-")
                               + "   [L:"
                               + juce::String (s.engineListenerHits)
                               + " P:"
                               + juce::String (msgParamNorm(), 3)
                               + " A:"
                               + juce::String (s.engineParamX256 / 256.0, 3)
                               + " det:" + juce::String (s.engineParamChanges)
                               + " sw:" + juce::String (s.engineSwitches)
                               + "]",
                           juce::dontSendNotification);
        labels[1].setText (
            "run: " + (s.runActive ? juce::String (s.notesEmitted) + " / "
                                         + juce::String (s.noteCount)
                                   : "-")
                + "   last emitted: "
                + (s.lastPitch >= 0
                       ? juce::String (s.lastPitch) + " vel "
                             + juce::String (s.lastVel)
                       : "-"),
            juce::dontSendNotification);
        labels[2].setText ("parity drops: " + juce::String (s.parityDrops)
                               + "   late singles: "
                               + juce::String (s.latePublishes)
                               + "   cuts: " + juce::String (s.cuts),
                           juce::dontSendNotification);
        labels[3].setText ("input events: " + juce::String ((int) s.inputEvents)
                               + "  (drops " + juce::String (s.inputDrops)
                               + ", mirror drops "
                               + juce::String (s.mirrorDrops) + ")",
                           juce::dontSendNotification);
        labels[4].setText ("beat: " + juce::String (s.beat, 3) + "   bpm: "
                               + juce::String (s.bpm, 2),
                           juce::dontSendNotification);
        if (spikeLogPath.isNotEmpty())
            appendSpikeLog (s);
    }

    static int engNameIndex (const runsp::UiMessage& s)
    {
        return juce::jlimit (0, 2, s.engineState);
    }

    float msgParamNorm() const
    {
        // Divider probe: the message-thread view of the engine parameter
        // (overlay Timer runs on the message thread).
        if (auto* p = dynamic_cast<juce::RangedAudioParameter*> (
                processor.apvts.getParameter ("engine")))
            return p->getValue();
        return -1.0f;
    }

    void appendSpikeLog (const runsp::UiMessage& s)
    {
        auto& ph = processor.playheadSnapshot;
        juce::File f (spikeLogPath);
        f.appendText (juce::String (juce::Time::getMillisecondCounter()) + ","
                      + juce::String (ph.ppqPosition.load(), 6) + ","
                      + juce::String (s.bpm, 4) + ","
                      + (s.runActive ? "1" : "0") + ","
                      + juce::String (s.notesEmitted) + ","
                      + juce::String (s.cuts) + "\n");
    }

    RunsProcessor& processor;
    juce::Label labels[5];
    juce::String spikeLogPath;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DebugOverlayPanel)
};

// --------------------------------------------------------------------------
// Editor layout + wiring

namespace
{
const char* kPitchNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G",
                                "G#", "A", "A#", "B" };

void styleParamRow (RunsEditor::ParamRow& row, const char* name,
                    const char* tooltipText, double lo, double hi,
                    double interval, double defaultValue)
{
    row.name.setText (name, juce::dontSendNotification);
    row.name.setColour (juce::Label::textColourId, runui::text());
    row.slider.setSliderStyle (juce::Slider::LinearHorizontal);
    row.slider.setRange (lo, hi, interval);
    row.slider.setValue (defaultValue, juce::dontSendNotification);
    row.slider.setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
    row.slider.setTooltip (tooltipText);
    row.name.setTooltip (tooltipText);
    row.readout.setTooltip (tooltipText);
    row.readout.setColour (juce::Label::textColourId, runui::dim());
    row.readout.setJustificationType (juce::Justification::centredLeft);
}
} // namespace

RunsEditor::RunsEditor (RunsProcessor& p)
    : AudioProcessorEditor (p), processor (p)
{
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

    // ---- Title + engine row (S9) ----------------------------------------
    title.setText ("RUNS ASSISTANT v"
#if defined (JucePlugin_VersionString)
                       + juce::String (JucePlugin_VersionString)
#else
                       + juce::String (0.1) + " (test)"
#endif
                   ,
                   juce::dontSendNotification);
    title.setColour (juce::Label::textColourId, runui::accent());
    title.setJustificationType (juce::Justification::centredLeft);
    title.setFont (runui::font (15.0f));
    addAndMakeVisible (title);

    const char* engName[3] = { "Off", "Up", "Down" };
    const char* engTip[3] = {
        "Engine off: all MIDI passes through unmodified. Switching while a "
        "run is active cuts the run.",
        "Engine up: play two notes together to fire a run going up from the "
        "lower to the higher pitch. Switching while a run is active cuts "
        "the run.",
        "Engine down: play two notes together to fire a run going down from "
        "the higher to the lower pitch. Switching while a run is active "
        "cuts the run."
    };
    for (int i = 0; i < 3; ++i)
    {
        engineButtons[i].setButtonText (engName[i]);
        runui::setToggleShape (engineButtons[i], 0); // plain flat text button
        engineButtons[i].setRadioGroupId (1);
        engineButtons[i].setTooltip (engTip[i]);
        addAndMakeVisible (engineButtons[i]);
    }
    // The buttons DON'T bind to the APVTS parameter: REAPER echoes host-cached
    // discrete-param values back on UI focus changes, and a gesture-driven
    // edit ping-ponged the state. Clicks go directly into the processor's
    // switch queue (requestEngineState); the visuals follow the REAL engine
    // state from the audio thread (Timer below), not the parameter (S3.1).
    for (int i = 0; i < 3; ++i)
    {
        engineButtons[i].setName ("Engine " + juce::String (engTip[i][0]));
        engineButtons[i].onClick = [this, i] { processor.requestEngineState (i); };
    }
    timerCallback(); // initial visual sync from the current engine state

    // ---- Scale rows (S9 / D12) ------------------------------------------
    tonicLabel.setText ("Tonic", juce::dontSendNotification);    tonicLabel.setColour (juce::Label::textColourId, runui::text());
    addAndMakeVisible (tonicLabel);
    for (int i = 0; i < 12; ++i)
        tonicCombo.addItem (kPitchNames[i], i + 1);
    addAndMakeVisible (tonicCombo);

    modeLabel.setText ("Mode", juce::dontSendNotification);
    modeLabel.setColour (juce::Label::textColourId, runui::text());
    addAndMakeVisible (modeLabel);
    // 17 entries: the 16 named modes + "Custom", names exactly matching the
    // Mode parameter strings (ScaleModel is the single source of truth, S9).
    for (int m = 0; m <= runsp::kCustomMode; ++m)
        modeCombo.addItem (m < runsp::kNumNamedModes
                               ? juce::String (runsp::modeName (m))
                               : juce::String ("Custom"),
                           m + 1);
    addAndMakeVisible (modeCombo);

    tonicCombo.onChange = [this] { refreshScaleTicks (false); };
    modeCombo.onChange = [this] { refreshScaleTicks (false); };
    tonicCombo.setTooltip ("Scale root (C..B), host-automatable; the grid "
                           "below and the run's scale follow it.");
    tonicLabel.setTooltip (tonicCombo.getTooltip());
    modeCombo.setTooltip ("Scale mode: choosing a mode re-ticks the grid; "
                          "manual ticks switch the mode to Custom. "
                          "Chromatic = all twelve ticked.");
    modeLabel.setTooltip (modeCombo.getTooltip());

    // populate the combos from the parameters first, then attach so the
    // attachments reflect the current values without spurious tick resets
    refreshScaleTicks (true);
    tonicAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            processor.apvts, "tonic", tonicCombo);
    modeAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            processor.apvts, "mode", modeCombo);

    for (int i = 0; i < 12; ++i)
    {
        runui::setToggleShape (pitchBoxes[i], 1); // checkbox square
        pitchBoxes[i].setButtonText (kPitchNames[i]);
        pitchBoxes[i].setName (kPitchNames[i]);
        pitchBoxes[i].setTooltip (
            "Tick to include pitch class "
            + juce::String (kPitchNames[i])
            + " in the run's scale (screen-reader: pitch class "
            + juce::String (kPitchNames[i]) + "). Changing any tick "
              "switches the mode to Custom and keeps the tick set.");
        pitchBoxes[i].onClick = [this, i] { pitchTickChanged (i); };
        addAndMakeVisible (pitchBoxes[i]);
    }
    processor.apvts.addParameterListener ("tonic", this);
    processor.apvts.addParameterListener ("mode", this);

    // ---- Slider rows (S9) -----------------------------------------------
    styleParamRow (beats, "Beats",
                   "Run length in host beats: how long the run takes to "
                   "sweep from the first note to the target pitch.",
                   1.0, 16.0, 1.0, 4.0);
    styleParamRow (density, "Density",
                   "Notes per beat: the run emits round(density x beats) "
                   "notes. 1.0 is one note per beat.",
                   1.0, 16.0, 0.0, 4.0);
    styleParamRow (curve, "Curve",
                   "Timing curve strength: 0% is a uniform sweep; higher "
                   "values play the start and end of the run deliberately "
                   "spread out and rush through the middle "
                   "(slow, quick, slow).",
                   0.0, 1.0, 0.0, 0.5);
    styleParamRow (accent, "Accent",
                   "Beat emphasis: on-beat notes play louder, scaled by how "
                   "close they land to the nearest beat; downbeats get the "
                   "strongest push.",
                   0.0, 1.0, 0.0, 0.5);
    styleParamRow (arc, "Arc",
                   "Velocity swell across the run: negative fades out, "
                   "positive fades in, applied as a final velocity "
                   "multiplier.",
                   -1.0, 1.0, 0.0, 0.0);
    const char* sliderIds[5] = { "beats", "density", "curve", "accent", "arc" };
    ParamRow* sliderRows[5] = { &beats, &density, &curve, &accent, &arc };
    for (int i = 0; i < 5; ++i)
    {
        addAndMakeVisible (sliderRows[i]->name);
        addAndMakeVisible (sliderRows[i]->slider);
        addAndMakeVisible (sliderRows[i]->readout);
        sliderRows[i]->attachment =
            std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
                processor.apvts, sliderIds[i], sliderRows[i]->slider);
        sliderRows[i]->slider.addListener (this);
    }
    updateReadouts();

    // ---- Walk radios (S9 / D10) -----------------------------------------
    walkLabel.setText ("Walk", juce::dontSendNotification);
    walkLabel.setColour (juce::Label::textColourId, runui::text());
    addAndMakeVisible (walkLabel);
    foldButton.setRadioGroupId (2);
    zigzagButton.setRadioGroupId (2);
    runui::setToggleShape (foldButton, 2);
    runui::setToggleShape (zigzagButton, 2);
    foldButton.setTooltip (
        "Fold: when the run needs more notes than scale degrees in the "
        "span, it bounces back at the span edges.");
    zigzagButton.setTooltip (
        "Zig-zag: when over-dense, the run stays monotonic with periodic "
        "single backward steps distributed through the run.");
    addAndMakeVisible (foldButton);
    addAndMakeVisible (zigzagButton);
    // The walk parameter is a 2-choice; the Zig-zag toggle maps toggle-on =
    // choice 1, toggle-off = choice 0 (Fold), so the radio pair mirrors the
    // parameter both ways (D13 choice mapping 0..63 Fold / 64..127 Zig-zag).
    walkAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            processor.apvts, "walk", zigzagButton);
    // Fold mirrors the inverted value via the editor APVTS listener; both
    // follow inbound CC/automation (note: Fold has no attachment - the
    // listener keeps the radio pair mutually exclusive).
    foldButton.setToggleState (
        ! zigzagButton.getToggleState(), juce::dontSendNotification);
    zigzagButton.onClick = [this]
    {
        zigzagButton.setToggleState (true, juce::dontSendNotification);
        foldButton.setToggleState (false, juce::dontSendNotification);
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (
            processor.apvts.getParameter ("walk"));
        if (p != nullptr)
            p->setValueNotifyingHost (p->convertTo0to1 (1.0f));
    };
    foldButton.onClick = [this]
    {
        foldButton.setToggleState (true, juce::dontSendNotification);
        zigzagButton.setToggleState (false, juce::dontSendNotification);
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (
            processor.apvts.getParameter ("walk"));
        if (p != nullptr)
            p->setValueNotifyingHost (p->convertTo0to1 (0.0f));
    };

    // ---- Overlap (legato) toggle (S5.8) ----------------------------------
    // The walk parameter is a 2-choice; the Overlap toggle is a plain bool
    // parameter, so the ButtonAttachment mirrors GUI, host automation and
    // the saved chunk state both ways.
    overlapLabel.setText ("Overlap", juce::dontSendNotification);
    overlapLabel.setColour (juce::Label::textColourId, runui::text());
    overlapLabel.setTooltip (overlapButton.getTooltip());
    addAndMakeVisible (overlapLabel);
    overlapButton.setName ("Legato overlap");
    overlapButton.setTooltip (
        "Legato overlap: hold each run note slightly past the next note-on "
        "so instruments with a legato mode (which listens for overlapping "
        "notes) engage it. Off: each note stops before the next starts.");
    addAndMakeVisible (overlapButton);
    overlapAttachment =
        std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (
            processor.apvts, "overlap", overlapButton);

    // ---- Bottom row (S9) -------------------------------------------------
    settingsButton.setTooltip (
        "Settings: engine switch source and event numbers, per-parameter CC "
        "bindings, tuning constants, reset to defaults.");
    settingsButton.onClick = [this] { ensureDialog(); };
    addAndMakeVisible (settingsButton);

    runui::setToggleShape (debugToggle, 2);
    debugToggle.setTooltip (
        "Show the debug status readout: engine state, pending note, run "
        "progress, counters, playhead beat and tempo.");
    debugToggle.onClick = [this]
    { toggleOverlay (debugToggle.getToggleState()); };
    addAndMakeVisible (debugToggle);

    // ---- Window geometry (S9 + RUNV footer, S7) --------------------------
    int w, h;
    processor.getEditorWindowSize (w, h);
    setResizeLimits (720, 320, 1400, 600); // S9 resize limits
    setSize (juce::jlimit (720, 1400, w), juce::jlimit (320, 600, h));
    startTimerHz (20); // engine-state mirror (host/CC writes included)
}

void RunsEditor::timerCallback()
{
    const int state = juce::jlimit (
        0, 2, processor.publishedEngineState.load (std::memory_order_relaxed));
    if (state != engineButtonState)
    {
        engineButtonState = state;
        refreshGuard = true;
        for (int i = 0; i < 3; ++i)
            engineButtons[i].setToggleState (i == state,
                                             juce::dontSendNotification);
        refreshGuard = false;
    }
}

RunsEditor::~RunsEditor()
{
    processor.apvts.removeParameterListener ("tonic", this);
    processor.apvts.removeParameterListener ("mode", this);
    // RUNV footer (S7): the last known geometry is the current one.
    processor.recordEditorWindowSize (getWidth(), getHeight());
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

void RunsEditor::paint (juce::Graphics& g)
{
    g.fillAll (runui::bg());
}

// D12: the 12 tick boxes always mirror the current scale. mode < 16: re-tick
// from the mode offsets relative to the shown tonic. mode == Custom: the
// stored mask (relative to the shown tonic) drives the ticks. A manual tick
// updates the stored mask and switches the Mode parameter to Custom, which
// re-computes the same set through the parameter listener (idempotent).
void RunsEditor::refreshScaleTicks (bool keepTicks)
{
    auto readParam = [&] (const char* id, float maxV)
    {
        auto* r = dynamic_cast<juce::RangedAudioParameter*> (
            processor.apvts.getParameter (id));
        return r != nullptr
                   ? juce::jlimit (0.0f, maxV,
                                   r->convertFrom0to1 (r->getValue()))
                   : 0.0f;
    };
    const int tonic = (int) std::lround (readParam ("tonic", 11.0f));
    const int mode =
        (int) std::lround (readParam ("mode", (float) runsp::kCustomMode));

    uint16_t mask = 0;
    if (keepTicks)
    {
        mask = processor.settings.customOffsets; // keep the shown tick set
    }
    else if (mode == runsp::kCustomMode)
    {
        mask = processor.settings.customOffsets;
    }
    else
    {
        const runsp::ModeInfo& m = runsp::modeInfo (mode);
        for (int i = 0; i < m.numOffsets; ++i)
            mask = (uint16_t) (mask | (1u << m.offsets[i]));
        processor.settings.customOffsets = mask;
    }
    refreshGuard = true;
    for (int pc = 0; pc < 12; ++pc)
    {
        const int rel = (pc - tonic + 12) % 12;
        pitchBoxes[pc].setToggleState ((mask >> rel) & 1u,
                                       juce::dontSendNotification);
    }
    refreshGuard = false;
}

void RunsEditor::parameterChanged (const juce::String& paramID, float)
{
    // Tonic/mode changed by CC mirror, host automation or a loaded chunk:
    // the tick grid re-derives from the parameters (D12); mode == Custom
    // keeps the stored tick set.
    if (paramID == "tonic" || paramID == "mode")
        refreshScaleTicks (false);
    if (paramID == "walk")
    {
        // Mirror the radio pair from inbound CC/automation (D13).
        auto* p = dynamic_cast<juce::RangedAudioParameter*> (
            processor.apvts.getParameter ("walk"));
        if (p != nullptr)
        {
            const int idx = (int) std::lround (
                juce::jlimit (0.0f, 1.0f, p->convertFrom0to1 (p->getValue())));
            refreshGuard = true;
            zigzagButton.setToggleState (idx == 1, juce::dontSendNotification);
            foldButton.setToggleState (idx == 0, juce::dontSendNotification);
            refreshGuard = false;
        }
    }
}

void RunsEditor::pitchTickChanged (int pitchClass)
{
    if (refreshGuard)
        return;
    auto* tonicParam = dynamic_cast<juce::RangedAudioParameter*> (
        processor.apvts.getParameter ("tonic"));
    auto* modeParam = dynamic_cast<juce::RangedAudioParameter*> (
        processor.apvts.getParameter ("mode"));
    if (tonicParam == nullptr || modeParam == nullptr)
        return;
    const int tonic = (int) std::lround (
        juce::jlimit (0.0f, 11.0f, tonicParam->convertFrom0to1 (tonicParam->getValue())));
    const int rel = (pitchClass - tonic + 12) % 12;
    uint16_t mask = processor.settings.customOffsets;
    if (pitchBoxes[pitchClass].getToggleState())
        mask = (uint16_t) (mask | (1u << rel));
    else
        mask = (uint16_t) (mask & ~(1u << rel));
    processor.settings.customOffsets = mask;
    processor.markStateDirty(); // chunk dirty (S7)
    if ((int) std::lround (modeParam->convertFrom0to1 (modeParam->getValue()))
        != runsp::kCustomMode)
        modeParam->setValueNotifyingHost (
            modeParam->convertTo0to1 ((float) runsp::kCustomMode));
    refreshScaleTicks (false); // re-sync the tick display with the new state
}

void RunsEditor::sliderValueChanged (juce::Slider*)
{
    updateReadouts();
}

void RunsEditor::updateReadouts()
{
    beats.readout.setText (
        juce::String (juce::roundToInt (
            juce::jlimit (1.0, 16.0, beats.slider.getValue()))),
        juce::dontSendNotification);
    density.readout.setText (juce::String (density.slider.getValue(), 1)
                                 + " n/beat",
                             juce::dontSendNotification);
    curve.readout.setText (juce::String (juce::roundToInt (
                               curve.slider.getValue() * 100.0))
                               + "%",
                           juce::dontSendNotification);
    accent.readout.setText (juce::String (juce::roundToInt (
                                accent.slider.getValue() * 100.0))
                                + "%",
                            juce::dontSendNotification);
    const int arcPct = juce::roundToInt (arc.slider.getValue() * 100.0);
    arc.readout.setText ((arcPct > 0 ? "+" : "") + juce::String (arcPct)
                             + "%",
                         juce::dontSendNotification);
}

void RunsEditor::ensureDialog()
{
    if (settingsDialog == nullptr)
    {
        settingsPanel = std::make_unique<SettingsPanel> (processor);
        settingsDialog = std::make_unique<SettingsDialog>();
        settingsPanel->setSize (560, 470);
        settingsDialog->setContentNonOwned (settingsPanel.get(), true);
    }
    settingsPanel->refresh();
    settingsDialog->centreAroundComponent (this, 570, 510);
    settingsDialog->setVisible (true);
    settingsDialog->toFront (true);
    if (! settingsDialog->isCurrentlyModal())
        settingsDialog->enterModalState (
            true,
            juce::ModalCallbackFunction::create ([] (int) {}),
            false);
}

void RunsEditor::toggleOverlay (bool on)
{
    if (on && overlayPanel == nullptr)
    {
        overlayPanel = std::make_unique<DebugOverlayPanel> (processor);
        addAndMakeVisible (overlayPanel.get());
    }
    else if (! on && overlayPanel != nullptr)
    {
        overlayPanel = nullptr; // destructor stops its Timer
    }
    resized();
}

void RunsEditor::resized()
{
    auto area = getLocalBounds().reduced (8);

    // Title + engine row (S9)
    auto top = area.removeFromTop (28);
    title.setBounds (top.removeFromLeft (200));
    auto engArea = top.removeFromRight (240);
    for (int i = 0; i < 3; ++i)
        engineButtons[i].setBounds (engArea.removeFromLeft (80).reduced (2, 2));
    area.removeFromTop (8);

    // Scale panel (D12)
    auto scalePanel = area.removeFromTop (78);
    auto comboRow = scalePanel.removeFromTop (26).reduced (0, 3);
    tonicLabel.setBounds (comboRow.removeFromLeft (52));
    tonicCombo.setBounds (comboRow.removeFromLeft (100));
    comboRow.removeFromLeft (16);
    modeLabel.setBounds (comboRow.removeFromLeft (52));
    modeCombo.setBounds (comboRow.removeFromLeft (280));
    scalePanel.removeFromTop (2);
    {
        auto tickRow = scalePanel.removeFromTop (26);
        const int w = tickRow.getWidth() / 12;
        for (int i = 0; i < 12; ++i)
            pitchBoxes[i].setBounds (tickRow.removeFromLeft (w).reduced (2, 3));
    }

    area.removeFromTop (8);

    // Parameter rows (S9)
    auto paramArea = area.removeFromTop (100);
    if (true)
    {
        auto r = paramArea.removeFromTop (28);
        beats.name.setBounds (r.removeFromLeft (56));
        beats.slider.setBounds (r.removeFromLeft (r.getWidth() / 2 - 200));
        beats.readout.setBounds (r.removeFromLeft (70));
        r.removeFromLeft (16);
        density.name.setBounds (r.removeFromLeft (64));
        density.slider.setBounds (r.removeFromLeft (r.getWidth() - 100));
        density.readout.setBounds (r);
    }
    paramArea.removeFromTop (2);
    {
        auto r = paramArea.removeFromTop (28);
        curve.name.setBounds (r.removeFromLeft (56));
        curve.slider.setBounds (r.removeFromLeft (140));
        curve.readout.setBounds (r.removeFromLeft (60));
        r.removeFromLeft (16);
        accent.name.setBounds (r.removeFromLeft (64));
        accent.slider.setBounds (r.removeFromLeft (140));
        accent.readout.setBounds (r.removeFromLeft (60));
        r.removeFromLeft (16);
        arc.name.setBounds (r.removeFromLeft (40));
        arc.slider.setBounds (r.removeFromLeft (r.getWidth() - 70));
        arc.readout.setBounds (r);
    }
    paramArea.removeFromTop (2);
    {
        auto r = paramArea.removeFromTop (28);
        walkLabel.setBounds (r.removeFromLeft (56));
        foldButton.setBounds (r.removeFromLeft (96).reduced (2, 2));
        zigzagButton.setBounds (r.removeFromLeft (116).reduced (2, 2));
        r.removeFromLeft (16);
        overlapLabel.setBounds (r.removeFromLeft (64));
        overlapButton.setBounds (r.removeFromLeft (140).reduced (2, 2));
    }
    area.removeFromTop (8);

    // Debug overlay strip (S9 bottom row) when toggled on
    if (overlayPanel != nullptr)
    {
        overlayPanel->setBounds (area.removeFromTop (5 * 18 + 20));
        area.removeFromTop (6);
    }

    // Bottom row (S9)
    auto bottom = area.removeFromTop (28);
    settingsButton.setBounds (bottom.removeFromLeft (120).reduced (2, 2));
    debugToggle.setBounds (bottom.removeFromRight (140).reduced (2, 2));

    // RUNV footer (S7): persist the current editor window size.
    processor.recordEditorWindowSize (getWidth(), getHeight());
}
