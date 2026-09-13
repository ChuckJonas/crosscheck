# crosscheck — Lichess client for CrossPoint. Requirements & Build Spec

*Repo: https://github.com/ChuckJonas/crosscheck (fork of `crosspoint-reader/crosspoint-reader`, based on stable `master`). Target: Xteink X4 Pro (ESP32-S3, 8 MB PSRAM, 480×800 16-gray e-ink, GT911 touch). Drafted 2026-09-11. Code references below were read on the `feat-sd-plugins` branch @ 4ebb101; the activity, Wi-Fi, HTTP and touch APIs cited are the same on `master`. Device menu label: "Chess".*

---

## 1. Goals and non-goals

**Goal:** add a "Chess" activity to CrossPoint that plays live games against humans on Lichess via the Board API, on the same device that remains a daily e-reader.

**Hard constraints**

- The reader must stay fully usable. Chess is one extra menu entry; nothing in the reader path changes.
- Wi-Fi and the TLS socket exist only while the Chess activity is on screen. Outside it, the device sleeps exactly as stock CrossPoint does.
- Recoverable at all times: the stock release `.bin` reflashes over anything we build.

**Non-goals (v1)**

- Bullet time controls (1+0, 2+1). Everything from 3+0 up is a target. Note the handicap: device round-trip (touch → Wi-Fi → Lichess → refresh) is ~0.5 s per move, which comes off your clock. At 3+0 that's ~20 s over a 40-move game; at 1+0 it's a third of your time. Increments absorb it entirely.
- Playing the on-device engine (no Stockfish). Puzzles are a stretch goal, computer opponents come from Lichess's side.
- Chat, tournaments, variants other than standard. Analysis board.
- Contributing upstream. CrossPoint's roadmap explicitly puts interactive apps out of scope; this lives as a fork, rebased on upstream periodically.
- Running as an SD plugin. Not possible: `plugin.js` executes in the phone's browser and `device.json` only renders catalog/browse/download screens. An on-device activity must be compiled into the firmware.

---

## 2. Phase 0 — Prove the toolchain (do this first)

Nothing else matters until a self-built binary runs on the device and the stock one can be restored.

**0.1 Repo setup**

- `ChuckJonas/crosscheck` is a GitHub fork of `crosspoint-reader/crosspoint-reader` (not a clone-and-push), so upstream is a remote and `gh repo sync` / `git fetch upstream && git rebase upstream/master` keep it current.
- Base branch: **`master`** (stable). `feat-sd-plugins` can be merged later if SD Plugins are wanted; it is not required for chess.
- Clone **with submodules** (`freeink-sdk` is a submodule): `git clone --recursive git@github.com:ChuckJonas/crosscheck.git`. If already cloned: `git submodule update --init --recursive`.
- Sync policy: rebase on `upstream/master` at the start of each milestone. Because all new code lives in `src/activities/chess/` and `lib/Chess/` (N3), conflicts should be limited to the menu-registration hunk and the translations table.
- Put this document at `docs/crosscheck-spec.md` in the repo.

**0.2 Environment**

- Install PlatformIO (`pip install platformio` or the VS Code extension).
- Read `docs/contributing/getting-started.md` and `development-workflow.md`. Note the build uses `platformio.local.ini` (gitignored) for personal env overrides such as `upload_port`.

**0.3 Safety net**

- Back up the entire SD card to disk.
- Download the current official X4 Pro release `.bin` from GitHub Releases and keep it next to the repo. Verify you can reflash it via `esptool.py --chip esp32s3 --port <port> --baud 921600 write_flash 0x10000 <bin>` or the web flasher at crosspointreader.com/#flash-tools.
- Confirm the unit is USB-unlocked: it will appear in the web flasher's serial picker. (Official-store units are guaranteed unlocked.)

**0.4 Build and flash unmodified source**

```
pio run -e x4pro            # must compile clean
pio run -e x4pro -t upload  # flashes over USB Serial/JTAG
pio device monitor          # serial log; build has ENABLE_SERIAL_LOG + CROSSPOINT_WAIT_FOR_USB_SERIAL
```

Exit criteria: device boots the self-built firmware, opens a book, and the serial monitor shows logs. Then reflash the official `.bin` and confirm it also boots. Only now start Phase 1.

**0.5 Version tag**

