#pragma once
#include <ArduinoJson.h>
#include <ChessFiles.h>
#include <LichessProtocol.h>
#include <PersistableStore.h>

#include <string>
#include <vector>

// Puzzles saved on the SD card for solving without a connection, plus the
// results that still have to go to Lichess.
//
// The puzzles live in lane files, /.crosspoint/lichess/puzzles-<angle>.ndjson,
// one puzzle per line, named after the angle they were downloaded with. Each
// line carries the puzzle's own Lichess themes, and the selection works on
// those: a puzzle from the mix lane still counts as an endgame when Lichess
// tagged it so. This store keeps in RAM only the ids with a theme mask, and
// on the card the played ids of each lane (`taken`) plus the pending results.
class PuzzleStore : public PersistableStore<PuzzleStore> {
 public:
  struct Result {
    char id[lichess::GAME_ID_LEN] = {};
    bool win = false;
  };

 private:
  struct Lane {
    char key[24] = {};
    std::vector<std::string> taken;
    int untagged = 0;  // lines without themes; they cannot be filtered and go
  };
  struct Id {
    char text[8] = {};
    uint8_t lane = 0;
    uint32_t mask = 0;  // bit i set when the puzzle has theme i of PUZZLE_THEMES
  };
  std::vector<Lane> lanes;
  std::vector<Id> ids;
  std::vector<Result> results;

  PuzzleStore() = default;
  ~PuzzleStore() = default;
  friend class PersistableStore<PuzzleStore>;

  void index();
  bool has(const char* id) const;
  int laneIndex(const char* key) const;
  bool taken(const Lane& lane, const char* id) const;
  // Rewrites a lane file without its played lines, plus new ones.
  int rewriteLane(int laneIndex, const std::vector<const lichess::Puzzle*>& fresh);

 public:
  // One download. The reply for 30 puzzles is about 36 KB of JSON, which is
  // as much as fits next to Wi-Fi.
  static constexpr int BATCH_SIZE = 30;
  // The automatic refill on connect tops the selection up to this many.
  static constexpr int TOP_UP = 30;
  static constexpr int REFILL_BELOW = 10;
  static constexpr int MAX_PUZZLES = 500;
  static constexpr int MAX_RESULTS = 300;
  // Results per post; the rest wait for the next post.
  static constexpr int MAX_SEND = 50;
  static constexpr int MAX_LANES = 32;
  // Played lines a lane may hold before its file is rewritten without them.
  static constexpr int COMPACT_AT = 24;

  static const char* getFilePath() { return chessfiles::PUZZLES_PATH; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Reads the state file and indexes the lane files.
  void load();
  // Unplayed puzzles: all of them, or those with any theme of the mask (0 means all).
  int count() const { return static_cast<int>(ids.size()); }
  int count(uint32_t mask) const;
  int pendingResults() const { return static_cast<int>(results.size()); }
  // Removes and returns a random unplayed puzzle with a theme of the mask
  // (0 means any), and saves. False when none fits.
  bool takeNext(uint32_t mask, lichess::Puzzle& out);
  // Appends the puzzles that are not saved yet to the angle's lane, and saves. Returns how many.
  int add(const char* angle, const std::vector<lichess::Puzzle>& more);
  void addResult(const char* id, bool win);
  // JSON body for POST /api/puzzle/batch/{angle}: {"solutions":[...]}.
  static void resultsJson(const std::vector<Result>& results, std::string& out);
  // Hands up to MAX_SEND pending results to a send; restore them if it fails.
  void takeResults(std::vector<Result>& out);
  void restoreResults(const std::vector<Result>& back);
  // Frees the RAM; the SD card keeps everything.
  void clearAll();
};

#define PUZZLE_STORE PuzzleStore::getInstance()
