#include "PluginPreview.h"

// ─────────────────────────────────────────────────────────────────────────────

PluginPreview::PluginPreview()
{
    formatManager_.registerBasicFormats();
}

PluginPreview::~PluginPreview()
{
    clear();
}

// ─────────────────────────────────────────────────────────────────────────────
// AudioProcessor lifecycle

void PluginPreview::prepareToPlay(double sampleRate, int /*samplesPerBlock*/)
{
    const juce::ScopedLock lock(lock_);
    outputSampleRate_ = sampleRate > 0.0 ? sampleRate : 44100.0;
}

void PluginPreview::releaseResources()
{
    stop();
}

// ─────────────────────────────────────────────────────────────────────────────
// Audio thread

void PluginPreview::render(juce::AudioBuffer<float>& buffer)
{
    // Fast path: nothing loaded or stopped — leave the buffer untouched so
    // the DAW's input audio passes through.
    int pos = playPos_.load(std::memory_order_relaxed);
    if (pos < 0)
        return;

    // Try to acquire the lock without blocking.  If the message thread is
    // in the middle of loadFile() we skip this block rather than stalling.
    const juce::ScopedTryLock tryLock(lock_);
    if (!tryLock.isLocked() || numSamplesLoaded_ == 0)
        return;

    const int outChannels = buffer.getNumChannels();
    const int outSamples  = buffer.getNumSamples();
    const int srcChannels = audioData_.getNumChannels();

    buffer.clear();

    int written = 0;
    while (written < outSamples)
    {
        if (pos >= numSamplesLoaded_)
        {
            if (looping_.load(std::memory_order_relaxed))
                pos = 0;
            else
            {
                playPos_.store(-1, std::memory_order_relaxed);
                break;
            }
        }

        const int toRead = juce::jmin(outSamples - written, numSamplesLoaded_ - pos);
        for (int ch = 0; ch < outChannels; ++ch)
            buffer.copyFrom(ch, written, audioData_, ch % srcChannels, pos, toRead);

        written += toRead;
        pos     += toRead;
    }

    // Persist the updated position (ignored if stop() set it to -1 concurrently).
    if (playPos_.load(std::memory_order_relaxed) >= 0)
        playPos_.store(pos, std::memory_order_relaxed);
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

    const int    numCh      = juce::jmin(static_cast<int>(reader->numChannels), 2);
    const int    numSamples = static_cast<int>(reader->lengthInSamples);
    const double srcRate    = reader->sampleRate;

    if (numSamples <= 0 || numCh <= 0)
        return false;

    // Decode the whole file into a local buffer (no lock needed — all local).
    juce::AudioBuffer<float> srcBuf(numCh, numSamples);
    reader->read(&srcBuf, 0, numSamples, 0, true, numCh > 1);
    reader.reset(); // close the file handle

    // Read the current output sample rate safely.
    double outRate;
    { const juce::ScopedLock lock(lock_); outRate = outputSampleRate_; }

    // Resample if the file rate differs from the DAW rate (linear interpolation).
    juce::AudioBuffer<float> outBuf;
    if (std::abs(srcRate - outRate) < 1.0)
    {
        outBuf = std::move(srcBuf);
    }
    else
    {
        const double ratio  = outRate / srcRate;
        const int    outLen = static_cast<int>(numSamples * ratio) + 2;
        outBuf.setSize(numCh, outLen);

        for (int ch = 0; ch < numCh; ++ch)
        {
            const float* src = srcBuf.getReadPointer(ch);
            float*       dst = outBuf.getWritePointer(ch);
            for (int i = 0; i < outLen; ++i)
            {
                const double srcIdx = i / ratio;
                const int    s0     = static_cast<int>(srcIdx);
                const double frac   = srcIdx - s0;
                if (s0 + 1 < numSamples)
                    dst[i] = static_cast<float>(src[s0] + frac * (src[s0 + 1] - src[s0]));
                else if (s0 < numSamples)
                    dst[i] = src[s0];
                else
                    dst[i] = 0.0f;
            }
        }
    }

    // Swap in the new buffer atomically under the lock.
    {
        const juce::ScopedLock lock(lock_);
        playPos_.store(-1, std::memory_order_relaxed);
        audioData_        = std::move(outBuf);
        numSamplesLoaded_ = audioData_.getNumSamples();
        currentFile_      = file;
    }
    return true;
}

void PluginPreview::clear()
{
    const juce::ScopedLock lock(lock_);
    playPos_.store(-1, std::memory_order_relaxed);
    audioData_.setSize(0, 0);
    numSamplesLoaded_ = 0;
    currentFile_      = juce::File();
}

void PluginPreview::play(bool loop)
{
    looping_.store(loop, std::memory_order_relaxed);
    playPos_.store(0, std::memory_order_relaxed);
}

void PluginPreview::stop()
{
    playPos_.store(-1, std::memory_order_relaxed);
}

void PluginPreview::setLooping(bool loop)
{
    looping_.store(loop, std::memory_order_relaxed);
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
    return numSamplesLoaded_ > 0 && currentFile_.existsAsFile();
}

bool PluginPreview::isPlaying() const
{
    return playPos_.load(std::memory_order_relaxed) >= 0;
}

juce::String PluginPreview::getFilePath() const
{
    const juce::ScopedLock lock(lock_);
    return currentFile_.getFullPathName();
}
