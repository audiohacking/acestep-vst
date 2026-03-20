#pragma once

#include <memory>
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>

// ── PluginPreview ──────────────────────────────────────────────────────────────
// Wraps juce::AudioTransportSource so the processor can play generated audio and
// library entries without a hand-rolled FIFO. Thread-safety model:
//   • loadFile / play / stop / clear / setLooping — message thread only
//   • render — audio thread (uses ScopedTryLock to avoid priority inversion)
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
    juce::AudioTransportSource    transportSource_;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource_;
    juce::File currentFile_;
    double     sampleRate_{ 44100.0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PluginPreview)
};
