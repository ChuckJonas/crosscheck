#include "StudyStore.h"

#include <Logging.h>
#include <Memory.h>

#include <cstdio>
#include <cstring>

namespace {
constexpr const char* DIR = chessfiles::STUDIES_DIR;
constexpr size_t CHUNK = 512;
}  // namespace

void StudyStore::pgnPath(const char* id, char* out, size_t size) { snprintf(out, size, "%s/%s.pgn", DIR, id); }
void StudyStore::partPath(const char* id, char* out, size_t size) { snprintf(out, size, "%s/%s.part", DIR, id); }
void StudyStore::indexPath(const char* id, char* out, size_t size) { snprintf(out, size, "%s/%s.json", DIR, id); }

void StudyStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["studies"].to<JsonArray>();
  for (const auto& e : list) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = e.info.id;
    o["name"] = e.info.name;
    o["updated"] = e.info.updatedAt;
    o["chapters"] = e.chapters;
  }
}

bool StudyStore::fromJson(JsonVariantConst doc) {
  list.clear();
  JsonArrayConst arr = doc["studies"].as<JsonArrayConst>();
  list.reserve(arr.size() < static_cast<size_t>(MAX_STUDIES) ? arr.size() : MAX_STUDIES);
  for (JsonVariantConst v : arr) {
    if (list.size() >= static_cast<size_t>(MAX_STUDIES)) break;
    Entry e;
    lichess::copyStr(e.info.id, sizeof(e.info.id), v["id"] | "");
    lichess::copyStr(e.info.name, sizeof(e.info.name), v["name"] | "");
    e.info.updatedAt = v["updated"] | 0u;
    e.chapters = v["chapters"] | 0;
    if (e.info.id[0]) list.push_back(e);
  }
  return true;
}

void StudyStore::load() {
  loadFromFile();
  char path[48];
  for (auto& e : list) {
    pgnPath(e.info.id, path, sizeof(path));
    e.saved = Storage.exists(path);
  }
}

StudyStore::Entry* StudyStore::findEntry(const char* id) {
  for (auto& e : list) {
    if (strcmp(e.info.id, id) == 0) return &e;
  }
  return nullptr;
}

const StudyStore::Entry* StudyStore::find(const char* id) const {
  for (const auto& e : list) {
    if (strcmp(e.info.id, id) == 0) return &e;
  }
  return nullptr;
}

void StudyStore::merge(const std::vector<lichess::StudyInfo>& fromApi) {
  std::vector<Entry> next;
  next.reserve(MAX_STUDIES);
  // Saved studies first, in their old order, then the rest of the new list.
  for (const auto& e : list) {
    if (e.saved) next.push_back(e);
  }
  for (const auto& info : fromApi) {
    if (next.size() >= static_cast<size_t>(MAX_STUDIES)) break;
    bool known = false;
    for (auto& e : next) {
      if (strcmp(e.info.id, info.id) == 0) {
        lichess::copyStr(e.info.name, sizeof(e.info.name), info.name);
        e.info.updatedAt = info.updatedAt;
        known = true;
      }
    }
    if (!known) {
      Entry e;
      e.info = info;
      next.push_back(e);
    }
  }
  list.swap(next);
  saveToFile();
}

void StudyStore::addOrUpdate(const lichess::StudyInfo& info) {
  Entry* e = findEntry(info.id);
  if (e) {
    if (info.name[0]) lichess::copyStr(e->info.name, sizeof(e->info.name), info.name);
  } else {
    if (list.size() >= static_cast<size_t>(MAX_STUDIES)) list.erase(list.begin());
    Entry fresh;
    fresh.info = info;
    list.push_back(fresh);
  }
  saveToFile();
}

