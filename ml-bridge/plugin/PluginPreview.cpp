#include "PluginPreview.h"

// ─────────────────────────────────────────────────────────────────────────────

PluginPreview::PluginPreview()
{
    formatManager_.registerBasicFormats();
    // Ensure the transport source is always prepared with safe defaults so that
    // setSource() called from loadFile() will properly call prepareToPlay() on
    // the reader source even if the DAW hasn't yet called prepareToPlay() on us.
    transportSource_.prepareToPlay(512, 44100.0);
}

PluginPreview::~PluginPreview()
{
    clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// AudioProcessor lifecycle

void PluginPreview::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    const juce::ScopedLock lock(lock_);
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
    transportSource_.prepareToPlay(samplesPerBlock, sampleRate_);
}

void PluginPreview::releaseResources()
{
    const juce::ScopedLock lock(lock_);
    transportSource_.releaseResources();
}

// ─────────────────────────────────────────────────────────────────────────────
// Audio thread

void PluginPreview::render(juce::AudioBuffer<float>& buffer)
{
    // Use a try-lock so the audio thread never blocks waiting for the message
    // thread (e.g. during loadFile).  If we can't take the lock, or nothing is
    // loaded / playing, leave the buffer as-is so input audio passes through.
    const juce::ScopedTryLock tryLock(lock_);
    if (!tryLock.isLocked() || readerSource_ == nullptr)
        return;

    // Only overwrite the buffer while a clip is actually playing.
    // AudioTransportSource::getNextAudioBlock fills with silence when stopped,
    // which would kill the pass-through signal, so we guard against that here.
    if (!transportSource_.isPlaying())
        return;

    // Clear the input first so we output clean preview audio, not a mix.
    buffer.clear();
    juce::AudioSourceChannelInfo info(&buffer, 0, buffer.getNumSamples());
    transportSource_.getNextAudioBlock(info);
}

// ─────────────────────────────────────────────────────────────────────────────
// Message-thread operations

bool PluginPreview::loadFile(const juce::File& file)
{
    if (!file.existsAsFile())
        return false;

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
    if (reader == nullptr)
        return false;

    const double readerSampleRate = reader->sampleRate;
    auto nextSource = std::make_unique<juce::AudioFormatReaderSource>(reader.release(), true);

    const juce::ScopedLock lock(lock_);
    transportSource_.stop();
    transportSource_.setSource(nullptr);
    readerSource_.reset();
    transportSource_.setSource(nextSource.get(), 0, nullptr, readerSampleRate);
    readerSource_ = std::move(nextSource);
    currentFile_  = file;
    return true;
}

void PluginPreview::clear()
{
    const juce::ScopedLock lock(lock_);
    transportSource_.stop();
    transportSource_.setSource(nullptr);
    readerSource_.reset();
    currentFile_ = juce::File();
}

void PluginPreview::play(bool loop)
{
    const juce::ScopedLock lock(lock_);
    if (readerSource_ == nullptr)
        return;
    readerSource_->setLooping(loop);
    transportSource_.setPosition(0.0);
    transportSource_.start();
}

void PluginPreview::stop()
{
    const juce::ScopedLock lock(lock_);
    transportSource_.stop();
}

void PluginPreview::setLooping(bool loop)
{
    const juce::ScopedLock lock(lock_);
    if (readerSource_ != nullptr)
        readerSource_->setLooping(loop);
}

void PluginPreview::revealToUser() const
{
    const juce::ScopedLock lock(lock_);
    if (currentFile_.existsAsFile())
        currentFile_.revealToUser();
}

// ─────────────────────────────────────────────────────────────────────────────
// Queries

bool PluginPreview::hasLoadedFile() const
{
    const juce::ScopedLock lock(lock_);
    return readerSource_ != nullptr && currentFile_.existsAsFile();
}

bool PluginPreview::isPlaying() const
{
    const juce::ScopedLock lock(lock_);
    return transportSource_.isPlaying();
}

juce::String PluginPreview::getFilePath() const
{
    const juce::ScopedLock lock(lock_);
    return currentFile_.getFullPathName();
}
