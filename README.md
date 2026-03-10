# acestep-vst

Standalone **AU** and **VST3** plugin for local AI music generation in your DAW — no servers, no cloud, no APIs. 

> Powered by [acestep.cpp](https://github.com/ServeurpersoCom/acestep.cpp).

## Features

- **Text-to-Music** — type a prompt, pick duration and quality, click Generate.
- **Cover / Repaint mode** — use any WAV/MP3 as a reference; the underlying model transforms it according to your prompt.
- **BPM auto-detect** — reads BPM from the DAW playhead automatically.
- **Generation queue / library** — browse, play, loop, delete, and drag previous generations directly into your DAW timeline.
- **Import & drag-drop** — import external WAV/MP3 files into the library, or drag them from Finder onto the plugin window to use as a cover reference.
- **Loop playback** — preview any library entry on loop inside the plugin output.
- **Settings** — configure binary directory, models directory, and output directory; all persisted in the DAW project.

## Quick start

1. Clone with submodules:
   ```bash
   git clone --recurse-submodules https://github.com/audiohacking/aceforge-vst.git
   ```

2. Build the JUCE plugin:
   ```bash
   cmake -B build -G "Unix Makefiles" -DCMAKE_OSX_ARCHITECTURES=arm64
   cmake --build build --config Release
   ```

3. Build the acestep.cpp inference engine:
   ```bash
   cmake -B vendor/acestep.cpp/build vendor/acestep.cpp
   cmake --build vendor/acestep.cpp/build --config Release -j$(sysctl -n hw.logicalcpu)
   ```

4. Download models (~7.7 GB):
   ```bash
   cd vendor/acestep.cpp && pip install hf && ./models.sh
   ```

5. Place binaries and models where the plugin finds them — see `ml-bridge/README.md`.

6. Copy built `.component` and `.vst3` into your plug-in folders, rescan in DAW.

Full build instructions and CI details: **ml-bridge/README.md** and **ml-bridge/BUILD_AND_CI.md**.

## Repo layout

| Path | Description |
|------|-------------|
| **ml-bridge/plugin/** | JUCE AU + VST3 plugin source (Processor + Editor) |
| **ml-bridge/BUILD_AND_CI.md** | Build steps and CI/release workflow |
| **vendor/acestep.cpp/** | [acestep.cpp](https://github.com/ServeurpersoCom/acestep.cpp) submodule |
| **ROADMAP.md** | Feature roadmap |

## License

Plugin uses JUCE (GPL mode). See repository license.
