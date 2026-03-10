# Build and CI — acestep-vst

## Local build (macOS)

From the **repo root**:

```bash
cmake -B build -G Xcode -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build build --config Release
```

Locate the built bundles (e.g. `find build -name "*.component" -o -name "*.vst3"`) and copy to:

- `~/Library/Audio/Plug-Ins/Components/` (AU)
- `~/Library/Audio/Plug-Ins/VST3/` (VST3)

Rescan plugins in your DAW. No server required — all inference runs locally.

### Local installer (.pkg)

After building:

```bash
./scripts/build-installer-pkg.sh [--sign-plugins] [--version 0.1.0]
```

- **Output:** `release-artefacts/AcestepVST-macOS-Installer.pkg`
- **Install:** `sudo installer -pkg release-artefacts/AcestepVST-macOS-Installer.pkg -target /`

---

## GitHub Actions — release workflow

- **File:** `.github/workflows/release-plugins.yml`
- **Triggers:** release published or workflow_dispatch
- **Runner:** `macos-latest` (Apple Silicon)
- **Output:** `AcestepVST-macOS-AU-VST3.zip` attached to the GitHub Release
