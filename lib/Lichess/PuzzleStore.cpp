#include "PuzzleStore.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PuzzleThemes.h>

#include <cstdio>
#include <cstring>

namespace {
constexpr size_t CHUNK = 512;
// A puzzle line is about 500 bytes; longer ones come from very long games.
constexpr size_t MAX_LINE = 3072;

void lanePath(const char* key, bool temp, char* out, size_t size) {
  snprintf(out, size, "%s/puzzles-%s.%s", chessfiles::DIR, key, temp ? "tmp" : "ndjson");
}

// Reads a line file one line at a time from a byte offset.
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

// A string field of a saved line without a JSON parse; the lines are written
// here, so the field is "key":"value" with no escapes in it.
bool lineField(const std::string& line, const char* key, std::string& out) {
  char pattern[24];
  snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
  const size_t at = line.find(pattern);
  if (at == std::string::npos) return false;
  const size_t start = at + strlen(pattern);
  const size_t end = line.find('"', start);
  if (end == std::string::npos) return false;
  out.assign(line, start, end - start);
  return !out.empty();
}

// The theme mask of comma separated theme keys.
uint32_t maskOf(const std::string& keys) {
  uint32_t mask = 0;
  size_t start = 0;
  while (start < keys.size()) {
    size_t end = keys.find(',', start);
    if (end == std::string::npos) end = keys.size();
    const std::string key = keys.substr(start, end - start);
    const int idx = puzzleThemeIndex(key.c_str());
    if (idx > 0 && idx < 32) mask |= 1u << idx;
    start = end + 1;
  }
  return mask;
}

void writeLine(const lichess::Puzzle& z, std::string& out) {
  JsonDocument doc;
  doc["id"] = z.id;
  doc["r"] = z.rating;
  if (!z.pgn.empty()) doc["pgn"] = z.pgn;
  doc["sol"] = z.solution;
  if (!z.themes.empty()) doc["t"] = z.themes;
  out.clear();
  serializeJson(doc, out);
  out.push_back('\n');
}

bool readPuzzle(JsonVariantConst v, lichess::Puzzle& z) {
  lichess::copyStr(z.id, sizeof(z.id), v["id"] | "");
  z.rating = v["r"] | 0;
  z.pgn = v["pgn"] | "";
  z.solution = v["sol"] | "";
  z.themes = v["t"] | "";
  return z.id[0] && !z.solution.empty() && !z.pgn.empty();
}

bool parseLine(const std::string& line, lichess::Puzzle& z) {
  JsonDocument doc;
  if (deserializeJson(doc, line) != DeserializationError::Ok) return false;
  return readPuzzle(doc.as<JsonVariantConst>(), z);
}
}  // namespace

void PuzzleStore::toJson(JsonDocument& doc) const {
  doc["v"] = 4;
  JsonObject laneObj = doc["lanes"].to<JsonObject>();
  for (const auto& lane : lanes) {
    JsonArray taken = laneObj[lane.key].to<JsonArray>();
    for (const auto& id : lane.taken) taken.add(id);
  }
  JsonArray done = doc["results"].to<JsonArray>();
  for (const auto& r : results) {
    JsonObject o = done.add<JsonObject>();
    o["id"] = r.id;
    o["win"] = r.win;
  }
}

bool PuzzleStore::fromJson(JsonVariantConst doc) {
  results.clear();
  lanes.clear();
  JsonObjectConst laneObj = doc["lanes"].as<JsonObjectConst>();
  for (JsonPairConst kv : laneObj) {
    if (lanes.size() >= static_cast<size_t>(MAX_LANES)) break;
    Lane lane;
    lichess::copyStr(lane.key, sizeof(lane.key), kv.key().c_str());
    if (!lane.key[0]) continue;
    for (JsonVariantConst t : kv.value().as<JsonArrayConst>()) {
      const char* id = t | "";
      if (id[0]) lane.taken.emplace_back(id);
    }
    lanes.push_back(lane);
  }
  JsonArrayConst done = doc["results"].as<JsonArrayConst>();
  results.reserve(done.size());
  for (JsonVariantConst v : done) {
    Result r;
    lichess::copyStr(r.id, sizeof(r.id), v["id"] | "");
    r.win = v["win"] | false;
    if (r.id[0]) results.push_back(r);
  }
  return true;
}

void PuzzleStore::load() {
  loadFromFile();  // false on the first run: nothing saved yet
  index();
  // Lines without themes cannot be filtered: they go.
  for (size_t li = 0; li < lanes.size(); ++li) {
    if (lanes[li].untagged > 0) {
      LOG_INF("PUZZLES", "Dropping %d untagged puzzles from lane %s", lanes[li].untagged, lanes[li].key);
      rewriteLane(static_cast<int>(li), {});
    }
  }
  saveToFile();
}

int PuzzleStore::laneIndex(const char* key) const {
  for (size_t i = 0; i < lanes.size(); ++i) {
    if (strcmp(lanes[i].key, key) == 0) return static_cast<int>(i);
  }
  return -1;
}

bool PuzzleStore::taken(const Lane& lane, const char* id) const {
  for (const auto& t : lane.taken) {
    if (t == id) return true;
  }
  return false;
}