int StudyStore::indexStudy(const char* id, char* nameOut, size_t nameSize) {
  if (nameOut && nameSize) nameOut[0] = '\0';
  char path[48];
  pgnPath(id, path, sizeof(path));
  HalFile file;
  if (!Storage.openFileForRead("STUDY", path, file)) return -1;
  auto buf = makeUniqueNoThrow<char[]>(CHUNK);
  if (!buf) {
    LOG_ERR("STUDY", "OOM: scan buffer");
    return -1;
  }
  // Chapters start at a line "[Event ...". The name comes from ChapterName,
  // else from Event. The study name comes from the first StudyName tag.
  JsonDocument doc;
  JsonArray chapters = doc["ch"].to<JsonArray>();
  doc["last"] = 0;
  std::vector<ChapterInfo> found;
  found.reserve(16);
  std::string line;
  uint32_t lineStart = 0;
  uint32_t offset = 0;
  bool haveChapter = false;
  ChapterInfo current;
  bool haveChapterName = false;
  auto finish = [&]() {
    if (haveChapter && found.size() < static_cast<size_t>(MAX_CHAPTERS)) found.push_back(current);
  };
  auto tagValue = [&](const char* key, char* out, size_t size) {
    // line: [Key "value"]
    const size_t klen = strlen(key);
    if (line.size() < klen + 4 || line.compare(1, klen, key) != 0 || line[klen + 1] != ' ' || line[klen + 2] != '"') {
      return false;
    }
    const size_t start = klen + 3;
    const size_t end = line.find('"', start);
    const size_t n = (end == std::string::npos ? line.size() : end) - start;
    snprintf(out, size, "%.*s", static_cast<int>(n), line.c_str() + start);
    return true;
  };
  int n;
  while ((n = file.read(buf.get(), CHUNK)) > 0) {
    for (int i = 0; i < n; ++i) {
      const char c = buf[i];
      ++offset;
      if (c != '\n') {
        if (line.size() < 160) line.push_back(c);
        continue;
      }
      if (line.compare(0, 7, "[Event ") == 0) {
        finish();
        current = ChapterInfo();
        current.at = lineStart;
        haveChapter = true;
        haveChapterName = false;
        tagValue("Event", current.name, sizeof(current.name));
      } else if (haveChapter && !haveChapterName && line.compare(0, 13, "[ChapterName ") == 0) {
        haveChapterName = tagValue("ChapterName", current.name, sizeof(current.name));
      } else if (nameOut && !nameOut[0] && line.compare(0, 11, "[StudyName ") == 0) {
        tagValue("StudyName", nameOut, nameSize);
      }
      line.clear();
      lineStart = offset;
    }
  }
  finish();
  for (size_t i = 0; i < found.size(); ++i) {
    found[i].len = (i + 1 < found.size() ? found[i + 1].at : offset) - found[i].at;
    JsonObject o = chapters.add<JsonObject>();
    o["n"] = found[i].name;
    o["at"] = found[i].at;
    o["len"] = found[i].len;
  }
  indexPath(id, path, sizeof(path));
  Storage.mkdir(DIR);
  if (!writeDocToFile(path, doc)) return -1;
  Entry* e = findEntry(id);
  if (e) {
    e->chapters = static_cast<uint16_t>(found.size());
    e->saved = true;
    saveToFile();
  }
  LOG_INF("STUDY", "%s: %d chapters indexed", id, static_cast<int>(found.size()));
  return static_cast<int>(found.size());
}

bool StudyStore::loadChapters(const char* id, std::vector<ChapterInfo>& out, int& last) const {
  char path[48];
  indexPath(id, path, sizeof(path));
  JsonDocument doc;
  out.clear();
  last = 0;
  if (!readDocFromFile(path, doc)) return false;
  last = doc["last"] | 0;
  JsonArrayConst arr = doc["ch"].as<JsonArrayConst>();
  out.reserve(arr.size());
  for (JsonVariantConst v : arr) {
    ChapterInfo c;
    lichess::copyStr(c.name, sizeof(c.name), v["n"] | "");
    c.at = v["at"] | 0u;
    c.len = v["len"] | 0u;
    out.push_back(c);
  }
  if (last < 0 || last >= static_cast<int>(out.size())) last = 0;
  return !out.empty();
}

void StudyStore::setLastChapter(const char* id, int index) const {
  char path[48];
  indexPath(id, path, sizeof(path));
  JsonDocument doc;
  if (!readDocFromFile(path, doc)) return;
  if ((doc["last"] | 0) == index) return;
  doc["last"] = index;
  writeDocToFile(path, doc);
}

bool StudyStore::readSpan(const char* id, uint32_t at, uint32_t len, std::string& out) const {
  out.clear();
  char path[48];
  pgnPath(id, path, sizeof(path));
  HalFile file;
  if (!Storage.openFileForRead("STUDY", path, file) || !file.seek(at)) return false;
  auto buf = makeUniqueNoThrow<char[]>(CHUNK);
  if (!buf) return false;
  out.reserve(len);
  while (len > 0) {
    const int n = file.read(buf.get(), len < CHUNK ? len : CHUNK);
    if (n <= 0) break;
    out.append(buf.get(), static_cast<size_t>(n));
    len -= static_cast<uint32_t>(n);
  }
  return true;
}

void StudyStore::remove(const char* id) {
  char path[48];
  pgnPath(id, path, sizeof(path));
  if (Storage.exists(path)) Storage.remove(path);
  indexPath(id, path, sizeof(path));
  if (Storage.exists(path)) Storage.remove(path);
  partPath(id, path, sizeof(path));
  if (Storage.exists(path)) Storage.remove(path);
  Entry* e = findEntry(id);
  if (e) {
    e->saved = false;
    e->chapters = 0;
    saveToFile();
  }
}

void StudyStore::clearAll() { std::vector<Entry>().swap(list); }

bool ChapterSource::open(const char* id, uint32_t at, uint32_t len) {
  char path[48];
  StudyStore::pgnPath(id, path, sizeof(path));
  left = 0;
  if (!Storage.openFileForRead("STUDY", path, file) || !file.seek(at)) return false;
  left = len;
  return true;
}

int ChapterSource::read(char* buf, size_t max) {
  if (left == 0) return 0;
  const int n = file.read(buf, left < max ? left : max);
  if (n <= 0) {
    left = 0;
    return 0;
  }
  left -= static_cast<uint32_t>(n);
  return n;
}
