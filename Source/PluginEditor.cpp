#include "PluginEditor.h"
#include "PluginParameters.h"

namespace
{
    const juce::Colour kBackground { 0xff1c1f24 };
    const juce::Colour kPanel { 0xff262a31 };
    const juce::Colour kAccent { 0xff4fc3a1 };
    const juce::Colour kText { 0xffe6e8eb };
    const juce::Colour kDim { 0xff8a919c };

    juce::String midiNoteName(int midiNote)
    {
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        return juce::String(names[midiNote % 12]) + juce::String(midiNote / 12 - 1);
    }
}

GuitarToMidiAudioProcessorEditor::GuitarToMidiAudioProcessorEditor(GuitarToMidiAudioProcessor& p)
    : AudioProcessorEditor(&p), processor(p)
{
    using namespace gtm::param;

    setup(mode, modeId, "Playing Mode",
          "Chords & Notes: polyphonic, for chords, arpeggios and melodies.\n"
          "Single Notes: monophonic lead mode, lowest latency, smooth pitch bends.");
    setup(response, responseId, "Response",
          "How long the plugin may listen after a pick attack before committing notes.\n"
          "Fast suits live playing; Accurate trades a little latency for fewer mistakes.");
    setup(gate, gateId, "Noise Gate",
          "Input level below which nothing is triggered. Raise it if hum or string noise "
          "produces notes; lower it for very quiet playing.");
    setup(pickSensitivity, pickSensitivityId, "Pick Sensitivity",
          "How easily a pick attack starts a new note. Raise for soft or fingerstyle "
          "playing; lower if notes retrigger on their own.");
    setup(maxNotes, maxPolyphonyId, "Max Notes", "Most notes that can sound at once (6 = every string).");

    setup(midiMode, midiModeId, "MIDI Output",
          "Single Channel works with any synth. MPE puts each note on its own channel "
          "so every string can bend independently (needs an MPE synth).");
    setup(channel, outputChannelId, "MIDI Channel", "Output channel in Single Channel mode.");
    setup(pitchBend, pitchBendId, "Pitch bend",
          "Send string bends and vibrato as pitch bend. In Single Channel mode this only "
          "applies in Single Notes mode (one bend lane can't bend each chord note).");
    setup(bendRange, bendRangeId, "Bend Range",
          "Pitch-bend range in semitones. Set your synth to the same value (the plugin "
          "also sends it as RPN 0 when pitch bend is on).");
    setup(velocityDynamics, velocityDynamicsId, "Velocity Dynamics",
          "0 = every note at a fixed velocity, 1 = velocity fully follows how hard you pick.");
    setup(transpose, transposeId, "Transpose", "Shift output notes, e.g. -12 for a bass sound.");
    setup(tuning, tuningId, "Tuning (A4)", "Reference pitch your guitar is tuned to.");
    setup(monitor, monitorInputId, "Pass guitar audio through",
          "Leave off to output only MIDI; turn on to hear the dry guitar from this track.");

    for (auto* s : { &pickSensitivity.slider, &velocityDynamics.slider })
        s->setNumDecimalPlacesToDisplay(2);

    midiMode.box.onChange = [this] { updateEnablement(); };
    pitchBend.button.onStateChange = [this] { updateEnablement(); };
    mode.box.onChange = [this] { updateEnablement(); };
    updateEnablement();

    setSize(640, 470);
    startTimerHz(30);
}

GuitarToMidiAudioProcessorEditor::~GuitarToMidiAudioProcessorEditor()
{
    stopTimer();
}

void GuitarToMidiAudioProcessorEditor::setup(LabelledSlider& c, const char* paramId, const juce::String& text,
                                             const juce::String& tooltip)
{
    c.slider.setSliderStyle(juce::Slider::LinearHorizontal);
    c.slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
    c.slider.setColour(juce::Slider::thumbColourId, kAccent);
    c.slider.setColour(juce::Slider::trackColourId, kAccent.withAlpha(0.6f));
    c.slider.setTooltip(tooltip);
    c.label.setText(text, juce::dontSendNotification);
    c.label.setColour(juce::Label::textColourId, kText);
    c.label.setTooltip(tooltip);
    addAndMakeVisible(c.slider);
    addAndMakeVisible(c.label);
    c.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(processor.apvts, paramId, c.slider);
}

void GuitarToMidiAudioProcessorEditor::setup(LabelledCombo& c, const char* paramId, const juce::String& text,
                                             const juce::String& tooltip)
{
    if (auto* choice = dynamic_cast<juce::AudioParameterChoice*>(processor.apvts.getParameter(paramId)))
        c.box.addItemList(choice->choices, 1);
    c.box.setTooltip(tooltip);
    c.label.setText(text, juce::dontSendNotification);
    c.label.setColour(juce::Label::textColourId, kText);
    c.label.setTooltip(tooltip);
    addAndMakeVisible(c.box);
    addAndMakeVisible(c.label);
    c.attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment>(processor.apvts, paramId, c.box);
}

void GuitarToMidiAudioProcessorEditor::setup(Toggle& c, const char* paramId, const juce::String& text,
                                             const juce::String& tooltip)
{
    c.button.setButtonText(text);
    c.button.setTooltip(tooltip);
    c.button.setColour(juce::ToggleButton::tickColourId, kAccent);
    c.button.setColour(juce::ToggleButton::textColourId, kText);
    addAndMakeVisible(c.button);
    c.attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(processor.apvts, paramId, c.button);
}

