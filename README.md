# crosscheck

A Lichess client for the Xteink X4 Pro e-reader. Play, review, solve puzzles, and read studies on an e-ink screen that runs for days.

crosscheck is a fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader), the open-source e-reader firmware. Everything about reading books, the device, Wi-Fi setup, fonts, and file transfer is CrossPoint's and is documented there. This README covers the Lichess app only.

## What it does

**Play**

- Rated or casual games against people from open seeks, rapid and classical.
- Challenges to the players you follow, or to anyone by name, at any time control.
- Games against the Lichess engine at levels 1 to 8.
- Live clocks, premoves, move history, material count, draw and resign, and the rating change at the end.
- A local two-player game on one device, no network.

**Games**

- Your recent games with a move browser.
- Computer analysis when Lichess has it: an evaluation chart you can tap to jump to a move, the judgments of each move, the better move drawn on the board, and jumps between mistakes.

**Puzzles**

- Hundreds of puzzles stored on the SD card, solved offline.
- Any set of themes at once, with counts of what is on the card.
- Results go to Lichess the next time the device connects, and your 30-day results by theme are one tap away.

**Studies**

- Lichess studies saved on the SD card: annotated games and opening lines with comments, variations, and arrows.
- A reading page for the commentary of each move, and a practice mode that hides the next move.

The lobby works offline. Anything that needs Lichess connects on its own when you tap it.

## Install

1. Download `firmware-x4pro.bin` from the [latest release](https://github.com/ChuckJonas/crosscheck/releases/latest).
2. Open https://crosspointreader.com/#flash-tools, select Xteink X4Pro, click **Custom .bin**, and upload the file. CrossPoint's note about USB-locked devices applies.
3. Updates arrive on the device: Settings → Check for updates reads crosscheck's releases, not CrossPoint's, so it never offers to replace crosscheck with stock firmware.

Only the X4 Pro is supported. The app needs a touch screen.

## Setup

1. Create a Lichess API token with [this prefilled link](https://lichess.org/account/oauth/token/create?scopes[]=board:play&scopes[]=challenge:write&scopes[]=challenge:read&scopes[]=puzzle:read&scopes[]=puzzle:write&scopes[]=study:read&scopes[]=follow:read&description=crosscheck+X4+Pro). It ticks every scope the app uses: `board:play`, `challenge:write`, `challenge:read`, `puzzle:read`, `puzzle:write`, `study:read`, `follow:read`. The device shows the same link as a QR code until a token is stored.
2. Enter the token on the device, or put it on the SD card in `/.crosspoint/lichess/lichess.json` as `{ "token": "..." }`. The device rewrites the file with the token scrambled as `token_obf`; a plain `token` field always wins.
3. To change or remove the token later, tap the header where the account name shows.

## Using it

**Play** has four sub-tabs. *Match* lists your ongoing games and the seek cards, with the Rated switch above them. *Computer* has the engine cards; the level is asked after the card. *Challenge* lists the players you follow, online ones first, with Type a name for anyone else. *Local* starts a game on the device.

**Games** tags the games that have computer analysis. Open one and the evaluation chart appears under the board. For a game without analysis, the menu's Request analysis shows a QR code of the game on Lichess, where the request is one tap; Check for analysis under the code then loads it into the open review.

**Puzzles** downloads in batches of 30, as many as you ask for, up to 500 on the card. Themes picks any number of themes; the tab tells you how many saved puzzles match. Next puzzle draws from the matching ones and downloads when nothing matches. Your results shows the weakest themes first.

**Studies** are documents Lichess users publish. To get one onto the device without typing: open it on lichess.org or in the Lichess app, choose **Clone**, then tap **My studies** on the device and tap the clone. It downloads to the card and opens offline from then on. Clones are private, which is what `study:read` is for. **By user** lists another player's public studies (`lichess` has hundreds of annotated tournament games and puzzle packs), and **Open by ID** takes a study id or link. In a chapter, tap the comment preview to read the full commentary of that move; the menu has Practice, which hides the next move and checks yours.

## What the Lichess API does not allow

- Blitz and bullet seeks. Lichess keeps those pools for its own clients. Blitz works against the engine and in challenges to a friend.
- Requesting computer analysis. It takes one tap on the website, and the QR code gets you there.
- Liked or bookmarked studies. Clone is the workaround.

## Development

Build with `pio run -e x4pro` and flash with `pio run -e x4pro -t upload`. The native tests for the move generator, the Lichess protocol, and the PGN parser run with `cmake -S test -B build && cmake --build build && ctest --test-dir build`.

The app lives in `src/activities/chess/`, `lib/Chess/`, and `lib/Lichess/`. [docs/crosscheck-dev-notes.md](docs/crosscheck-dev-notes.md) explains the architecture, the Lichess and display findings that shaped it, the release steps, and a QA checklist. [docs/crosscheck-spec.md](docs/crosscheck-spec.md) is the original plan. CrossPoint's own development guide is in [AGENTS.md](AGENTS.md) and `docs/contributing/`.

A release is a tag `crosscheck-vX.Y.Z` after setting the same value in `platformio.ini`; the workflow builds the firmware and publishes it.

## Credits

Built on [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader). The chess piece images are the Lichess "cburnett" set by Colin M.L. Burnett, licensed under [CC BY-SA 3.0](https://creativecommons.org/licenses/by-sa/3.0/). Lichess is a free, open-source chess server; crosscheck is not affiliated with Lichess, Xteink, or any device manufacturer.
