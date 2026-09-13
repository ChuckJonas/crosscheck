#pragma once
#include <ArduinoJson.h>
#include <ChessFiles.h>
#include <HalStorage.h>
#include <LichessProtocol.h>
#include <PersistableStore.h>
#include <Pgn.h>

#include <string>
#include <vector>

// A study saved on the SD card: /.crosspoint/studies/<id>.pgn holds the
// export, <id>.json the chapter offsets and the last chapter read. The list
// of known studies lives in /.crosspoint/studies.json.
struct ChapterInfo {
  char name[48] = {};
  uint32_t at = 0;   // byte offset of the chapter in the PGN file
  uint32_t len = 0;  // bytes up to the next chapter
};

class StudyStore : public PersistableStore<StudyStore> {
 public:
  struct Entry {
    lichess::StudyInfo info;
    uint16_t chapters = 0;
    bool saved = false;  // the PGN file is on the card
  };

 private:
  std::vector<Entry> list;

  StudyStore() = default;
  ~StudyStore() = default;
  friend class PersistableStore<StudyStore>;

  Entry* findEntry(const char* id);

 public:
  static constexpr int MAX_STUDIES = 40;
  static constexpr int MAX_CHAPTERS = 64;

  static const char* getFilePath() { return chessfiles::STUDIES_PATH; }
  static const char* dirPath() { return chessfiles::STUDIES_DIR; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  static void pgnPath(const char* id, char* out, size_t size);
  static void partPath(const char* id, char* out, size_t size);
  static void indexPath(const char* id, char* out, size_t size);

  // Reads the list and checks which PGN files are on the card.
  void load();
  const std::vector<Entry>& entries() const { return list; }
  int count() const { return static_cast<int>(list.size()); }
  const Entry* find(const char* id) const;
  // The list from Lichess replaces the entries that are not saved; saved
  // entries keep their place and take the new name. Saves the list.
  void merge(const std::vector<lichess::StudyInfo>& fromApi);
  // Adds a study opened by id (or updates its name). Saves the list.
  void addOrUpdate(const lichess::StudyInfo& info);
  // Scans a downloaded PGN file for its chapters and writes the index file.
  // Returns the chapter count, or -1 when the file cannot be read. The study
  // name from the file goes to nameOut when the file has one.
  int indexStudy(const char* id, char* nameOut, size_t nameSize);
  bool loadChapters(const char* id, std::vector<ChapterInfo>& out, int& last) const;
  void setLastChapter(const char* id, int index) const;
  // Reads part of the PGN file, for a node's comment.
  bool readSpan(const char* id, uint32_t at, uint32_t len, std::string& out) const;
  // Deletes the files and marks the entry as not saved. Saves the list.
  void remove(const char* id);
  void clearAll();
};

#define STUDY_STORE StudyStore::getInstance()

// A chapter of a saved study as a PGN source for the tree parser.
class ChapterSource final : public chess::pgn::Source {
 public:
  bool open(const char* id, uint32_t at, uint32_t len);
  int read(char* buf, size_t max) override;

 private:
  HalFile file;
  uint32_t left = 0;
};
