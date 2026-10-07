cmake_minimum_required(VERSION 3.20)
project(wavemorphfs VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3)
pkg_check_modules(FLAC REQUIRED IMPORTED_TARGET flac)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)
find_package(nlohmann_json 3.10 REQUIRED)
find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

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
target_include_directories(wmcore PUBLIC src)
target_compile_definitions(wmcore PUBLIC FUSE_USE_VERSION=312)
target_link_libraries(wmcore PUBLIC PkgConfig::FUSE3 PkgConfig::FLAC PkgConfig::LIBAV nlohmann_json::nlohmann_json Threads::Threads)

add_executable(wavemorphfs src/main.cpp)
target_link_libraries(wavemorphfs PRIVATE wmcore)
install(TARGETS wavemorphfs RUNTIME DESTINATION bin)

option(WAVEMORPH_TESTS "Build the test program" ON)
if(WAVEMORPH_TESTS)
  enable_testing()
  add_executable(wavemorphfs-tests tests/main.cpp tests/test_flac.cpp tests/test_formats.cpp tests/test_tags.cpp)
  target_link_libraries(wavemorphfs-tests PRIVATE wmcore)
  add_test(NAME unit COMMAND wavemorphfs-tests)
endif()
