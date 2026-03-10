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

// ── Logging ───────────────────────────────────────────────────────────────────

void writeToLogFile(const juce::String& message)
{
    juce::Logger::writeToLog("[AcestepVST] " + message);
#if JUCE_DEBUG
    std::cerr << "[AcestepVST] " << message.toRawUTF8() << std::endl;
#endif
    juce::File logDir = juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                            .getChildFile("Library").getChildFile("Logs");
    if (logDir.exists() || logDir.createDirectory())
    {
        juce::File logFile = logDir.getChildFile("AcestepVST.log");
        juce::String line  = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H:%M:%S")
                           + " " + message + "\n";
        std::string path = logFile.getFullPathName().toStdString();
        std::ofstream f(path, std::ios::app);
        if (f) { f << line.toRawUTF8(); f.flush(); }
    }
}

void logError(const juce::String& msg) { writeToLogFile("ERROR: " + msg); }
void logTrace(const juce::String& msg) { writeToLogFile("TRACE: " + msg); }

// ── State label ───────────────────────────────────────────────────────────────

const char* stateToString(AcestepAudioProcessor::State s)
{
    switch (s)
    {
    case AcestepAudioProcessor::State::Idle:       return "Idle";
    case AcestepAudioProcessor::State::Submitting: return "Generating lyrics & codes (LLM)\xe2\x80\xa6";
    case AcestepAudioProcessor::State::Running:    return "Synthesising audio (DiT+VAE)\xe2\x80\xa6";
    case AcestepAudioProcessor::State::Succeeded:  return "Ready";
    case AcestepAudioProcessor::State::Failed:     return "Failed";
    }
    return "";
}

// ── Subprocess timeouts ───────────────────────────────────────────────────────
static constexpr int kLmTimeoutMs      = 300'000;  // ace-qwen3: 5 min
static constexpr int kDitTimeoutMs     = 600'000;  // dit-vae:  10 min
// ── Stereo channel count used throughout the playback pipeline ────────────────
static constexpr int kNumOutputChannels = 2;
// ── Supported audio extensions (in priority order) ────────────────────────────
static const juce::StringArray kAudioExts{ "wav", "mp3" };

} // namespace

// ═════════════════════════════════════════════════════════════════════════════
// Global config helpers
// ═════════════════════════════════════════════════════════════════════════════

static juce::File getGlobalConfigFile()
{
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
               .getChildFile("AcestepVST")
               .getChildFile("config.xml");
}

// ═════════════════════════════════════════════════════════════════════════════
// Construction
// ═════════════════════════════════════════════════════════════════════════════

AcestepAudioProcessor::AcestepAudioProcessor()
    : AudioProcessor(BusesProperties()
#if !JucePlugin_IsMidiEffect
#if !JucePlugin_IsSynth
                         .withInput("Input",  juce::AudioChannelSet::stereo(), true)
#endif
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)
#endif
      )
{
    playbackBuffer_.resize(static_cast<size_t>(kPlaybackFifoFrames) * kNumOutputChannels, 0.0f);
    {
        juce::ScopedLock l(statusLock_);
        statusText_ = "Idle \xe2\x80\x94 enter a prompt and click Generate.";
    }
    // Load globally-persisted paths so they are available even in a fresh project.
    loadSettingsFromGlobalConfig();
}

AcestepAudioProcessor::~AcestepAudioProcessor()
{
    cancelPendingUpdate();
}

// ═════════════════════════════════════════════════════════════════════════════
// Path helpers
// ═════════════════════════════════════════════════════════════════════════════

void AcestepAudioProcessor::setBinariesPath(const juce::String& p)
{
    { juce::ScopedLock l(pathsLock_); binariesPath_ = p; }
    saveSettingsToGlobalConfig();
}
juce::String AcestepAudioProcessor::getBinariesPath() const
{
    juce::ScopedLock l(pathsLock_); return binariesPath_;
}

void AcestepAudioProcessor::setModelsPath(const juce::String& p)
{
    { juce::ScopedLock l(pathsLock_); modelsPath_ = p; }
    saveSettingsToGlobalConfig();
}
juce::String AcestepAudioProcessor::getModelsPath() const
{
    juce::ScopedLock l(pathsLock_); return modelsPath_;
}

void AcestepAudioProcessor::setOutputPath(const juce::String& p)
{
    { juce::ScopedLock l(pathsLock_); outputPath_ = p; }
    saveSettingsToGlobalConfig();
}
juce::String AcestepAudioProcessor::getOutputPath() const
{
    juce::ScopedLock l(pathsLock_); return outputPath_;
}

