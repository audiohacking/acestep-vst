# acestep-vst — Design & usage plan (historical)

Design and usage plan for the plugin, and concepts we can borrow from the ACE Studio approach.

---

## 1. What the plugin currently allows

| Area | Current state |
|------|----------------|
| **DAW presence** | Inserts as **Instrument** (AU + VST3), stereo output, no MIDI input. Host sees “AceForge Bridge”. |
| **Audio** | **Silence only.** `processBlock` clears output buffers; no playback buffer or AceForge audio yet. |
| **AceForgeClient** | **Implemented** (macOS): `healthCheck()`, `startGeneration(params)`, `getStatus(jobId)`, `getProgress()`, `fetchAudio(path)`. **Not wired** into the processor (no background thread, no calls from UI or state). |
| **UI** | **Placeholder:** one static line of text (“AceForge Bridge – connect to AceForge API”). No controls, no status, no connection check. |
| **State** | No stored base URL, no job ID, no playback buffer, no WAV decoding. |
| **Presets / automation** | No parameters exposed; no getStateInformation/setStateInformation for URL or prompt. |

So today: the plugin is a **shell** — it shows up in the DAW and outputs silence. All the pieces to talk to AceForge exist in the client library but are unused in the plugin.

---

## 2. Target usage (how we want it to be used)

### Primary workflow (text-to-music in the DAW)

1. User inserts **AceForge Bridge** on an instrument track.
2. User opens the plugin UI, sees **connection status** (e.g. “AceForge connected” or “Cannot reach AceForge”).
3. User enters a **text prompt** (e.g. “upbeat electronic beat with synth”), optionally **duration** and **task type**.
4. User clicks **Generate**; plugin shows **progress** (e.g. “Queued…”, “Running…”, or a progress bar).
5. When generation **succeeds**, the plugin **downloads the WAV**, decodes it, and **plays it back** on that track (optionally loop or one-shot).
6. User can **export / bounce** the track as usual in the DAW, or trigger another generation with a new prompt.

### Secondary workflows (later)

- **Reference / cover:** Use a reference track (AceForge refs or URL) and `taskType` (e.g. `cover`, `audio2audio`) from the same UI.
- **Browse / reload:** List recent or saved generations (AceForge songs/history) and reload one into the playback buffer.
- **Headless / minimal UI:** Some users may prefer a tiny UI (prompt + Generate + status) or even no UI (prompt/params via host automation only); design should allow a “compact” mode later.

---

## 3. Do we need a UI?

**Yes, for the primary workflow.** Reasons:

- **Prompt and Generate** — Users must enter text and trigger generation; that needs at least a text field and a button.
- **Status and errors** — “Connecting…”, “Generating…”, “Failed: &lt;reason&gt;” (or “AceForge not running”) need a visible place. Without that, the plugin feels broken when AceForge is down or the job fails.
- **Progress** — Optional but strongly recommended (queue position, ETA, or progress bar) so users know the job is running.
- **Playback control** — Play/stop or “reload last” can live in the same UI.

We can keep the UI **minimal** (single panel: connection, prompt, duration, Generate, progress, play/stop) and avoid feature creep. No need to mirror the full AceForge app.

**Optional later:** “Compact” mode (fewer controls) or host-automatable parameters so power users can drive the plugin without opening the GUI.

---

## 4. Concepts we can borrow from the ACE Studio approach

From reverse engineering ACE Studio and ACE Bridge 2:

| Concept | ACE Studio / Bridge | Use in AceForge Bridge |
|--------|----------------------|-------------------------|
| **Bridge as thin client** | ACE Bridge 2 links only system frameworks; uses `socket`/`connect` to talk to the host. No heavy SDK inside the plugin. | We already do this: plugin is a thin client; AceForge runs separately. Keep logic in the host app; plugin only sends requests and plays back audio. |
| **Async task pipeline** | App uses typed tasks: e.g. `SingingMambaDecoderTaskResult` → `SingingMambaVocoderTaskResult` → `SingingMambaSynthesisResult`; errors as `*ErrorType` + message. | Model our flow as **states**: Idle → Submitting → Queued → Running → Succeeded / Failed. One “generation task” per job; UI and playback react to state + error message. |
| **Network + file entitlements** | Bridge declares `network.client` and file read-write. | We need **outgoing HTTP** (and possibly file write for cache); document entitlements and request “Outgoing Connections (Client)” (or equivalent) so the plugin can call AceForge. |
| **Instrument, not effect** | ACE Bridge 2 is an **Instrument** (synth), not an effect. | We already register as synth (stereo out, no audio in). Fits “generate then play” rather than “process existing audio” (we can add effect/ref later). |
| **Optional WebView UI** | ACE Bridge 2 uses WebKit; UI might be web-based. | We don’t need a WebView for v1; native JUCE UI is simpler and sufficient. We can consider an embedded web UI later if we want to reuse AceForge’s React UI. |
| **Single job at a time** | AceForge API queues jobs; one runs at a time. | Plugin should assume **one active job per instance** (or one “current” job whose result we play). Simple state machine. |
| **Clear error reporting** | Studio has distinct error types and messages. | Surface **AceForge errors** in the UI: `result.error`, `lastError()`, and connection failures (e.g. “Cannot reach AceForge at …”). |