In `platformio.local.ini`, extend `x4pro` with `-DCROSSPOINT_VERSION=\"${crosspoint.version}-crosscheck\"` so Settings → About shows which build is running.

---

## 3. Architecture

### 3.1 Where it plugs in

| Concern | Existing CrossPoint piece to reuse | Notes |
|---|---|---|
| Screen lifecycle | `src/activities/Activity.h` (`onEnter`, `onExit`, `loop`, `preventAutoSleep`, `skipLoopDelay`) | One new `ChessActivity`, plus sub-screens as subactivities (`startActivityForResult`) |
| UI/touch | FreeInkUI via `UiAppHost` (`docs/contributing/touch-and-ui.md`) for menus/dialogs; a hand-drawn board for the game surface | The doc mandates FUI for lists/dialogs. The board itself is custom drawing, like the reader page, and consumes touch through the same snapshot the host provides. Back/Home/Menu edge swipes are global — do not reimplement |
| Wi-Fi bring-up | `WifiSelectionActivity` launched as a subactivity, exactly as `KOReaderSyncActivity::onEnter` does | Handles saved networks, password entry, AP list |
| Wi-Fi teardown | `WiFi.disconnect(false)` + `esp_wifi_stop()` in `onExit`, as `KOReaderSyncActivity` does | KOSync additionally does a silent reboot on exit to reclaim heap fragmented by TLS; evaluate whether we need this (see §6) |
| HTTPS | `freeink::SecureHttpClient` (kept-alive TLS session) and `HttpDownloader` (`fetchUrl` with a streaming `DataCallback`) | TLS is unverified on this platform; fine for Lichess but be aware |
| JSON | ArduinoJson (already a dep) with filters | Lichess game-state events are small; NDJSON lines parse individually |
| Persistence | `/.crosspoint/chess.json` on SD (token, preferences, last game id) | Same pattern as `KOReaderSettings` / plugin token files |
| Display | `HalDisplay::displayBuffer(FAST_REFRESH)` for move updates; `FULL_REFRESH` periodically to clear ghosting; `displayBufferAsync` where supported | X4 Pro panel supports fast partial refresh ~200–300 ms |
| Menu entry | Add to the settings/home menu where `KOReaderSync` / `PluginCatalog` are registered; string via the translations table | Follow `tr(STR_*)` conventions or strings will fail the i18n lint |

### 3.2 Module layout (proposed)

```
src/activities/chess/
  ChessActivity.{h,cpp}        # top-level: lobby, Wi-Fi gate, state machine
  ChessBoardView.{h,cpp}       # board rendering + touch-to-square mapping
  ChessGameScreen.{h,cpp}      # in-game: board, clocks, action bar
lib/Chess/
  Position.{h,cpp}             # board state, FEN/UCI, legal move generation
  Lichess.{h,cpp}              # Board API client (REST + NDJSON stream)
  ChessSettings.{h,cpp}        # token/prefs persistence
test/                          # native unit tests for Position + NDJSON parsing
```

Keep `lib/Chess/` free of Arduino/display includes so it compiles and tests natively (`pio test -e native` per `testing-debugging.md`).

---

## 4. Functional requirements

### 4.1 Setup (one-time)

- **R1** User enters a Lichess personal API token. Entry path: web File Manager card **or** on-device keyboard (`KeyboardEntryActivity`). Recommended: a tiny `plugin.js` card in the SD-plugin system (if on `feat-sd-plugins`) that writes `/.crosspoint/chess.json` — it's 20 lines and avoids typing a 20-char token on e-ink. Fallback: on-device keyboard.
- **R2** Token scopes required: `board:play` (play), `challenge:write` (seek/accept). Document this on the setup screen.
- **R3** On first entry, validate with `GET /api/account`; display username.

### 4.2 Lobby

The lobby mimics the Lichess mobile app's home screen: in-progress games on top, then a grid of time-control cards; tapping a card seeks immediately.

