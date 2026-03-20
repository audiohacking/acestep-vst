<img width="180" alt="logo" src="https://github.com/user-attachments/assets/ae211c8f-eb2e-4434-a657-028e876f82e3" />

# acestep-vst

Standalone **AU** (macOS) and **VST3** (macOS · Linux · Windows) plugin for local AI music
generation in your DAW — no servers, no cloud, no APIs.

> Powered by [acestep.cpp](https://github.com/audiohacking/acestep.cpp).

## Features

- **Text-to-Music** — type a prompt, pick duration and quality, click Generate.
- **Cover / Repaint mode** — use any WAV/MP3 as a reference; the model transforms it according to your prompt.
- **BPM auto-detect** — reads BPM from the DAW playhead automatically.
- **AudioTransportSource preview** — generated audio plays back immediately with proper JUCE transport; loop and stop from the Library tab.
- **Generation library** — browse, play, loop, delete, and drag previous generations directly into your DAW timeline.
- **Import & drag-drop** — import external WAV/MP3 files or drag them onto the plugin window.
- **Drag to DAW** — drag any library entry straight onto the DAW timeline (OS-level file drag).
- **Settings** — configure binary directory, models directory, and output directory; persisted in the DAW project.

<img width="844" height="520" alt="image" src="https://github.com/user-attachments/assets/335c76bb-a519-48fd-b8f6-9da3f38e093e" />

## Quick start

### 1. Install the plugin

Download the latest release for your platform from the
[Releases page](https://github.com/audiohacking/acestep-vst/releases):

| Platform | File | Formats |
|----------|------|---------|
| macOS (Apple Silicon) | `AcestepVST-macOS-Installer.pkg` or `…AU-VST3.zip` | AU + VST3 |
| Linux x86-64 | `AcestepVST-Linux-VST3.tar.gz` | VST3 |
| Windows x86-64 | `AcestepVST-Windows-VST3.zip` | VST3 |

Each bundle already contains the `ace-lm` and `ace-synth` inference engines — no
separate binary installation required.

### 2. Download models (~7.7 GB)

```bash
cd vendor/acestep.cpp && ./models.sh
```

Or download manually from Hugging Face and place the `.gguf` files in a folder of
your choice.

### 3. Configure paths in the plugin

Open the plugin in your DAW, go to the **Settings** tab and set:

- **Binaries directory** — leave empty if using the bundled release (auto-detected).
- **Models directory** — folder containing the four `.gguf` model files.
- **Output directory** — where generated audio is saved (default: platform app-data folder).

### 4. Generate

Switch to the **Generate** tab, type a prompt, and click **Generate**.

---

## Building from source

See **ml-bridge/BUILD_AND_CI.md** for full build instructions (macOS · Linux · Windows).

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/audiohacking/acestep-vst.git

# macOS
cmake -B build -G Xcode -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build --config Release

# Linux (install JUCE deps first — see BUILD_AND_CI.md)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build

# Windows
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

## Repo layout

| Path | Description |
|------|-------------|
| **ml-bridge/plugin/** | JUCE AU + VST3 plugin source |
| **ml-bridge/BUILD_AND_CI.md** | Build steps and CI/release workflow |
| **vendor/acestep.cpp/** | [acestep.cpp](https://github.com/audiohacking/acestep.cpp) submodule |
| **ROADMAP.md** | Feature roadmap |

## License

Plugin uses JUCE (GPL mode). See repository license.
