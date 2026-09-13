# crosscheck development notes

This file records how the Chess app is built and how to test it. The requirements are in [crosscheck-spec.md](crosscheck-spec.md).

## Code layout

| Path | Content |
|---|---|
| `lib/Chess/Position.{h,cpp}` | Board state, FEN, legal move generation, UCI moves, standard algebraic notation parser. No Arduino includes. |
| `lib/Chess/LichessProtocol.{h,cpp}` | NDJSON line splitter and Board API message parsing. No Arduino includes. |
| `lib/Lichess/LichessClient.{h,cpp}` | HTTPS client with three FreeRTOS tasks: event stream, game stream, command runner. |
| `lib/Lichess/ChessSettings.{h,cpp}` | Token and preferences in `/.crosspoint/lichess/lichess.json`. |
| `lib/Chess/Pgn.{h,cpp}` | PGN movetext as a tree (variations, comments, glyphs), a SAN writer, and comment cleanup with arrows and circles. Native tests in `test/chess/PgnTest.cpp`. |
| `lib/Lichess/StudyStore.{h,cpp}` | Studies on the card: `/.crosspoint/lichess/studies/<id>.pgn`, a chapter index in `<id>.json`, the list in `/.crosspoint/lichess/studies.json`. |
| `lib/Lichess/PuzzleThemes.h` | The puzzle themes offered for downloads (Lichess angle keys). |
| `src/activities/chess/ChessStudyActivity.*` | Study reader: chapter list, board with comments and variations, practice mode. |
| `lib/Lichess/PuzzleStore.{h,cpp}` | Saved puzzles in `/.crosspoint/lichess/puzzles.ndjson` (one per line) plus the cursor and pending results in `/.crosspoint/lichess/puzzles.json`. |
| `src/activities/chess/ChessActivity.*` | Lobby: token setup, Wi-Fi gate, ongoing games, time-control cards, seek, challenge. |
| `src/activities/chess/ChessGameActivity.*` | Game screen for local games, Lichess games, game review, and puzzles. |
| `src/activities/chess/ChessBoardView.*` | Board drawing and tap-to-square mapping. |
| `src/activities/chess/ChessPieceBitmaps.h` | Generated 1-bit piece images. |
| `scripts/gen_chess_pieces.py` | Regenerates the piece images from the cburnett SVG files. |
| `test/chess/` | Native tests: perft, FEN, UCI, protocol parsing. |

The spec put the Lichess client in `lib/Chess`. It lives in `lib/Lichess` instead, because it needs Arduino and FreeRTOS headers. `lib/Chess` stays free of them so the native tests build.

## Upstream files touched

- `src/activities/ActivityManager.h` and `.cpp`: the `CHESS` home menu item.
- `src/activities/home/HomeActivity.h` and `.cpp`: the Chess row, shown only on boards with touch.
- `lib/I18n/translations/english.yaml`: the `STR_CHESS_*` strings.
- `test/CMakeLists.txt`: one `add_subdirectory` line.
- `AGENTS.md` and `README.md`: notes and the piece-set credit.

## Threads and data flow