- **R4 In-progress games (top section).** From `GET /api/account/playing` on lobby entry (and on every return from a game). One row per game: opponent name + rating, time control, colour, and a **"Your move"** badge when `isMyTurn`. Tap → resume (opens the game stream). Section is hidden when empty. Correspondence and live games both appear; live games in progress get a clock icon.
- **R5 Time-control cards.** A 3-column grid of tap targets, each showing the control (e.g. "3+0") and its category label (Blitz / Rapid / Classical). Default set, in order: `3+0`, `3+2`, `5+0`, `5+3`, `10+0`, `10+5`, `15+10`, `30+0`, `Custom`. Bullet cards (`1+0`, `2+1`) are omitted in v1 (see non-goals); add later behind a setting if wanted. Card layout: 3 rows × 3, ~150 px square, ratings for the matching category shown small under each card.
- **R6 Tap = seek.** Tapping a card fires `POST /api/board/seek` with `time`, `increment`, `rated` (from a lobby toggle, default rated), `color=random`. A "Searching…" screen with a Cancel button replaces the lobby; the seek request stays open until matched (see §5.3). On match, go straight into the game screen.
- **R6a Custom card** opens a small FUI dialog: minutes stepper, increment stepper, rated toggle. Last custom values persisted.
- **R6b Header bar:** username and a *Challenge a friend* button (username entry → `POST /api/challenge/{username}` with the currently selected time control).

### 4.3 Game

- **R7** Board rendered full-width (480 px → 60 px squares), flipped for black, coordinates on edges, last move highlighted, check indicator.
- **R8** Touch input: tap piece → legal destination squares shown → tap destination. Tap same piece to cancel. Promotion → 4-option popup (FUI `optionDialog`).
- **R9** Client-side legality check (own move generator) before sending, so illegal taps never hit the network. Server remains authoritative; on 400 from Lichess, revert and re-sync from stream.
- **R10** Clocks: display both, updated locally from the last stream event (`wtime`/`btime` + elapsed), redrawn at 1 Hz using fast refresh of the clock region only. Below 20 s, refresh every tick; otherwise every 10 s to save the panel.
- **R11** Actions via a bottom bar / menu gesture: Resign (confirm), Offer/Accept draw, Abort (first moves), Flip board, Back to lobby (game stays active on Lichess).
- **R12** Game end: show result (mate/resign/timeout/draw), rating change if present, buttons *Rematch* / *New seek* / *Lobby*.

### 4.3a Opponent-move notification ("your turn")

The device has no speaker or vibration motor. The primary attention channel is the **frontlight** (`HalFrontlight`, already in the HAL). Panel refresh is *not* used as a notification: a full refresh costs ~1–2 s of blank/flashing screen, which is dead time on your clock, so refresh policy is decided by image quality alone (§6).

- **R12a Frontlight pulse (primary cue, all time controls).** On each `gameState` event that contains a new opponent move: if the light is off or dim, raise it to a set level for ~300 ms and restore; if already bright, dip it briefly instead. Configurable: pulse length, off. Never pulse while the user's own move is pending or during the user's tap sequence.
- **R12b Your-turn chrome.** The active side's clock is drawn inverted (black band, white digits), and a thin border is drawn around the board when it is your move. Both update with the same fast partial refresh that draws the opponent's move (~200–300 ms), so the state is legible immediately.
- **R12c Board update.** The opponent's move is drawn with a **fast partial refresh of the board region only**; the last-move squares get a border highlight. Full refreshes happen only on the ghost-clearing schedule (every N moves, on game start, on game end) — the same at every time control. Optional setting for correspondence/classical: *full refresh on every opponent move* for people who prefer a crisp board and don't care about the 1–2 s.
- **R12d Away notification (stretch).** If the user has left the game screen for the lobby while a live game continues, a frontlight pulse still fires on opponent move, and the in-progress row shows the "Your move" badge.

### 4.4 Correspondence mode (v1.1)

- **R13** Optional: on `sleep.enter`, if a correspondence game is pending, keep no radio — instead show the position on the sleep screen as the "wallpaper" (write to `/sleep.bmp`). Zero power cost, nice touch.

---

## 5. Lichess Board API — what we use

Base: `https://lichess.org`. Auth header `Authorization: Bearer <token>` on every call.

