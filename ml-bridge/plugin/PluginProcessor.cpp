#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include <thread>

namespace
{
void writeToLogFile(const juce::String& message)
{
    juce::Logger::writeToLog("[AceForgeBridge] " + message);
#if JUCE_DEBUG
    std::cerr << "[AceForgeBridge] " << message.toRawUTF8() << std::endl;
#endif
    juce::File logDir = juce::File::getSpecialLocation(juce::File::userHomeDirectory).getChildFile("Library").getChildFile("Logs");
    if (logDir.exists() || logDir.createDirectory())
    {
        juce::File logFile = logDir.getChildFile("AceForgeBridge.log");
        juce::String line = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S") + " " + message + "\n";
        std::string path = logFile.getFullPathName().toStdString();
        std::ofstream f(path, std::ios::app);
        if (f)
        {
            f << line.toRawUTF8();
            f.flush();
        }
    }
}

void logErrorToFileAndStderr(const juce::String& message)
{
    writeToLogFile("ERROR: " + message);
}

// Trace steps so after a crash you can open ~/Library/Logs/AceForgeBridge.log and see last step reached
void logTrace(const juce::String& message)
{
    writeToLogFile("TRACE: " + message);
}

const char* stateToString(AceForgeBridgeAudioProcessor::State s)
{
    switch (s)
    {
    case AceForgeBridgeAudioProcessor::State::Idle:       return "Idle";
    case AceForgeBridgeAudioProcessor::State::Submitting: return "Generating lyrics & codes (LLM)…";
    case AceForgeBridgeAudioProcessor::State::Running:    return "Synthesising audio (DiT+VAE)…";
    case AceForgeBridgeAudioProcessor::State::Succeeded:  return "Ready";
    case AceForgeBridgeAudioProcessor::State::Failed:     return "Failed";
    }
    return "";
}

// Timeout constants for the two subprocess steps
static constexpr int kLmTimeoutMs  = 300'000;  // ace-qwen3: 5 minutes
static constexpr int kDitTimeoutMs = 600'000;  // dit-vae:   10 minutes

// Audio formats produced by dit-vae (in priority order)
static const juce::StringArray kSupportedAudioExts { "wav", "mp3" };
} // namespace

AceForgeBridgeAudioProcessor::AceForgeBridgeAudioProcessor()
    : AudioProcessor(BusesProperties()
#if !JucePlugin_IsMidiEffect
#if !JucePlugin_IsSynth
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
#endif
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
      )
{
    playbackBuffer_.resize(kPlaybackFifoFrames * 2, 0.0f);
    {
        juce::ScopedLock l(statusLock_);
        statusText_ = "Idle - enter a prompt and click Generate.";
    }
}

AceForgeBridgeAudioProcessor::~AceForgeBridgeAudioProcessor()
{
    cancelPendingUpdate();
}

// ── Path helpers ──────────────────────────────────────────────────────────────

void AceForgeBridgeAudioProcessor::setBinariesPath(const juce::String& path)
{
    juce::ScopedLock l(pathsLock_);
    binariesPath_ = path;
}

juce::String AceForgeBridgeAudioProcessor::getBinariesPath() const
{
    juce::ScopedLock l(pathsLock_);
    return binariesPath_;
}

void AceForgeBridgeAudioProcessor::setModelsPath(const juce::String& path)
{
    juce::ScopedLock l(pathsLock_);
    modelsPath_ = path;
}

juce::String AceForgeBridgeAudioProcessor::getModelsPath() const
{
    juce::ScopedLock l(pathsLock_);
    return modelsPath_;
}

juce::File AceForgeBridgeAudioProcessor::getModelsDirectory() const
{
    juce::String mp;
    {
        juce::ScopedLock l(pathsLock_);
        mp = modelsPath_;
    }
    if (!mp.isEmpty())
        return juce::File(mp);
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
               .getChildFile("AceForgeBridge")
               .getChildFile("models");
}