- The UI loop task never blocks on the network.
- Two stream tasks hold one HTTPS connection each. The event stream opens after the first ongoing-games list and stays open for the whole session, because Lichess derives player presence from it. It replays a start line for every existing game when it opens, so the lobby only opens a game from that stream when the game is not in the ongoing list. A gap there makes the opponent see "opponent left". The game stream follows the open game. Each NDJSON line is parsed into a shared `GameSnapshot` under a mutex. The task then posts a small event to a FreeRTOS queue.
- The command task runs short requests from a queue over one kept-alive connection: account lookup, ongoing games, moves, resign, draw, challenge, recent games, puzzles. A seek uses its own connection that stays open until Lichess finds an opponent. Cancel closes that socket, which cancels the seek on the server, and bumps a generation counter so a seek or engine request still waiting in the queue is dropped. A game that starts anyway, for example from a challenge accepted late, is opened from the lobby when its start event arrives.
- The lobby shows the presets Lichess pairs from open seeks (10+0, 10+5, 15+10, 30+0, 30+20, Custom), a Rated toggle, and Computer / Challenge / Local game. Computer opens the engine screen: the full app preset list (3+0 to 30+20, Custom); a card opens a level picker 1 to 8 with the approximate rating, defaulting to the last level, and the game starts after the pick, always casual. Lichess refuses open seeks under 8 minutes estimated with a 400 "Invalid time control" (verified on 2026-09-11 for 3+0, 3+2, and 5+0). When a seek or challenge is refused, the error screen shows the server's reason text and has a Back button. Rematch after an engine game starts a new engine game at the same level and clock.
- The lobby has a tab bar under the header: Play, Games, Puzzles. The active tab is remembered in `lobbyTab`.
- Games: the last 12 games from `GET /api/games/user/{id}` (one NDJSON line each, moves in standard algebraic notation). A row opens the game in review mode: the final position, the players, the result, and the move browser (Prev, Next, End). The notation is converted with `chess::replaySan` when the row is tapped.
- Puzzles work offline. `PuzzleStore` keeps the downloaded puzzles in `/.crosspoint/lichess/puzzles.ndjson`, one per line, oldest first, up to 500; they never sit in RAM as a whole. `/.crosspoint/lichess/puzzles.json` holds the byte offset of the next unplayed line (`cursor`) and the results not yet sent (up to 300). On entry the store indexes the unplayed lines (ids only, 8 bytes each). Next puzzle reads the line at the cursor and moves the cursor; when the last line is played the file is deleted. A download rewrites the file (unplayed lines, then the new ones, into `puzzles.tmp`, then remove and rename), because `HalStorage` has no append mode. A version 1 `puzzles.json` (puzzles inline) is moved to the line file on the first load. Next puzzle takes the next saved puzzle; when none is saved it connects, downloads 30, and opens one. "Download puzzles" opens a count picker (30, 60, 120, 240, 500, capped by the room left); the lobby then fetches batch after batch, the tab shows "Downloading X of Y" with a Stop button, and the Puzzles tab shows "Puzzles ready to solve: N" and "Results to send: M". Download comes from `GET /api/puzzle/batch/mix?nb=N&difficulty=...` (scope `puzzle:read`, anonymous fallback around rating 1500); 30 per request, because the reply is about 36 KB of JSON and must fit next to Wi-Fi. Results are sent with `POST /api/puzzle/batch/mix` (scope `puzzle:write`; a refused token falls back to an anonymous post so the batch still arrives), up to 50 per post, whose reply carries a fresh batch. Results leave the store when a send starts and are restored if it fails, so a reply lost to another screen cannot send them twice. A sync runs once per connection when results are pending or fewer than 10 puzzles remain (topping up to 30), after every fifth result while connected, and with every download. The "next" endpoint delivers no FEN, only the game's moves and `initialPly`; the position after all the moves is the puzzle, and the solver moves next. A win is a solve without a wrong move or a shown solution; a wrong move counts as a loss even if the puzzle is finished afterwards.
- Studies are documents that Lichess users publish: annotated games, opening lines, puzzle packs. The Studies tab lists the signed-in user's studies from `GET /api/study/by/{userId}` (scope `study:read` for private ones; anonymous fallback lists the public ones), another user's public studies by username (By user; the `lichess` account has hundreds, the newest 40 are listed), and any study opened by id or link. Every study opened by id or link is saved on the card and stays readable offline; "Download N listed studies" fetches all listed studies that are not on the card, one after another. There is no API for liked or bookmarked studies. The recommended way to get a study onto the device is Clone on lichess.org (from the phone or the browser), then My studies on the device: the clone appears and one tap downloads it. Private clones need `study:read`. The README documents this path for users. The Lichess Practice pages are studies too, but their chapters hold only a position and a goal for the site's engine, so they are of no use here. A row that is not on the card downloads `GET /api/study/{id}.pgn?clocks=false&comments=true&variations=true&orientation=true` straight to `/.crosspoint/lichess/studies/<id>.part` on the client task, then renames it to `.pgn`; a study can be hundreds of KB. `StudyStore::indexStudy` scans the file once for lines that start with `[Event ` and records each chapter's offset, length, and name (`ChapterName`, else `Event`) in `<id>.json` with the last chapter read. The reader parses one chapter at a time with `chess::pgn::parse` over a `ChapterSource` that reads only that span: the tree holds moves (16 bytes per node, up to 1500), comments stay in the file as offsets and are read when a node is shown, so RAM does not grow with the chapter text. Comments drop `[%clk]`, `[%eval]`, and `[%anno]` commands, keep `[%cal]` arrows and `[%csl]` circles for the board, and map figurines to letters. A move that cannot be read skips the rest of its variation. The reader shows the board, a strip with Prev, the move, Next, and Menu, a row of alternative moves when the position has several, and a two-line preview of the comment; a tap on the preview opens the comment on its own page in the serif reading font, with Board, Prev, and Next at the bottom, so annotated games read like a book with the board one tap away. Playing a move on the board that exists in the tree follows it. Practice mode (menu) hides the comment and the next move for the reader's side (the chapter `Orientation`), plays the other side's replies from the tree at random when there are several, marks wrong moves with a cross, and counts found and missed moves; Show reveals the move. The menu also flips the board, moves between chapters, and deletes the study from the card.
- Puzzle themes and results. "Theme" on the Puzzles tab picks the Lichess angle for downloads (`puzzleTheme` in the settings, `mix` by default). "Your results" loads `GET /api/puzzle/dashboard/30` and lists the themes weakest first (lowest performance); tapping one sets it as the download theme.
- Analysis. The recent games list makes a second small request with `analysed=true` to tag the games that have computer analysis (" - analysed" in the row). Opening such a game fetches `GET /game/export/{id}?evals=true` and passes one `AnalysisPly` per ply (centipawns or mate, judgment, best move) to the review. The review shows the evaluation of the shown position and a bar under the status line, names the judgment of the move that led to it with the better move in SAN, draws that move as an arrow from the position before it, and the menu has Next mistake and Previous mistake. A game without analysis has Request analysis in the menu: a QR code of the game page on Lichess, where computer analysis is one tap; there is no API to request it.
- Challenge opens the Friends screen: the players the user follows from `GET /api/rel/following` (scope `follow:read`), with presence from the public `GET /api/users/status?ids=...` (online players first). A tap on a player opens the time control dialog with the name filled in; "Type a name" covers everyone else. Without the scope the screen says so and offers the typed name.
- Release: push a tag `crosscheck-vX.Y.Z` on the `crosscheck` branch after setting `[crosscheck] version` in `platformio.ini` to the same value. `.github/workflows/crosscheck-release.yml` builds `x4pro-gh_release` and publishes a GitHub release with `firmware-x4pro.bin`. Users flash it with the CrossPoint web flasher ("Custom .bin"); afterwards the updater reads this fork's releases (`OTA_RELEASE_API_URL`, and the version compare skips the tag prefix), so Settings → Check for updates delivers the next crosscheck release. Upstream's `release.yml` only runs for digit-led tags in this fork.
- Files: everything the app writes lives in `/.crosspoint/lichess/` (`lichess.json` settings and token, `puzzles.json` and `puzzles.ndjson`, `studies.json`, `studies/`). `chessfiles::migrate()` moves files from older builds into the folder on entry. The account row on the Play tab opens a popup to enter a new token or remove it.
- Flow rule: the lobby works offline, and there is no Connect button. Every action that needs Lichess connects on its own and then continues (`connectThen` remembers the action as `Pending`; `runPending` runs it when the connection is ready): a card, Custom → Play, a level pick on the Computer screen, a challenge, the Games tab or Refresh, Download, and Next puzzle with an empty store. Screens that only configure (Computer, Custom, the challenge username) open offline. The lobby never connects on entry: the Play tab offers "Load ongoing games", and the first action that needs Lichess connects. "Connected" means the account is known, the ongoing list is loaded, and the event stream is open (`accountReady` is set on the first ongoing list, so a pending seek cannot mistake the stream's replay of an old game for a match). The header shows the username, or "Offline" with a token and no connection. Without a token the Play tab shows the token help with a QR code of the prefilled token page (`https://lichess.org/account/oauth/token/create?scopes[]=board:play&scopes[]=challenge:write&scopes[]=challenge:read&scopes[]=puzzle:read&scopes[]=puzzle:write&scopes[]=study:read&scopes[]=follow:read&description=crosscheck+X4+Pro`); a card tapped then asks for the token first and continues after it. Cancelling the Wi-Fi picker, Back on the error screen, and cancelling the token entry drop the pending action.
- Popup callbacks and client events run on the loop task. Every change to state the render task reads happens under `RenderLock`.
- The game screen polls the event queue in `loop()`. On `GameUpdated` it copies the snapshot, replays the move list from the start position, and redraws.
- Moves are applied on the board before the server confirms them. If the server rejects a move, the screen resyncs from the snapshot.
- Lichess drops an idle connection after about ten seconds, which made a move after a pause cost a TLS handshake (about 1 s instead of 0.2 s). During a live game the command task sends a small request every 5 seconds to keep the connection open.
- At the end of a rated game the client fetches the game export and shows the rating change in the status line.
- Material: the clock row of the side that is ahead shows "+N" (pawn 1, knight 3, bishop 3, rook 5, queen 9).
- Move history: swipe left on the board, or the left side button, steps one move back; swipe right or the right side button steps forward. While browsing, the status line says "Move N of M", the bar shows Prev, Next, Live, and taps on the board are ignored. A new opponent move does not leave browsing. Back returns to the live position.
- Premove: during the opponent's turn a tap on your own piece shows where it could go, computed as if it were your move. A tap on a destination sets the premove, drawn with a double border. When the opponent's move arrives, the premove is played at once if it is still legal, otherwise dropped. Any tap on the board cancels it; promotion premoves make a queen.
- Reconnect: after an unexpected stream close, the stream task waits 1, 2, 4, 8, 16, 30, 30, 30 seconds between attempts, then reports failure. Each reconnect replays the full game from `gameFull`.

## Wi-Fi and power

- The lobby turns Wi-Fi on when a connection is needed (see the lobby note above) and off in `onExit` with `WiFi.disconnect(true)` and `WiFi.mode(WIFI_OFF)`, the same calls `main.cpp` uses before sleep. A bare `esp_wifi_stop()` (the KOReader sync pattern) only works because that screen reboots afterwards; without the reboot the Arduino layer never restarts the driver and every later connect fails.
- Modem sleep is on in the lobby and off during a game.
- Auto-sleep is blocked only while an online game is in progress.
- The lobby logs free heap, largest free block, and battery percent on enter and exit. Compare the exit values across several sessions to check for leaks. Measured on 2026-09-11: 231 KB free before Wi-Fi, 160 KB with Wi-Fi and the client tasks, 144 KB with both streams open, 184 KB after exit.

## Display

- The SDK has a windowed refresh, but the UC8179 driver of the X4 Pro does not implement it and falls back to a full frame. Every update repaints the whole frame with a fast refresh. Full refreshes happen only on game start, game end, a board flip, the "Refresh screen" menu item, and optionally on every opponent move. There is no periodic full refresh: the every-eighth-update flash of earlier builds read as random flashes during play. The panel driver itself only forces a full flash after a resync request, never on a timer.
- Fast refreshes in the game screen are asynchronous with one rule that makes them correct on the UC8179: `render()` starts the refresh and then waits for the panel, holding the render lock, before it returns. So nothing draws into the framebuffer before the driver's finish step has copied it into the panel's OLD plane, and only the render task ever runs that step. The loop is not blocked by this because it never waits for the lock; it queues input instead. Builds m3.4 to m3.10 and m3.14 drew before the finish step, so the OLD plane got the new frame and changed pixels were not driven: faint, ghosted updates. A fast refresh takes 562 ms on the panel.
- Touch is polled by the main loop, never by interrupt. The game loop therefore captures input on every pass without waiting for the render lock and applies it once the lock is free. Before this, a tap made while the panel was busy was lost.
- Clocks redraw every `clockRefreshSec` seconds (1, 2, 5, or 10; default 5, changeable in the game menu, which reopens to show the new value), or every second when either clock is under 20 seconds.
- The game menu item "Full refresh on opponent move" (setting `fullRefreshEveryMove`) forces a full refresh whenever the opponent moves. Off by default.

## Frontlight cue

The frontlight pulse on an opponent move is off by default (`pulseMs` 0). The game menu item "Frontlight pulse on opponent move" turns it on with a 300 ms pulse: a dark light turns on, a lit light dims to a quarter or brightens if it was already dim. Config version 3 turned it off for files saved by earlier builds.

## Token setup

1. Create a personal API token at `https://lichess.org/account/oauth/token` with the scopes `board:play` and `challenge:write`.
2. Either type it on the device with the on-screen keyboard, or put it on the SD card in `/.crosspoint/lichess/lichess.json`:

```json
{ "token": "lip_xxxxxxxxxxxxxxxx" }
```

The device rewrites the file in obfuscated form on the next save. The file carries a `cfgVersion`; version 2 moved the stored clock refresh default from 1 to 5 seconds, version 3 turned the frontlight pulse off. The obfuscation ties the file to the device MAC address. It is not encryption.

## Build tags

`platformio.local.ini` sets `-DCROSSPOINT_VERSION` to `1.6.0-crosscheck-<tag>`. Bump the tag before every flash. Settings → About shows it.

## Native tests

```bash
cmake -S test -B build/test -DCMAKE_BUILD_TYPE=Release
cmake --build build/test --target ChessPositionTest
build/test/chess/ChessPositionTest
```

## QA checklist

Local game:

1. Home → Chess → Local game. Play to checkmate. Promote a pawn and pick a knight.
2. The action bar under the status line has New game, Flip board, and Menu. The menu also opens with a swipe down from the top edge.

Lobby:

3. Without a token the screen shows the help text and the Enter token button.
4. Enter the token. The device joins Wi-Fi, then shows the username in the header and ratings at the bottom.
5. Ongoing correspondence games appear at the top. Tap one to open it.
6. Toggle Rated. Reopen Chess and confirm the toggle was saved.
7. Custom card: change minutes and increment, then Play.
7a. Computer: tap the button, tap a card, pick a level. A game against the Lichess engine opens directly. Blitz works here.

Online game:

8. Tap 10+0. The screen says "Searching 10+0 Rated" with a Cancel button. Cancel returns to the lobby.
9. Tap 10+0 again and wait for a match. The game opens with the opponent on top.
10. Play a move. Confirm it appears on lichess.org within a second.
11. Wait for the opponent's move. The frontlight pulses and the board updates.
12. The action bar shows Resign, Draw, and Menu during a game. Resign asks for confirmation. After the game the bar shows Rematch, Lobby, and Menu.
13. Finish a game. The status line shows the result. Menu → Rematch sends a challenge.
14. Back to lobby during a game. The game stays live on Lichess. Reopen it from the ongoing list.
15. Leave Chess. Confirm Wi-Fi is off: the device sleeps as usual and the serial log shows the exit heap values.
16. Challenge a friend by username. The screen waits until the friend accepts.

Games and puzzles:

17. Games tab: the last 12 games load. Tap one: the final position shows with the result. Prev steps back through the moves; End returns to the final position.
18. Puzzles tab: pick a difficulty, tap Next puzzle. Play the solving move. A wrong move draws a cross and the top row says "Not that move". After the scripted reply, continue until "Puzzle solved". Solution plays the next move for you. Next puzzle loads another.
19. Offline: leave Wi-Fi range or open the app before connecting. The Puzzles tab shows "Puzzles ready to solve: N" and Next puzzle works. Results to send appear after solving. Connect later and check on lichess.org/training/dashboard that the results arrived.
20. Download puzzles: pick 120 in the popup. The tab shows "Downloading 30 of 120", then 60, 90, 120, and the count rises by about 30 each step (duplicates are skipped). Stop ends the download after the current batch. Leave the app and come back: the count is the same, and the serial log shows "[PUZZLES] N puzzles saved".
22. Studies tab: Refresh lists your studies. Tap one that is not on the device: it downloads and opens on the chapter list. Open a chapter: Prev and Next step through the line, the comment shows under the strip and pages on a tap, alternative moves appear as buttons, arrows from the annotation show on the board. Menu → Practice this line: the next move is hidden; play it on the board. A wrong move shows a cross; Show reveals it. Leave Wi-Fi range: the study still opens from the card.
23. Open by ID with a public study link, for example lichess.org/study/Y1yXP80U (FIDE Candidates 2026 annotations, 56 chapters, about 300 KB).
24. Puzzles tab → Theme: pick Endgame, download 30, and check the puzzles are endgames. Your results: the weakest themes list first; tap one and the theme line changes.
21. One-tap flow: open the app on the Puzzles tab (offline), switch to Play, tap 10+0. The device connects and the seek starts without a second tap. Do the same for Computer → card → level, and for the Games tab.
