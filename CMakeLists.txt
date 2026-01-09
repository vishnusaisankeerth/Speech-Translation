cmake_minimum_required(VERSION 3.16)
project(camb_speech_translate LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Keep third party builds minimal
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

# Some toolchains need this for static libs that use pthread
set(THREADS_PREFER_PTHREAD_FLAG ON)

add_subdirectory(third_party/whisper.cpp EXCLUDE_FROM_ALL)
add_subdirectory(third_party/bergamot-translator EXCLUDE_FROM_ALL)

add_executable(speech_translate
  src/main.cpp
)

target_include_directories(speech_translate PRIVATE
  ${CMAKE_SOURCE_DIR}/third_party/whisper.cpp
  ${CMAKE_SOURCE_DIR}/third_party/bergamot-translator/src
)

# Link Whisper target with fallbacks
if(TARGET whisper)
  target_link_libraries(speech_translate PRIVATE whisper)
elseif(TARGET libwhisper)
  target_link_libraries(speech_translate PRIVATE libwhisper)
elseif(TARGET whisper-lib)
  target_link_libraries(speech_translate PRIVATE whisper-lib)
else()
  message(FATAL_ERROR "whisper target not found (expected whisper/libwhisper/whisper-lib).")
endif()

# Link Bergamot target with fallbacks
set(_BERGAMOT_CANDIDATES
  bergamot-translator
  bergamot_translator
  bergamot
  bergamot-lib
  bergamot_translator_lib
  bergamot-translator-static
  bergamot_translator_static
)

set(_BERGAMOT_TARGET "")
foreach(t IN LISTS _BERGAMOT_CANDIDATES)
  if(TARGET ${t})
    set(_BERGAMOT_TARGET ${t})
    break()
  endif()
endforeach()

if(_BERGAMOT_TARGET STREQUAL "")
  get_property(_allTargets DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/bergamot-translator PROPERTY BUILDSYSTEM_TARGETS)
  message(STATUS "bergamot targets:")
  foreach(tt IN LISTS _allTargets)
    message(STATUS "  - ${tt}")
  endforeach()
  message(FATAL_ERROR "bergamot target not found. see list above.")
else()
  message(STATUS "Linking bergamot target: ${_BERGAMOT_TARGET}")
  target_link_libraries(speech_translate PRIVATE ${_BERGAMOT_TARGET})
endif()

# Sndfile
find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
target_link_libraries(speech_translate PRIVATE PkgConfig::SNDFILE)

# Threads
find_package(Threads REQUIRED)
target_link_libraries(speech_translate PRIVATE Threads::Threads)

# On Linux sometimes dl is needed depending on how third party is built
if(UNIX AND NOT APPLE)
  target_link_libraries(speech_translate PRIVATE dl)
endif()
