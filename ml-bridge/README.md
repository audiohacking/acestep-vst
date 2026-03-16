# acestep-vst — AU/VST3 Plugin

JUCE **AU** and **VST3** plugin that runs **local AI music generation** directly inside your DAW using the [acestep.cpp](https://github.com/ServeurpersoCom/acestep.cpp) inference engine. No external server required — everything runs on your machine.

- **Target:** macOS (Apple Silicon); build via CMake + JUCE.
- **Inference engine:** `acestep.cpp` (submodule at `vendor/acestep.cpp`), built separately.

---

## What the plugin does

1. **Generate** — Enter a prompt (e.g. "upbeat electronic beat, 10s"), choose duration (10–30 s) and quality (Fast / High), click **Generate**. The plugin runs `ace-lm` (LLM step) then `ace-synth` (DiT+VAE synthesis) locally and plays the result through the plugin output.
2. **Playback** — When generation succeeds, the audio plays once through the plugin output (so you can hear it and/or record the track in the DAW).
3. **Library** — Each successful generation is saved as a WAV under **~/Library/Application Support/AcestepVST/Generations/** (e.g. `gen_20250206_143022.wav`). The plugin UI shows a **Library** list (newest first) with a **Refresh** button.
4. **Add to DAW** — Select a library row, then:
   - **Insert into DAW** (macOS): Opens the file with **Logic Pro** (a new project with that audio). You can then drag the audio from that project into your main project, or use **Reveal in Finder** and drag the file from Finder onto your timeline.
   - **Reveal in Finder**: Opens Finder with the file selected so you can drag it into Logic (or any DAW).
   - **Double-click** a row: Copies the file path to the clipboard.

---

## Requirements

- macOS (Apple Silicon). AU and VST3 are built; install and rescan in your DAW.
- **acestep.cpp binaries** (`ace-lm` and `ace-synth`) built and placed where the plugin can find them (see below).
- **ACE-Step GGUF models** downloaded (see below).

---

## Build and install

### 1. Clone with submodules

```bash
git clone --recurse-submodules https://github.com/audiohacking/acestep-vst.git
# or, after a plain clone:
git submodule update --init
```

### 2. Build the JUCE plugin

From the **repo root**:

```bash
cmake -B build -G "Unix Makefiles" -DCMAKE_OSX_ARCHITECTURES=arm64
# or: cmake -B build -G Xcode -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build --config Release
```

Built artefacts:

- **AU:** `build/ml-bridge/plugin/AcestepVST_artefacts/Release/AU/acestep-vst.component`
- **VST3:** `build/ml-bridge/plugin/AcestepVST_artefacts/Release/VST3/acestep-vst.vst3`

Copy them to:

- `~/Library/Audio/Plug-Ins/Components/` (AU)
- `~/Library/Audio/Plug-Ins/VST3/` (VST3)

Then rescan plugins in your DAW.

### 3. Build the acestep.cpp inference engine

```bash
# macOS / Apple Silicon (Metal auto-detected)
cmake -B vendor/acestep.cpp/build vendor/acestep.cpp
cmake --build vendor/acestep.cpp/build --config Release -j$(sysctl -n hw.logicalcpu)
```

This produces `ace-lm` and `ace-synth` in `vendor/acestep.cpp/build/`.

### 4. Download ACE-Step models

```bash
cd vendor/acestep.cpp
pip install hf
./models.sh   # downloads ~7.7 GB of Q8_0 GGUFs into vendor/acestep.cpp/models/
```

### 5. Place binaries and models where the plugin expects them

You can configure the paths to the binaries and models directly in the plugin's **Settings** tab (the third tab in the UI), which opens automatically on first launch. No need to move files into a specific directory — just point the settings to wherever you built or downloaded them.

**Default locations** (used if no custom path is set in Settings):

```bash
# Default binaries location: next to the AU bundle
~/Library/Audio/Plug-Ins/Components/ace-lm
~/Library/Audio/Plug-Ins/Components/ace-synth

# Default models location:
~/Library/Application Support/AcestepVST/models/

# Default output / generations location:
~/Library/Application Support/AcestepVST/Generations/
```

To use the defaults, copy the files there:

```bash
# Copy binaries next to the AU bundle (default location)
cp vendor/acestep.cpp/build/ace-lm     ~/Library/Audio/Plug-Ins/Components/
cp vendor/acestep.cpp/build/ace-synth  ~/Library/Audio/Plug-Ins/Components/

# Copy models to the default models directory
mkdir -p ~/Library/Application\ Support/AcestepVST/models/
cp vendor/acestep.cpp/models/*.gguf ~/Library/Application\ Support/AcestepVST/models/
```

Or use the **Settings** tab in the plugin UI to point to any directory of your choice.

### Installer (.pkg)

After building, from the repo root:

```bash
./scripts/build-installer-pkg.sh --sign-plugins --version 0.1.0
```

- **Output:** `release-artefacts/AcestepVST-macOS-Installer.pkg` and `release-artefacts/AcestepVST-macOS-AU-VST3.zip`.
- **Install:** `sudo installer -pkg release-artefacts/AcestepVST-macOS-Installer.pkg -target /` or open the `.pkg` in Finder.

See **BUILD_AND_CI.md** for details and GitHub Actions release.

---

## Repo layout (ml-bridge)

| Path | Description |
|------|-------------|
| **plugin/** | JUCE plugin (Processor + Editor), AU + VST3 target |
| **AceForgeClient/** | Legacy HTTP client (unused; kept for reference) |
| **BUILD_AND_CI.md** | Build steps and CI/release workflow |
| **DEBUGGING.md** | Crash / log and trace notes |

---

## Architecture (brief)

- **Plugin:** Stereo output. Background thread: writes `request.json` to a temp directory → spawns `ace-lm` (LLM: generates lyrics + audio codes) → spawns `ace-synth` (DiT+VAE: synthesises audio) → reads output WAV/MP3 → hands bytes to message thread → decodes (WAV or MP3) → fills double-buffered playback FIFO → saves to library.
- **No external server.** All inference runs locally via `acestep.cpp` subprocesses.
- **Binary and model paths** are persisted in the plugin state (DAW project file). They can be configured in the **Settings** tab in the plugin UI. They default to: binaries next to the plugin bundle, models in `~/Library/Application Support/AcestepVST/models/`.

---

## Optional / future

See **ROADMAP.md** in the repo root for the full feature plan.

- **Cover / repaint mode:** Pass a source audio file to `ace-synth --src-audio`.
- **Lego / stem mode:** Multi-track generation.
- **Windows / Linux builds** via CI.
