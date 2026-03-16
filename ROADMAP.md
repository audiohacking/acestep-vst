# acestep-vst — Roadmap

Tracks planned features for the standalone **acestep-vst** AU/VST3 plugin powered by
[acestep.cpp](https://github.com/ServeurpersoCom/acestep.cpp).

---

## Phase 1 — Submodule, standalone engine & rename ✅

- [x] Add `acestep.cpp` as `vendor/` git submodule.
- [x] Remove AceForge HTTP server dependency entirely.
- [x] Replace `AceForgeClient` HTTP pipeline with `juce::ChildProcess` subprocess pipeline (`ace-lm` → `ace-synth`).
- [x] **Rename plugin** from "AceForge Bridge" to **"acestep-vst"** (`AcestepVST` CMake target, `AcestepAudioProcessor` class).
- [x] Enable `JUCE_USE_MP3AUDIOFORMAT` to decode MP3 output from `ace-synth`.
- [x] Persist binary/model/output paths in DAW project state.

---

## Phase 2 — Generation queue / library player ✅

- [x] **Library list** — browse all previous generations (WAV + MP3), sorted newest-first.
- [x] **Sidecar prompts** — write a `.txt` next to each generated file; display the prompt in the list.
- [x] **Preview** — click ▶ Preview to load any library entry into the playback buffer; press ■ Stop to stop.
- [x] **Loop** — toggle ⟳ Loop to repeat the current audio continuously.
- [x] **Delete** — remove a library entry (file + sidecar) with one click.
- [x] **Import** — Import File… button copies any external WAV/MP3 into the library.
- [x] **Drag from OS** — drop WAV/MP3 files from Finder/Explorer onto the plugin window to import them into the library (Library tab) or set them as a cover reference (Generate tab).
- [x] **Drag to DAW** — drag a library row to insert the file into the DAW timeline at the current cursor position.
- [x] **Use as Reference** — one click sets a library entry as the cover reference and switches to Cover Mode.

---

## Phase 3 — Cover / repaint mode ✅

- [x] **Reference audio** — Browse… or drag-drop a WAV/MP3 as the source audio for cover mode.
- [x] **Cover strength slider** — 0–1 slider controls how closely the output follows the reference.
- [x] **Mode toggle** — "Text-to-Music" vs "Cover Mode" buttons with clear visual distinction.
- [x] Pass `--src-audio` and `--cover-strength` to `ace-synth` CLI.
- [x] Embed `src_audio`, `cover_strength`, and `task_type: "cover"` in `request.json`.

---

## Phase 4 — Settings & BPM ✅

- [x] **Settings tab** — configure binaries directory, models directory, and output directory; changes persisted in DAW project XML.
- [x] **BPM auto-detect** — read BPM from the DAW playhead in every `processBlock()`; auto-populate the BPM field (editable override).
- [x] **BPM in request.json** — pass detected/entered BPM to `ace-lm` for rhythmically consistent output.

---

## Phase 5 — Model management & first-run UX

- [x] **First-run Settings tab** — plugin opens on Settings tab when binaries are not yet configured, guiding the user to set paths on first launch.
- [ ] **Model presence detection** — scan model folder on startup; show clear warning in Settings if models are missing.
- [ ] **Download wizard** — wrap `models.sh` with a progress panel inside the plugin UI.

---

## Phase 6 — OS & accelerator support

- [ ] **Linux / CUDA / ROCm / Vulkan** builds via CI matrix.
- [ ] **Windows** support.
- [ ] **GitHub Actions matrix** — `macos-latest` (arm64, Metal) + `ubuntu-latest` CPU fallback.

---

## Phase 7 — Lego / stem mode

- [ ] Generate individual stems (drums, bass, chords, melody).
- [ ] Multi-track insert: place each stem on its own DAW track.

---

## Phase 8 — Batch generation & variants

- [ ] `--batch N` — generate N variations; present as alternatives in the library.
- [ ] Side-by-side comparison view.

---

## Updating the submodule

```bash
git -C vendor/acestep.cpp fetch origin
git -C vendor/acestep.cpp checkout origin/master
git add vendor/acestep.cpp
git commit -m "chore: update acestep.cpp submodule"
```