juce::File AcestepAudioProcessor::getModelsDirectory() const
{
    juce::String mp;
    { juce::ScopedLock l(pathsLock_); mp = modelsPath_; }
    if (!mp.isEmpty()) return juce::File(mp);
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
               .getChildFile("AcestepVST").getChildFile("models");
}

juce::File AcestepAudioProcessor::getLibraryDirectory() const
{
    juce::String op;
    { juce::ScopedLock l(pathsLock_); op = outputPath_; }
    juce::File dir = op.isEmpty()
        ? juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
              .getChildFile("AcestepVST").getChildFile("Generations")
        : juce::File(op);
    if (!dir.exists()) dir.createDirectory();
    return dir;
}

bool AcestepAudioProcessor::areBinariesReady() const
{
    juce::String bp;
    { juce::ScopedLock l(pathsLock_); bp = binariesPath_; }
    juce::File binDir = bp.isEmpty()
        ? juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory()
        : juce::File(bp);
    return binDir.getChildFile("ace-qwen3").existsAsFile()
        && binDir.getChildFile("dit-vae").existsAsFile();
}

// ═════════════════════════════════════════════════════════════════════════════
// AudioProcessor boilerplate
// ═════════════════════════════════════════════════════════════════════════════

void AcestepAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    juce::ignoreUnused(samplesPerBlock);
    sampleRate_.store(sampleRate);
}
void AcestepAudioProcessor::releaseResources() {}

// ═════════════════════════════════════════════════════════════════════════════
// processBlock — audio thread
// ═════════════════════════════════════════════════════════════════════════════

