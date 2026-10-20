# Building `call_assist`

`call_assist` is a single self-contained Win32 executable written in C. There is
no package manager step, no Python, and no runtime DLL beyond what ships with
Windows — the only optional dependency, `whisper.cpp`, is statically linked and
fetched by CMake on demand.

This document covers every supported build path. For what the program *does* and
how to configure it at runtime, see [README.md](README.md).

---

## 1. What you get

| Configuration | Artifact | Approx. size | Local STT |
| --- | --- | --- | --- |
| `CALLASSIST_WITH_WHISPER=ON` (default) | `call_assist.exe` | ~4.5 MiB | yes, in-process |
| `CALLASSIST_WITH_WHISPER=OFF` | `call_assist.exe` | ~860 KiB | no — remote HTTP only |

Both are standalone: copy the `.exe` anywhere and run it. A Whisper **model
file** is a separate download (§7) and is never compiled in.

---

## 2. Prerequisites

Common to every path:

- **CMake** ≥ 3.16
- A **C11** compiler. If `CALLASSIST_WITH_WHISPER=ON`, a **C++17** compiler too —
  `whisper.cpp` is C++, and the final link is driven as C++.
- A **resource compiler** (`rc.exe` with MSVC, `windres` with MinGW). The build
  calls `enable_language(RC)` for `src/app.rc` (icon + manifest), so this is not
  optional.
- Git, if you let CMake fetch `whisper.cpp` itself.

The target is Windows only. On a non-Windows host without a cross toolchain,
CMake stops immediately:

```
FATAL_ERROR: call_assist targets Windows. Cross-compile with cmake/mingw-w64-x86_64.cmake.
```

`CMAKE_BUILD_TYPE` defaults to `Release` when neither it nor
`CMAKE_CONFIGURATION_TYPES` is set.

---

## 3. Windows, MinGW-w64 + Ninja (recommended)

This is the path that produces the shipping binary, and the only one that gets
the CPU feature flags right by default — build on the machine that will run it
and `ggml` is compiled for that CPU.

```powershell
# MinGW-w64 (e.g. C:\mingw64\bin) and ninja.exe must be on PATH
cmake -S . -B build-full -G Ninja `
      -DCMAKE_BUILD_TYPE=Release `
      -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON
cmake --build build-full -j
# -> build-full\call_assist.exe
```

The three `GGML_*` flags are what make local transcription fast; see §5.

---

## 4. Windows, MSVC

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
# -> build\Release\call_assist.exe
```

Multi-config generator, so the configuration is chosen at build time with
`--config`, not at configure time. MSVC picks up `/utf-8 /W3` and
`_CRT_SECURE_NO_WARNINGS` automatically.

---

## 5. Cross-compile from Linux (MinGW-w64)

```bash
sudo apt install mingw-w64 cmake
scripts/build-mingw.sh                 # whisper linked in
scripts/build-mingw.sh --no-whisper    # ~860 KiB, remote STT only
# -> build-mingw/call_assist.exe
```

The script is a thin wrapper that sets the toolchain file, forces `Release`, and
builds with `-j$(nproc)`. Equivalent long form:

```bash
cmake -S . -B build-mingw \
      -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DCALLASSIST_WITH_WHISPER=ON
