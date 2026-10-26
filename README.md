# call_assist

A Windows call assistant written in C (Win32, no framework). It listens to your
microphone **and** your speakers, transcribes with Whisper, sends the rolling
transcript to any OpenAI-compatible chat API, and types the answer into a
click-through overlay that floats above whatever you are doing.

```
WASAPI mic ──┐
             ├─ sum → 16 kHz mono → VAD → utterance
WASAPI loop ─┘                                │
                                              ▼
                        Whisper  (whisper.cpp in-process, or remote HTTP)
                                              │
                                        rolling transcript
                                              │
                     system prompt from .env + transcript
                                              ▼
                     POST /v1/chat/completions  (SSE streamed)
                                              ▼
                          layered always-on-top overlay
```

Everything is one self-contained `call_assist.exe` — no runtime DLLs beyond
what ships with Windows, no Python, no service.

## Build

```powershell
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build            # -> build\call_assist.exe
```

Needs MinGW-w64 and Ninja on `PATH`; `whisper.cpp` is fetched on the first
configure. MSVC, cross-compiling from Linux, the full CMake option matrix,
offline builds, the CUDA server and troubleshooting are all in
**[BUILD.md](BUILD.md)**. Host-side tests: `./tests/run.sh`.

## Prompts

Every prompt the app sends lives in **`.env`**, not in the binary and not in
the admin panel. Copy the template and edit it:

```powershell
copy .env.example .env
```

`.env` is read after the built-in fallbacks and after `config.ini`, so what it
sets wins. It is looked for next to `call_assist.exe` first, then at
`%APPDATA%\CallAssist\.env`.

| Key | Used for |
| --- | --- |
| `CALLASSIST_SYSTEM_PROMPT` | The main system prompt, sent with every request. |
| `CALLASSIST_USER_TEMPLATE` | User-message template. `{transcript}` `{last}` `{time}` are substituted. |
| `CALLASSIST_SNIP_PROMPT` | Sent alongside a screenshot (`Ctrl+Alt+S`). |
| `CALLASSIST_STAGE1_PROMPT` | Two-stage mode, first pass: the quick answer only. |
| `CALLASSIST_STAGE2_PROMPT` | Two-stage mode, second pass: the supporting detail. |

Values may be bare, `"double quoted"` with `\n` `\t` `\\` `\"` escapes, or
`'single quoted'` and literal; quoted values may span lines, which is usually
what you want for a long prompt. Blank lines and whole-line `#` comments are
ignored, a key with an empty value counts as unset, and the last duplicate of a
key wins.

A prompt supplied by `.env` is shown **read-only** in the admin panel and is not
written into `config.ini` — so there is never a second copy to go stale. Delete
the key from `.env` to get the field back.

> Every prompt must keep the `ANSWER:` / `CONTEXT:` / `BONUS:` markers, each at
> the start of its own line. The overlay panes are filled by splitting on them,
> so a missing marker leaves that pane empty.

With no `.env` present the app still runs, on the neutral English fallbacks
compiled into `src/config.c`.

## First run

1. Start `call_assist.exe`. It lives in the tray; the admin panel opens
   automatically on first launch.
