# AceForge Bridge — Roadmap

This document tracks the planned feature progression for integrating
[acestep.cpp](https://github.com/ServeurpersoCom/acestep.cpp) as the local,
standalone AI inference engine inside the AceForge Bridge AU/VST3 plugin.

---

## Phase 1 — Submodule & project scaffolding ✅

- [x] Add `acestep.cpp` as a `vendor/` git submodule so it can be updated
      independently as the upstream project evolves.
- [x] Update GitHub Actions checkout to pull submodules recursively.
- [x] Document build steps for both the JUCE plugin and the acestep.cpp engine.
- [x] Add `.gitignore` rules for submodule build artefacts.

---

## Phase 2 — First-run setup & model management

- [ ] **Settings page** — on first plugin initialisation (and via a settings
      panel) prompt the user to choose:
  - Folder for GGUF model files — platform defaults:
    - macOS: `~/Library/Application Support/AceForgeBridge/models/`
    - Linux: `~/.config/AceForgeBridge/models/`
    - Windows: `%APPDATA%\AceForgeBridge\models\`
  - Folder for generated audio output — platform defaults:
    - macOS: `~/Library/Application Support/AceForgeBridge/Generations/`
    - Linux: `~/.config/AceForgeBridge/Generations/`
    - Windows: `%APPDATA%\AceForgeBridge\Generations\`
- [ ] **Model presence detection** — scan the model folder for the four required
      GGUFs (`Qwen3-Embedding-0.6B-Q8_0.gguf`, `acestep-5Hz-lm-4B-Q8_0.gguf`,
      `acestep-v15-turbo-Q8_0.gguf`, `vae-BF16.gguf`). If all are present,
      proceed normally; otherwise offer a download wizard.
- [ ] **Model download wizard** — wrap `models.sh` (or call the Hugging Face
      API directly) with a progress panel inside the plugin UI. Allow the user
      to choose quantisation (`Q8_0` by default, Q4/Q5/Q6 for space savings).
- [ ] **Graceful failure** — clear error messages when models are missing or
      partially downloaded; never crash the DAW host.

---

## Phase 3 — OS detection & platform builds

- [ ] **OS/accelerator detection** — at CMake configure time and at runtime,
      detect the target backend:
  - macOS → Metal (default; no extra flag needed)
  - Linux + NVIDIA → `GGML_CUDA=ON`
  - Linux + AMD → `GGML_HIP=ON`
  - Linux Vulkan → `GGML_VULKAN=ON`
  - CPU fallback → `GGML_BLAS=ON`
- [ ] **GitHub Actions matrix** — extend the CI workflow to build on:
  - `macos-latest` (arm64, Metal) — **first priority**
  - `ubuntu-latest` with CUDA runner — second priority
  - `ubuntu-latest` CPU-only — always-available fallback
- [ ] Each CI matrix leg produces a platform-specific zip and installer.

---

## Phase 4 — Basic text-to-music generation (acestep.cpp)

Replace the existing AceForge HTTP-based generation with a self-contained local
pipeline using the bundled acestep.cpp binaries.

- [ ] **Binary discovery** — locate `ace-qwen3` and `dit-vae` next to the
      plugin bundle (or in a configurable path).
- [ ] **Generation queue** — a background queue that serialises requests so the
      DAW audio thread is never blocked; show queue position in the UI.
- [ ] **Request JSON builder** — translate plugin UI fields (prompt, duration,
      inference steps, guidance scale, instrumental flag) into the
      `request.json` format accepted by `ace-qwen3`.
- [ ] **Two-stage pipeline** — run `ace-qwen3` then `dit-vae`; stream stderr
      progress into the plugin status label.
- [ ] **Audio result** — load the output WAV/MP3 into the playback buffer and
      add it to the library, exactly as the current API path does.
- [ ] **Drag-to-DAW** — improve JUCE drag-and-drop so generated files can be
      dropped directly onto the DAW timeline at the playhead cursor position
      (see [JUCE forum reference](https://forum.juce.com/t/drag-and-drop-sources-and-destinations/55859/2)).
- [ ] **Insert at cursor** — use `juce::DragAndDropContainer::startDraggingExternalFiles`
      with the generated WAV path so the DAW receives a proper file drop at
      the current transport position.

---

## Phase 5 — Cover / repaint mode

Transform an existing audio file into a new style using `--src-audio`.

- [ ] **Audio file input** — accept WAV/MP3 files dropped onto the plugin UI
      (from Finder, Explorer, or the DAW itself) as source audio.
- [ ] **Cover parameters** — expose `audio_cover_strength` (0–1 slider) and
      style prompt in the UI.
- [ ] **No-LLM path** — call `dit-vae` directly with `--src-audio`; skip the
      `ace-qwen3` step for faster turnaround.

---

## Phase 6 — Lego / stem mode

Generate individual stems (drums, bass, chords, melody) that can each be placed
independently on the DAW timeline.

- [ ] **Stem request** — extend the request JSON with stem-separation metadata
      once acestep.cpp exposes it.
- [ ] **Multi-track insert** — insert each stem onto its own track in the DAW
      (Logic Pro / Ableton) at the cursor position.
- [ ] **Stem library** — group stems by generation session in the library panel.

---

## Phase 7 — Batch generation & variants

- [ ] **LLM batch** (`--batch N`) — generate N lyrically distinct songs from
      one prompt; present them as alternatives in the library.
- [ ] **DiT batch** — generate subtle render variations of the same song for
      cherry-picking.
- [ ] **Comparison view** — side-by-side waveform previews in the plugin UI.

---

## Phase 8 — LoRA / fine-tuned model support

- [ ] **LoRA adapter picker** — allow users to point to a PEFT directory or a
      ComfyUI `.safetensors` file to steer generation style.
- [ ] **Adapter library** — list available adapters; toggle per-generation.

---

## Updating the submodule

```bash
# Pull the latest upstream acestep.cpp changes
# (upstream uses the 'master' branch; verify with `git -C vendor/acestep.cpp branch -a`)
git -C vendor/acestep.cpp fetch origin
git -C vendor/acestep.cpp checkout origin/master
git add vendor/acestep.cpp
git commit -m "chore: update acestep.cpp submodule"
```