cmake --build build-mingw -j"$(nproc)"
```

If your toolchain is not under `/usr/x86_64-w64-mingw32`, point `MINGW_SYSROOT`
at it — as a CMake variable or an environment variable; the toolchain file reads
both:

```bash
MINGW_SYSROOT=/opt/mingw64 scripts/build-mingw.sh
```

> **Speed note.** A cross-compiled `ggml` only gets baseline SSE4.2, because the
> build cannot probe the target CPU. Local transcription is substantially slower
> than a native build. Either build on the target machine (§3) or add
> `-DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON` — but then the binary requires a
> CPU that has those instructions.

---

## 6. Build options

| Option | Default | Meaning |
| --- | --- | --- |
| `CALLASSIST_WITH_WHISPER` | `ON` | Link `whisper.cpp` for local in-process STT. `OFF` drops ~3.6 MiB and leaves remote STT only. |
| `CALLASSIST_WHISPER_DIR` | *(empty)* | Use an existing `whisper.cpp` checkout instead of downloading one. |
| `CALLASSIST_WHISPER_TAG` | `v1.7.4` | Tag to fetch when `CALLASSIST_WHISPER_DIR` is empty. |
| `MINGW_SYSROOT` | `/usr/x86_64-w64-mingw32` | MinGW sysroot location (cross builds only). |
| `GGML_AVX2` / `GGML_FMA` / `GGML_F16C` | off when cross-compiling | CPU features for `ggml`. Big effect on local STT speed. |

### Offline / vendored whisper.cpp

By default CMake fetches `whisper.cpp` with `FetchContent` (shallow clone,
pinned to `CALLASSIST_WHISPER_TAG`). To build without network access, clone it
yourself and pass the path:

```bash
git clone --depth 1 --branch v1.7.4 https://github.com/ggml-org/whisper.cpp.git ../whisper.cpp
cmake -B build -DCALLASSIST_WHISPER_DIR=../whisper.cpp
```

The nested build is configured with `WHISPER_BUILD_TESTS`,
`WHISPER_BUILD_EXAMPLES`, `WHISPER_BUILD_SERVER`, `BUILD_SHARED_LIBS` and
`GGML_OPENMP` all forced `OFF`, so only the static library is produced.

---

## 7. Runtime data: model and prompts

Neither of these is a build input. Nothing downloads a model during the build,
and `models/`, `*.bin` and `.env` are all gitignored.

### Prompts

Every prompt the app sends comes from `.env`, not from the binary. After
building, copy the template next to the executable:

```powershell
copy .env.example build-full\.env
```

It is looked for next to `call_assist.exe` first, then at
`%APPDATA%\CallAssist\.env`. With no `.env` the app still runs, on the neutral
fallbacks in `src/config.c`. The keys and the file format are documented in
`.env.example` and in the README.

### Model

```powershell
.\scripts\get-model.ps1                        # base.en, ~148 MB, fast
.\scripts\get-model.ps1 -Model small           # ~488 MB
.\scripts\get-model.ps1 -Model large-v3-turbo  # ~1.6 GB, best quality
```

```bash
scripts/get-model.sh base.en                   # same, POSIX
```

Both write `models/ggml-<model>.bin` and skip the download if the file already
exists. Point **Settings → Speech-to-text → Model file** at the result.

---

## 8. Tests

`tests/run.sh` builds the portable core — JSON parser, resampler, VAD, WAV
writer, `.env` reader — with the host compiler and exercises it. No Windows
headers, no cross toolchain, runs on Linux or macOS:

```bash
./tests/run.sh          # honours $CC, defaults to cc
```

It stages the sources into a temp directory next to `tests/shim/util.h`, a
Win32-free stand-in, because `#include "util.h"` resolves against the including
file's own directory first. The temp directory is removed on exit.

---

## 9. GPU Whisper server (optional)

`server/` is independent of the Windows build: a CUDA `whisper.cpp` behind a
token-checking nginx gateway, serving the same OpenAI route the app expects.
Requires Docker with the NVIDIA container runtime.

```bash
cd server
./setup.sh                       # writes .env: random token, GPU arch, bind addr
./get-model.sh large-v3-turbo    # ~1.6 GB into server/models/
docker compose up -d --build     # first build ~8-10 min for a single arch
./smoke-test.sh                  # transcribes the bundled jfk.wav
```

`setup.sh` detects the GPU compute capability via `nvidia-smi` and restricts the
CUDA build to that one arch — this is the difference between an ~8 minute and a
~40 minute image build. It is safe to re-run; an existing `.env` is left
untouched.

**`server/.env` holds a live bearer token and is gitignored. Never commit it.**
Anyone holding that token can run transcription on your GPU. Only the gateway
publishes a host port; the whisper container has none, because the whisper.cpp
server has no authentication of its own. See the README for the full variable
reference.

---

## 10. Troubleshooting

**The exe starts and then hangs before the window appears.**
A `whisper.cpp` link drags in `libstdc++`, `libgcc` and `libwinpthread`. Without
static linking the process hangs before reaching `main` on any machine that does
not happen to have MinGW on `PATH`. The build passes `-static` for exactly this
reason — if you have edited the link options, put it back.

**`windres` errors, or the icon and manifest are missing.**
The resource compiler needs the same preprocessor defines as the C sources. The
build appends `-DUNICODE -D_UNICODE` to `CMAKE_RC_FLAGS`; a custom
`CMAKE_RC_FLAGS` that overwrites rather than appends will break `src/app.rc`.

**`FATAL_ERROR: call_assist targets Windows`.**
You are configuring natively on a non-Windows host. Use §5.

**CMake cannot find `whisper.cpp`, or the clone fails.**
No network, or a proxy. Vendor the checkout and use `-DCALLASSIST_WHISPER_DIR=`
(§6).

**Local transcription is far slower than expected.**
A cross-compiled `ggml` without `GGML_AVX2`/`FMA`/`F16C`. See the speed note in
§5. Note also that local Whisper runs on the CPU unless your `whisper.cpp` build
has a GPU backend compiled in — the **Use GPU** checkbox only takes effect if it
does.

**Stale configuration after changing options.**
`CALLASSIST_WHISPER_DIR` and the `GGML_*` flags are cached. Delete the build
directory rather than reconfiguring over it.

---

## 11. Verifying a build

```powershell
.\build-full\call_assist.exe
```

It has no console window — it is a `WIN32` subsystem binary that lives in the
tray, and the admin panel opens by itself on first launch. Two quick checks:

- **Settings → LLM → Test** does a real round trip to the configured endpoint.
- `%APPDATA%\CallAssist\call_assist.log` records startup, the audio engine, and
  each STT/LLM stage. If the overlay never appears, read this first.