void AcestepAudioProcessor::processBlock(juce::AudioBuffer<float>& buffer,
                                         juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused(midiMessages);
    juce::ScopedNoDenormals noDenormals;

    // ── Update BPM from DAW playhead ─────────────────────────────────────────
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto bpm = pos->getBpm())
                hostBpm_.store(*bpm, std::memory_order_relaxed);

    const int numSamples = buffer.getNumSamples();
    const int numCh      = buffer.getNumChannels();
    if (numCh < 2) { buffer.clear(); return; }

    // ── Handle stop request ──────────────────────────────────────────────────
    if (stopRequested_.exchange(false, std::memory_order_acq_rel))
    {
        playbackFifo_.reset();
        loopReadPos_.store(0, std::memory_order_relaxed);
        buffer.clear();
        return;
    }

    // ── Accept new audio handed off by the message thread ────────────────────
    if (pendingPlaybackReady_.exchange(false, std::memory_order_acq_rel))
    {
        const int N      = pendingPlaybackFrames_.load(std::memory_order_acquire);
        const int bufIdx = pendingPlaybackBufIdx_.load(std::memory_order_acquire);
        const std::vector<float>& srcBuf = pendingPlaybackBuf_[bufIdx];
        if (N > 0 && N <= kPlaybackFifoFrames
            && srcBuf.size() >= static_cast<size_t>(N) * kNumOutputChannels)
        {
            playbackFifo_.reset();
            int s1, b1, s2, b2;
            playbackFifo_.prepareToWrite(N, s1, b1, s2, b2);
            const float* src = srcBuf.data();
            auto copyBlock = [&](int fStart, int count, int srcOff)
            {
                for (int i = 0; i < count; ++i)
                {
                    const size_t fi = static_cast<size_t>(fStart + i);
                    const int    si = (srcOff + i) * 2;
                    if (fi * 2 + 1 < playbackBuffer_.size())
                    {
                        playbackBuffer_[fi * 2]     = src[si];
                        playbackBuffer_[fi * 2 + 1] = src[si + 1];
                    }
                }
            };
            copyBlock(s1, b1, 0);
            copyBlock(s2, b2, b1);
            playbackFifo_.finishedWrite(b1 + b2);
        }
    }

    // ── Loop playback (reads directly from loopBuf_, wraps around) ───────────
    if (loopPlayback_.load(std::memory_order_relaxed))
    {
        const int active = loopBufActive_.load(std::memory_order_acquire);
        const int total  = loopFrames_[active];
        if (total > 0 && static_cast<int>(loopBuf_[active].size()) >= total * kNumOutputChannels)
        {
            int rp = loopReadPos_.load(std::memory_order_relaxed);
            const float* lb = loopBuf_[active].data();
            for (int i = 0; i < numSamples; ++i)
            {
                if (rp >= total) rp = 0;
                buffer.setSample(0, i, lb[static_cast<size_t>(rp) * 2]);
                buffer.setSample(1, i, lb[static_cast<size_t>(rp) * 2 + 1]);
                ++rp;
            }
            loopReadPos_.store(rp, std::memory_order_relaxed);
            return;
        }
    }

    // ── One-shot FIFO playback ────────────────────────────────────────────────
    int s1, b1, s2, b2;
    playbackFifo_.prepareToRead(numSamples, s1, b1, s2, b2);
    auto readFrames = [&](int fStart, int count, int bufOff)
    {
        for (int i = 0; i < count; ++i)
        {
            const size_t base = static_cast<size_t>(fStart + i) * 2u;
            if (base + 1 < playbackBuffer_.size() && bufOff + i < numSamples)
            {
                buffer.setSample(0, bufOff + i, playbackBuffer_[base]);
                buffer.setSample(1, bufOff + i, playbackBuffer_[base + 1]);
            }
        }
    };
    readFrames(s1, b1, 0);
    readFrames(s2, b2, b1);
    playbackFifo_.finishedRead(b1 + b2);

    for (int i = b1 + b2; i < numSamples; ++i)
    {
        buffer.setSample(0, i, 0.0f);
        buffer.setSample(1, i, 0.0f);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Generation — public entry point
// ═════════════════════════════════════════════════════════════════════════════

void AcestepAudioProcessor::startGeneration(const juce::String& prompt,
                                             int durationSeconds, int inferenceSteps,
                                             juce::File coverFile, float coverStrength,
                                             float bpm,
                                             const juce::String& lyrics, int seed)
{
    // Only one generation at a time.
    State expected = state_.load();
    while (expected == State::Submitting || expected == State::Running)
        return;
    while (!state_.compare_exchange_weak(expected, State::Submitting))
        if (expected == State::Submitting || expected == State::Running)
            return;
    {
        juce::ScopedLock l(statusLock_);
        statusText_ = stateToString(State::Submitting);
    }
    triggerAsyncUpdate();
    std::thread t(&AcestepAudioProcessor::runGenerationThread, this,
                  prompt, durationSeconds, inferenceSteps,
                  std::move(coverFile), coverStrength, bpm,
                  lyrics,
                  seed);
    t.detach();
}

// ═════════════════════════════════════════════════════════════════════════════
// Generation — background thread
// ═════════════════════════════════════════════════════════════════════════════

void AcestepAudioProcessor::runGenerationThread(juce::String prompt, int durationSec,
                                                 int inferenceSteps, juce::File coverFile,
                                                 float coverStrength, float bpm,
                                                 juce::String lyrics, int seed)
{
    // ── 1. Resolve binary and model directories ───────────────────────────────
    juce::String bp, mp;
    { juce::ScopedLock l(pathsLock_); bp = binariesPath_; mp = modelsPath_; }

    juce::File binDir = bp.isEmpty()
        ? juce::File::getSpecialLocation(juce::File::currentApplicationFile).getParentDirectory()
        : juce::File(bp);

    juce::File aceQwen3 = binDir.getChildFile("ace-qwen3");
    juce::File ditVae   = binDir.getChildFile("dit-vae");

    if (!aceQwen3.existsAsFile() || !ditVae.existsAsFile())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Binaries not found in: " + binDir.getFullPathName()
                   + "\nBuild: cmake -B vendor/acestep.cpp/build vendor/acestep.cpp"
                   + " && cmake --build vendor/acestep.cpp/build --config Release";
        statusText_ = lastError_;
        logError(lastError_);
        triggerAsyncUpdate();
        return;
    }

    juce::File modelsDir        = mp.isEmpty() ? getModelsDirectory() : juce::File(mp);
    juce::File lmModel          = modelsDir.getChildFile("acestep-5Hz-lm-4B-Q8_0.gguf");
    juce::File textEncoderModel = modelsDir.getChildFile("Qwen3-Embedding-0.6B-Q8_0.gguf");
    juce::File ditModel         = modelsDir.getChildFile("acestep-v15-turbo-Q8_0.gguf");
    juce::File vaeModel         = modelsDir.getChildFile("vae-BF16.gguf");

    if (!lmModel.existsAsFile() || !textEncoderModel.existsAsFile()
        || !ditModel.existsAsFile() || !vaeModel.existsAsFile())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Models not found in: " + modelsDir.getFullPathName()
                   + "\nDownload: cd vendor/acestep.cpp && ./models.sh";
        statusText_ = lastError_;
        logError(lastError_);
        triggerAsyncUpdate();
        return;
    }

    // ── 2. Create temp work directory ─────────────────────────────────────────
    juce::File tmpDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                            .getChildFile("AcestepVST")
                            .getChildFile("gen_" + juce::Time::getCurrentTime()
                                                        .formatted("%Y%m%d_%H%M%S"));
    if (!tmpDir.createDirectory())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Cannot create temp dir: " + tmpDir.getFullPathName();
        statusText_ = lastError_;
        logError(lastError_);
        triggerAsyncUpdate();
        return;
    }

    // ── 3. Write request.json ─────────────────────────────────────────────────
    juce::File requestFile = tmpDir.getChildFile("request.json");
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty("caption",         prompt);
        obj->setProperty("lyrics",          lyrics.isEmpty() ? "[Instrumental]" : lyrics);
        obj->setProperty("inference_steps", inferenceSteps <= 0 ? 8 : inferenceSteps);
        obj->setProperty("duration",        durationSec   <= 0 ? 10 : durationSec);

        // BPM: use caller-supplied value if non-zero, else fall back to host BPM
        float effectiveBpm = bpm > 0.0f ? bpm : static_cast<float>(hostBpm_.load(std::memory_order_relaxed));
        if (effectiveBpm > 0.0f)
            obj->setProperty("bpm", juce::roundToInt(effectiveBpm));

        // Seed: -1 means random / let the engine choose
        if (seed >= 0)
            obj->setProperty("seed", seed);

        // Cover / repaint mode
        const bool isCover = coverFile.existsAsFile();
        if (isCover)
        {
            obj->setProperty("src_audio",      coverFile.getFullPathName());
            obj->setProperty("cover_strength", juce::jlimit(0.0f, 1.0f, coverStrength));
            obj->setProperty("task_type",      "cover");
        }

        juce::String json = juce::JSON::toString(juce::var(obj));
        if (!requestFile.replaceWithText(json))
        {
            state_.store(State::Failed);
            juce::ScopedLock l(statusLock_);
            lastError_ = "Cannot write request.json to " + tmpDir.getFullPathName();
            statusText_ = lastError_;
            logError(lastError_);
            triggerAsyncUpdate();
            tmpDir.deleteRecursively();
            return;
        }
    }
    logTrace("request.json written to " + requestFile.getFullPathName());

    // ── 4. ace-qwen3 (LLM: text → audio codes) ───────────────────────────────
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
        logError(lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }
    lmProc.waitForProcessToFinish(kLmTimeoutMs);
    const int lmExit = static_cast<int>(lmProc.getExitCode());
    logTrace("ace-qwen3 exit=" + juce::String(lmExit));
    if (lmExit != 0)
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "ace-qwen3 failed (exit " + juce::String(lmExit) + ")";
        statusText_ = lastError_;
        logError(lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }

    juce::File enrichedRequest = tmpDir.getChildFile("request0.json");
    if (!enrichedRequest.existsAsFile())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "ace-qwen3 did not produce request0.json in " + tmpDir.getFullPathName();
        statusText_ = lastError_;
        logError(lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }

    // ── 5. dit-vae (DiT + VAE: synthesise audio) ──────────────────────────────
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

    // Pass source audio for cover / repaint mode
    if (coverFile.existsAsFile())
    {
        ditArgs.add("--src-audio");      ditArgs.add(coverFile.getFullPathName());
        ditArgs.add("--cover-strength"); ditArgs.add(juce::String(coverStrength, 2));
    }

    juce::ChildProcess ditProc;
    if (!ditProc.start(ditArgs, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Failed to start dit-vae: " + ditVae.getFullPathName();
        statusText_ = lastError_;
        logError(lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }
    ditProc.waitForProcessToFinish(kDitTimeoutMs);
    const int ditExit = static_cast<int>(ditProc.getExitCode());
    logTrace("dit-vae exit=" + juce::String(ditExit));
    if (ditExit != 0)
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "dit-vae failed (exit " + juce::String(ditExit) + ")";
        statusText_ = lastError_;
        logError(lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }

    // ── 6. Find output audio file ─────────────────────────────────────────────
    juce::File outputFile;
    juce::String audioExt;
    for (const juce::String& ext : kAudioExts)
    {
        juce::File c = tmpDir.getChildFile("request00." + ext);
        if (c.existsAsFile()) { outputFile = c; audioExt = ext; break; }
    }
    if (!outputFile.existsAsFile())
    {
        juce::Array<juce::File> found;
        tmpDir.findChildFiles(found, juce::File::findFiles, false,
                              "*." + kAudioExts.joinIntoString(";*."));
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
        lastError_ = "dit-vae produced no output in " + tmpDir.getFullPathName();
        statusText_ = lastError_;
        logError(lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }
    logTrace("output audio: " + outputFile.getFullPathName());

    // ── 7. Read bytes and hand off to message thread ──────────────────────────
    juce::MemoryBlock rawBytes;
    if (!outputFile.loadFileAsData(rawBytes) || rawBytes.getSize() == 0)
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Failed to read output audio: " + outputFile.getFullPathName();
        statusText_ = lastError_;
        logError(lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }

    {
        juce::ScopedLock l(pendingWavLock_);
        pendingWavBytes_.resize(rawBytes.getSize());
        std::memcpy(pendingWavBytes_.data(), rawBytes.getData(), rawBytes.getSize());
        pendingAudioExt_ = audioExt;
        pendingPrompt_   = prompt;
    }
    triggerAsyncUpdate();
    tmpDir.deleteRecursively();
}

// ═════════════════════════════════════════════════════════════════════════════
// Playback control (message-thread safe)
// ═════════════════════════════════════════════════════════════════════════════

void AcestepAudioProcessor::stopPlayback()
{
    stopRequested_.store(true, std::memory_order_release);
    {
        juce::ScopedLock l(statusLock_);
        if (state_.load() == State::Succeeded)
            statusText_ = "Idle \xe2\x80\x94 enter a prompt and click Generate.";
    }
}

void AcestepAudioProcessor::setLoopPlayback(bool loop)
{
    loopPlayback_.store(loop, std::memory_order_relaxed);
}

void AcestepAudioProcessor::previewLibraryEntry(const juce::File& file)
{
    // Don't interrupt an active generation.
    const auto st = state_.load();
    if (st == State::Submitting || st == State::Running)
        return;
    {
        juce::ScopedLock l(pendingPreviewLock_);
        pendingPreviewFile_ = file;
    }
    triggerAsyncUpdate();
}

// ═════════════════════════════════════════════════════════════════════════════
// Playback internals
// ═════════════════════════════════════════════════════════════════════════════

void AcestepAudioProcessor::pushSamplesToPlayback(const float* interleaved, int numFrames,
                                                   int sourceChannels, double sourceSampleRate)
{
    logTrace("pushSamplesToPlayback: frames=" + juce::String(numFrames)
           + " ch=" + juce::String(sourceChannels)
           + " rate=" + juce::String(sourceSampleRate));

    if (numFrames <= 0 || interleaved == nullptr) return;

    const double hostRate = sampleRate_.load(std::memory_order_relaxed);
    const double ratio    = sourceSampleRate > 0.0 ? hostRate / sourceSampleRate : 1.0;
    const int outFrames   = static_cast<int>(std::round(static_cast<double>(numFrames) * ratio));
    if (outFrames <= 0 || outFrames > kPlaybackFifoFrames)
    {
        logTrace("pushSamplesToPlayback: skipped (outFrames=" + juce::String(outFrames) + ")");
        return;
    }

    const int writeIdx = nextWriteIdx_.load(std::memory_order_relaxed);
    std::vector<float>& outBuf = pendingPlaybackBuf_[writeIdx];
    outBuf.resize(static_cast<size_t>(outFrames) * kNumOutputChannels);
    float* out = outBuf.data();

    for (int i = 0; i < outFrames; ++i)
    {
        const double srcIdx = ratio > 0.0 ? static_cast<double>(i) / ratio
                                          : static_cast<double>(i);
        const int i0 = std::min(std::max(0, static_cast<int>(srcIdx)), numFrames - 1);
        const int i1 = std::min(i0 + 1, numFrames - 1);
        const float t = static_cast<float>(srcIdx - std::floor(srcIdx));
        float l = 0.0f, r = 0.0f;
        if (sourceChannels >= 1)
        {
            l = interleaved[i0 * sourceChannels] * (1.0f - t)
              + interleaved[i1 * sourceChannels] * t;
            r = sourceChannels >= 2
                  ? interleaved[i0 * sourceChannels + 1] * (1.0f - t)
                  + interleaved[i1 * sourceChannels + 1] * t
                  : l;
        }
        out[i * 2]     = l;
        out[i * 2 + 1] = r;
    }

    pendingPlaybackFrames_.store(outFrames,    std::memory_order_release);
    pendingPlaybackBufIdx_.store(writeIdx,     std::memory_order_release);
    pendingPlaybackReady_.store(true,          std::memory_order_release);
    nextWriteIdx_.store(1 - writeIdx,          std::memory_order_release);
    logTrace("pushSamplesToPlayback: done (" + juce::String(outFrames) + " frames)");
}

void AcestepAudioProcessor::setLoopData(const float* interleaved, int numFrames)
{
    if (numFrames <= 0 || interleaved == nullptr) return;
    const int writeSlot = nextLoopWrite_.load(std::memory_order_relaxed);
    loopBuf_[writeSlot].assign(interleaved, interleaved + static_cast<size_t>(numFrames) * kNumOutputChannels);
    loopFrames_[writeSlot] = numFrames;
    loopReadPos_.store(0, std::memory_order_relaxed);
    loopBufActive_.store(writeSlot, std::memory_order_release);
    nextLoopWrite_.store(1 - writeSlot, std::memory_order_relaxed);
}

// ═════════════════════════════════════════════════════════════════════════════
// decodeAndPushAudio — message thread
// ═════════════════════════════════════════════════════════════════════════════

void AcestepAudioProcessor::decodeAndPushAudio(const std::vector<uint8_t>& bytes,
                                                const juce::String& ext,
                                                const juce::String& promptForLibrary)
{
    const bool isGenResult = promptForLibrary.isNotEmpty();

    juce::AudioFormatManager fm;
    fm.registerFormat(new juce::WavAudioFormat(), true);
#if JUCE_USE_MP3AUDIOFORMAT
    fm.registerFormat(new juce::MP3AudioFormat(), false);
#endif
    auto mis = std::make_unique<juce::MemoryInputStream>(bytes.data(), bytes.size(), false);
    std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(std::move(mis)));

    if (!reader)
    {
        if (isGenResult)
        {
            state_.store(State::Failed);
            juce::ScopedLock l(statusLock_);
            lastError_ = "Failed to decode audio (" + ext + ")";
            statusText_ = lastError_;
            logError(lastError_);
        }
        return;
    }

    const double fileSR     = reader->sampleRate;
    const int    numCh      = static_cast<int>(reader->numChannels);
    const int    numSamples = static_cast<int>(reader->lengthInSamples);
    logTrace("audio: rate=" + juce::String(fileSR) + " ch=" + juce::String(numCh)
           + " samples=" + juce::String(numSamples));

    if (numSamples <= 0 || numCh <= 0)
    {
        if (isGenResult)
        {
            state_.store(State::Failed);
            juce::ScopedLock l(statusLock_);
            lastError_ = "Invalid audio (no samples)";
            statusText_ = lastError_;
            logError(lastError_);
        }
        return;
    }

    juce::AudioBuffer<float> fb(numCh, numSamples);
    if (!reader->read(&fb, 0, numSamples, 0, true, true))
    {
        if (isGenResult)
        {
            state_.store(State::Failed);
            juce::ScopedLock l(statusLock_);
            lastError_ = "Failed to read audio samples";
            statusText_ = lastError_;
            logError(lastError_);
        }
        return;
    }

    // Build interleaved buffer (2-ch)
    std::vector<float> interleaved(static_cast<size_t>(numSamples) * kNumOutputChannels);
    for (int i = 0; i < numSamples; ++i)
    {
        interleaved[static_cast<size_t>(i) * kNumOutputChannels]      = numCh > 0 ? fb.getSample(0, i) : 0.0f;
        interleaved[static_cast<size_t>(i) * kNumOutputChannels + 1u] = numCh > 1
            ? fb.getSample(1, i) : interleaved[static_cast<size_t>(i) * kNumOutputChannels];
    }

    pushSamplesToPlayback(interleaved.data(), numSamples, 2, fileSR);
    setLoopData(interleaved.data(), numSamples);
    playbackBufferReady_.store(true);

    if (isGenResult)
    {
        state_.store(State::Succeeded);
        {
            juce::ScopedLock l(statusLock_);
            statusText_ = "Generated \xe2\x80\x94 playing. Drag from Library into your DAW.";
        }

        // Save to library
        try
        {
            juce::File libDir  = getLibraryDirectory();
            juce::String base  = "gen_" + juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
            juce::File wavFile = libDir.getChildFile(base + ".wav");
            auto outStream = wavFile.createOutputStream();
            if (outStream)
            {
                juce::WavAudioFormat wf;
                // createWriterFor takes ownership of the raw stream pointer;
                // release() gives it up so the unique_ptr won't double-delete.
                auto* rawStream = outStream.release();
                // args: stream, sampleRate, numChannels, bitsPerSample, metadata, qualityOption
                JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wdeprecated-declarations")
                auto* writerRaw = wf.createWriterFor(rawStream, fileSR,
                                                     static_cast<unsigned int>(numCh), 24, {}, 0);
                JUCE_END_IGNORE_WARNINGS_GCC_LIKE
                if (writerRaw)
                {
                    std::unique_ptr<juce::AudioFormatWriter> writer(writerRaw);
                    if (writer->writeFromAudioSampleBuffer(fb, 0, numSamples))
                        addToLibrary(wavFile, promptForLibrary);
                    // writer destructor flushes and deletes rawStream
                }
                else
                {
                    delete rawStream; // writer failed to take ownership
                }
            }
            logTrace("library save done");
        }
        catch (const std::exception& e) { logError("Library save failed: " + juce::String(e.what())); }
        catch (...) { logError("Library save failed (unknown)"); }
    }
    else
    {
        juce::ScopedLock l(statusLock_);
        statusText_ = "Previewing \xe2\x80\x94 press Stop to stop.";
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// handleAsyncUpdate — message thread
// ═════════════════════════════════════════════════════════════════════════════

void AcestepAudioProcessor::handleAsyncUpdate()
{
    logTrace("handleAsyncUpdate");

    // ── Preview request takes priority ────────────────────────────────────────
    juce::File previewFile;
    {
        juce::ScopedLock l(pendingPreviewLock_);
        previewFile = pendingPreviewFile_;
        pendingPreviewFile_ = juce::File();
    }
    if (previewFile.existsAsFile())
    {
        juce::MemoryBlock mb;
        if (previewFile.loadFileAsData(mb) && mb.getSize() > 0)
        {
            std::vector<uint8_t> bytes(mb.getSize());
            std::memcpy(bytes.data(), mb.getData(), mb.getSize());
            juce::String rawExt = previewFile.getFileExtension()
                                             .trimCharactersAtStart(".")
                                             .toLowerCase();
            decodeAndPushAudio(bytes, rawExt, {}); // empty prompt → preview only
        }
        return;
    }

    // ── Generation result ─────────────────────────────────────────────────────
    std::vector<uint8_t> audioBytes;
    juce::String audioExt, promptForLib;
    {
        juce::ScopedLock l(pendingWavLock_);
        if (pendingWavBytes_.empty()) return;
        audioBytes   = std::move(pendingWavBytes_);
        pendingWavBytes_.clear();
        audioExt     = pendingAudioExt_;
        promptForLib = pendingPrompt_;
    }

    try
    {
        decodeAndPushAudio(audioBytes, audioExt, promptForLib);
    }
    catch (const std::exception& e)
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_  = juce::String("Decode error: ") + e.what();
        statusText_ = lastError_;
        logError(lastError_);
    }
    catch (...)
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_  = "Decode error (unknown)";
        statusText_ = lastError_;
        logError(lastError_);
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Library management
// ═════════════════════════════════════════════════════════════════════════════

std::vector<AcestepAudioProcessor::LibraryEntry>
AcestepAudioProcessor::getLibraryEntries() const
{
    juce::Array<juce::File> files;
    auto libDir = getLibraryDirectory();
    libDir.findChildFiles(files, juce::File::findFiles, false, "*.wav;*.mp3");

    std::vector<LibraryEntry> entries;
    for (const juce::File& f : files)
    {
        juce::String prompt;
        juce::File sidecar = f.withFileExtension("txt");
        if (sidecar.existsAsFile())
            prompt = sidecar.loadFileAsString().trim();
        entries.push_back({ f, prompt, f.getLastModificationTime() });
    }
    std::sort(entries.begin(), entries.end(),
              [](const LibraryEntry& a, const LibraryEntry& b) { return a.time > b.time; });
    return entries;
}

void AcestepAudioProcessor::addToLibrary(const juce::File& audioFile,
                                          const juce::String& prompt)
{
    // Write prompt to a sidecar .txt so the UI can display it
    if (prompt.isNotEmpty())
        audioFile.withFileExtension("txt").replaceWithText(prompt);
}

bool AcestepAudioProcessor::deleteLibraryEntry(const juce::File& file)
{
    const bool ok = file.deleteFile();
    juce::File sidecar = file.withFileExtension("txt");
    if (sidecar.existsAsFile()) sidecar.deleteFile();
    return ok;
}

bool AcestepAudioProcessor::importAudioFile(const juce::File& sourceFile)
{
    if (!sourceFile.existsAsFile()) return false;
    juce::String ext = sourceFile.getFileExtension().toLowerCase();
    if (ext != ".wav" && ext != ".mp3") return false;
    juce::File dest = getLibraryDirectory().getChildFile(
        "import_" + juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S") + ext);
    return sourceFile.copyFileTo(dest);
}

// ═════════════════════════════════════════════════════════════════════════════
// Global config — persists settings across all DAW sessions / projects
// ═════════════════════════════════════════════════════════════════════════════

void AcestepAudioProcessor::saveSettingsToGlobalConfig() const
{
    juce::String bp, mp, op;
    { juce::ScopedLock l(pathsLock_); bp = binariesPath_; mp = modelsPath_; op = outputPath_; }
    auto xml = std::make_unique<juce::XmlElement>("AcestepConfig");
    xml->setAttribute("binariesPath", bp);
    xml->setAttribute("modelsPath",   mp);
    xml->setAttribute("outputPath",   op);
    juce::File cfg = getGlobalConfigFile();
    cfg.getParentDirectory().createDirectory();
    xml->writeTo(cfg);
}

void AcestepAudioProcessor::loadSettingsFromGlobalConfig()
{
    juce::File cfg = getGlobalConfigFile();
    if (!cfg.existsAsFile()) return;
    auto xml = juce::XmlDocument::parse(cfg);
    if (xml && xml->hasTagName("AcestepConfig"))
    {
        juce::ScopedLock l(pathsLock_);
        binariesPath_ = xml->getStringAttribute("binariesPath");
        modelsPath_   = xml->getStringAttribute("modelsPath");
        outputPath_   = xml->getStringAttribute("outputPath");
        logTrace("Global config loaded");
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Status accessors
// ═════════════════════════════════════════════════════════════════════════════

juce::String AcestepAudioProcessor::getStatusText() const
{
    juce::ScopedLock l(statusLock_); return statusText_;
}
juce::String AcestepAudioProcessor::getLastError() const
{
    juce::ScopedLock l(statusLock_); return lastError_;
}

// ═════════════════════════════════════════════════════════════════════════════
// AudioProcessor boilerplate
// ═════════════════════════════════════════════════════════════════════════════

const juce::String AcestepAudioProcessor::getName() const { return JucePlugin_Name; }
bool AcestepAudioProcessor::acceptsMidi()  const { return false; }
bool AcestepAudioProcessor::producesMidi() const { return false; }
bool AcestepAudioProcessor::isMidiEffect() const { return false; }
double AcestepAudioProcessor::getTailLengthSeconds() const { return 0.0; }
int AcestepAudioProcessor::getNumPrograms()  { return 1; }
int AcestepAudioProcessor::getCurrentProgram() { return 0; }
void AcestepAudioProcessor::setCurrentProgram(int i) { juce::ignoreUnused(i); }
const juce::String AcestepAudioProcessor::getProgramName(int i) { juce::ignoreUnused(i); return {}; }
void AcestepAudioProcessor::changeProgramName(int i, const juce::String& n) { juce::ignoreUnused(i, n); }

bool AcestepAudioProcessor::isBusesLayoutSupported(
    const juce::AudioProcessor::BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::mono()
        || layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

bool AcestepAudioProcessor::hasEditor() const { return true; }
juce::AudioProcessorEditor* AcestepAudioProcessor::createEditor()
{
    return new AcestepAudioProcessorEditor(*this);
}

// ── State persistence ─────────────────────────────────────────────────────────

void AcestepAudioProcessor::getStateInformation(juce::MemoryBlock& destData)
{
    juce::String bp, mp, op;
    { juce::ScopedLock l(pathsLock_); bp = binariesPath_; mp = modelsPath_; op = outputPath_; }
    auto xml = std::make_unique<juce::XmlElement>("AcestepState");
    xml->setAttribute("binariesPath", bp);
    xml->setAttribute("modelsPath",   mp);
    xml->setAttribute("outputPath",   op);
    copyXmlToBinary(*xml, destData);
}

void AcestepAudioProcessor::setStateInformation(const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml && xml->hasTagName("AcestepState"))
    {
        {
            juce::ScopedLock l(pathsLock_);
            binariesPath_ = xml->getStringAttribute("binariesPath");
            modelsPath_   = xml->getStringAttribute("modelsPath");
            outputPath_   = xml->getStringAttribute("outputPath");
        }
        // Keep global config in sync so paths survive fresh projects too.
        saveSettingsToGlobalConfig();
    }
}

// ── Plugin factory ────────────────────────────────────────────────────────────
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new AcestepAudioProcessor();
}
