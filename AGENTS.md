# AGENTS.md

Instructions for an AI coding agent setting up `call_assist` for the person who
asked you to. Work through the three steps in order. The details behind each
step are in [BUILD.md](BUILD.md) and [README.md](README.md); read them when
something here is not enough.

`call_assist` is a Windows-only Win32 app written in C. It builds into a single
`call_assist.exe` with no runtime DLLs.

## 1. Clone

```powershell
git clone https://github.com/Im-Sergey-Danilov/AI_teleprompter.git
cd AI_teleprompter
```

Skip this if you are already inside a checkout (there is a `CMakeLists.txt` and
a `src/` folder next to this file).

## 2. Build

### Check what is already installed

You need all of these on `PATH`:

| Tool | Check | Why |
| --- | --- | --- |
| CMake ≥ 3.16 | `cmake --version` | build system |
| Ninja | `ninja --version` | generator used below |
| MinGW-w64 GCC (C11 + C++17) | `gcc --version`, `g++ --version` | compiler; `whisper.cpp` is C++ |
| `windres` | `windres --version` | compiles `src/app.rc` (icon + manifest); required |
| Git | `git --version` | CMake fetches `whisper.cpp` on first configure |

If they are all there, go straight to "Compile".

### Install what is missing

Tell the person what you are about to install before you install it. Prefer
`winget`, which ships with Windows 10/11:

```powershell
winget install --id Kitware.CMake -e
winget install --id Ninja-build.Ninja -e
winget install --id Git.Git -e
winget install --id BrechtSanders.WinLibs.POSIX.UCRT -e   # MinGW-w64: gcc, g++, windres
```

Then open a new shell, or refresh `PATH`, and run the checks again. If
`gcc`/`windres` are still not found, find the WinLibs `mingw64\bin` folder and
add it to `PATH` for the session.

Without `winget`, use MSYS2 (`pacman -S mingw-w64-ucrt-x86_64-toolchain
mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja`) or Visual Studio 2022
with the "Desktop development with C++" workload, and follow the MSVC section of
BUILD.md.

On Linux you can only cross-compile: `sudo apt install mingw-w64 cmake` and
then `scripts/build-mingw.sh`. The result still runs only on Windows.

### Compile

```powershell
cmake -S . -B build-full -G Ninja `
      -DCMAKE_BUILD_TYPE=Release `
      -DGGML_AVX2=ON -DGGML_FMA=ON -DGGML_F16C=ON
cmake --build build-full -j
```

The output is `build-full\call_assist.exe`. The `GGML_*` flags make local
speech recognition fast. Leave them out if the CPU is older than about 2013
(no AVX2), otherwise the exe will crash on start.

If the build fails:

- If the `whisper.cpp` download fails (no network or a proxy), see "Offline /
  vendored whisper.cpp" in BUILD.md, or configure with
  `-DCALLASSIST_WITH_WHISPER=OFF`. That build has remote speech-to-text only.
- If you changed options, delete `build-full` and configure again. Options are
  cached.
- For anything else, see "Troubleshooting" in BUILD.md.

Optionally run the host-side tests from a POSIX shell (Git Bash works):
`./tests/run.sh`.

## 3. Adjust the prompts in `.env` to the person's language

Every prompt the app sends to the model comes from `.env`. The template,
`.env.example`, is in English. The answers should come back in the language the
person speaks on their calls.

1. **Find out the language.** If the person has not said which language, ask
   them. Do not guess from their name, OS locale or keyboard layout.
2. **Create `.env`** from the template, next to the exe:

   ```powershell
   copy .env.example build-full\.env
   ```

3. **Translate the prompt values** in `build-full\.env` into that language:
   `CALLASSIST_SYSTEM_PROMPT`, `CALLASSIST_SNIP_PROMPT`,
   `CALLASSIST_STAGE1_PROMPT`, `CALLASSIST_STAGE2_PROMPT` and
   `CALLASSIST_USER_TEMPLATE`. In the system prompt, also tell the model to
   always answer in that language. Keep the meaning, keep it short, and do not
   add new instructions.

   Hard rules. If you break one of these, the overlay stays empty or shows
   garbage:

   - **Do not translate the markers.** `ANSWER:`, `CONTEXT:` and `BONUS:` stay
     exactly as they are, in English, each at the start of its own line. The app
     splits the reply on them.
   - **Do not translate the placeholders.** `{transcript}`, `{last}` and
     `{time}` in `CALLASSIST_USER_TEMPLATE` stay as they are.
   - **Do not rename the keys.** Translate only the text inside the quotes.
   - Keep each value in double quotes. If a value contains a `"`, write it as
     `\"`.
   - Save the file as UTF-8. A BOM is fine.

4. **Do not commit `.env`.** It is gitignored on purpose; leave `.env.example`
   in English.

### Speech recognition has to match too

Translating the prompts is only half the job. Tell the person:

- The default Whisper language is `ru`. Set it in **Settings →
  Speech-to-text → Language** (or `auto`) on first launch. Do not create
  `%APPDATA%\CallAssist\config.ini` yourself to set it, because the app treats a
  missing `config.ini` as its first run and opens the setup panel.
- `*.en` models (for example the default `base.en`) understand English only. For
  any other language, download a multilingual model:

  ```powershell
  .\scripts\get-model.ps1 -Model small            # ~488 MB
  .\scripts\get-model.ps1 -Model large-v3-turbo   # ~1.6 GB, best quality
  ```

  Then point **Settings → Speech-to-text → Model file** at the file in
  `models\`.

## Finish

Report to the person:

- the path to `call_assist.exe` and to the `.env` you wrote
- anything you installed
- what they still have to do themselves: start the exe, enter the LLM endpoint,
  API key and model in Settings, press **Test**, pick the Whisper language and
  model. "First run" in README.md walks through it.

Never ask for, store or commit their API keys. They type keys into the
Settings panel, which stores them encrypted under `%APPDATA%\CallAssist`.