void PuzzleStore::index() {
  ids.clear();
  ids.reserve(64);
  std::string line;
  std::string field;
  for (size_t li = 0; li < lanes.size(); ++li) {
    Lane& lane = lanes[li];
    char path[64];
    char temp[64];
    lanePath(lane.key, false, path, sizeof(path));
    lanePath(lane.key, true, temp, sizeof(temp));
    lane.untagged = 0;
    if (!Storage.exists(path)) {
      // An add that stopped between remove and rename left only the new file.
      if (Storage.exists(temp)) Storage.rename(temp, path);
      if (!Storage.exists(path)) {
        lane.taken.clear();
        continue;
      }
    }
    HalFile file;
    if (!Storage.openFileForRead("PUZZLES", path, file)) continue;
    LineReader reader(file, 0);
    if (!reader.ok()) {
      LOG_ERR("PUZZLES", "OOM: line buffer");
      return;
    }
    while (ids.size() < static_cast<size_t>(MAX_PUZZLES) && reader.next(line)) {
      Id id;
      id.lane = static_cast<uint8_t>(li);
      if (!lineField(line, "id", field) || field.size() >= sizeof(id.text)) continue;
      if (taken(lane, field.c_str())) continue;
      lichess::copyStr(id.text, sizeof(id.text), field.c_str());
      if (!lineField(line, "t", field)) {
        ++lane.untagged;
        continue;
      }
      id.mask = maskOf(field);
      ids.push_back(id);
    }
  }
  LOG_INF("PUZZLES", "%d puzzles saved in %d lanes, %d results to send", count(), static_cast<int>(lanes.size()),
          pendingResults());
}

int PuzzleStore::count(uint32_t mask) const {
  if (mask == 0) return count();
  int n = 0;
  for (const auto& id : ids) n += (id.mask & mask) ? 1 : 0;
  return n;
}

bool PuzzleStore::has(const char* id) const {
  for (const auto& have : ids) {
    if (strcmp(have.text, id) == 0) return true;
  }
  return false;
}

bool PuzzleStore::takeNext(uint32_t mask, lichess::Puzzle& out) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    const int fitting = count(mask);
    if (fitting == 0) return false;
    // A random one of the fitting puzzles, so the themes stay mixed.
    int pick = static_cast<int>(random(fitting));
    size_t at = 0;
    for (; at < ids.size(); ++at) {
      if (mask != 0 && !(ids[at].mask & mask)) continue;
      if (pick == 0) break;
      --pick;
    }
    if (at >= ids.size()) return false;
    const Id target = ids[at];
    ids.erase(ids.begin() + static_cast<long>(at));
    Lane& lane = lanes[target.lane];
    char path[64];
    lanePath(lane.key, false, path, sizeof(path));
    // The line is found by its id, reading the lane from its head.
    bool found = false;
    {
      HalFile file;
      if (Storage.exists(path) && Storage.openFileForRead("PUZZLES", path, file)) {
        LineReader reader(file, 0);
        std::string line;
        std::string field;
        while (reader.ok() && reader.next(line)) {
          if (!lineField(line, "id", field) || field != target.text) continue;
          found = parseLine(line, out);
          break;
        }
      }
    }
    if (!found) {
      LOG_ERR("PUZZLES", "Puzzle %s is not in its lane file", target.text);
      continue;
    }
    lane.taken.emplace_back(target.text);
    int left = 0;
    for (const auto& id : ids) left += id.lane == target.lane ? 1 : 0;
    if (left == 0) {
      lane.taken.clear();
      if (Storage.exists(path)) Storage.remove(path);
    } else if (lane.taken.size() >= static_cast<size_t>(COMPACT_AT)) {
      rewriteLane(target.lane, {});
    }
    saveToFile();
    return true;
  }
  saveToFile();
  return false;
}

int PuzzleStore::rewriteLane(int li, const std::vector<const lichess::Puzzle*>& fresh) {
  Lane& lane = lanes[li];
  char path[64];
  char temp[64];
  lanePath(lane.key, false, path, sizeof(path));
  lanePath(lane.key, true, temp, sizeof(temp));
  // Without an append mode the file is written anew: the unplayed lines of
  // the old file, then the new ones, into a file that takes the old one's place.
  int written = 0;
  {
    HalFile out;
    Storage.mkdir(chessfiles::DIR);
    if (!Storage.openFileForWrite("PUZZLES", temp, out)) {
      LOG_ERR("PUZZLES", "Cannot write %s", temp);
      return 0;
    }
    if (Storage.exists(path)) {
      HalFile in;
      if (Storage.openFileForRead("PUZZLES", path, in)) {
        LineReader reader(in, 0);
        std::string line;
        std::string field;
        while (reader.ok() && reader.next(line)) {
          if (!lineField(line, "id", field) || taken(lane, field.c_str())) continue;
          if (!lineField(line, "t", field)) continue;  // untagged lines are dropped
          line.push_back('\n');
          out.write(line.data(), line.size());
        }
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
  if (Storage.exists(path)) Storage.remove(path);
  if (!Storage.rename(temp, path)) LOG_ERR("PUZZLES", "Cannot rename %s", temp);
  lane.taken.clear();
  index();
  return written;
}

int PuzzleStore::add(const char* angle, const std::vector<lichess::Puzzle>& more) {
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
  int li = laneIndex(angle);
  if (li < 0) {
    if (lanes.size() >= static_cast<size_t>(MAX_LANES)) return 0;
    Lane lane;
    lichess::copyStr(lane.key, sizeof(lane.key), angle);
    lanes.push_back(lane);
    li = static_cast<int>(lanes.size()) - 1;
  }
  const int written = rewriteLane(li, fresh);
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
  std::vector<Lane>().swap(lanes);
  std::vector<Id>().swap(ids);
  std::vector<Result>().swap(results);
}
