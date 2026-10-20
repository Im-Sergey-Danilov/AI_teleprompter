cmake_minimum_required(VERSION 3.16)
project(call_assist C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(NOT WIN32)
  message(FATAL_ERROR "call_assist targets Windows. Cross-compile with cmake/mingw-w64-x86_64.cmake.")
endif()

enable_language(RC)

option(CALLASSIST_WITH_WHISPER "Link whisper.cpp for local (in-process) speech-to-text" ON)
set(CALLASSIST_WHISPER_DIR "" CACHE PATH "Path to an existing whisper.cpp checkout (empty = download)")
set(CALLASSIST_WHISPER_TAG "v1.7.4" CACHE STRING "whisper.cpp git tag to fetch")

add_executable(call_assist WIN32
  src/main.c
  src/config.c
  src/dotenv.c
  src/log.c
  src/util.c
  src/json.c
  src/ringbuf.c
  src/resample.c
  src/vad.c
  src/audio.c
  src/wav.c
  src/http.c
  src/stt.c
  src/stt_local.c
  src/stt_remote.c
  src/llm.c
  src/transcript.c
  src/overlay.c
  src/snip.c
  src/settings_ui.c
  src/app.rc
)

target_include_directories(call_assist PRIVATE src)
target_compile_definitions(call_assist PRIVATE
  UNICODE _UNICODE
  WINVER=0x0A00 _WIN32_WINNT=0x0A00
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  COBJMACROS
)

target_link_libraries(call_assist PRIVATE
  ole32 oleaut32 uuid winhttp comctl32 comdlg32 shell32 user32 gdi32 advapi32 crypt32 shlwapi windowscodecs
)

if(MSVC)
  target_compile_options(call_assist PRIVATE /utf-8 /W3 /D_CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(call_assist PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
  target_link_options(call_assist PRIVATE -municode -mwindows)
  # Linking whisper.cpp drags in libstdc++/libgcc/libwinpthread. Without this
  # the exe starts and then hangs before reaching main on any machine that
  # does not happen to have MinGW on PATH - so keep it genuinely standalone.
  target_link_options(call_assist PRIVATE -static)
  # windres needs the same defines to pick the right resource paths
  set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -DUNICODE -D_UNICODE")
endif()

# ---------------------------------------------------------------- whisper.cpp
if(CALLASSIST_WITH_WHISPER)
  # whisper.cpp is C++; the app itself stays C but must link as C++.
  enable_language(CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  set(WHISPER_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
  set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(WHISPER_BUILD_SERVER   OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS      OFF CACHE BOOL "" FORCE)
  set(GGML_OPENMP            OFF CACHE BOOL "" FORCE)

  if(CALLASSIST_WHISPER_DIR)
    add_subdirectory(${CALLASSIST_WHISPER_DIR} ${CMAKE_BINARY_DIR}/whisper-build EXCLUDE_FROM_ALL)
  else()
    include(FetchContent)
    FetchContent_Declare(whisper
      GIT_REPOSITORY https://github.com/ggml-org/whisper.cpp.git
      GIT_TAG        ${CALLASSIST_WHISPER_TAG}
      GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(whisper)
  endif()

  target_link_libraries(call_assist PRIVATE whisper)
  target_compile_definitions(call_assist PRIVATE CALLASSIST_WITH_WHISPER=1)
  set_target_properties(call_assist PROPERTIES LINKER_LANGUAGE CXX)
endif()

install(TARGETS call_assist RUNTIME DESTINATION .)
