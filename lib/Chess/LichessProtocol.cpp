#include "LichessProtocol.h"

#include <ArduinoJson.h>

#include <cstring>

namespace lichess {

void copyStr(char* dst, size_t dstSize, const char* src) {
  if (dstSize == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t n = 0;
  while (n < dstSize - 1 && src[n]) {
    dst[n] = src[n];
    ++n;
  }
  dst[n] = '\0';
}

void NdjsonSplitter::feed(const uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    const char c = static_cast<char>(data[i]);
    if (c != '\n') {
      if (buffer.size() < maxLine) buffer.push_back(c);
      continue;
    }
    // Strip a trailing carriage return.
    if (!buffer.empty() && buffer.back() == '\r') buffer.pop_back();
    if (buffer.empty()) {
      if (onHeartbeat) onHeartbeat(ctx);
    } else if (onLine) {
      onLine(ctx, buffer.c_str(), buffer.size());
    }
    buffer.clear();
  }
}

void NdjsonSplitter::flush() {
  if (!buffer.empty() && buffer.back() == '\r') buffer.pop_back();
  if (!buffer.empty() && onLine) onLine(ctx, buffer.c_str(), buffer.size());
  buffer.clear();
}

GameStatus statusFromName(const char* name) {
  if (!name) return GameStatus::Unknown;
  struct Entry {
    const char* name;
    GameStatus status;
  };
  static constexpr Entry TABLE[] = {
      {"created", GameStatus::Created},   {"started", GameStatus::Started},
      {"aborted", GameStatus::Aborted},   {"mate", GameStatus::Mate},
      {"resign", GameStatus::Resign},     {"stalemate", GameStatus::Stalemate},
      {"timeout", GameStatus::Timeout},   {"draw", GameStatus::Draw},
      {"outoftime", GameStatus::OutOfTime}, {"cheat", GameStatus::Cheat},
      {"noStart", GameStatus::NoStart},   {"unknownFinish", GameStatus::UnknownFinish},
      {"variantEnd", GameStatus::VariantEnd},
  };
  for (const Entry& e : TABLE) {
    if (strcmp(e.name, name) == 0) return e.status;
  }
  return GameStatus::Unknown;
}

bool isFinished(GameStatus s) { return s != GameStatus::Unknown && s != GameStatus::Created && s != GameStatus::Started; }

namespace {

void readPlayer(JsonVariantConst v, Player& p) {
  copyStr(p.name, NAME_LEN, v["name"] | "");
  p.rating = v["rating"] | 0;
  p.provisional = v["provisional"] | false;
  p.aiLevel = v["aiLevel"] | 0;
}

void readState(JsonVariantConst s, GameSnapshot& g) {
  g.moves = s["moves"] | "";
  g.wtimeMs = s["wtime"] | 0u;
  g.btimeMs = s["btime"] | 0u;
  g.status = statusFromName(s["status"] | "");
  const char* winner = s["winner"] | "";
  g.hasWinner = winner[0] != '\0';
  if (g.hasWinner) g.winner = strcmp(winner, "white") == 0 ? chess::Color::White : chess::Color::Black;
  g.whiteOffersDraw = s["wdraw"] | false;
  g.blackOffersDraw = s["bdraw"] | false;
}

// Case-insensitive compare of a user id against a player name.
bool sameUser(const char* id, const char* name) {
  if (!id || !name) return false;
  for (;; ++id, ++name) {
    const char a = (*id >= 'A' && *id <= 'Z') ? static_cast<char>(*id + 32) : *id;
    const char b = (*name >= 'A' && *name <= 'Z') ? static_cast<char>(*name + 32) : *name;
    if (a != b) return false;
    if (a == '\0') return true;
  }
}

}  // namespace

GameLineKind parseGameLine(const char* line, size_t len, GameSnapshot& g, const char* myUserId) {
  JsonDocument doc;
  if (deserializeJson(doc, line, len) != DeserializationError::Ok) return GameLineKind::Ignored;
  const char* type = doc["type"] | "";
  if (strcmp(type, "gameFull") == 0) {
    copyStr(g.id, GAME_ID_LEN, doc["id"] | "");
    g.rated = doc["rated"] | false;
    copyStr(g.speed, sizeof(g.speed), doc["speed"] | "");
    readPlayer(doc["white"], g.white);
    readPlayer(doc["black"], g.black);
    g.initialMs = doc["clock"]["initial"] | 0u;
    g.incrementMs = doc["clock"]["increment"] | 0u;
    // Board API ids are lowercase user ids; player names keep display case.
    const char* whiteId = doc["white"]["id"] | "";
    g.myColor = sameUser(myUserId, whiteId) || sameUser(myUserId, g.white.name) ? chess::Color::White
                                                                                : chess::Color::Black;
    g.opponentGone = false;
    g.claimWinInSeconds = -1;
    readState(doc["state"], g);
    return GameLineKind::GameFull;
  }
  if (strcmp(type, "gameState") == 0) {
    readState(doc, g);
    return GameLineKind::GameState;
  }
  if (strcmp(type, "opponentGone") == 0) {
    g.opponentGone = doc["gone"] | false;
    g.claimWinInSeconds = doc["claimWinInSeconds"] | -1;
    return GameLineKind::OpponentGone;
  }
  if (strcmp(type, "chatLine") == 0) return GameLineKind::ChatLine;
  return GameLineKind::Ignored;
}

bool parseAccount(const char* json, size_t len, Account& out) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len) != DeserializationError::Ok) return false;
  const char* username = doc["username"] | "";
  if (username[0] == '\0') return false;
  copyStr(out.username, NAME_LEN, username);
  copyStr(out.id, NAME_LEN, doc["id"] | username);
  JsonVariantConst perfs = doc["perfs"];
  out.bullet = perfs["bullet"]["rating"] | 0;
  out.blitz = perfs["blitz"]["rating"] | 0;
  out.rapid = perfs["rapid"]["rating"] | 0;
  out.classical = perfs["classical"]["rating"] | 0;
  out.correspondence = perfs["correspondence"]["rating"] | 0;
  return true;
}