| Purpose | Endpoint | Shape |
|---|---|---|
| Validate token | `GET /api/account` | JSON |
| Ongoing games | `GET /api/account/playing` | JSON `{nowPlaying:[…]}` |
| Event stream | `GET /api/stream/event` | NDJSON: `gameStart`, `gameFinish`, `challenge`, `challengeDeclined` |
| Game stream | `GET /api/board/game/stream/{gameId}` | NDJSON: first line `gameFull` (players, clock, initial state), then `gameState` (`moves` as UCI list, `wtime`, `btime`, `status`), `chatLine`, `opponentGone` |
| Make move | `POST /api/board/game/{gameId}/move/{uci}` | `{ok:true}` or 400 |
| Resign / abort / draw | `POST /api/board/game/{id}/resign`, `/abort`, `/draw/{yes|no}` | |
| Seek | `POST /api/board/seek` (form: `time`, `increment`, `rated`, `color`) | Streaming; keep open until a game starts, then the event stream reports `gameStart` |
| Challenge user | `POST /api/challenge/{username}` | JSON |
| Keep-alive | Streams send `\n` heartbeats every ~6 s | Use to detect a dead socket |

### 5.1 Streaming on a microcontroller

- Use `HttpDownloader::fetchUrl(url, DataCallback)` or a raw `SecureHttpClient` read loop; buffer bytes until `\n`, parse each complete line with ArduinoJson into a small `StaticJsonDocument`, dispatch, discard. Never buffer the whole response.
- `gameFull.state.moves` for a long game can be ~1 KB; fine. Replay all moves through `Position` to rebuild the board on (re)connect — this makes reconnection trivial and stateless.
- Two streams are open during a game (event stream + game stream) plus occasional POSTs. The kept-alive `SecureHttpClient` in CrossPoint is one session; we need either a second client instance for the game stream or a design where the event stream is only open in the lobby. **Recommendation for v1:** lobby holds the event stream; entering a game closes it and opens the game stream; `gameFinish` is detected from `gameState.status` instead. One socket at a time keeps heap predictable.
- POST moves on the same session as the stream is not possible on plain HTTP/1.1; moves need a second short-lived connection. Budget: one persistent stream + one transient POST client. Measure heap after TLS handshake ×2 in Phase 3.

### 5.2 Reconnect policy

- Heartbeat missing for >15 s → close, reconnect the game stream with backoff (1, 2, 4, 8 s, cap 30 s), replay `gameFull`.
- Wi-Fi drop → `WiFi.reconnect()` then same. Show a small "reconnecting" badge; never block the UI thread.

### 5.3 Seek handling

`POST /api/board/seek` blocks until matched. Run it as the "stream" while in the seeking screen; a `Cancel` tap closes the socket, which cancels the seek server-side.

---

## 6. Power and Wi-Fi policy (the requirement you care about)

**Principle:** the radio is a resource owned by `ChessActivity`. It is acquired in `onEnter` and released in `onExit`. No other code path touches it.

| State | Wi-Fi | Notes |
|---|---|---|
| Reader / Home / Settings / sleep | **Off** (stock behavior, `esp_wifi_stop`) | Identical to shipping CrossPoint |
| Chess lobby | On, modem-sleep **enabled** (`WiFi.setSleep(true)`) | Polling ongoing games; latency tolerance is high |
| In game (live clock) | On, modem-sleep **disabled** (`WiFi.setSleep(false)`) | KOSync notes modem sleep causes stalls that surface as HTTP timeouts. Accept the ~80–120 mA draw during play |
| In game, correspondence | On only while the screen is active; exit → off | No background polling; user reopens to check |
| Leaving Chess (Back/Home gesture) | `WiFi.disconnect(false); esp_wifi_stop();` | Game stays live on Lichess; opponent sees you as gone after Lichess's grace period |
| Auto-sleep | `preventAutoSleep()` **only while a live game is in progress** | Lobby and post-game screens must let the device sleep normally |

**Additional rules**

- Never persist a Wi-Fi-on state across a deep sleep. On boot, CrossPoint starts with Wi-Fi off; we rely on that.
- Frontlight: unchanged — user controls it via the existing swipe panel.
- Refresh budget: a full refresh every N fast refreshes (start N=8) or on every game-end; fast refreshes only touch dirty rects (board region / clock region).
- Heap: TLS handshake needs ~40–50 KB free contiguous internal RAM. `KOReaderSyncActivity` frees the EPUB object before sync and silently reboots after to defragment. We enter from the menu, not the reader, so no EPUB is loaded; measure with `ESP.getMaxAllocHeap()` at Phase 3 and decide whether an exit-reboot is needed. With 8 MB PSRAM available on the S3, move all non-TLS buffers (board images, move lists, NDJSON line buffer) to PSRAM (`ps_malloc` / `heap_caps_malloc(MALLOC_CAP_SPIRAM)`).
- Battery telemetry: log battery % on enter/exit of Chess to serial during development; target ≥3 h continuous blitz on a full charge.

