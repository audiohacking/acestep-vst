# Build and CI — acestep-vst

## Quick start with pre-built binaries

Download the plugin and bundled acestep.cpp binaries from the
[Releases page](https://github.com/audiohacking/acestep-vst/releases):

| Platform | Artifact | Plugin formats |
|----------|----------|----------------|
| macOS (Apple Silicon) | `AcestepVST-macOS-AU-VST3.zip` or `AcestepVST-macOS-Installer.pkg` | AU + VST3 |
| Linux x86-64 | `AcestepVST-Linux-VST3.tar.gz` | VST3 |
| Windows x86-64 | `AcestepVST-Windows-VST3.zip` | VST3 |

Each bundle includes `ace-lm` and `ace-synth` (the inference engines) inside the plugin
bundle — no separate binary installation required.  Just install the plugin and point the
**Settings → Models directory** at your downloaded models.

---

## Building from source

### macOS

```bash
cmake -B build -G Xcode -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build --config Release
```

Locate the built bundles and copy to:

- `~/Library/Audio/Plug-Ins/Components/` (AU)
- `~/Library/Audio/Plug-Ins/VST3/` (VST3)

### Linux

```bash
# Install JUCE system dependencies (Ubuntu/Debian)
sudo apt-get install -y \
  libx11-dev libxcomposite-dev libxcursor-dev libxext-dev \
  libxinerama-dev libxrandr-dev libxrender-dev \
  libfreetype6-dev libfontconfig1-dev libasound2-dev libgl1-mesa-dev

cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Copy `build/.../.vst3` to `~/.vst3/` and rescan in your DAW.

### Windows

```bat
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Copy the `.vst3` bundle to `%COMMONPROGRAMFILES%\VST3\` and rescan in your DAW.

---

## Providing the acestep.cpp inference engine

### Option A — Use the bundled pre-built release (recommended)

Plugin release archives already include `ace-lm` and `ace-synth` inside the bundle
(`Contents/MacOS/`, `Contents/x86_64-linux/`, or `Contents/x86_64-win/`).
No extra steps required.

The engine binaries come from:
[audiohacking/acestep.cpp releases](https://github.com/audiohacking/acestep.cpp/releases)
(currently pinned to `v0.0.3` in CI).

### Option B — Build from source (optional)

```bash
# macOS / Linux
cmake -B vendor/acestep.cpp/build vendor/acestep.cpp
cmake --build vendor/acestep.cpp/build --config Release -j$(nproc)

# Windows
cmake -S vendor/acestep.cpp -B vendor/acestep.cpp/build
cmake --build vendor/acestep.cpp/build --config Release
```

Then set **Settings → Binaries directory** to `vendor/acestep.cpp/build/`.

---

## Downloading models (~7.7 GB)

```bash
cd vendor/acestep.cpp && ./models.sh
```

Then set **Settings → Models directory** in the plugin.

---

## macOS local installer (.pkg)

```bash
./scripts/build-installer-pkg.sh [--sign-plugins] [--version 0.1.0]
```

**Output:** `release-artefacts/AcestepVST-macOS-Installer.pkg`

---

## GitHub Actions — release workflow

**File:** `.github/workflows/release-plugins.yml`
**Triggers:** release published or `workflow_dispatch`

| Job | Runner | Output |
|-----|--------|--------|
| `build-macos` | `macos-latest` | `AcestepVST-macOS-AU-VST3.zip`, `AcestepVST-macOS-Installer.pkg` |
| `build-linux` | `ubuntu-22.04` | `AcestepVST-Linux-VST3.tar.gz` |
| `build-windows` | `windows-latest` | `AcestepVST-Windows-VST3.zip` |

Each job downloads the corresponding platform binary archive from
`audiohacking/acestep.cpp` at the version set in `env.ACESTEP_RELEASE`
and embeds `ace-lm`/`ace-synth` (plus runtime libraries) inside the
plugin bundle before packaging.

To update the bundled engine version, change `ACESTEP_RELEASE` at the
top of `.github/workflows/release-plugins.yml`.