bool AceForgeBridgeAudioProcessor::areBinariesReady() const
{
    juce::String bp;
    {
        juce::ScopedLock l(pathsLock_);
        bp = binariesPath_;
    }
    juce::File binDir = bp.isEmpty()
        ? juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory()
        : juce::File(bp);
    return binDir.getChildFile("ace-qwen3").existsAsFile()
        && binDir.getChildFile("dit-vae").existsAsFile();
}

// ── AudioProcessor boilerplate ────────────────────────────────────────────────

void AceForgeBridgeAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);
    sampleRate_.store(sampleRate);
}

void AceForgeBridgeAudioProcessor::releaseResources() {}

// ── Generation ────────────────────────────────────────────────────────────────

void AceForgeBridgeAudioProcessor::startGeneration(const juce::String& prompt, int durationSeconds, int inferenceSteps)
{
    // Only one generation at a time: atomically transition to Submitting only from a non-busy state.
    State expected = state_.load();
    while (expected == State::Submitting || expected == State::Running)
        return;
    while (!state_.compare_exchange_weak(expected, State::Submitting))
    {
        if (expected == State::Submitting || expected == State::Running)
            return;
    }
    {
        juce::ScopedLock l(statusLock_);
        statusText_ = stateToString(State::Submitting);
    }
    triggerAsyncUpdate();
    std::thread t(&AceForgeBridgeAudioProcessor::runGenerationThread, this, prompt, durationSeconds, inferenceSteps);
    t.detach();
}

