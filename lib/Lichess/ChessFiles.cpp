#include "ChessFiles.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstdio>

namespace chessfiles {

namespace {
void moveFile(const char* from, const char* to) {
  if (!Storage.exists(from) || Storage.exists(to)) return;
  if (Storage.rename(from, to)) {
    LOG_INF("CHESS", "Moved %s to %s", from, to);
  } else {
    LOG_ERR("CHESS", "Could not move %s to %s", from, to);
  }
}
}  // namespace

void migrate() {
  Storage.mkdir(DIR);
  moveFile("/.crosspoint/lichess.json", SETTINGS_PATH);
  moveFile("/.crosspoint/puzzles.json", PUZZLES_PATH);
  moveFile("/.crosspoint/puzzles.ndjson", PUZZLE_LINES_PATH);
  if (Storage.exists("/.crosspoint/puzzles.tmp")) Storage.remove("/.crosspoint/puzzles.tmp");
  moveFile("/.crosspoint/studies.json", STUDIES_PATH);
  const char* oldDir = "/.crosspoint/studies";
  if (Storage.exists(oldDir)) {
    Storage.mkdir(STUDIES_DIR);
    const std::vector<String> names = Storage.listFiles(oldDir, 200);
    for (const String& name : names) {
      char from[80];
      char to[80];
      snprintf(from, sizeof(from), "%s/%s", oldDir, name.c_str());
      snprintf(to, sizeof(to), "%s/%s", STUDIES_DIR, name.c_str());
      moveFile(from, to);
    }
    Storage.rmdir(oldDir);
  }
}

}  // namespace chessfiles