---

## 7. Non-functional requirements

- **N1** Move latency (tap → confirmed on screen) ≤ 700 ms on a home Wi-Fi network; ≤ 1.5 s p95.
- **N2** Zero regressions in reader: run the existing test suite (`pio test`) and manually open EPUB/TXT after every milestone.
- **N3** Rebase-friendly: all new code under `src/activities/chess/` and `lib/Chess/`; upstream files touched only at the menu-registration point and `platformio.ini`/translations. Keep the diff to upstream files under ~30 lines.
- **N4** Works with buttons only as a degraded mode? **No** — v1 is touch-only (X4 Pro). Guard the menu entry behind `hasTouch()` so X3/X4 builds don't show it.
- **N5** Unit tests for `Position` (perft to depth 3 on a few standard positions) and NDJSON line splitting run natively.

---

## 8. Milestones

| # | Deliverable | Exit test |
|---|---|---|
| M0 | Toolchain proven (§2) | Self-built firmware boots; stock reflash works |
| M1 | `ChessActivity` stub in the menu, draws a static board with pieces, tap highlights a square | Serial log shows square coordinates on tap |
| M2 | `lib/Chess/Position` with legal move generation, native tests green; local two-player hot-seat on device | Perft(3) matches known counts; can play a full game offline |
| M3 | Wi-Fi gate + `GET /api/account` + lobby (card grid, in-progress list) | Username/rating shown; cards render; Wi-Fi off after Back |
| M4 | Join ongoing correspondence game, stream `gameFull`, make a move via POST | Move appears on lichess.org |
| M5 | Live game: card-tap seek, clocks, draw/resign, reconnect | Complete a 10+0 rapid game, then a 3+0 blitz game, start to finish |
| M6 | Power polish: modem-sleep policy, refresh budget, heap measurements, battery test | ≥3 h blitz; no leak across 10 enter/exit cycles (heap returns to baseline) |
| M7 | Nice-to-haves: promotion UI polish, board themes, sleep-screen position, plugin.js token entry | — |

---

## 9. Risks and mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| Heap fragmentation from repeated TLS sessions | Medium | Single persistent stream client; PSRAM for everything else; measure at M3; fall back to KOSync's exit-reboot |
| Lichess stream stalls under modem sleep | High (documented by KOSync) | Disable modem sleep during live games only |
| E-ink ghosting makes highlighted squares unreadable | Medium | Full refresh cadence; design highlights as borders rather than fills |
| CrossPoint upstream churn breaks the fork | Medium | Isolated directories; rebase monthly; pin the base commit in README |
| Touch controller mis-taps on 60 px squares | Low | Add a 4 px dead zone at square edges; show selected piece feedback before accepting the second tap |
| Token stored in plaintext on SD | Accepted | Same as every other CrossPoint credential; document; use a token with minimal scopes |

---

## 10. Claude Code project notes

- Repo root = the `crosscheck` fork. Upstream already ships a `CLAUDE.md`; append a `## crosscheck` section rather than replacing it, pointing at: `docs/contributing/*.md`, `src/activities/reader/KOReaderSyncActivity.cpp` (Wi-Fi lifecycle reference), `src/activities/plugins/PluginCatalogActivity.cpp` (streaming HTTP + ArduinoJson filter reference), `docs/contributing/touch-and-ui.md` (mandatory UI conventions), and this spec.
- The upstream `CLAUDE.md`/`AGENTS.md` already exist; read them first — they encode lint rules (translations, HAL-only hardware access, no raw `InputManager`).
- Standing instructions worth adding: "never touch `lib/Epub` or `src/activities/reader/*` except `KOReaderSyncActivity` for reference"; "all Wi-Fi calls live in `ChessActivity` only"; "run `pio run -e x4pro` before claiming a change compiles."
- Useful references outside the repo: Lichess Board API docs (lichess.org/api#tag/Board), `crosspet` fork (interactive activity precedent), SUMI firmware's chess module (offline chess UI on a C3 — worth a look for board drawing), `x4-boy` (touch/refresh tricks on Xteink panels).
