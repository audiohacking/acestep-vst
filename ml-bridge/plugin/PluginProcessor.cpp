#include "PluginProcessor.h"
#include "PluginEditor.h"
#include <cstring>
#include <fstream>
#include <iostream>
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
    juce::File logDir = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                            .getChildFile("AcestepVST");
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

// ── Readable timestamp ─────────────────────────────────────────────────────────

static juce::String logTimestamp()
{
    return juce::Time::getCurrentTime().formatted("%H:%M:%S");
}

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
static constexpr int kLmTimeoutMs  = 300'000;  // ace-lm:    5 min
static constexpr int kDitTimeoutMs = 600'000;  // ace-synth: 10 min

// ── Platform-specific binary names ────────────────────────────────────────────
#if JUCE_WINDOWS
static const juce::String kAceLmName    = "ace-lm.exe";
static const juce::String kAceSynthName = "ace-synth.exe";
#else
static const juce::String kAceLmName    = "ace-lm";
static const juce::String kAceSynthName = "ace-synth";
#endif

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
// Streaming log
// ═════════════════════════════════════════════════════════════════════════════

void AcestepAudioProcessor::appendToLog(const juce::String& text)
{
    if (text.isEmpty()) return;
    juce::ScopedLock l(logLock_);
    pendingLog_ += text;
    // Ensure the buffer always ends with a newline so log entries stay separated.
    if (!pendingLog_.endsWithChar('\n'))
        pendingLog_ += "\n";
}

void AcestepAudioProcessor::clearLog()
{
    juce::ScopedLock l(logLock_);
    pendingLog_.clear();
}

juce::String AcestepAudioProcessor::getAndClearNewLog()
{
    juce::ScopedLock l(logLock_);
    juce::String result = pendingLog_;
    pendingLog_.clear();
    return result;
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

juce::File AcestepAudioProcessor::getBundledBinariesDirectory()
{
    // The CI build embeds ace-lm and ace-synth alongside the plugin's own binary
    // inside the bundle (e.g. Contents/MacOS/ on macOS, Contents/x86_64-linux/
    // on Linux, Contents/x86_64-win/ on Windows).  currentExecutableFile points
    // at that binary, so its parent directory is exactly where the bundled
    // binaries live.  (currentApplicationFile would point at the bundle root,
    // whose parent is the folder *containing* the bundle — the wrong level.)
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile)
               .getParentDirectory();
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
    juce::File binDir = bp.isEmpty() ? getBundledBinariesDirectory() : juce::File(bp);
    return binDir.getChildFile(kAceLmName).existsAsFile()
        && binDir.getChildFile(kAceSynthName).existsAsFile();
}

// ═════════════════════════════════════════════════════════════════════════════
// AudioProcessor boilerplate
// ═════════════════════════════════════════════════════════════════════════════

void AcestepAudioProcessor::prepareToPlay(double sampleRate, int samplesPerBlock)
{
    preview_.prepareToPlay(sampleRate, samplesPerBlock);
}

