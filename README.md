# AceForge Bridge — AU/VST3 Plugin

Experimental **AU** and **VST3** plugin for **macOS (Apple Silicon)** that runs **local AI music generation** inside your DAW using [acestep.cpp](https://github.com/ServeurpersoCom/acestep.cpp) — no external server required.

- **Target:** macOS (Apple Silicon); build via CMake + JUCE.
- **Engine:** `ace-qwen3` + `dit-vae` from `acestep.cpp` (submodule at `vendor/acestep.cpp`).

## What it does

- **Generate** — Enter a prompt, set duration and quality, click Generate. The plugin runs the local inference engine and plays the result once through the plugin output.
- **Library** — Successful generations are saved as WAVs; the plugin lists them (newest first). You can **Insert into DAW** (opens the file in Logic Pro) or **Reveal in Finder** and drag the file into your DAW timeline.

Full description, build details, and installer steps: **ml-bridge/README.md**.

<img width="600" alt="Screenshot 2026-02-06 at 16 42 12" src="https://github.com/user-attachments/assets/1efbd3b3-4112-4aca-ad15-2221fd99f4d7" />


## Quick start

1. **Clone with submodules** (or run `git submodule update --init` after a plain clone):
   ```bash
   git clone --recurse-submodules https://github.com/audiohacking/aceforge-vst.git
   ```

2. **Build the JUCE plugin** (from repo root):
   ```bash
   cmake -B build -G "Unix Makefiles" -DCMAKE_OSX_ARCHITECTURES=arm64
   cmake --build build --config Release
   ```
   Or use the Xcode generator: `-G Xcode` instead of `-G "Unix Makefiles"`.

3. **Build the acestep.cpp inference engine** (macOS / Apple Silicon):
   ```bash
   cmake -B vendor/acestep.cpp/build vendor/acestep.cpp
   cmake --build vendor/acestep.cpp/build --config Release -j$(sysctl -n hw.logicalcpu)
   ```
   See [`vendor/acestep.cpp/README.md`](vendor/acestep.cpp/README.md) for Linux (CUDA/ROCm/Vulkan) instructions.

4. **Download models** (~7.7 GB):
   ```bash
   cd vendor/acestep.cpp && pip install hf && ./models.sh
   ```

5. **Install** — Copy the built `.component` and `.vst3` from the build tree into your plug-in folders, or build the installer:
   ```bash
   ./scripts/build-installer-pkg.sh --sign-plugins --version 0.1.0
   ```
   Then install `release-artefacts/AceForgeBridge-macOS-Installer.pkg` (or open it in Finder).

6. **Place binaries and models** where the plugin can find them (see `ml-bridge/README.md` for the exact paths), then rescan plugins in your DAW.

## Repo layout

| Path | Description |
|------|-------------|
| **ml-bridge/** | Plugin source and docs |
| **ml-bridge/plugin/** | JUCE AU + VST3 target (CMake) |
| **ml-bridge/BUILD_AND_CI.md** | Build and GitHub Actions release |
| **vendor/acestep.cpp/** | [acestep.cpp](https://github.com/ServeurpersoCom/acestep.cpp) submodule — local AI inference engine |
| **ROADMAP.md** | Feature roadmap for local AI integration |
| **.github/workflows/release-plugins.yml** | Build and release on GitHub Release |

## Releases

Creating a **GitHub Release** (e.g. tag `v0.1.0`) triggers the workflow and attaches **AceForgeBridge-macOS-AU-VST3.zip** to that release.

## License

See repository license. Plugin uses JUCE (GPL mode).
