#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "../processor/PluginProcessor.h"
#include "Theme.h"

#include <memory>

class SettingsPanel;
class DebugOverlayPanel;
class CurveView;
class MidiDragButton;

// S9 panel (plan.md P3). Layout shell + dark theme + data-bound controls:
//   title row:   title + Settings button + engine Off/Up/Down toggles
//   scale row:   tonic combo, mode combo (17 entries), 12 pitch-class ticks
//   param rows:  Beats (stepped), Density, Curve, Accent, Arc + readouts
//                and a "ccNN:VV" bound-CC badge on every CC-mapped control
//   walk row:    Fold / Zig-zag radios (walk param) + Overlap legato toggle
//   curve strip: square curve shape preview + Humanize toggle / Seed field /
//                humanize strength sliders (S5.9) to the right of it (S9)
//   rule + "Midi Export" heading
//   export row:  Start / Target note dropdowns, their velocities, Drag MIDI
//   scale warn:  red line when Start/Target falls outside the selected scale
//   rule + "Copy & Paste Settings Between Runs Assistant Instances" heading
//   bottom row:  Copy / Paste clipboard preset buttons + Debug overlay (S9)
// Engine / scale / walk / overlap / slider state lives in the APVTS, so host
// automation and bound-CC mirrors arrive through the attachments (S4/S11);
// the processor observes an engine value change at block time and cuts the
// active run (S5.6).
class RunsEditor : public juce::AudioProcessorEditor,
                   private juce::Slider::Listener,
                   private juce::TextEditor::Listener,
                   private juce::Timer,
                   private juce::AudioProcessorValueTreeState::Listener
{
public:
    explicit RunsEditor (RunsProcessor& p);
    ~RunsEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // Slider + readout bundle (S9 rows); top-level for the styling helper.
    struct ParamRow
    {
        juce::Label name;
        juce::Slider slider;
        juce::Label readout;
        // Bound-CC badge right of the readout: "cc88:29" or "cc:--" (none).
        juce::Label cc;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment>
            attachment;
    };

private:
    // juce::Slider::Listener: refresh the value readouts
    void sliderValueChanged (juce::Slider*) override;
    // TextEditor::Listener: the S5.9 Seed field commits on Return / focus
    // loss (parse + clamp, then through the parameter notify path).
    void textEditorReturnKeyPressed (juce::TextEditor&) override;
    void textEditorFocusLost (juce::TextEditor&) override;
    // juce::Timer: mirrors the real engine state (audio->UI FIFO) onto the
    // engine buttons (param echoes must not drive the visuals)
    void timerCallback() override;
    // APVTS listener: tonic/mode changes (GUI combos, CC/automation, load)
    // re-tick the pitch-class grid (D12)
    void parameterChanged (const juce::String& paramID,
                           float newValue) override;

    void refreshScaleTicks (bool keepTicks);
    void pitchTickChanged (int pitchClass);
    // Offline export: warns (red line + red dropdown + tooltip) when Start
    // and/or Target is outside the selected tonic/mode/tick set. Advisory
    // only - the render still snaps the endpoints inward (S5.3).
    void refreshScaleWarning();
    void updateReadouts();
    // Bound-CC badges (S4/S9): "ccNN:VV" beside every CC-mapped control -
    // VV = the CC value reproducing the current setting (processor inverse
    // of the applyBoundCc map), "cc:--" when unbound / not CC-controlled.
    // Refreshed from the 20 Hz timer (settings have no change listener) and
    // from updateReadouts() for instant feedback while dragging.
    void updateCcBadges();
    void ensureDialog();
    void toggleOverlay (bool on);
    // Clipboard preset (S9): JSON text <-> system clipboard
    void copyToClipboard();
    void pasteFromClipboard();
    // Mirrors the walk radio pair from the parameter (Fold has no
    // attachment; D13) - used by the APVTS listener and after a paste.
    void syncWalkRadios();
    // S5.9 seed field: write the parsed value into the "seed" parameter
    // (Return / focus loss) and pull the current parameter back into the
    // field from the 20 Hz timer (skipped while the field has focus).
    void commitSeed();
    void refreshSeedText();
    // Transient button-label feedback for the clipboard actions
    void flashButton (juce::TextButton& button, const juce::String& text);

    RunsProcessor& processor;

    runui::RunsLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltips { this, 400 };

    // Title row
    juce::Label title;
    juce::ToggleButton engineButtons[3];
    int engineButtonState = -1; // mirrors publishedEngineState; -1 unsynced
    // Bound-CC badge left of the engine buttons (cc87:60 / cc:-- unless the
    // engine source is CC).
    juce::Label engineCc;

    // Scale rows
    juce::Label tonicLabel, modeLabel;
    // Bound-CC badges right of the two combos.
    juce::Label tonicCc, modeCc;
    juce::ComboBox tonicCombo, modeCombo;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment>
        tonicAttachment, modeAttachment;
    juce::ToggleButton pitchBoxes[12];
    bool refreshGuard = false;

    // Parameter sliders + readouts
    ParamRow beats, density, curve, accent, arc;
    // Last badge text per label (updateCcBadges only writes on change; the
    // badges refresh at 20 Hz). Index order: 0..4 beats..arc, 5 tonic,
    // 6 mode, 7 walk, 8 engine.
    juce::String ccTextCache[9];

    // Walk radios + overlap (legato) toggle
    juce::Label walkLabel;
    // Bound-CC badge right of the Fold / Zig-zag radios.
    juce::Label walkCc;
    juce::ToggleButton foldButton { "Fold" };
    juce::ToggleButton zigzagButton { "Zig-zag" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        walkAttachment;
    juce::Label overlapLabel;
    juce::ToggleButton overlapButton { "Overlap" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        overlapAttachment;

    // S5.9 humanize cluster (curve strip, right of the preview): the toggle
    // has an attachment; the seed field edits the "seed" param by hand; the
    // two strength sliders edit the chunk-only humanize tunables directly
    // (no APVTS, no badge - none of the three is CC-mappable in v1).
    juce::ToggleButton humanizeButton { "Humanize" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment>
        humanizeAttachment;
    juce::Label seedLabel;
    juce::TextEditor seedEditor;
    juce::Label hvelName, hvelReadout;
    juce::Slider hvelSlider;
    juce::Label htimeName, htimeReadout;
    juce::Slider htimeSlider;

    // Bottom row + its section heading and the rule above the heading (the
    // rule itself is painted by the editor, not a child component).
    juce::Label clipboardHeader;
    juce::Rectangle<int> clipboardRule;
    juce::TextButton copyButton { "Copy to Clipboard" };
    juce::TextButton pasteButton { "Paste from Clipboard" };
    juce::TextButton settingsButton { "Settings" };
    juce::ToggleButton debugToggle { "Debug overlay" };

    // Offline export row (session-only: no APVTS, no chunk, dropped when the
    // editor closes). Explicit run endpoints + the trigger velocities that
    // feed the S5.3 base fade, then the drag affordance for the rendered .mid.
    juce::Label fromLabel, targetLabel;
    juce::ComboBox fromCombo, targetCombo;
    ParamRow velFrom, velTo;
    std::unique_ptr<MidiDragButton> dragButton;
    int exportFrom = 60;   // C4
    int exportTarget = 72; // C5
    // "Midi Export" heading + the rule above it (painted by the editor), and
    // the red warning line under the export row for out-of-scale endpoints.
    juce::Label exportHeader;
    juce::Rectangle<int> exportRule;
    juce::Label scaleWarning;
    // Renders the export and returns its path (empty on failure, where the
    // reason is flashed on the drag button instead).
    juce::String prepareMidiDrag();
    // Clipboard feedback: the flashed button, its normal label and the
    // remaining ticks of the 20 Hz restore timer.
    juce::TextButton* flashedButton = nullptr;
    juce::String flashedRestore;
    int flashTicks = 0;

    // owned in the .cpp (defined types live there)
    std::unique_ptr<SettingsPanel> settingsPanel;
    std::unique_ptr<juce::DialogWindow> settingsDialog;
    std::unique_ptr<DebugOverlayPanel> overlayPanel;
    std::unique_ptr<CurveView> curveView;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RunsEditor)
};
