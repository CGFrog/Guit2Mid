#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>

GuitarToMidiAudioProcessor::GuitarToMidiAudioProcessor()
    : AudioProcessor(BusesProperties()
                          .withInput("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMS", gtm::param::createLayout())
{
}

bool GuitarToMidiAudioProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    if (in.isDisabled() || out.isDisabled())
        return false;
    if (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo())
        return false;
    return in == out;
}

gtm::EngineSettings GuitarToMidiAudioProcessor::readSettings() const
{
    auto get = [this](const char* id) { return apvts.getRawParameterValue(id)->load(); };
    using namespace gtm::param;

    gtm::EngineSettings s;
    s.mode = (int) get(modeId) == 1 ? gtm::EngineSettings::Mode::mono : gtm::EngineSettings::Mode::poly;
    s.response = (int) get(responseId);
    s.gateDb = get(gateId);
    s.onsetSensitivity = get(pickSensitivityId);
    s.velocitySensitivity = get(velocityDynamicsId);
    s.maxPolyphony = (int) get(maxPolyphonyId);
    s.tuningA4 = get(tuningId);
    s.midi.mpe = (int) get(midiModeId) == 1;
    s.midi.channel = (int) get(outputChannelId);
    s.midi.bendEnabled = get(pitchBendId) > 0.5f;
    s.midi.bendRange = (int) get(bendRangeId);
    s.midi.transpose = (int) get(transposeId);
    return s;
}

void GuitarToMidiAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    engine_.setSettings(readSettings());
    engine_.prepare(sampleRate);
    monoBuffer_.assign((size_t) std::max(512, samplesPerBlock), 0.0f);
    chunkMidi_.ensureSize(4096);

    // Notes are committed as soon as they're identified, not after a fixed
    // delay, so there is no constant latency to report. Reporting one would
    // also make hosts delay everything else, which is wrong for live playing.
    setLatencySamples(0);
}

void GuitarToMidiAudioProcessor::releaseResources()
{
}

void GuitarToMidiAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    midiMessages.clear(); // this plugin only emits its own MIDI

    const int numSamples = buffer.getNumSamples();
    const int numInputChannels = std::min(getTotalNumInputChannels(), buffer.getNumChannels());

    engine_.setSettings(readSettings());
    if (wasBypassed_)
    {
        engine_.reset();
        wasBypassed_ = false;
    }

    // Downmix to mono and run the engine in chunks that fit the scratch buffer
    // (hosts may occasionally send bigger blocks than announced).
    const int chunk = (int) monoBuffer_.size();
    for (int start = 0; start < numSamples; start += chunk)
    {
        const int n = std::min(chunk, numSamples - start);
        std::fill(monoBuffer_.begin(), monoBuffer_.begin() + n, 0.0f);
        for (int ch = 0; ch < numInputChannels; ++ch)
            juce::FloatVectorOperations::add(monoBuffer_.data(), buffer.getReadPointer(ch, start), n);
        if (numInputChannels > 1)
            juce::FloatVectorOperations::multiply(monoBuffer_.data(), 1.0f / (float) numInputChannels, n);

        if (start == 0)
            engine_.process(monoBuffer_.data(), n, midiMessages);
        else
        {
            chunkMidi_.clear();
            engine_.process(monoBuffer_.data(), n, chunkMidi_);
            midiMessages.addEvents(chunkMidi_, 0, n, start);
        }
    }

    displayState.inputLevelDb.store(engine_.getLevelDb());
    std::array<int, 6> notes {};
    const int count = engine_.getActiveNotes(notes.data(), (int) notes.size());
    for (int i = 0; i < count; ++i)
        displayState.notes[(size_t) i].store(notes[(size_t) i]);
    displayState.numNotes.store(count);

    if (apvts.getRawParameterValue(gtm::param::monitorInputId)->load() < 0.5f)
        buffer.clear();
}

void GuitarToMidiAudioProcessor::processBlockBypassed(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    // Never leave notes hanging when the plugin is bypassed mid-note.
    midiMessages.clear();
    if (!wasBypassed_)
    {
        engine_.allNotesOff(midiMessages, 0);
        wasBypassed_ = true;
    }
    displayState.numNotes.store(0);
    juce::ignoreUnused(buffer);
}

juce::AudioProcessorEditor* GuitarToMidiAudioProcessor::createEditor()
{
    return new GuitarToMidiAudioProcessorEditor(*this);
}

void GuitarToMidiAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    if (auto state = apvts.copyState(); state.isValid())
    {
        juce::MemoryOutputStream stream(destData, true);
        state.writeToStream(stream);
    }
}

void GuitarToMidiAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto tree = juce::ValueTree::readFromData(data, (size_t) sizeInBytes);
    if (tree.isValid())
        apvts.replaceState(tree);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new GuitarToMidiAudioProcessor();
}
