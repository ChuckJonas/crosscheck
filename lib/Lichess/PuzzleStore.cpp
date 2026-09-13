#include "PuzzleStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <cstring>

namespace {
constexpr const char* LINES_PATH = chessfiles::PUZZLE_LINES_PATH;
constexpr const char* TEMP_PATH = chessfiles::PUZZLE_TEMP_PATH;
constexpr size_t CHUNK = 512;
// A puzzle line is about 500 bytes; longer ones come from very long games.
constexpr size_t MAX_LINE = 3072;

// Reads the line file one line at a time from a byte offset.
class LineReader {
 public:
  LineReader(HalFile& file, size_t pos) : file(file), pos(pos), buf(makeUniqueNoThrow<char[]>(CHUNK)) {}
  bool ok() const { return buf != nullptr; }
  size_t position() const { return pos; }

  // Reads the next line without its newline. False at the end of the file.
  bool next(std::string& line) {
    line.clear();
    if (!file.seek(pos)) return false;
    bool complete = false;
    while (!complete) {
      const int n = file.read(buf.get(), CHUNK);
      if (n <= 0) break;
      int used = n;
      for (int i = 0; i < n; ++i) {
        if (buf[i] == '\n') {
          used = i + 1;
          complete = true;
          break;
        }
      }
      const size_t keep = complete ? used - 1 : used;
      if (line.size() + keep <= MAX_LINE) line.append(buf.get(), keep);
      pos += used;
    }
    return complete || !line.empty();
  }

 private:
  HalFile& file;
  size_t pos;
  std::unique_ptr<char[]> buf;
};

// The id of a saved line without a JSON parse: lines start with {"id":"...".
bool lineId(const std::string& line, char* out, size_t outSize) {
  const size_t at = line.find("\"id\":\"");
  if (at == std::string::npos) return false;
  const size_t start = at + 6;
  const size_t end = line.find('"', start);
  if (end == std::string::npos || end == start || end - start >= outSize) return false;
  memcpy(out, line.data() + start, end - start);
  out[end - start] = '\0';
  return true;
}

void writeLine(const lichess::Puzzle& z, std::string& out) {
  JsonDocument doc;
  doc["id"] = z.id;
  doc["r"] = z.rating;
  if (z.fen[0]) doc["fen"] = z.fen;
  if (z.lastMove[0]) doc["lm"] = z.lastMove;
  if (!z.pgn.empty()) doc["pgn"] = z.pgn;
  doc["sol"] = z.solution;
  out.clear();
  serializeJson(doc, out);
  out.push_back('\n');
}

bool readPuzzle(JsonVariantConst v, lichess::Puzzle& z) {
  lichess::copyStr(z.id, sizeof(z.id), v["id"] | "");
  z.rating = v["r"] | 0;
  lichess::copyStr(z.fen, sizeof(z.fen), v["fen"] | "");
  lichess::copyStr(z.lastMove, sizeof(z.lastMove), v["lm"] | "");
  z.pgn = v["pgn"] | "";
  z.solution = v["sol"] | "";
  return z.id[0] && !z.solution.empty() && (z.fen[0] || !z.pgn.empty());
}

bool parseLine(const std::string& line, lichess::Puzzle& z) {
  JsonDocument doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok) return false;
  return readPuzzle(doc.as<JsonVariantConst>(), z);
}
}  // namespace

void PuzzleStore::toJson(JsonDocument& doc) const {
  doc["v"] = 2;
  doc["cursor"] = static_cast<uint32_t>(cursor);
  JsonArray done = doc["results"].to<JsonArray>();
  for (const auto& r : results) {
    JsonObject o = done.add<JsonObject>();
    o["id"] = r.id;
    o["win"] = r.win;
  }
}

bool PuzzleStore::fromJson(JsonVariantConst doc) {
  results.clear();
  legacy.clear();
  cursor = doc["cursor"] | 0u;
  JsonArrayConst done = doc["results"].as<JsonArrayConst>();
  results.reserve(done.size());
  for (JsonVariantConst v : done) {
    Result r;
    lichess::copyStr(r.id, sizeof(r.id), v["id"] | "");
    r.win = v["win"] | false;
    if (r.id[0]) results.push_back(r);
  }
  // A version 1 file carried the puzzles themselves.
  JsonArrayConst old = doc["puzzles"].as<JsonArrayConst>();
  if (old.size() > 0) {
    legacy.reserve(old.size());
    for (JsonVariantConst v : old) {
      lichess::Puzzle z;
      if (readPuzzle(v, z)) legacy.push_back(z);
    }
    requestResave();
  }
  return true;
}

void PuzzleStore::load() {
  loadFromFile();  // false on the first run: nothing saved yet
  index();
  if (!legacy.empty()) {
    std::vector<lichess::Puzzle> moved;
    moved.swap(legacy);
    add(moved);
  }
}

