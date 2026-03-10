#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <vector>

class AcestepAudioProcessor : public juce::AudioProcessor,
                              public juce::AsyncUpdater
{
public:
    // Submitting = running ace-qwen3 (LLM step, text-to-music) or preparing cover request
    // Running    = running dit-vae  (DiT + VAE synthesis)
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

    // ── Generation ────────────────────────────────────────────────────────────
    // coverFile : non-empty → cover / repaint mode (pass --src-audio to dit-vae).
    // bpm       : 0 = auto-detect from DAW playhead.
    // lyrics    : lyrics text; use "[Instrumental]" for no vocals.
    // seed      : -1 = random.
    void startGeneration(const juce::String& prompt,
                         int   durationSeconds = 10,
                         int   inferenceSteps  = 8,
                         juce::File coverFile  = {},
                         float coverStrength   = 0.5f,
                         float bpm             = 0.0f,
                         const juce::String& lyrics = "[Instrumental]",
                         int   seed            = -1);

    // ── Path configuration (persisted in DAW project state) ──────────────────
    void setBinariesPath(const juce::String& path);
    juce::String getBinariesPath() const;

    void setModelsPath(const juce::String& path);
    juce::String getModelsPath() const;

    void setOutputPath(const juce::String& path);
    juce::String getOutputPath() const;

    juce::File getModelsDirectory()  const;
    juce::File getLibraryDirectory() const;  // where generated audio is saved

    bool areBinariesReady() const;

    // ── State / status ────────────────────────────────────────────────────────
    State        getState()      const { return state_.load(); }
    juce::String getStatusText() const;
    juce::String getLastError()  const;

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
                             juce::File coverFile, float coverStrength, float bpm,
                             juce::String lyrics, int seed);

    // Decode raw audio bytes and push to playback + loop buffers.
    // If promptForLibrary is non-empty the result is saved to the library and
    // state is set to Succeeded; otherwise (preview) only playback is updated.
    void decodeAndPushAudio(const std::vector<uint8_t>& bytes,
                            const juce::String& ext,
                            const juce::String& promptForLibrary);

    void pushSamplesToPlayback(const float* interleaved, int numFrames,
                               int sourceChannels, double sourceSampleRate);
    // Write to the inactive loop buffer and publish atomically (audio-thread safe).
    void setLoopData(const float* interleaved, int numFrames);

    // ── Settings paths ────────────────────────────────────────────────────────
    mutable juce::CriticalSection pathsLock_;
    juce::String binariesPath_, modelsPath_, outputPath_;

    // ── State machine ─────────────────────────────────────────────────────────
    std::atomic<State> state_{ State::Idle };
    mutable juce::CriticalSection statusLock_;
    juce::String lastError_, statusText_;

    // ── BPM from DAW playhead ─────────────────────────────────────────────────
    std::atomic<double> hostBpm_{ 0.0 };

    // ── One-shot playback FIFO ────────────────────────────────────────────────
    // ~24 s of stereo audio at 44.1 kHz (2^20 frames × 2 channels × 4 bytes = 8 MB)
    static constexpr int kPlaybackFifoFrames = 1 << 20;
    juce::AbstractFifo   playbackFifo_{ kPlaybackFifoFrames };
    std::vector<float>   playbackBuffer_;         // interleaved stereo, kPlaybackFifoFrames*2
    std::atomic<bool>    playbackBufferReady_{ false };

    // Double-buffer handoff: message thread → audio thread
    std::vector<float>   pendingPlaybackBuf_[2];
    std::atomic<int>     pendingPlaybackFrames_{ 0 };
    std::atomic<int>     pendingPlaybackBufIdx_{ 0 };
    std::atomic<int>     nextWriteIdx_{ 0 };
    std::atomic<bool>    pendingPlaybackReady_{ false };

    // ── Loop playback (double-buffered, audio-thread safe) ────────────────────
    // The message thread always writes to the INACTIVE slot and then atomically
    // publishes via loopBufActive_. The audio thread only reads the ACTIVE slot.
    std::vector<float>   loopBuf_[2];
    int                  loopFrames_[2]{ 0, 0 };  // frame counts for each slot
    std::atomic<int>     loopBufActive_{ 0 };     // slot the audio thread reads
    std::atomic<int>     nextLoopWrite_{ 0 };     // slot the message thread writes
    std::atomic<int>     loopReadPos_{ 0 };
    std::atomic<bool>    loopPlayback_{ false };

    // ── Stop signal (audio thread polls) ─────────────────────────────────────
    std::atomic<bool>    stopRequested_{ false };

    // ── Host sample rate ──────────────────────────────────────────────────────
    std::atomic<double>  sampleRate_{ 44100.0 };

    // ── Pending generation result (background thread → message thread) ────────
    mutable juce::CriticalSection pendingWavLock_;
    std::vector<uint8_t> pendingWavBytes_;
    juce::String         pendingAudioExt_;
    juce::String         pendingPrompt_;

    // ── Pending preview request (UI → message thread) ─────────────────────────
    mutable juce::CriticalSection pendingPreviewLock_;
    juce::File           pendingPreviewFile_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AcestepAudioProcessor)
};
