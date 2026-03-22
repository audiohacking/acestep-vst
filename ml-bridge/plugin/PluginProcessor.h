#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <vector>

#include "PluginPreview.h"

class AcestepAudioProcessor : public juce::AudioProcessor,
                               public juce::AsyncUpdater
{
public:
    // Submitting = running ace-lm (LLM step, text-to-music) or preparing cover request
    // Running    = running ace-synth (DiT + VAE synthesis)
    enum class State { Idle, Submitting, Running, Succeeded, Failed };

    AcestepAudioProcessor();
    ~AcestepAudioProcessor() override;

    // ── AudioProcessor overrides ──────────────────────────────────────────────
    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;
    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram(int index) override;
    const juce::String getProgramName(int index) override;
    void changeProgramName(int index, const juce::String& newName) override;

    bool isBusesLayoutSupported(const juce::AudioProcessor::BusesLayout&) const override;

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    void handleAsyncUpdate() override;

    // ── Global settings persistence (survives across all DAW sessions) ────────
    void saveSettingsToGlobalConfig() const;
    void loadSettingsFromGlobalConfig();

    // ── DiT model selection ───────────────────────────────────────────────────
    // Turbo = fast (default), Sft = fine-tuned quality, Base = base model
    // (required for Lego mode — auto-selected when legoTrack is non-empty).
    enum class DitModel { Turbo = 0, Sft = 1, Base = 2 };

    void     setDitModel(DitModel m);
    DitModel getDitModel() const;

    static juce::String ditModelFilename(DitModel m);

    // ── Generation ────────────────────────────────────────────────────────────
    // srcAudioFile : non-empty → cover mode (repaint) or lego backing track.
    // bpm          : 0 = auto-detect from DAW playhead.
    // lyrics       : lyrics text; use "[Instrumental]" for no vocals.
    // seed         : -1 = random.
    // ditModel     : model variant; lego mode always forces Base.
    // legoTrack    : non-empty → lego mode; value is the instrument track name
    //                (e.g. "guitar").  Requires srcAudioFile to be set.
    void startGeneration(const juce::String& prompt,
                         int   durationSeconds = 10,
                         int   inferenceSteps  = 8,
                         juce::File srcAudioFile = {},
                         float coverStrength   = 0.5f,
                         float bpm             = 0.0f,
                         const juce::String& lyrics    = "[Instrumental]",
                         int   seed            = -1,
                         DitModel ditModel     = DitModel::Turbo,
                         const juce::String& legoTrack = {});

    // ── Audio format ──────────────────────────────────────────────────────────
    // WAV  → passes --wav to ace-synth (48 kHz PCM, lossless)
    // MP3  → default ace-synth output (lossy)
    enum class AudioFormat { WAV = 0, MP3 = 1 };

    void setAudioFormat(AudioFormat fmt);
    AudioFormat getAudioFormat() const;

    // ── Path configuration (persisted in DAW project state) ──────────────────
    void setBinariesPath(const juce::String& path);
    juce::String getBinariesPath() const;

    void setModelsPath(const juce::String& path);
    juce::String getModelsPath() const;

    void setOutputPath(const juce::String& path);
    juce::String getOutputPath() const;

    juce::File getModelsDirectory()  const;
    juce::File getLibraryDirectory() const;  // where generated audio is saved

    // Returns the directory inside the plugin bundle where ace-lm / ace-synth
    // are embedded by the CI build (same folder as the plugin's own binary).
    static juce::File getBundledBinariesDirectory();

    bool areBinariesReady() const;

    // ── State / status ────────────────────────────────────────────────────────
    State        getState()      const { return state_.load(); }
    juce::String getStatusText() const;
    juce::String getLastError()  const;

    // ── Streaming log (background thread writes; message thread drains) ───────
    // Returns any new log lines appended since the last call, then clears the
    // pending buffer. Safe to call from the message thread at any frequency.
    juce::String getAndClearNewLog();

    // ── Host BPM — updated every processBlock() from the DAW playhead ─────────
    double getHostBpm() const { return hostBpm_.load(std::memory_order_relaxed); }

    // ── Playback control ──────────────────────────────────────────────────────
    void previewLibraryEntry(const juce::File& file); // load + play on msg thread
    void stopPlayback();
    void setLoopPlayback(bool loop);
    bool isLoopPlayback() const { return loopPlayback_.load(); }

    // ── Library management ────────────────────────────────────────────────────
    struct LibraryEntry { juce::File file; juce::String prompt; juce::Time time; };
    std::vector<LibraryEntry> getLibraryEntries() const;
    void addToLibrary(const juce::File& audioFile, const juce::String& prompt);
    bool deleteLibraryEntry(const juce::File& file);
    bool importAudioFile(const juce::File& sourceFile); // copy to library dir

private:
    void runGenerationThread(juce::String prompt, int durationSec, int inferenceSteps,
                             juce::File srcAudioFile, float coverStrength, float bpm,
                             juce::String lyrics, int seed,
                             DitModel ditModel, juce::String legoTrack);

    // ── Settings paths ────────────────────────────────────────────────────────
    mutable juce::CriticalSection pathsLock_;
    juce::String binariesPath_, modelsPath_, outputPath_;
    AudioFormat  audioFormat_{ AudioFormat::WAV };   // default WAV (48 kHz)
    DitModel     ditModel_   { DitModel::Turbo  };   // default Turbo

    // ── State machine ─────────────────────────────────────────────────────────
    std::atomic<State> state_{ State::Idle };
    mutable juce::CriticalSection statusLock_;
    juce::String lastError_, statusText_;

    // ── BPM from DAW playhead ─────────────────────────────────────────────────
    std::atomic<double> hostBpm_{ 0.0 };

    // ── Audio preview (AudioTransportSource-backed, thread-safe) ─────────────
    PluginPreview preview_;
    std::atomic<bool> loopPlayback_{ false };

    // ── Pending generation result (background thread → message thread) ────────
    // The generation thread copies the output file to the library directory and
    // hands off the path via AsyncUpdater so the message thread can load it into
    // the preview engine and start playback.
    mutable juce::CriticalSection pendingLibraryFileLock_;
    juce::File   pendingLibraryFile_;

    // ── Streaming log ─────────────────────────────────────────────────────────
    // Background thread appends lines; message thread drains via getAndClearNewLog().
    mutable juce::CriticalSection logLock_;
    juce::String pendingLog_;
    void appendToLog(const juce::String& text);
    void clearLog();

    static AudioFormat parseAudioFormat(const juce::String& s)
    {
        return (s == "mp3") ? AudioFormat::MP3 : AudioFormat::WAV;
    }

    static DitModel parseDitModel(const juce::String& s)
    {
        if (s == "sft")  return DitModel::Sft;
        if (s == "base") return DitModel::Base;
        return DitModel::Turbo;
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AcestepAudioProcessor)
};
