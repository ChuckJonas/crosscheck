#pragma once

// Where the chess app keeps its files on the SD card.
namespace chessfiles {
constexpr const char* DIR = "/.crosspoint/lichess";
constexpr const char* SETTINGS_PATH = "/.crosspoint/lichess/lichess.json";
constexpr const char* PUZZLES_PATH = "/.crosspoint/lichess/puzzles.json";
constexpr const char* PUZZLE_LINES_PATH = "/.crosspoint/lichess/puzzles.ndjson";
constexpr const char* PUZZLE_TEMP_PATH = "/.crosspoint/lichess/puzzles.tmp";
constexpr const char* STUDIES_PATH = "/.crosspoint/lichess/studies.json";
constexpr const char* STUDIES_DIR = "/.crosspoint/lichess/studies";

// Creates the folder and moves files that older builds kept directly in
// /.crosspoint into it.
void migrate();
}  // namespace chessfiles