bool parseNowPlaying(const char* json, size_t len, std::vector<OngoingGame>& out, size_t maxGames) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len) != DeserializationError::Ok) return false;
  JsonArrayConst games = doc["nowPlaying"].as<JsonArrayConst>();
  out.clear();
  out.reserve(maxGames);
  for (JsonVariantConst v : games) {
    if (out.size() >= maxGames) break;
    OngoingGame g;
    copyStr(g.gameId, GAME_ID_LEN, v["gameId"] | "");
    copyStr(g.opponent, NAME_LEN, v["opponent"]["username"] | "");
    g.opponentRating = v["opponent"]["rating"] | 0;
    g.myColor = strcmp(v["color"] | "white", "white") == 0 ? chess::Color::White : chess::Color::Black;
    copyStr(g.speed, sizeof(g.speed), v["speed"] | "");
    g.rated = v["rated"] | false;
    g.isMyTurn = v["isMyTurn"] | false;
    g.secondsLeft = v["secondsLeft"] | 0;
    copyStr(g.lastMove, sizeof(g.lastMove), v["lastMove"] | "");
    copyStr(g.fen, sizeof(g.fen), v["fen"] | "");
    out.push_back(g);
  }
  return true;
}

bool parseEventLine(const char* line, size_t len, StreamEvent& out) {
  JsonDocument doc;
  if (deserializeJson(doc, line, len) != DeserializationError::Ok) return false;
  out = StreamEvent{};
  const char* type = doc["type"] | "";
  if (strcmp(type, "gameStart") == 0 || strcmp(type, "gameFinish") == 0) {
    out.kind = type[4] == 'S' ? EventKind::GameStart : EventKind::GameFinish;
    copyStr(out.gameId, GAME_ID_LEN, doc["game"]["gameId"] | (doc["game"]["id"] | ""));
    const char* color = doc["game"]["color"] | "";
    if (color[0]) {
      out.hasColor = true;
      out.color = strcmp(color, "black") == 0 ? chess::Color::Black : chess::Color::White;
    }
    return true;
  }
  if (strcmp(type, "challenge") == 0) {
    out.kind = EventKind::Challenge;
    copyStr(out.challengeId, GAME_ID_LEN, doc["challenge"]["id"] | "");
    copyStr(out.challenger, NAME_LEN, doc["challenge"]["challenger"]["name"] | "");
    return true;
  }
  if (strcmp(type, "challengeDeclined") == 0) {
    out.kind = EventKind::ChallengeDeclined;
    copyStr(out.challengeId, GAME_ID_LEN, doc["challenge"]["id"] | "");
    return true;
  }
  if (strcmp(type, "challengeCanceled") == 0) {
    out.kind = EventKind::ChallengeCanceled;
    copyStr(out.challengeId, GAME_ID_LEN, doc["challenge"]["id"] | "");
    return true;
  }
  return true;
}

