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
// The puzzles live in /.crosspoint/puzzles.ndjson, one per line, oldest first,
// and never in RAM as a whole. This store keeps only their ids, the byte
// offset of the next unplayed line (`cursor`), and the pending results, in
// /.crosspoint/puzzles.json.
class PuzzleStore : public PersistableStore<PuzzleStore> {
 public:
  struct Result {
    char id[lichess::GAME_ID_LEN] = {};
    bool win = false;
  };

 private:
  struct Id {
    char text[8] = {};
  };
  // Unplayed puzzles in file order; rebuilt from the line file by index().
  std::vector<Id> ids;
  std::vector<Result> results;
  // Puzzles read from a version 1 file; load() moves them to the line file.
  std::vector<lichess::Puzzle> legacy;
  size_t cursor = 0;

  PuzzleStore() = default;
  ~PuzzleStore() = default;
  friend class PersistableStore<PuzzleStore>;

  void index();
  bool has(const char* id) const;

 public:
  // One download. The reply for 30 puzzles is about 36 KB of JSON, which is
  // as much as fits next to Wi-Fi.
  static constexpr int BATCH_SIZE = 30;
  // The automatic refill on connect tops the store up to this many.
  static constexpr int TOP_UP = 30;
  static constexpr int REFILL_BELOW = 10;
  static constexpr int MAX_PUZZLES = 500;
  static constexpr int MAX_RESULTS = 300;
  // Results per post; the rest wait for the next post.
  static constexpr int MAX_SEND = 50;

  static const char* getFilePath() { return chessfiles::PUZZLES_PATH; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  // Reads the state file and indexes the line file.
  void load();
  int count() const { return static_cast<int>(ids.size()); }
  int pendingResults() const { return static_cast<int>(results.size()); }
  // Removes and returns the next puzzle, and saves. False when none is saved.
  bool takeNext(lichess::Puzzle& out);
  // Appends the puzzles that are not saved yet, and saves. Returns how many.
  int add(const std::vector<lichess::Puzzle>& more);
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