void AcestepAudioProcessor::releaseResources()
{
    preview_.releaseResources();
}

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

    if (buffer.getNumChannels() == 0) return;

    // Pass through the input audio unchanged when preview is not active.
    // render() will clear and overwrite the buffer only while a clip is playing.
    preview_.render(buffer);
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
    // Clear the log so each generation starts with a fresh output panel.
    clearLog();
    appendToLog("[" + logTimestamp() + "] Starting generation\xe2\x80\xa6");
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

    juce::File binDir = bp.isEmpty() ? getBundledBinariesDirectory() : juce::File(bp);

    juce::File aceLm    = binDir.getChildFile(kAceLmName);
    juce::File aceSynth = binDir.getChildFile(kAceSynthName);

    if (!aceLm.existsAsFile() || !aceSynth.existsAsFile())
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Bundled binaries not found in: " + binDir.getFullPathName()
                   + "\nThe plugin bundle should include ace-lm and ace-synth."
                   + "\nFor development, set a custom path in Settings.";
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

    // ── 4. ace-lm (LLM: text → audio codes) ─────────────────────────────────
    {
        juce::ScopedLock l(statusLock_);
        statusText_ = stateToString(State::Submitting);
    }
    triggerAsyncUpdate();

    appendToLog("[" + logTimestamp() + "] Running ace-lm (LLM step)\xe2\x80\xa6");

    juce::StringArray lmArgs;
    lmArgs.add(aceLm.getFullPathName());
    lmArgs.add("--request"); lmArgs.add(requestFile.getFullPathName());
    lmArgs.add("--lm");      lmArgs.add(lmModel.getFullPathName());

    juce::ChildProcess lmProc;
    if (!lmProc.start(lmArgs, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Failed to start ace-lm: " + aceLm.getFullPathName();
        statusText_ = lastError_;
        logError(lastError_);
        appendToLog("[" + logTimestamp() + "] ERROR: " + lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }
    lmProc.waitForProcessToFinish(kLmTimeoutMs);
    {
        // Capture all output produced by ace-lm and stream it to the log panel.
        const juce::String lmOutput = lmProc.readAllProcessOutput().trimEnd();
        if (lmOutput.isNotEmpty())
            appendToLog(lmOutput);
    }
    const int lmExit = static_cast<int>(lmProc.getExitCode());
    appendToLog("[" + logTimestamp() + "] ace-lm exit=" + juce::String(lmExit));
    logTrace("ace-lm exit=" + juce::String(lmExit));
    if (lmExit != 0)
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "ace-lm failed (exit " + juce::String(lmExit) + ")";
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
        lastError_ = "ace-lm did not produce request0.json in " + tmpDir.getFullPathName();
        statusText_ = lastError_;
        logError(lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }

    // ── 5. ace-synth (DiT + VAE: synthesise audio) ────────────────────────────
    state_.store(State::Running);
    {
        juce::ScopedLock l(statusLock_);
        statusText_ = stateToString(State::Running);
    }
    appendToLog("[" + logTimestamp() + "] Running ace-synth (DiT+VAE step)\xe2\x80\xa6");

    juce::StringArray ditArgs;
    ditArgs.add(aceSynth.getFullPathName());
    ditArgs.add("--request");    ditArgs.add(enrichedRequest.getFullPathName());
    ditArgs.add("--embedding");  ditArgs.add(textEncoderModel.getFullPathName());
    ditArgs.add("--dit");        ditArgs.add(ditModel.getFullPathName());
    ditArgs.add("--vae");        ditArgs.add(vaeModel.getFullPathName());

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
        lastError_ = "Failed to start ace-synth: " + aceSynth.getFullPathName();
        statusText_ = lastError_;
        logError(lastError_);
        appendToLog("[" + logTimestamp() + "] ERROR: " + lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }
    ditProc.waitForProcessToFinish(kDitTimeoutMs);
    {
        // Capture all output produced by ace-synth and stream it to the log panel.
        const juce::String ditOutput = ditProc.readAllProcessOutput().trimEnd();
        if (ditOutput.isNotEmpty())
            appendToLog(ditOutput);
    }
    const int ditExit = static_cast<int>(ditProc.getExitCode());
    appendToLog("[" + logTimestamp() + "] ace-synth exit=" + juce::String(ditExit));
    logTrace("ace-synth exit=" + juce::String(ditExit));
    if (ditExit != 0)
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "ace-synth failed (exit " + juce::String(ditExit) + ")";
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
        lastError_ = "ace-synth produced no output in " + tmpDir.getFullPathName();
        statusText_ = lastError_;
        logError(lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }
    logTrace("output audio: " + outputFile.getFullPathName());

    // ── 7. Copy output directly to the library and hand off to message thread ─
    // We skip the decode/re-encode round-trip and copy the file as-is.  The
    // message thread (handleAsyncUpdate) then loads it straight into the
    // AudioTransportSource-backed preview engine and starts playback.
    juce::File libDir  = getLibraryDirectory();
    juce::String base  = "gen_" + juce::Time::getCurrentTime().formatted("%Y%m%d_%H%M%S");
    juce::File destFile = libDir.getChildFile(base + "." + audioExt);

    if (!outputFile.copyFileTo(destFile))
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_ = "Failed to copy generated audio to library: " + destFile.getFullPathName();
        statusText_ = lastError_;
        logError(lastError_);
        triggerAsyncUpdate();
        tmpDir.deleteRecursively();
        return;
    }

    // Register in library: write sidecar prompt file alongside the audio file
    addToLibrary(destFile, prompt);

    logTrace("library file: " + destFile.getFullPathName());
    appendToLog("[" + logTimestamp() + "] Generation complete \xe2\x80\x94 saved to: "
                + destFile.getFileName());

    {
        juce::ScopedLock l(pendingLibraryFileLock_);
        pendingLibraryFile_ = destFile;
    }
    triggerAsyncUpdate();
    tmpDir.deleteRecursively();
}

// ═════════════════════════════════════════════════════════════════════════════
// Playback control (message-thread safe)
// ═════════════════════════════════════════════════════════════════════════════

void AcestepAudioProcessor::stopPlayback()
{
    preview_.stop();
    juce::ScopedLock l(statusLock_);
    if (state_.load() == State::Succeeded)
        statusText_ = "Idle \xe2\x80\x94 enter a prompt and click Generate.";
}

void AcestepAudioProcessor::setLoopPlayback(bool loop)
{
    loopPlayback_.store(loop, std::memory_order_relaxed);
    preview_.setLooping(loop);
}

void AcestepAudioProcessor::previewLibraryEntry(const juce::File& file)
{
    // Don't interrupt an active generation.
    const auto st = state_.load();
    if (st == State::Submitting || st == State::Running)
        return;

    if (!preview_.loadFile(file))
    {
        juce::ScopedLock l(statusLock_);
        lastError_  = "Preview failed: could not load " + file.getFileName();
        statusText_ = lastError_;
        logError(lastError_);
        return;
    }

    preview_.play(loopPlayback_.load(std::memory_order_relaxed));
    juce::ScopedLock l(statusLock_);
    statusText_ = "Previewing \xe2\x80\x94 press Stop to stop.";
}

// ═════════════════════════════════════════════════════════════════════════════
// handleAsyncUpdate — message thread
// ═════════════════════════════════════════════════════════════════════════════

void AcestepAudioProcessor::handleAsyncUpdate()
{
    logTrace("handleAsyncUpdate");

    juce::File libFile;
    {
        juce::ScopedLock l(pendingLibraryFileLock_);
        if (!pendingLibraryFile_.existsAsFile()) return;
        libFile             = pendingLibraryFile_;
        pendingLibraryFile_ = juce::File();
    }

    if (!preview_.loadFile(libFile))
    {
        state_.store(State::Failed);
        juce::ScopedLock l(statusLock_);
        lastError_  = "Failed to load generated audio: " + libFile.getFullPathName();
        statusText_ = lastError_;
        logError(lastError_);
        return;
    }

    preview_.play(loopPlayback_.load(std::memory_order_relaxed));
    state_.store(State::Succeeded);
    {
        juce::ScopedLock l(statusLock_);
        statusText_ = "Generated \xe2\x80\x94 playing. Drag from Library into your DAW.";
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