bool parseGameSummary(const char* line, size_t len, const char* myUserId, GameSummary& out) {
  JsonDocument doc;
  if (deserializeJson(doc, line, len) != DeserializationError::Ok) return false;
  const char* id = doc["id"] | "";
  if (id[0] == '\0') return false;
  // Only standard chess replays on this board.
  if (strcmp(doc["variant"] | "standard", "standard") != 0) return false;
  copyStr(out.id, GAME_ID_LEN, id);
  out.rated = doc["rated"] | false;
  copyStr(out.speed, sizeof(out.speed), doc["speed"] | "");
  // Export clocks are in seconds; the game stream uses milliseconds.
  out.initialMs = (doc["clock"]["initial"] | 0u) * 1000u;
  out.incrementMs = (doc["clock"]["increment"] | 0u) * 1000u;
  out.status = statusFromName(doc["status"] | "");
  const char* winner = doc["winner"] | "";
  out.hasWinner = winner[0] != '\0';
  if (out.hasWinner) out.winner = strcmp(winner, "white") == 0 ? chess::Color::White : chess::Color::Black;
  JsonVariantConst white = doc["players"]["white"];
  JsonVariantConst black = doc["players"]["black"];
  copyStr(out.white.name, NAME_LEN, white["user"]["name"] | "");
  out.white.rating = white["rating"] | 0;
  out.white.aiLevel = white["aiLevel"] | 0;
  copyStr(out.black.name, NAME_LEN, black["user"]["name"] | "");
  out.black.rating = black["rating"] | 0;
  out.black.aiLevel = black["aiLevel"] | 0;
  const char* whiteId = white["user"]["id"] | "";
  out.myColor = sameUser(myUserId, whiteId) ? chess::Color::White : chess::Color::Black;
  out.movesSan = doc["moves"] | "";
  return true;
}

namespace {
// One puzzle entry: {"game":{"pgn":...},"puzzle":{"id","rating","solution",...}}.
bool readPuzzleEntry(JsonVariantConst entry, Puzzle& out) {
  JsonVariantConst p = entry["puzzle"];
  const char* id = p["id"] | "";
  if (id[0] == '\0') return false;
  copyStr(out.id, GAME_ID_LEN, id);
  out.rating = p["rating"] | 0;
  copyStr(out.fen, sizeof(out.fen), p["fen"] | "");
  copyStr(out.lastMove, sizeof(out.lastMove), p["lastMove"] | "");
  out.pgn = entry["game"]["pgn"] | "";
  out.solution.clear();
  for (JsonVariantConst m : p["solution"].as<JsonArrayConst>()) {
    const char* uci = m | "";
    if (!uci[0]) continue;
    if (!out.solution.empty()) out.solution.push_back(' ');
    out.solution.append(uci);
  }
  return (out.fen[0] != '\0' || !out.pgn.empty()) && !out.solution.empty();
}
}  // namespace