We are **not** copying: ARA, stem splitting, voice cloning, or the full ACE Studio project format. We focus on **trigger generation → poll → fetch WAV → play**.

---

## 5. Required design (minimal v1)

### 5.1 State machine (processor)

- **States:** `Idle` | `Submitting` | `Queued` | `Running` | `Succeeded` | `Failed`.
- **Stored:** `baseUrl` (string), `currentJobId` (string), `lastError` (string), `playbackBuffer` (ring buffer of float samples), `playbackPosition` (read head), `sampleRate` (from host).
- **Background thread** (or timer): from Submitting → start generation; in Queued/Running → poll status; on Succeeded → fetch WAV, decode to float, fill `playbackBuffer`; on Failed → set `lastError`, transition to Failed.
- **Audio thread:** in `processBlock`, read from `playbackBuffer` into output; if buffer empty or Idle/Failed, output silence. Never call AceForgeClient or HTTP from the audio thread.

### 5.2 Parameters (optional for v1, good for automation)

- **Host-automatable:** e.g. “Prompt” (string), “Duration” (int), “Generate trigger” (bool or int). Allows scripting and automation without opening the UI.
- **Non-automatable:** Base URL (or host:port), connection status, progress, error message.

### 5.3 UI (minimal)

- **Connection:** Status line or icon (“Connected to AceForge” / “Disconnected” / “Checking…”). Optional: editable base URL (host:port).
- **Inputs:** Text field (prompt), duration dropdown or slider (e.g. 15–60 s), optional task type (text2music default).
- **Actions:** “Generate” button (disabled when not connected or when Submitting/Queued/Running).
- **Feedback:** Progress (text or bar) when Queued/Running; error message when Failed; “Playing” or “Stopped” when Succeeded.
- **Playback:** Optional “Stop” or “Play again” for the current buffer.

### 5.4 Audio path

- **WAV handling:** When job succeeds, `fetchAudio(audioUrl)` returns raw WAV bytes. Decode (e.g. 44.1 kHz stereo, 16-bit or 32-bit) to float and push into a **lock-free ring buffer** (or double buffer). Respect host sample rate: resample if needed or document “AceForge outputs 44.1 kHz”.
- **Playback:** In `processBlock`, read `samplesPerBlock` frames from the ring buffer into the output buffers. If underrun, fill with silence. Optionally loop the buffer until user stops or triggers a new generation.

### 5.5 Persistence

- **Save/load:** At least save `baseUrl` (and optionally last prompt/duration) in `getStateInformation` / `setStateInformation` so the project remembers the connection and user preferences.

---

## 6. Implementation order (suggested)

1. **State + background thread** — Add state enum and a thread (or timer) that runs the flow: healthCheck → startGeneration → poll getStatus → on success fetchAudio. No UI yet; log or store status/lastError.
2. **WAV decode + ring buffer** — Decode WAV bytes to float; push into a thread-safe ring buffer. In `processBlock`, read from the ring buffer (or silence). Verify playback in the DAW.
3. **Minimal UI** — Connection status, prompt, duration, Generate button, progress/error text. Wire button to start generation (on background thread); wire state updates to refresh UI (thread-safe).
4. **Polish** — Base URL in UI and state, getStateInformation/setStateInformation, optional host parameters for automation, “compact” layout option.

---

## 7. JUCE audio output: processBlock and “timeline” vs realtime

### How JUCE plugins output audio

