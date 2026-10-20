# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(wavemorphfs VERSION 0.2.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(WAVEMORPH_VERSION "" CACHE STRING "Version reported by --version; empty: the project version (CI sets it from the tag or commit)")
option(WAVEMORPH_WERROR "Treat compiler warnings as errors" OFF)
set(WAVEMORPH_SANITIZE "" CACHE STRING "Sanitizers to build with, e.g. address,undefined")

# <format> needs GCC 13 / Clang 17; FUSE_USE_VERSION 312 needs libfuse 3.12;
# AVChannelLayout needs FFmpeg 5.1; 64-bit WavPack configuration needs libwavpack 5
find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3>=3.12)
pkg_check_modules(FLAC REQUIRED IMPORTED_TARGET flac>=1.3)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET libavformat>=59.27 libavcodec>=59.37 libavutil>=57.28 libswscale>=6.7)
pkg_check_modules(WAVPACK REQUIRED IMPORTED_TARGET wavpack>=5.0)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
if(WAVEMORPH_WERROR)
  add_compile_options(-Werror)
endif()
if(WAVEMORPH_SANITIZE)
  add_compile_options(-fsanitize=${WAVEMORPH_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${WAVEMORPH_SANITIZE})
endif()

# not cached itself, so a new project version reaches existing build directories
set(WAVEMORPH_VERSION_STRING "${WAVEMORPH_VERSION}")
if(NOT WAVEMORPH_VERSION_STRING)
  set(WAVEMORPH_VERSION_STRING "${PROJECT_VERSION}")
endif()
configure_file(src/version.hpp.in generated/version.hpp @ONLY)

# everything except main(), shared by the program and the tests
add_library(wmcore STATIC
  src/util.cpp
  src/md5.cpp
  src/charset.cpp
  src/tags.cpp
  src/cue.cpp
  src/flac.cpp
  src/track.cpp
  src/avimage.cpp
  src/encoded.cpp
  src/wavpack.cpp
  src/apetag.cpp
  src/vfile.cpp
  src/retag.cpp
  src/id3.cpp
  src/mp4.cpp
  src/dst.cpp
  src/sacd.cpp
  src/cache.cpp
  src/sidecar.cpp
  src/writeback.cpp
  src/library.cpp
  src/placement.cpp
  src/cover.cpp
  src/fs.cpp
)
target_include_directories(wmcore PUBLIC src ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_compile_definitions(wmcore PUBLIC FUSE_USE_VERSION=312)
target_link_libraries(wmcore PUBLIC PkgConfig::FUSE3 PkgConfig::FLAC PkgConfig::LIBAV PkgConfig::WAVPACK nlohmann_json::nlohmann_json Threads::Threads)

add_executable(wavemorphfs src/main.cpp)
target_link_libraries(wavemorphfs PRIVATE wmcore)
install(TARGETS wavemorphfs RUNTIME DESTINATION bin)

option(WAVEMORPH_TESTS "Build the test program" ON)
if(WAVEMORPH_TESTS)
  enable_testing()
  add_executable(wavemorphfs-tests
    tests/main.cpp
    tests/test_audio.cpp
    tests/test_flac.cpp
    tests/test_filters.cpp
    tests/test_formats.cpp
    tests/test_fs.cpp
    tests/test_library.cpp
    tests/test_sacd.cpp
    tests/test_tagformats.cpp
    tests/test_tags.cpp
    tests/test_wavpack.cpp
  )
  target_link_libraries(wavemorphfs-tests PRIVATE wmcore)
  add_test(NAME unit COMMAND wavemorphfs-tests)
endif()