2. **Speech-to-text**
   - *Local*: download a model (`scripts\get-model.ps1`, or grab
     `ggml-*.bin` from <https://huggingface.co/ggerganov/whisper.cpp>) and point
     **Model file** at it. `base.en` is the fast default; `large-v3-turbo` is the
     accurate one.
   - *Remote*: any OpenAI-compatible `/v1/audio/transcriptions` endpoint.
3. **LLM and prompt** — endpoint, API key, model. Press **Test** to verify the
   round trip before you are on a call. The prompts themselves come from
   `.env` (see [Prompts](#prompts)), so the panel shows them read-only.
4. **Audio** — pick the devices, then watch the level meter while nobody is
   talking and set the VAD threshold a few dB above the idle level.
5. Press **OK**, then `Ctrl+Alt+1` to start listening. Talk. Answers appear in
   the overlay.

## Hotkeys

| Default | Action |
| --- | --- |
| `Ctrl+Alt+1` | **Start / stop — the same key does both** |
| `Ctrl+Alt+S` | Drag a region of the screen and ask the model about it |
| `Ctrl+Alt+Space` | Answer now, from the transcript so far |
| `Ctrl+Alt+H` | Show / hide the overlay |
| `Ctrl+Alt+X` | Clear the transcript and the overlay |
| `Ctrl+Alt+Up` / `Down` | Scroll the focused overlay pane |
| `Ctrl+Alt+Right` | Move the focus to the next pane |
| `Ctrl+Alt+G` | Move mode: drag / resize the overlay, then press again |

The start/stop trigger can also be bound to a **mouse button** (middle, Mouse 4
or Mouse 5) in **Settings → Triggers**. `RegisterHotKey` cannot see mouse
buttons, so this runs a low-level hook; the bound button is swallowed while it
is bound and does nothing else until you unbind it.

All four are rebindable in **Settings → Triggers**; clear a field to disable one.
If another app already owns a combination, the status bar says so on Apply.

**The app boots idle.** Nothing is captured until you press `Ctrl+Alt+1`; press
it again to stop. Tick *Settings → Triggers → Startup* if you would rather it
begin the moment it launches.

## Push to talk (default)

With **Settings → Triggers → Push to talk** on, the trigger is a record button:
the first press starts capturing, the second stops it, sends the whole take to
Whisper in one request, and asks the model once. Nothing is transcribed or sent
in between, so there is no rolling commentary and no transcript of the parts of
the call you did not ask about.

Turn it off to get the original continuous behaviour: the VAD cuts utterances as
they happen and the assistant answers as you go.

## The overlay

The answer area is four panes, each scrolling on its own:

| Pane | What it holds |
| --- | --- |
| **HEARD** | exactly what was transcribed and sent to the model |
| **ANSWER** | what to say now — kept as short as the model can make it |
| **CONTEXT** | the facts behind it: numbers, names, definitions |
| **COULD ADD** | one genuinely strong point, or a dash if there isn't one |

The model is asked for `ANSWER:` / `CONTEXT:` / `BONUS:` markers and the text is
split on them; if it ignores the format, everything lands in **ANSWER** rather
than disappearing. Reasoning models that stream a `<think>` scratchpad as
ordinary content have it stripped before it reaches the panes.

Hover over a pane and turn the mouse wheel to scroll it, even with
click-through on: a low-level mouse hook catches the wheel over the panes and
hands it to the overlay, so it does not scroll whatever is *behind* it. Clicks
still pass through, and the wheel anywhere else on screen is left alone.

Without the mouse, scroll with `Ctrl+Alt+Up` / `Ctrl+Alt+Down`; `Ctrl+Alt+Right`
moves the focus, and the focused pane is the one with the bright title and
thumb.

Panes hold at their **first** line. Text that arrives while you are reading grows
out of sight below rather than dragging your eyes down with it, so nothing moves
under you mid-sentence — scroll down when you want the rest.

## Moving the overlay

Click-through is what stops the overlay from swallowing your clicks, and it is
also what stops you dragging it. `Ctrl+Alt+G` lends it the mouse for a moment:
drag it anywhere, resize from the bottom-right corner, scroll with the wheel,
then press the same key again. Leaving move mode restores click-through and
writes the new rectangle to `config.ini`, so it comes back where you left it.

Starting a new recording clears the panes immediately, so what is on screen is
always the answer to the question you just asked — the earlier exchanges are
still sent to the model as history, they are simply not shown.

## Two-stage answers

The model is asked twice: first for `ANSWER:` alone, then — once that is on
screen — for `CONTEXT:` and `BONUS:`. The reply you actually have to say appears
in roughly a third of the time it would take to generate all three sections,
and the details fill in underneath while you are already talking. The second
request carries the first answer as history, so it elaborates rather than
repeats.

Turn it off with *Settings → LLM → Two-stage* to get everything in one request.
The two instructions are `CALLASSIST_STAGE1_PROMPT` and
`CALLASSIST_STAGE2_PROMPT` in `.env`.

## Screenshot questions

`Ctrl+Alt+S` dims the screen, you drag a rectangle, and that region is sent to
the model as a PNG alongside `CALLASSIST_SNIP_PROMPT`. The overlay is
hidden for the capture, so the assistant never ends up reading its own previous
answer back to itself, and the selection chrome takes itself down before the
pixels are read.

The shipped prompt is aimed at what usually ends up on screen — a task, a
question, or a piece of code — and asks for the answer itself rather than a
description of the picture. The fallbacks compiled in are English; set
`CALLASSIST_SNIP_PROMPT` in `.env` for another language.

## Reading the overlay

The status row carries a coloured dot so you can tell at a glance whether the
assistant is actually hearing anything:

| Dot | Meaning |
| --- | --- |
| 🟢 green, breathing | Listening |
| 🟠 amber | Transcribing (Whisper is working through the queue) |
| 🔵 accent | Thinking (waiting on the LLM) |
| ⚪ grey ring | Paused — press `Ctrl+Alt+1` |

The dot pulses once a second while active, so a frozen overlay looks different
from a live one. Pressing the listen hotkey also un-hides the overlay, so you
always get visual confirmation that it started.

## Admin panel reference

**Speech-to-text** — engine (local/remote), model file, thread count, GPU toggle,
remote URL/key/model, spoken language, translate-to-English. **Test** sends one
second of tone through whichever engine is selected and reports the round trip,
so endpoint and token can be verified before a call rather than during one.

The language defaults to **`ru`**, on both the app and the server. Pinning a
language is more accurate than `auto` when you know what will be spoken — but it
is a hard constraint, not a hint: Whisper will render *any* speech into that
language. Asked for `ru`, an English sentence comes back as
`"И так, мои дорогие американцы..."` rather than English. Switch the combo to
`auto` for mixed-language calls.

**LLM and prompt** — endpoint, key, model, temperature, max tokens, how many
seconds of transcript to send, SSE streaming on/off, and read-only views of the
system prompt and user-message template as `.env` supplies them. The template
understands three placeholders:

| Placeholder | Expands to |
| --- | --- |
| `{transcript}` | The last *N* seconds of transcript, one timestamped line per utterance |
| `{last}` | Just the most recent utterance |
| `{time}` | Local wall-clock time |

**Audio** — which sources to capture, which endpoint each uses, per-source gain,
a live input meter, and the four VAD parameters (threshold, end-of-speech
silence, minimum speech, maximum segment length).

The threshold is a *floor*, not the whole story: the VAD tracks the background
level continuously and puts the real gate a fixed margin above it, so a call
with a constant bed of noise still gets segmented. Set the floor a few dB above
the idle level and the adaptive part handles the rest.

**Chunk continuous speech after** is the latency dial. When the speaker pauses,
an utterance closes after *end-of-speech silence* and you get the transcript
about that long afterwards. When nobody pauses, this is what bounds the wait
instead - the default 2500 ms puts a line in the overlay every ~2 s. Whisper
itself costs ~0.2 s, so this setting is essentially the whole delay. Lowering it
buys speed at the cost of context: each chunk is transcribed on its own, so
shorter pieces get less accurate punctuation and more words split across cuts.

**Triggers** — push-to-talk on/off, automatic answering with a minimum length, a
cooldown and an optional "only if it looks like a question" filter, the five
hotkeys, and the optional mouse-button binding.

Auto-answering waits for the previous answer to finish before starting another,
because a new request cancels the one in flight and utterances arrive faster
than the model can talk. `Ctrl+Alt+Space` overrides that and answers now.

**Overlay** — opacity, font size, colours, click-through, whether to show the
recent transcript strip, exclusion from screen capture, and the window rectangle.

Settings live in `%APPDATA%\CallAssist\config.ini`; the log is
`call_assist.log` in the same folder (**tray → Open log folder**). Both API keys
are encrypted at rest with DPAPI, so the ini is only readable by your Windows
account.

## How the audio path works

Both sources are captured through WASAPI in shared mode — the microphone as a
normal capture stream, the speakers via a loopback stream on the render
endpoint. Each is downmixed to mono, resampled to 16 kHz (box-filter decimation
plus linear interpolation) and pushed into its own ring buffer. A mixer thread
paced by the wall clock reads both, sums them with the configured gains, and
feeds 20 ms frames to an energy VAD. A source that is merely late (WASAPI hands
over ~10 ms packets while the mixer wakes every 5 ms) holds the mixer back for
up to 120 ms; only one that has genuinely gone quiet is zero-filled, which is
the normal state of a loopback stream since it delivers nothing at all while
the system is silent. Zero-filling a late source instead would punch
digital-silence holes into the middle of speech.

The VAD gate is adaptive: the background level is tracked continuously and the
gate sits `VAD_MARGIN_DB` above it, with the configured threshold as an absolute
floor and 4 dB of hysteresis on the way out. A fixed gate cannot segment a live
call — anything with a continuous bed stays above it forever, the utterance
never closes, and the only thing that ever fires is the max-segment cap. When
the VAD sees enough trailing silence it emits the utterance — with ~320 ms of
pre-roll so word onsets survive — and that buffer goes to the STT queue. Speech
that simply never stops is cut at the quietest frame once it passes
`VAD_SOFT_CAP_MS`, so a monologue streams out in pieces instead of arriving as
one 20-second block.

**Both sources are summed into a single stream**, so the transcript has no
speaker labels and the model cannot tell your voice from theirs. If it starts
answering your own sentences, drop **mic gain** toward `0`, or turn on
*Triggers → only when it looks like a question*.

## GPU Whisper server (`server/`)

Rather than running Whisper on the laptop's CPU, point the app's **Remote**
engine at a box with a GPU. `server/` builds whisper.cpp with CUDA, runs it
behind a token-checking nginx gateway, and serves the exact OpenAI route the app
expects.

```
Windows app ──POST /v1/audio/transcriptions──> nginx (Bearer token)
                                                  │
                                                  ▼
                                        whisper-server, CUDA
```

```bash
cd server
./setup.sh                       # writes .env: random token, GPU arch, bind addr
./get-model.sh large-v3-turbo    # ~1.6 GB into server/models/
docker compose up -d --build     # first build takes ~10 min
./smoke-test.sh                  # transcribes the bundled jfk.wav
```

`setup.sh` prints the endpoint URL and API key. Put them in the Windows app
under **Settings → Speech-to-text → Remote endpoint**:

| Field | Value |
| --- | --- |
| Engine | Remote |
| URL | `http://<server>:8910/v1/audio/transcriptions` |
| API key | the token from `server/.env` |
| Model | anything (whisper-server ignores it; the model is chosen server-side) |

Everything is driven by `server/.env`:

| Variable | Default | Meaning |
| --- | --- | --- |
| `API_TOKEN` | *(random)* | Bearer token the gateway requires |
| `WHISPER_BIND` | first non-docker IP | Host interface to publish on. `127.0.0.1` = local only; `0.0.0.0` exposes it on **every** interface |
| `WHISPER_PORT` | `8910` | Published port |
| `WHISPER_MODEL` | `ggml-large-v3-turbo.bin` | File in `server/models/` |
| `WHISPER_LANG` | `ru` | Language when a request does not specify one (`auto` = detect) |
| `WHISPER_THREADS` | `4` | CPU threads for the non-GPU parts |
| `CUDA_ARCH` | detected | Compute capability (89 = RTX 40xx, 86 = 30xx, 75 = 20xx) |

**Only the gateway is published — the whisper container has no host ports.**
whisper.cpp's server has no authentication of its own, which is exactly why the
gateway is in front of it. Keep `WHISPER_BIND` on a LAN or VPN address unless
you genuinely intend to publish it, and treat `API_TOKEN` as a secret: anyone
holding it can run transcription on your GPU.

Useful commands:

```bash
docker compose logs -f whisper     # model load, per-request timings
docker compose restart whisper     # after changing the model in .env
nvidia-smi                         # confirm VRAM is actually in use
```

### Measured on an RTX 4060 Ti

`large-v3-turbo`, CUDA, flash attention on:

| | |
| --- | --- |
| VRAM held | 1.95 GB (model 1.62 GB) |
| 11 s of speech | **~220 ms** end to end, including HTTP and the gateway |
| Effective speed | ~50x realtime |

That is far below the VAD's own end-of-utterance delay, so transcription is not
the bottleneck in the pipeline — the segmenter is. With the defaults, a pause
costs 400 ms of silence hangover and uninterrupted speech is cut every 2500 ms;
both are in **Settings → Audio**.

## Known limitations

- No speaker diarisation, by design (the two streams are mixed before STT).
- Energy VAD only. Tracking the noise floor makes it usable on a live call, but
  it is not Silero: it segments on level alone, so a loud continuous source is
  chopped at its quietest moments rather than at real sentence boundaries.
- Local Whisper runs on the CPU unless your whisper.cpp build has a GPU backend
  compiled in; the **Use GPU** checkbox only takes effect if it does.
- The overlay renders plain text. Markdown from the model is shown verbatim.

## License

MIT - free to use, change and share, commercially or not. See [LICENSE](LICENSE).