- **All output goes through `processBlock()`.** The DAW calls it every time it needs a block of audio (realtime or during offline render). There is no separate “write to timeline” API in JUCE or VST/AU. To “return audio to the DAW”, we fill the output buffers in `processBlock`; the host then either plays that buffer or records it (e.g. when the user records the track or freezes it).
- **Standard pattern:** Synths and generators allocate or use an internal buffer (or FIFO). When new content is ready (e.g. from a background thread), they hand it off to the audio thread (e.g. double-buffer or lock-free FIFO). In `processBlock` they read from that buffer into `buffer` (the output). Our design follows this: AceForge WAV → decode on message thread → push into double-buffer → audio thread copies into FIFO and reads into `processBlock` output.

### “Returning audio chunks into the timeline”

- **Realtime playback:** What we do now is correct for “play generated audio on the track”: the DAW calls `processBlock`, we write samples into the buffer, and the user hears (and can record) that output. No design change needed for that.
- **Timeline as “clip”:** If the goal is “drop a generated clip onto the timeline” (like a frozen region), that is **host-specific**. DAWs do not expose a standard way for a plugin to “insert a region here”. Options:
  - **Freeze / Bounce:** User freezes or bounces the track; the host runs the plugin and records our `processBlock` output to a new clip/file. No plugin API change.
  - **Export from plugin:** We could add “Export WAV” in the UI, save to a file, and the user drags it into the timeline. That’s a UI feature, not a different output model.
- So the current “processing” design (output in realtime from `processBlock`) is the right one for DAW playback and for freeze/bounce. “Chunks into the timeline” is achieved by the host recording our output or the user importing an exported file.

### JUCE examples that generate audio blocks

- **Official:** [JUCE Plugin Examples](https://juce.com/learn/tutorials/tutorial_plugin_examples) — e.g. **AudioPluginDemo** and **Multi-Out Synth** show `processBlock` filling the output buffer (and optional multi-bus layout).
- **Tutorials:** [Processing audio input](https://juce.com/learn/tutorials/tutorial_processing_audio_input), [Simple synth / noise](https://juce.com/learn/tutorials/tutorial_simple_synth_noise) — same idea: write into the `AudioBuffer` in `processBlock`.
- **FIFO:** [AbstractFifo](https://docs.juce.com/master/classAbstractFifo.html) is single-reader, single-writer; only the audio thread should call `reset()`. We use a double-buffer from message thread → audio thread, then the audio thread alone writes into the FIFO and reads from it in `processBlock`.

### Crash and error visibility

- **Double-buffer handoff:** The message thread must not overwrite the buffer the audio thread is reading. We use two buffers and alternate (`pendingPlaybackBuffer_[0]` / `[1]`, `nextWriteIndex_`); the message thread always writes to the “other” buffer.
- **Logging:** Errors are written to `getStatusText()` / `getLastError()` and also to **JUCE Logger** and **~/Library/Logs/AcestepVST.log** (and stderr in Debug). If the host crashes, check that log file and the DAW’s crash report (e.g. Console.app on macOS).

---

### Library + drag into DAW (no "realtime-only" lock-in)

We are **not** bound to realtime DSP-only. The plugin also acts as a **library** of generations and lets users **drag audio into the DAW** via the OS drag-and-drop API:

- **Library:** On each successful generation we save a WAV to `~/Library/Application Support/AcestepVST/Generations/` (e.g. `gen_YYYYMMDD_HHMMSS.wav`) and keep feeding the realtime playback FIFO for preview.
- **UI:** A "Library" list in the editor shows current and previous generations (all `.wav` files in that folder, newest first).
- **Drag into DAW:** JUCE's **`DragAndDropContainer::performExternalDragDropOfFiles(...)`** starts a native OS file drag. When the user drags a library row, we pass the WAV path; the user can drop it onto the DAW timeline (or anywhere). The DAW typically creates a clip from the dropped file. No VST/AU "timeline insert" API is required.

So we support both **realtime playback** (optional preview) and **drag-from-library into the DAW** for placing generated audio on the timeline.

---

## 8. Out of scope for v1

- ARA, MIDI input, stem splitting, voice cloning.
- Browsing AceForge song list or history in the plugin (can add later).
- Reference/cover flow in the UI (API supports it; we can add a second tab or panel later).
- Windows build (AceForgeClient is macOS-only for now; Windows client or JUCE networking later).

---

This document is the single place for **current capabilities**, **usage plan**, **UI necessity**, **ACE-inspired concepts**, and **required design** for the experimental plugin. We can refine it as we implement.