int parsePuzzleBatch(const char* json, size_t len, std::vector<Puzzle>& out, size_t maxPuzzles) {
  // Only the fields the board needs survive the parse; a 30-puzzle batch is
  // about 36 KB of JSON and the full document would not fit next to Wi-Fi.
  JsonDocument filter;
  filter["puzzles"][0]["game"]["pgn"] = true;
  filter["puzzles"][0]["puzzle"]["id"] = true;
  filter["puzzles"][0]["puzzle"]["rating"] = true;
  filter["puzzles"][0]["puzzle"]["solution"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, json, len, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return 0;
  int added = 0;
  for (JsonVariantConst entry : doc["puzzles"].as<JsonArrayConst>()) {
    if (out.size() >= maxPuzzles) break;
    Puzzle z;
    if (!readPuzzleEntry(entry, z)) continue;
    out.push_back(z);
    ++added;
  }
  return added;
}

bool parsePuzzle(const char* json, size_t len, Puzzle& out) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len) != DeserializationError::Ok) return false;
  JsonVariantConst p = doc["puzzle"];
  const char* id = p["id"] | "";
  if (id[0] == '\0') return false;
  copyStr(out.id, GAME_ID_LEN, id);
  out.rating = p["rating"] | 0;
  copyStr(out.fen, sizeof(out.fen), p["fen"] | "");
  copyStr(out.lastMove, sizeof(out.lastMove), p["lastMove"] | "");
  out.pgn = doc["game"]["pgn"] | "";
  out.solution.clear();
  for (JsonVariantConst m : p["solution"].as<JsonArrayConst>()) {
    const char* uci = m | "";
    if (!uci[0]) continue;
    if (!out.solution.empty()) out.solution.push_back(' ');
    out.solution.append(uci);
  }
  return (out.fen[0] != '\0' || !out.pgn.empty()) && !out.solution.empty();
}

bool parseFollowingLine(const char* line, size_t len, Friend& out) {
  // The line is a full user profile; only the id and name are kept.
  JsonDocument filter;
  filter["id"] = true;
  filter["username"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, line, len, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return false;
  const char* id = doc["id"] | "";
  if (id[0] == '\0') return false;
  copyStr(out.id, sizeof(out.id), id);
  copyStr(out.name, sizeof(out.name), doc["username"] | id);
  out.online = false;
  out.playing = false;
  return true;
}

bool applyUsersStatus(const char* json, size_t len, std::vector<Friend>& list) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len) != DeserializationError::Ok) return false;
  for (JsonVariantConst v : doc.as<JsonArrayConst>()) {
    const char* id = v["id"] | "";
    for (auto& f : list) {
      if (strcmp(f.id, id) != 0) continue;
      f.online = v["online"] | false;
      f.playing = v["playing"] | false;
    }
  }
  return true;
}

bool parseStudyLine(const char* line, size_t len, StudyInfo& out) {
  JsonDocument doc;
  if (deserializeJson(doc, line, len) != DeserializationError::Ok) return false;
  const char* id = doc["id"] | "";
  if (id[0] == '\0') return false;
  copyStr(out.id, sizeof(out.id), id);
  copyStr(out.name, sizeof(out.name), doc["name"] | "");
  const uint64_t ms = doc["updatedAt"] | 0ull;
  out.updatedAt = static_cast<uint32_t>(ms / 1000);
  return true;
}

bool parseDashboard(const char* json, size_t len, PuzzleDashboard& out, size_t maxThemes) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len) != DeserializationError::Ok) return false;
  JsonVariantConst global = doc["global"];
  out.played = global["nb"] | 0;
  out.wins = global["firstWins"] | 0;
  out.performance = global["performance"] | 0;
  out.themes.clear();
  JsonObjectConst themes = doc["themes"].as<JsonObjectConst>();
  out.themes.reserve(themes.size() < maxThemes ? themes.size() : maxThemes);
  for (JsonPairConst kv : themes) {
    if (out.themes.size() >= maxThemes) break;
    ThemeStat t;
    copyStr(t.key, sizeof(t.key), kv.key().c_str());
    copyStr(t.name, sizeof(t.name), kv.value()["theme"] | kv.key().c_str());
    JsonVariantConst r = kv.value()["results"];
    t.played = r["nb"] | 0;
    t.wins = r["firstWins"] | 0;
    t.performance = r["performance"] | 0;
    out.themes.push_back(t);
  }
  // Weakest first: lowest performance, then most played.
  for (size_t i = 1; i < out.themes.size(); ++i) {
    ThemeStat key = out.themes[i];
    size_t j = i;
    while (j > 0 && (out.themes[j - 1].performance > key.performance ||
                     (out.themes[j - 1].performance == key.performance && out.themes[j - 1].played < key.played))) {
      out.themes[j] = out.themes[j - 1];
      --j;
    }
    out.themes[j] = key;
  }
  return true;
}