void GuitarToMidiAudioProcessorEditor::updateEnablement()
{
    const bool mpe = midiMode.box.getSelectedItemIndex() == 1;
    const bool mono = mode.box.getSelectedItemIndex() == 1;
    channel.slider.setEnabled(!mpe);
    channel.label.setEnabled(!mpe);
    const bool bendUsable = mpe || mono;
    pitchBend.button.setEnabled(bendUsable);
    const bool bendOn = bendUsable && pitchBend.button.getToggleState();
    bendRange.slider.setEnabled(bendOn || mpe);
    bendRange.label.setEnabled(bendOn || mpe);
}

void GuitarToMidiAudioProcessorEditor::paint(juce::Graphics& g)
{
    g.fillAll(kBackground);

    auto header = getLocalBounds().removeFromTop(44).reduced(16, 0);
    g.setColour(kText);
    g.setFont(juce::Font(juce::FontOptions(22.0f, juce::Font::bold)));
    g.drawText("Guitar to MIDI", header, juce::Justification::centredLeft);

    // Now-playing display.
    g.setColour(kPanel);
    g.fillRoundedRectangle(notesArea.toFloat(), 8.0f);
    g.setColour(notesText.isEmpty() ? kDim : kAccent);
    g.setFont(juce::Font(juce::FontOptions(28.0f, juce::Font::bold)));
    g.drawText(notesText.isEmpty() ? juce::String("-") : notesText, notesArea.reduced(14, 0), juce::Justification::centred);

    // Input meter with the gate threshold marked.
    auto meter = meterArea.toFloat();
    g.setColour(kPanel);
    g.fillRoundedRectangle(meter, 3.0f);
    auto toX = [&](float db) { return meter.getX() + meter.getWidth() * juce::jlimit(0.0f, 1.0f, (db + 80.0f) / 80.0f); };
    const float gateDb = processor.apvts.getRawParameterValue(gtm::param::gateId)->load();
    const bool open = displayedLevelDb > gateDb;
    g.setColour(open ? kAccent : kDim);
    g.fillRoundedRectangle(meter.withRight(toX(displayedLevelDb)), 3.0f);
    g.setColour(juce::Colours::orange);
    g.fillRect(juce::Rectangle<float>(toX(gateDb) - 1.0f, meter.getY() - 3.0f, 2.0f, meter.getHeight() + 6.0f));
    g.setColour(kDim);
    g.setFont(juce::Font(juce::FontOptions(12.0f)));
    g.drawText("Input " + juce::String(displayedLevelDb, 0) + " dB   (orange = gate)",
               meterArea.translated(0, meterArea.getHeight() + 2).withHeight(16), juce::Justification::centredLeft);

    g.setColour(kDim);
    g.setFont(juce::Font(juce::FontOptions(13.0f, juce::Font::bold)));
    g.drawText("DETECTION", detectionHeader, juce::Justification::centredLeft);
    g.drawText("MIDI OUTPUT", outputHeader, juce::Justification::centredLeft);
}

void GuitarToMidiAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced(16);
    area.removeFromTop(34);

    notesArea = area.removeFromTop(56);
    area.removeFromTop(10);
    meterArea = area.removeFromTop(8);
    area.removeFromTop(26);

    auto left = area.removeFromLeft(area.getWidth() / 2).withTrimmedRight(12);
    auto right = area.withTrimmedLeft(12);

    const int rowH = 28, gap = 6, labelW = 118;
    auto row = [&](juce::Rectangle<int>& col, juce::Component& label, juce::Component& control)
    {
        auto r = col.removeFromTop(rowH);
        label.setBounds(r.removeFromLeft(labelW));
        control.setBounds(r);
        col.removeFromTop(gap);
    };

    detectionHeader = left.removeFromTop(20);
    row(left, mode.label, mode.box);
    row(left, response.label, response.box);
    row(left, gate.label, gate.slider);
    row(left, pickSensitivity.label, pickSensitivity.slider);
    row(left, maxNotes.label, maxNotes.slider);
    row(left, tuning.label, tuning.slider);
    left.removeFromTop(4);
    monitor.button.setBounds(left.removeFromTop(rowH));

    outputHeader = right.removeFromTop(20);
    row(right, midiMode.label, midiMode.box);
    row(right, channel.label, channel.slider);
    row(right, velocityDynamics.label, velocityDynamics.slider);
    row(right, transpose.label, transpose.slider);
    right.removeFromTop(0);
    pitchBend.button.setBounds(right.removeFromTop(rowH).withTrimmedLeft(labelW));
    right.removeFromTop(gap);
    row(right, bendRange.label, bendRange.slider);
}

void GuitarToMidiAudioProcessorEditor::timerCallback()
{
    const auto& state = processor.displayState;

    // Meter ballistics: fast attack, slower release.
    const float level = state.inputLevelDb.load();
    displayedLevelDb = level > displayedLevelDb ? level : std::max(level, displayedLevelDb - 1.5f);

    const int transposeBy = (int) processor.apvts.getRawParameterValue(gtm::param::transposeId)->load();
    juce::String text;
    const int n = std::min(6, state.numNotes.load());
    for (int i = 0; i < n; ++i)
        text << (i > 0 ? "  " : "") << midiNoteName(juce::jlimit(0, 127, state.notes[(size_t) i].load() + transposeBy));
    notesText = text;

    updateEnablement();
    repaint();
}
