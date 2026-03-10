# ACE-Step VST — root CMake
cmake_minimum_required(VERSION 3.22)
project(AcestepVST VERSION 0.1.0)

# Enable languages needed by the project (on macOS)
if(APPLE)
  enable_language(OBJCXX)
endif()

add_subdirectory(ml-bridge/plugin)

# ── acestep.cpp ───────────────────────────────────────────────────────────────
# acestep.cpp lives in vendor/acestep.cpp as a git submodule.
# It is built separately from the JUCE plugin (its own cmake + binary outputs).
# Run `git submodule update --init` after cloning to populate the submodule.
#
# macOS / Apple Silicon (Metal, default):
#   cmake -B vendor/acestep.cpp/build vendor/acestep.cpp
#   cmake --build vendor/acestep.cpp/build --config Release -j$(sysctl -n hw.logicalcpu)
#
# Linux + CUDA:
#   cmake -B vendor/acestep.cpp/build vendor/acestep.cpp -DGGML_CUDA=ON
#   cmake --build vendor/acestep.cpp/build --config Release -j$(nproc)
# ─────────────────────────────────────────────────────────────────────────────