void AceForgeBridgeAudioProcessor::runGenerationThread(juce::String prompt, int durationSec, int inferenceSteps)
{
    // ── 1. Resolve binary and model directories ────────────────────────────────
    juce::String bp, mp;
    {
        juce::ScopedLock l(pathsLock_);
        bp = binariesPath_;
        mp = modelsPath_;
    }

    juce::File binDir = bp.isEmpty()
        ? juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory()
        : juce::File(bp);

    juce::File aceQwen3 = binDir.getChildFile("ace-qwen3");
    juce::File ditVae   = binDir.getChildFile("dit-vae");

    if (!aceQwen3.existsAsFile() || !ditVae.existsAsFile())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "acestep.cpp binaries not found in: " + binDir.getFullPathName()
                   + "\nBuild with: cmake -B vendor/acestep.cpp/build vendor/acestep.cpp"
                   + "\n            cmake --build vendor/acestep.cpp/build --config Release";
        statusText_ = lastError_;
        logErrorToFileAndStderr(lastError_);
        triggerAsyncUpdate();
        return;
    }

    juce::File modelsDir = mp.isEmpty() ? getModelsDirectory() : juce::File(mp);
    juce::File lmModel          = modelsDir.getChildFile("acestep-5Hz-lm-4B-Q8_0.gguf");
    juce::File textEncoderModel = modelsDir.getChildFile("Qwen3-Embedding-0.6B-Q8_0.gguf");
    juce::File ditModel         = modelsDir.getChildFile("acestep-v15-turbo-Q8_0.gguf");
    juce::File vaeModel         = modelsDir.getChildFile("vae-BF16.gguf");

    if (!lmModel.existsAsFile() || !textEncoderModel.existsAsFile()
        || !ditModel.existsAsFile() || !vaeModel.existsAsFile())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "ACE-Step models not found in: " + modelsDir.getFullPathName()
                   + "\nDownload with: cd vendor/acestep.cpp && ./models.sh";
        statusText_ = lastError_;
        logErrorToFileAndStderr(lastError_);
        triggerAsyncUpdate();
        return;
    }

    // ── 2. Create a temporary work directory for this generation ───────────────
    juce::File tmpDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getChildFile("AceForgeBridge")
                            .getChildFile("gen_" + juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S"));
    if (!tmpDir.createDirectory())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Cannot create temp directory: " + tmpDir.getFullPathName();
        statusText_ = lastError_;
        logErrorToFileAndStderr(lastError_);
        triggerAsyncUpdate();
        return;
    }

    // ── 3. Write request.json ──────────────────────────────────────────────────
    juce::File requestFile = tmpDir.getChildFile("request.json");
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("caption",         prompt);
        obj->setProperty("lyrics",          "[Instrumental]");
        obj->setProperty("inference_steps", inferenceSteps <= 0 ? 8 : inferenceSteps);
        obj->setProperty("duration",        durationSec   <= 0 ? 10 : durationSec);
        juce::String json = juce::JSON::toString(juce::var(obj));
        if (!requestFile.replaceWithText(json))
        {
            state_.store(State::Failed);
            juce::ScopedLock l(statusLock_);
            lastError_ = "Cannot write request.json to " + tmpDir.getFullPathName();
            statusText_ = lastError_;
            logErrorToFileAndStderr(lastError_);
            triggerAsyncUpdate();
            tmpDir.deleteRecursively();
            return;
        }
    }
    logTrace("runGenerationThread: request.json written to " + requestFile.getFullPathName());

    // ── 4. Step 1 — ace-qwen3 (LLM: generates lyrics + audio codes) ──────────
    {
        juce::ScopedLock l(statusLock_);
        statusText_ = stateToString(State::Submitting);
    }
    triggerAsyncUpdate();

    juce::StringArray lmArgs;
    lmArgs.add(aceQwen3.getFullPathName());
    lmArgs.add("--request"); lmArgs.add(requestFile.getFullPathName());
    lmArgs.add("--model");   lmArgs.add(lmModel.getFullPathName());

    juce::ChildProcess lmProc;
    if (!lmProc.start(lmArgs, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Failed to start ace-qwen3: " + aceQwen3.getFullPathName();
        statusText_ = lastError_;
        logErrorToFileAndStderr(lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }

    lmProc.waitForProcessToFinish(kLmTimeoutMs);
    const int lmExit = lmProc.getExitCode();
    logTrace("runGenerationThread: ace-qwen3 exited with " + juce::String(lmExit));
    if (lmExit != 0)
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "ace-qwen3 failed (exit " + juce::String(lmExit) + ")";
        statusText_ = lastError_;
        logErrorToFileAndStderr(lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }

    // ace-qwen3 writes request0.json next to request.json
    juce::File enrichedRequest = tmpDir.getChildFile("request0.json");
    if (!enrichedRequest.existsAsFile())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "ace-qwen3 did not produce request0.json in " + tmpDir.getFullPathName();
        statusText_ = lastError_;
        logErrorToFileAndStderr(lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }

    // ── 5. Step 2 — dit-vae (DiT + VAE: synthesises audio) ───────────────────
    state_.store(State::Running);
    {
        juce::ScopedLock l(statusLock_);
        statusText_ = stateToString(State::Running);
    }
    triggerAsyncUpdate();

    juce::StringArray ditArgs;
    ditArgs.add(ditVae.getFullPathName());
    ditArgs.add("--request");      ditArgs.add(enrichedRequest.getFullPathName());
    ditArgs.add("--text-encoder"); ditArgs.add(textEncoderModel.getFullPathName());
    ditArgs.add("--dit");          ditArgs.add(ditModel.getFullPathName());
    ditArgs.add("--vae");          ditArgs.add(vaeModel.getFullPathName());

    juce::ChildProcess ditProc;
    if (!ditProc.start(ditArgs, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Failed to start dit-vae: " + ditVae.getFullPathName();
        statusText_ = lastError_;
        logErrorToFileAndStderr(lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }

    ditProc.waitForProcessToFinish(kDitTimeoutMs);
    const int ditExit = ditProc.getExitCode();
    logTrace("runGenerationThread: dit-vae exited with " + juce::String(ditExit));
    if (ditExit != 0)
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "dit-vae failed (exit " + juce::String(ditExit) + ")";
        statusText_ = lastError_;
        logErrorToFileAndStderr(lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }

    // ── 6. Locate the output audio file ───────────────────────────────────────
    // dit-vae writes request00.mp3 (or .wav) next to the --request file
    juce::File outputFile;
    juce::String audioExt;
    for (const juce::String& ext : kSupportedAudioExts)
    {
        juce::File candidate = tmpDir.getChildFile("request00." + ext);
        if (candidate.existsAsFile())
        {
            outputFile = candidate;
            audioExt   = ext;
            break;
        }
    }
    if (!outputFile.existsAsFile())
    {
        // Fallback: take the first audio file of a supported type found in tmpDir
        juce::Array<juce::File> found;
        tmpDir.findChildFiles(found, juce::File::findFiles, false,
                              kSupportedAudioExts.joinIntoString(";", "*."));
        if (!found.isEmpty())
        {
            outputFile = found[0];
            audioExt   = outputFile.getFileExtension().trimCharactersAtStart(".");
        }
    }
    if (!outputFile.existsAsFile())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "dit-vae produced no audio output in " + tmpDir.getFullPathName();
        statusText_ = lastError_;
        logErrorToFileAndStderr(lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }
    logTrace("runGenerationThread: output audio at " + outputFile.getFullPathName());

    // ── 7. Read audio bytes and hand off to the message thread for decoding ───
    juce::MemoryBlock rawBytes;
    if (!outputFile.loadFileAsData(rawBytes) || rawBytes.getSize() == 0)
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Failed to read output audio: " + outputFile.getFullPathName();
        statusText_ = lastError_;
        logErrorToFileAndStderr(lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }

    {
        juce::ScopedLock l(pendingWavLock_);
        pendingWavBytes_.resize(rawBytes.getSize());
        std::memcpy(pendingWavBytes_.data(), rawBytes.getData(), rawBytes.getSize());
        pendingAudioExt_   = audioExt;
        pendingPrompt_     = prompt;
        pendingDurationSec_ = durationSec;
    }
    triggerAsyncUpdate();
    tmpDir.deleteRecursively();
}

void AceForgeBridgeAudioProcessor::pushSamplesToPlayback(const float* interleaved, int numFrames,
                                                         int sourceChannels, double sourceSampleRate)
{
    logTrace("pushSamplesToPlayback: numFrames=" + juce::String(numFrames) + " ch=" + juce::String(sourceChannels) + " rate=" + juce::String(sourceSampleRate));
    if (numFrames <= 0 || interleaved == nullptr)
        return;
    const double hostRate = sampleRate_.load(std::memory_order_relaxed);
    const double ratio = sourceSampleRate > 0.0 ? hostRate / sourceSampleRate : 1.0;
    const int outFrames = static_cast<int>(std::round(static_cast<double>(numFrames) * ratio));
    if (outFrames <= 0 || outFrames > kPlaybackFifoFrames)
    {
        logTrace("pushSamplesToPlayback: skipped (outFrames=" + juce::String(outFrames) + ")");
        return;
    }

    // Write into the buffer the audio thread is not reading (alternate 0/1)
    const int writeIdx = nextWriteIndex_.load(std::memory_order_relaxed);
    std::vector<float>& outBuf = pendingPlaybackBuffer_[writeIdx];
    logTrace("pushSamplesToPlayback: resizing buffer to " + juce::String(outFrames * 2) + " floats");
    outBuf.resize(static_cast<size_t>(outFrames) * 2u);
    float* out = outBuf.data();
    for (int i = 0; i < outFrames; ++i)
    {
        const double srcIdx = ratio > 0.0 ? (double)i / ratio : (double)i;
        const int i0 = std::min(std::max(0, static_cast<int>(srcIdx)), numFrames - 1);
        const int i1 = std::min(i0 + 1, numFrames - 1);
        const float t = static_cast<float>(srcIdx - std::floor(srcIdx));
        float l = 0.0f, r = 0.0f;
        if (sourceChannels >= 1)
        {
            l = interleaved[i0 * sourceChannels] * (1.0f - t) + interleaved[i1 * sourceChannels] * t;
            r = sourceChannels >= 2
                    ? interleaved[i0 * sourceChannels + 1] * (1.0f - t) + interleaved[i1 * sourceChannels + 1] * t
                    : l;
        }
        out[i * 2] = l;
        out[i * 2 + 1] = r;
    }
    pendingPlaybackFrames_.store(outFrames, std::memory_order_release);
    pendingPlaybackBufferIndex_.store(writeIdx, std::memory_order_release);
    pendingPlaybackReady_.store(true, std::memory_order_release);
    nextWriteIndex_.store(1 - writeIdx, std::memory_order_release);
    logTrace("pushSamplesToPlayback: done");
}

void AceForgeBridgeAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                                juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    const int numCh = buffer.getNumChannels();
    if (numCh < 2)
    {
        buffer.clear();
        return;
    }

    // Apply any new playback buffer from the message thread; only we (audio thread) may reset the fifo (JUCE AbstractFifo is single-reader single-writer; reset from another thread causes crashes).
    if (pendingPlaybackReady_.exchange(false, std::memory_order_acq_rel))
    {
        const int N = pendingPlaybackFrames_.load(std::memory_order_acquire);
        const int bufIdx = pendingPlaybackBufferIndex_.load(std::memory_order_acquire);
        const std::vector<float>& srcBuf = pendingPlaybackBuffer_[bufIdx];
        if (N > 0 && N <= kPlaybackFifoFrames && srcBuf.size() >= static_cast<size_t>(N) * 2u)
        {
            playbackFifo_.reset();
            int start1, block1, start2, block2;
            playbackFifo_.prepareToWrite(N, start1, block1, start2, block2);
            const float* src = srcBuf.data();
            auto copyBlock = [&](int fifoStart, int count, int srcOffset)
            {
                for (int i = 0; i < count && (fifoStart + i) * 2 + 1 < static_cast<int>(playbackBuffer_.size()); ++i)
                {
                    const int s = (srcOffset + i) * 2;
                    playbackBuffer_[static_cast<size_t>(fifoStart + i) * 2u] = src[s];
                    playbackBuffer_[static_cast<size_t>(fifoStart + i) * 2u + 1u] = src[s + 1];
                }
            };
            copyBlock(start1, block1, 0);
            copyBlock(start2, block2, block1);
            playbackFifo_.finishedWrite(block1 + block2);
        }
    }

    int start1, block1, start2, block2;
    playbackFifo_.prepareToRead(numSamples, start1, block1, start2, block2);

    auto readFrames = [&](int fifoStart, int count, int bufferOffset)
    {
        for (int i = 0; i < count; ++i)
        {
            const size_t base = static_cast<size_t>(fifoStart + i) * 2u;
            if (base + 1 < playbackBuffer_.size() && bufferOffset + i < numSamples)
            {
                buffer.setSample(0, bufferOffset + i, playbackBuffer_[base]);
                buffer.setSample(1, bufferOffset + i, playbackBuffer_[base + 1]);
            }
        }
    };
    readFrames(start1, block1, 0);
    readFrames(start2, block2, block1);
    playbackFifo_.finishedRead(block1 + block2);

    const int readCount = block1 + block2;
    for (int i = readCount; i < numSamples; ++i)
    {
        buffer.setSample(0, i, 0.0f);
        buffer.setSample(1, i, 0.0f);
    }
}

juce::String AceForgeBridgeAudioProcessor::getStatusText() const
{
    juce::ScopedLock l(statusLock_);
    return statusText_;
}

juce::String AceForgeBridgeAudioProcessor::getLastError() const
{
    juce::ScopedLock l(statusLock_);
    return lastError_;
}

juce::File AceForgeBridgeAudioProcessor::getLibraryDirectory() const
{
    juce::File dir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                         .getChildFile("AceForgeBridge")
                         .getChildFile("Generations");
    if (!dir.exists())
        dir.createDirectory();
    return dir;
}

std::vector<AceForgeBridgeAudioProcessor::LibraryEntry> AceForgeBridgeAudioProcessor::getLibraryEntries() const
{
    juce::Array<juce::File> wavs;
    getLibraryDirectory().findChildFiles(wavs, juce::File::findFiles, false, "*.wav");
    std::vector<LibraryEntry> entries;
    for (const juce::File& f : wavs)
        entries.push_back({ f, f.getFileName().upToFirstOccurrenceOf(".", false, false), f.getLastModificationTime() });
    std::sort(entries.begin(), entries.end(),
              [](const LibraryEntry& a, const LibraryEntry& b) { return a.time > b.time; });
    return entries;
}

void AceForgeBridgeAudioProcessor::addToLibrary(const juce::File& wavFile, const juce::String& prompt)
{
    juce::ignoreUnused(wavFile, prompt);
    // File is already on disk; optional: write prompt to .txt sidecar for display
}

void AceForgeBridgeAudioProcessor::handleAsyncUpdate()
{
    logTrace("handleAsyncUpdate: start");
    std::vector<uint8_t> audioBytes;
    juce::String audioExt;
    juce::String promptForLibrary;
    {
        juce::ScopedLock l(pendingWavLock_);
        if (pendingWavBytes_.empty())
            return;
        audioBytes = std::move(pendingWavBytes_);
        pendingWavBytes_.clear();
        audioExt          = pendingAudioExt_;
        promptForLibrary  = pendingPrompt_;
    }
    logTrace("handleAsyncUpdate: got audio bytes, size=" + juce::String(audioBytes.size()) + " ext=" + audioExt);

    try
    {
        juce::AudioFormatManager fm;
        fm.registerFormat(new juce::WavAudioFormat(), true);
#if JUCE_USE_MP3AUDIOFORMAT
        fm.registerFormat(new juce::MP3AudioFormat(), false);
#endif
        logTrace("handleAsyncUpdate: creating MemoryInputStream");
        auto mis = std::make_unique<juce::MemoryInputStream>(audioBytes.data(), audioBytes.size(), false);
        logTrace("handleAsyncUpdate: creating reader");
        std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(std::move(mis)));
        if (!reader)
        {
            state_.store(State::Failed);
            juce::ScopedLock l(statusLock_);
            lastError_ = "Failed to decode audio (" + audioExt + ")";
            statusText_ = lastError_;
            logErrorToFileAndStderr(lastError_);
            return;
        }
        const double fileSampleRate = reader->sampleRate;
        const int numCh = static_cast<int>(reader->numChannels);
        const int numSamples = static_cast<int>(reader->lengthInSamples);
        logTrace("handleAsyncUpdate: audio info rate=" + juce::String(fileSampleRate) + " ch=" + juce::String(numCh) + " samples=" + juce::String(numSamples));
        if (numSamples <= 0 || numCh <= 0)
        {
            state_.store(State::Failed);
            juce::ScopedLock l(statusLock_);
            lastError_ = "Invalid audio file (no samples)";
            statusText_ = lastError_;
            logErrorToFileAndStderr(lastError_);
            return;
        }
        logTrace("handleAsyncUpdate: allocating fileBuffer");
        juce::AudioBuffer<float> fileBuffer(numCh, numSamples);
        logTrace("handleAsyncUpdate: reading samples");
        if (!reader->read(&fileBuffer, 0, numSamples, 0, true, true))
        {
            state_.store(State::Failed);
            juce::ScopedLock l(statusLock_);
            lastError_ = "Failed to read audio samples";
            statusText_ = lastError_;
            logErrorToFileAndStderr(lastError_);
            return;
        }
        logTrace("handleAsyncUpdate: building interleaved buffer");
        std::vector<float> interleaved(static_cast<size_t>(numSamples) * 2u);
        for (int i = 0; i < numSamples; ++i)
        {
            interleaved[static_cast<size_t>(i) * 2u] = numCh > 0 ? fileBuffer.getSample(0, i) : 0.0f;
            interleaved[static_cast<size_t>(i) * 2u + 1u] = numCh > 1 ? fileBuffer.getSample(1, i) : interleaved[static_cast<size_t>(i) * 2u];
        }
        logTrace("handleAsyncUpdate: calling pushSamplesToPlayback");
        pushSamplesToPlayback(interleaved.data(), numSamples, 2, fileSampleRate);
        playbackBufferReady_.store(true);
        state_.store(State::Succeeded);
        {
            juce::ScopedLock l(statusLock_);
            statusText_ = "Generated - playing.";
        }
        logTrace("handleAsyncUpdate: playback updated, saving to library");
        // Save to library so user can drag into DAW (own try so a file error doesn't lose playback)
        try
        {
            juce::File libDir = getLibraryDirectory();
            juce::String baseName = "gen_" + juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
            juce::File wavFile = libDir.getChildFile(baseName + ".wav");
            std::unique_ptr<juce::OutputStream> outStream = wavFile.createOutputStream();
            if (outStream != nullptr)
            {
                juce::WavAudioFormat wavFormat;
                auto options = juce::AudioFormatWriterOptions{}
                                  .withSampleRate(fileSampleRate)
                                  .withNumChannels(numCh)
                                  .withBitsPerSample(24);
                if (auto writer = wavFormat.createWriterFor(outStream, options))
                {
                    if (writer->writeFromAudioSampleBuffer(fileBuffer, 0, numSamples))
                        addToLibrary(wavFile, promptForLibrary);
                }
            }
            logTrace("handleAsyncUpdate: library save done");
        }
        catch (const std::exception& e)
        {
            logErrorToFileAndStderr("Library save failed: " + juce::String(e.what()));
        }
        catch (...)
        {
            logErrorToFileAndStderr("Library save failed (unknown)");
        }
        logTrace("handleAsyncUpdate: done");
    }
    catch (const std::exception& e)
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = juce::String("Decode error: ") + e.what();
        statusText_ = lastError_;
        logErrorToFileAndStderr(lastError_);
    }
    catch (...)
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Decode error (unknown)";
        statusText_ = lastError_;
        logErrorToFileAndStderr(lastError_);
    }
}