void PuzzleStore::index() {
  ids.clear();
  if (!Storage.exists(LINES_PATH)) {
    // An add that stopped between remove and rename left only the new file.
    if (Storage.exists(TEMP_PATH)) Storage.rename(TEMP_PATH, LINES_PATH);
    if (!Storage.exists(LINES_PATH)) {
      cursor = 0;
      return;
    }
  }
  HalFile file;
  if (!Storage.openFileForRead("PUZZLES", LINES_PATH, file)) return;
  if (cursor > file.size()) cursor = 0;
  LineReader reader(file, cursor);
  if (!reader.ok()) {
    LOG_ERR("PUZZLES", "OOM: line buffer");
    return;
  }
  ids.reserve(64);
  std::string line;
  while (ids.size() < static_cast<size_t>(MAX_PUZZLES) && reader.next(line)) {
    Id id;
    if (lineId(line, id.text, sizeof(id.text))) ids.push_back(id);
  }
  LOG_INF("PUZZLES", "%d puzzles saved, %d results to send", count(), pendingResults());
}

bool PuzzleStore::has(const char* id) const {
  for (const auto& have : ids) {
    if (strcmp(have.text, id) == 0) return true;
  }
  return false;
}

bool PuzzleStore::takeNext(lichess::Puzzle& out) {
  bool found = false;
  while (!found && !ids.empty()) {
    std::string line;
    size_t next = 0;
    {
      HalFile file;
      if (!Storage.exists(LINES_PATH) || !Storage.openFileForRead("PUZZLES", LINES_PATH, file)) break;
      LineReader reader(file, cursor);
      if (!reader.ok() || !reader.next(line)) break;
      next = reader.position();
    }
    cursor = next;
    ids.erase(ids.begin());
    found = parseLine(line, out);
  }
  if (!found) ids.clear();  // the index no longer matches the file
  if (ids.empty()) {
    cursor = 0;
    if (Storage.exists(LINES_PATH)) Storage.remove(LINES_PATH);
  }
  saveToFile();
  return found;
}

int PuzzleStore::add(const std::vector<lichess::Puzzle>& more) {
  std::vector<const lichess::Puzzle*> fresh;
  fresh.reserve(more.size());
  for (const auto& z : more) {
    if (ids.size() + fresh.size() >= static_cast<size_t>(MAX_PUZZLES)) break;
    if (!z.id[0] || has(z.id)) continue;
    bool dup = false;
    for (const auto* f : fresh) dup = dup || strcmp(f->id, z.id) == 0;
    if (!dup) fresh.push_back(&z);
  }
  if (fresh.empty()) return 0;

  // The file is rewritten without an append mode: the unplayed lines of the
  // old file, then the new ones, go to a new file that takes the old one's place.
  int written = 0;
  {
    HalFile out;
    Storage.mkdir(chessfiles::DIR);
    if (!Storage.openFileForWrite("PUZZLES", TEMP_PATH, out)) {
      LOG_ERR("PUZZLES", "Cannot write %s", TEMP_PATH);
      return 0;
    }
    if (!ids.empty() && Storage.exists(LINES_PATH)) {
      HalFile in;
      auto buf = makeUniqueNoThrow<char[]>(CHUNK);
      if (!buf) {
        LOG_ERR("PUZZLES", "OOM: copy buffer");
        return 0;
      }
      if (Storage.openFileForRead("PUZZLES", LINES_PATH, in) && in.seek(cursor)) {
        int n;
        while ((n = in.read(buf.get(), CHUNK)) > 0) out.write(buf.get(), static_cast<size_t>(n));
      }
    }
    std::string line;
    for (const lichess::Puzzle* z : fresh) {
      writeLine(*z, line);
      if (line.size() > MAX_LINE) continue;
      out.write(line.data(), line.size());
      ++written;
    }
    out.flush();
  }
  if (Storage.exists(LINES_PATH)) Storage.remove(LINES_PATH);
  if (!Storage.rename(TEMP_PATH, LINES_PATH)) LOG_ERR("PUZZLES", "Cannot rename %s", TEMP_PATH);
  cursor = 0;
  index();
  saveToFile();
  return written;
}

void PuzzleStore::addResult(const char* id, bool win) {
  if (results.size() >= static_cast<size_t>(MAX_RESULTS)) results.erase(results.begin());
  Result r;
  lichess::copyStr(r.id, sizeof(r.id), id);
  r.win = win;
  results.push_back(r);
}

void PuzzleStore::resultsJson(const std::vector<Result>& results, std::string& out) {
  out = "{\"solutions\":[";
  for (size_t i = 0; i < results.size(); ++i) {
    if (i) out.push_back(',');
    out += "{\"id\":\"";
    out += results[i].id;
    out += results[i].win ? "\",\"win\":true,\"rated\":true}" : "\",\"win\":false,\"rated\":true}";
  }
  out += "]}";
}

void PuzzleStore::takeResults(std::vector<Result>& out) {
  const size_t n = results.size() < static_cast<size_t>(MAX_SEND) ? results.size() : static_cast<size_t>(MAX_SEND);
  out.assign(results.begin(), results.begin() + n);
  results.erase(results.begin(), results.begin() + n);
}

void PuzzleStore::restoreResults(const std::vector<Result>& back) {
  results.insert(results.begin(), back.begin(), back.end());
  while (results.size() > static_cast<size_t>(MAX_RESULTS)) results.erase(results.begin());
}

void PuzzleStore::clearAll() {
  std::vector<Id>().swap(ids);
  std::vector<Result>().swap(results);
  std::vector<lichess::Puzzle>().swap(legacy);
}
