#pragma once

#include <atomic>
#include <juce_audio_formats/juce_audio_formats.h>

// ── PluginPreview ──────────────────────────────────────────────────────────────
// Loads an audio file entirely into an AudioBuffer<float> and plays it back
// through the processBlock render path.  No background threads or
// AudioTransportSource are required.
//
// Thread-safety model:
//   • loadFile / play / stop / clear / setLooping — message thread only
//   • render — audio thread; uses ScopedTryLock + atomics (wait-free fast path)
//   • prepareToPlay / releaseResources — message thread (JUCE contract)
class PluginPreview final
{
public:
    PluginPreview();
    ~PluginPreview();

    // AudioProcessor lifecycle — forward from AcestepAudioProcessor
    void prepareToPlay(double sampleRate, int samplesPerBlock);
    void releaseResources();

    // Called from processBlock (audio thread)
    void render(juce::AudioBuffer<float>& buffer);

    // Message-thread operations
    [[nodiscard]] bool loadFile(const juce::File& file);
    void clear();
    void play(bool loop = false);
    void stop();
    void setLooping(bool loop);
    void revealToUser() const;

    // Queries (may be called from message thread)
    [[nodiscard]] bool hasLoadedFile() const;
    [[nodiscard]] bool isPlaying() const;
    [[nodiscard]] juce::String getFilePath() const;

private:
    mutable juce::CriticalSection lock_;
    juce::AudioFormatManager      formatManager_;

    // Decoded audio stored entirely in memory (message thread writes, audio thread reads)
    juce::AudioBuffer<float> audioData_;
    int                      numSamplesLoaded_{ 0 };
    double                   outputSampleRate_{ 44100.0 };
    juce::File               currentFile_;

    // Playback state — atomics allow play/stop without acquiring the lock
    std::atomic<int>  playPos_{ -1 };    // -1 = stopped; >= 0 = current sample index
    std::atomic<bool> looping_{ false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginPreview)
};