const juce::String AceForgeBridgeAudioProcessor::getName() const { return JucePlugin_Name; }
bool AceForgeBridgeAudioProcessor::acceptsMidi() const { return false; }
bool AceForgeBridgeAudioProcessor::producesMidi() const { return false; }
bool AceForgeBridgeAudioProcessor::isMidiEffect() const { return false; }
double AceForgeBridgeAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int AceForgeBridgeAudioProcessor::getNumPrograms() { return 1; }
int AceForgeBridgeAudioProcessor::getCurrentProgram() { return 0; }
void AceForgeBridgeAudioProcessor::setCurrentProgram(int index) { juce::ignoreUnused(index); }
const juce::String AceForgeBridgeAudioProcessor::getProgramName(int index)
{
    juce::ignoreUnused(index);
    return {};
}
void AceForgeBridgeAudioProcessor::changeProgramName(int index, const juce::String& newName)
{
    juce::ignoreUnused(index, newName);
}

bool AceForgeBridgeAudioProcessor::isBusesLayoutSupported(const juce::AudioProcessor::BusesLayout& layouts) const
{
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono() &&
        layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;
    return true;
}

bool AceForgeBridgeAudioProcessor::hasEditor() const { return true; }
juce::AudioProcessorEditor* AceForgeBridgeAudioProcessor::createEditor()
{
    return new AceForgeBridgeAudioProcessorEditor(*this);
}
void AceForgeBridgeAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::String bp, mp;
    {
        juce::ScopedLock l(pathsLock_);
        bp = binariesPath_;
        mp = modelsPath_;
    }
    auto xml = std::make_unique<juce::XmlElement>("AceForgeBridgeState");
    xml->setAttribute("binariesPath", bp);
    xml->setAttribute("modelsPath",   mp);
    copyXmlToBinary(*xml, destData);
}
void AceForgeBridgeAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml && xml->hasTagName("AceForgeBridgeState"))
    {
        juce::ScopedLock l(pathsLock_);
        binariesPath_ = xml->getStringAttribute("binariesPath");
        modelsPath_   = xml->getStringAttribute("modelsPath");
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AceForgeBridgeAudioProcessor();
}