bool parseAnalysis(const char* json, size_t len, std::vector<AnalysisPly>& out, size_t maxPlies) {
  // The variations and comments are dropped; the board rebuilds the best move.
  JsonDocument filter;
  filter["analysis"][0]["eval"] = true;
  filter["analysis"][0]["mate"] = true;
  filter["analysis"][0]["best"] = true;
  filter["analysis"][0]["judgment"]["name"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, json, len, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return false;
  JsonArrayConst arr = doc["analysis"].as<JsonArrayConst>();
  out.clear();
  if (arr.size() == 0) return false;
  out.reserve(arr.size() < maxPlies ? arr.size() : maxPlies);
  for (JsonVariantConst v : arr) {
    if (out.size() >= maxPlies) break;
    AnalysisPly p;
    int cp = v["eval"] | 0;
    if (cp > 30000) cp = 30000;
    if (cp < -30000) cp = -30000;
    p.cp = static_cast<int16_t>(cp);
    p.mate = static_cast<int8_t>(v["mate"] | 0);
    const char* name = v["judgment"]["name"] | "";
    p.judgment = strcmp(name, "Inaccuracy") == 0 ? 1 : strcmp(name, "Mistake") == 0 ? 2 : strcmp(name, "Blunder") == 0 ? 3 : 0;
    copyStr(p.best, sizeof(p.best), v["best"] | "");
    out.push_back(p);
  }
  return true;
}

bool parseGameId(const char* line, size_t len, char* out, size_t outSize) {
  JsonDocument filter;
  filter["id"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, line, len, DeserializationOption::Filter(filter)) != DeserializationError::Ok) return false;
  const char* id = doc["id"] | "";
  if (id[0] == '\0') return false;
  copyStr(out, outSize, id);
  return true;
}

bool parseRatingDiff(const char* json, size_t len, chess::Color side, int& outDiff) {
  JsonDocument doc;
  if (deserializeJson(doc, json, len) != DeserializationError::Ok) return false;
  JsonVariantConst player = doc["players"][side == chess::Color::White ? "white" : "black"];
  if (player["ratingDiff"].isNull()) return false;
  outDiff = player["ratingDiff"] | 0;
  return true;
}

int countMoves(const std::string& moves) {
  int count = 0;
  bool inToken = false;
  for (const char c : moves) {
    if (c == ' ') {
      inToken = false;
    } else if (!inToken) {
      inToken = true;
      ++count;
    }
  }
  return count;
}

int replayMoves(const std::string& moves, chess::Position& out, chess::Move* lastMove, int maxMoves) {
  chess::Position start;
  return replayMovesFrom(start, moves, out, lastMove, maxMoves);
}

int replayMovesFrom(const chess::Position& base, const std::string& moves, chess::Position& out,
                    chess::Move* lastMove, int maxMoves) {
  out = base;
  if (lastMove) *lastMove = chess::Move{};
  int count = 0;
  size_t i = 0;
  while (i < moves.size() && (maxMoves < 0 || count < maxMoves)) {
    while (i < moves.size() && moves[i] == ' ') ++i;
    if (i >= moves.size()) break;
    char uci[8] = {};
    size_t n = 0;
    while (i < moves.size() && moves[i] != ' ' && n < sizeof(uci) - 1) uci[n++] = moves[i++];
    chess::Move m;
    if (!chess::moveFromUci(uci, m) || !out.isLegal(m)) return -1;
    out.makeMove(m);
    if (lastMove) *lastMove = m;
    ++count;
  }
  return count;
}

}  // namespace lichess
