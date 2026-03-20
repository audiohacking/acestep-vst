# acestep-vst — root CMake
cmake_minimum_required(VERSION 3.22)
project(AcestepVST VERSION 0.1.0)

# Objective-C++ is only needed on macOS (AU format)
if(APPLE)
  enable_language(OBJCXX)
endif()

add_subdirectory(ml-bridge/plugin)

# ── acestep.cpp ───────────────────────────────────────────────────────────────
# Pre-built binaries are available at:
#   https://github.com/audiohacking/acestep.cpp/releases
#
# To build from source (optional):
#   macOS:  cmake -B vendor/acestep.cpp/build vendor/acestep.cpp
#           cmake --build vendor/acestep.cpp/build --config Release -j$(sysctl -n hw.logicalcpu)
#   Linux:  cmake -B vendor/acestep.cpp/build vendor/acestep.cpp
#           cmake --build vendor/acestep.cpp/build --config Release -j$(nproc)
#   Windows: cmake -S vendor/acestep.cpp -B vendor/acestep.cpp/build
#            cmake --build vendor/acestep.cpp/build --config Release
# ─────────────────────────────────────────────────────────────────────────────
