# Debugging acestep-vst (especially crashes when audio returns)

## What happens when the API returns audio (the crash-prone path)

1. **Background thread** (`runGenerationThread`): AceForge returns “succeeded” and a WAV URL. We call `fetchAudio(url)` → get raw WAV bytes. We copy them into `pendingWavBytes_` under lock and call `triggerAsyncUpdate()`.

2. **Message thread** (`handleAsyncUpdate`): Wakes up, takes `pendingWavBytes_`, then:
   - Creates `AudioFormatManager` + `WavAudioFormat`, wraps bytes in `MemoryInputStream`.
   - `fm.createReaderFor(std::move(mis))` → decode WAV header and get a reader.
   - Reads into `AudioBuffer<float> fileBuffer`.
   - Builds interleaved float buffer.
   - **`pushSamplesToPlayback(...)`** → writes into one of the two `pendingPlaybackBuffer_[0/1]` (resize + fill), sets atomics so the audio thread will pick it up.
   - Optionally saves the same audio to a file in the library folder (WAV write).
   - Sets state to Succeeded.

3. **Audio thread** (`processBlock`): Called by the host every few ms. If `pendingPlaybackReady_` was set:
   - Reads from `pendingPlaybackBuffer_[bufIdx]`, copies into `playbackBuffer_` and feeds `playbackFifo_`, then reads from the FIFO into the output buffer.

So the crash can be:
- In the **message thread** (during WAV decode, interleave, pushSamplesToPlayback, or file save), or
- In the **audio thread** (when copying from `pendingPlaybackBuffer_` or from `playbackBuffer_` into the output).

If the process is killed (SIGKILL/crash), the **log file** only shows what was already flushed. We write each trace line and then **flush** the log file, so the **last line in the log is the last step we reached before the crash**.

---

## What you’ll see in the log

Open **`~/Library/Logs/AcestepVST.log`** after a crash. You’ll see lines like:

```
... TRACE: handleAsyncUpdate: start
... TRACE: handleAsyncUpdate: got WAV bytes, size=...
... TRACE: handleAsyncUpdate: creating MemoryInputStream
... TRACE: handleAsyncUpdate: creating reader
... TRACE: handleAsyncUpdate: WAV info rate=... ch=... samples=...
... TRACE: handleAsyncUpdate: allocating fileBuffer
... TRACE: handleAsyncUpdate: reading samples
... TRACE: handleAsyncUpdate: building interleaved buffer
... TRACE: handleAsyncUpdate: calling pushSamplesToPlayback
... TRACE: pushSamplesToPlayback: numFrames=...
... TRACE: pushSamplesToPlayback: resizing buffer to ...
... TRACE: pushSamplesToPlayback: done
... TRACE: handleAsyncUpdate: playback updated, saving to library
... TRACE: handleAsyncUpdate: library save done
... TRACE: handleAsyncUpdate: done
```

**The last TRACE line** is the last step that completed before the crash. That narrows it down to the **next** operation (e.g. crash inside “creating reader”, or right after “resizing buffer”, or in the audio thread which we don’t trace to avoid touching the audio thread with file I/O).

---

## Getting the actual crash location (stack trace)

The log only tells you “how far we got”. To see **where** it crashed (line of code, stack), use one of these:

### 1. macOS crash report (easiest)

After the DAW crashes:

- Open **Console.app** (Applications → Utilities).
- Sidebar: **Crash Reports** or **Log Reports**; or open **~/Library/Logs/DiagnosticReports/** in Finder.
- Find the newest report for your host (e.g. `Ableton Live_2025-…crash` or `Logic Pro_…crash`).
- Open it. The **“Crashed Thread”** section and **“Thread 0 Crashed”** backtrace show the exact frames. Look for `acestep-vst` or `AcestepVST` in the stack.

### 2. Run the DAW under lldb (full control)

From Terminal:

```bash
lldb /Applications/YourDAW.app/Contents/MacOS/YourDAW
# e.g. lldb /Applications/Ableton\ Live\ 12\ Suite.app/Contents/MacOS/Ableton\ Live\ 12
(lldb) run
```

Reproduce the crash (generate in the plugin). When it crashes, lldb stops. Then:

```text
(lldb) bt
```

Shows the full crash stack. Use `frame variable` and `frame select` to inspect.

### 3. Run the DAW from Terminal (stderr)

If the host prints to stderr, you can at least see any `fprintf(stderr, ...)` or our DEBUG logs:

```bash
/Applications/YourDAW.app/Contents/MacOS/YourDAW 2>&1 | tee daw.log
```

Then inspect `daw.log` after a crash. This does **not** give a stack trace; the crash report or lldb does.

---

## Summary

| Goal | What to use |
|------|-------------|
| Last step before crash | `~/Library/Logs/AcestepVST.log` → last TRACE line |
| Exact crash line + stack | Crash report in Console / DiagnosticReports, or run DAW under `lldb` and use `bt` |

Logic flow when audio returns: **background thread** → copies WAV into `pendingWavBytes_` and triggers async update → **message thread** decodes WAV, calls **pushSamplesToPlayback**, then optionally saves to library → **audio thread** in **processBlock** copies from the pending buffer into the FIFO and into the output. The crash is in one of these three places; the log + crash report together tell you which.
