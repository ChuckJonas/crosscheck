#pragma once
// Lichess Board API message parsing. No Arduino or network includes, so this
// file builds in the native test environment. ArduinoJson is header-only.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Position.h"

namespace lichess {

// Splits a byte stream into newline-terminated lines. Empty lines are the
// Lichess keep-alive heartbeat and are reported through onHeartbeat.
class NdjsonSplitter {
 public:
  using LineFn = void (*)(void* ctx, const char* line, size_t len);
  using HeartbeatFn = void (*)(void* ctx);

  explicit NdjsonSplitter(size_t maxLine = 8192) : maxLine(maxLine) { buffer.reserve(512); }
  void setCallbacks(void* ctx, LineFn onLine, HeartbeatFn onHeartbeat) {
    this->ctx = ctx;
    this->onLine = onLine;
    this->onHeartbeat = onHeartbeat;
  }
  void feed(const uint8_t* data, size_t len);
  // Emits a final line that had no newline, at the end of a response.
  void flush();
  void reset() { buffer.clear(); }

 private:
  std::string buffer;
  size_t maxLine;
  void* ctx = nullptr;
  LineFn onLine = nullptr;
  HeartbeatFn onHeartbeat = nullptr;
};

enum class GameStatus : uint8_t {
  Unknown,
  Created,
  Started,
  Aborted,
  Mate,
  Resign,
  Stalemate,
  Timeout,
  Draw,
  OutOfTime,
  Cheat,
  NoStart,
  UnknownFinish,
  VariantEnd,
};

GameStatus statusFromName(const char* name);
bool isFinished(GameStatus s);

constexpr size_t GAME_ID_LEN = 16;
constexpr size_t NAME_LEN = 32;

struct Player {
  char name[NAME_LEN] = {};
  int rating = 0;
  bool provisional = false;
  // Lichess engine strength 1..8 when this side is the computer, else 0.
  int aiLevel = 0;
};

// Full state of one game as the Board API reports it.
struct GameSnapshot {
  char id[GAME_ID_LEN] = {};
  Player white;
  Player black;
  chess::Color myColor = chess::Color::White;
  bool rated = false;
  char speed[16] = {};
  // Clock settings in milliseconds. initialMs is 0 for correspondence games.
  uint32_t initialMs = 0;
  uint32_t incrementMs = 0;
  uint32_t wtimeMs = 0;
  uint32_t btimeMs = 0;
  GameStatus status = GameStatus::Unknown;
  // Winner color when status is a decisive finish.
  bool hasWinner = false;
  chess::Color winner = chess::Color::White;
  bool whiteOffersDraw = false;
  bool blackOffersDraw = false;
  // UCI moves separated by spaces, as sent by Lichess.
  std::string moves;
  // Opponent left the game; claimWinInSeconds counts down to a claimable win.
  bool opponentGone = false;
  int claimWinInSeconds = -1;
};

enum class GameLineKind : uint8_t { Ignored, GameFull, GameState, ChatLine, OpponentGone };

// Parses one line of the game stream into g. Returns the line kind.
GameLineKind parseGameLine(const char* line, size_t len, GameSnapshot& g, const char* myUserId);

struct Account {
  char id[NAME_LEN] = {};
  char username[NAME_LEN] = {};
  int bullet = 0;
  int blitz = 0;
  int rapid = 0;
  int classical = 0;
  int correspondence = 0;
};

bool parseAccount(const char* json, size_t len, Account& out);

struct OngoingGame {
  char gameId[GAME_ID_LEN] = {};
  char opponent[NAME_LEN] = {};
  int opponentRating = 0;
  chess::Color myColor = chess::Color::White;
  char speed[16] = {};
  bool rated = false;
  bool isMyTurn = false;
  int secondsLeft = 0;
  char lastMove[6] = {};
  char fen[96] = {};
};

bool parseNowPlaying(const char* json, size_t len, std::vector<OngoingGame>& out, size_t maxGames = 8);

enum class EventKind : uint8_t { Ignored, GameStart, GameFinish, Challenge, ChallengeDeclined, ChallengeCanceled };

struct StreamEvent {
  EventKind kind = EventKind::Ignored;
  char gameId[GAME_ID_LEN] = {};
  // Our color in a GameStart event, when Lichess includes it.
  bool hasColor = false;
  chess::Color color = chess::Color::White;
  char challengeId[GAME_ID_LEN] = {};
  char challenger[NAME_LEN] = {};
};

bool parseEventLine(const char* line, size_t len, StreamEvent& out);

// One study from GET /api/study/by/{username} (one NDJSON line each).
struct StudyInfo {
  char id[GAME_ID_LEN] = {};
  char name[64] = {};
  uint32_t updatedAt = 0;  // seconds since the epoch
};
bool parseStudyLine(const char* line, size_t len, StudyInfo& out);

// A player the user follows, from GET /api/rel/following, with the presence
// flags of GET /api/users/status.
struct Friend {
  char id[NAME_LEN] = {};
  char name[NAME_LEN] = {};
  bool online = false;
  bool playing = false;
};
bool parseFollowingLine(const char* line, size_t len, Friend& out);
// Applies the online and playing flags of a users-status reply to the list.
bool applyUsersStatus(const char* json, size_t len, std::vector<Friend>& list);

// Results per puzzle theme from GET /api/puzzle/dashboard/{days}.
struct ThemeStat {
  char key[24] = {};   // the angle for a batch download
  char name[32] = {};  // display name from Lichess
  uint16_t played = 0;
  uint16_t wins = 0;
  int16_t performance = 0;
};
struct PuzzleDashboard {
  uint16_t played = 0;
  uint16_t wins = 0;
  int16_t performance = 0;
  std::vector<ThemeStat> themes;  // weakest first
};
bool parseDashboard(const char* json, size_t len, PuzzleDashboard& out, size_t maxThemes = 40);

// One finished game from the games export (one NDJSON line per game).
struct GameSummary {
  char id[GAME_ID_LEN] = {};
  Player white;
  Player black;
  chess::Color myColor = chess::Color::White;
  bool rated = false;
  char speed[16] = {};
  uint32_t initialMs = 0;
  uint32_t incrementMs = 0;
  GameStatus status = GameStatus::Unknown;
  bool hasWinner = false;
  chess::Color winner = chess::Color::White;
  // Moves in standard algebraic notation, as Lichess exports them.
  std::string movesSan;
  // Lichess has computer analysis for this game.
  bool analysed = false;
};

// Parses one line of GET /api/games/user/{username}. myUserId picks our color.
bool parseGameSummary(const char* line, size_t len, const char* myUserId, GameSummary& out);

// A puzzle from GET /api/puzzle/next or /api/puzzle/daily.
struct Puzzle {
  char id[GAME_ID_LEN] = {};
  int rating = 0;
  // Position to solve, when the response carries one (the daily puzzle does).
  char fen[96] = {};
  // Otherwise the game's moves in standard notation; the puzzle position is
  // the position after all of them, and the solver moves next.
  std::string pgn;
  // The opponent's last move, for the highlight (daily puzzle only).
  char lastMove[6] = {};
  // Solution in UCI, solver and opponent moves alternating, space separated.
  std::string solution;
};

bool parsePuzzle(const char* json, size_t len, Puzzle& out);

// Parses the "puzzles" array of GET or POST /api/puzzle/batch/{angle}.
// Appends up to maxPuzzles entries to out and returns how many were added.
int parsePuzzleBatch(const char* json, size_t len, std::vector<Puzzle>& out, size_t maxPuzzles);

// Reads the rating change for one side from a JSON game export. Returns false
// when the export carries no rating change (casual game, or not finished).
bool parseRatingDiff(const char* json, size_t len, chess::Color side, int& outDiff);

// One ply of a server analysis: the evaluation after the move, and the
// judgment of the move when a better one existed.
struct AnalysisPly {
  int16_t cp = 0;        // centipawns from White's side; not used when mate is set
  int8_t mate = 0;       // moves to mate, positive when White mates
  uint8_t judgment = 0;  // 0 none, 1 inaccuracy, 2 mistake, 3 blunder
  char best[6] = {};     // the better move, UCI, when judged
};
// Reads the analysis array of GET /game/export/{id}?evals=true. False without one.
bool parseAnalysis(const char* json, size_t len, std::vector<AnalysisPly>& out, size_t maxPlies = 400);
// Reads the id of one game export line.
bool parseGameId(const char* line, size_t len, char* out, size_t outSize);

// Applies the UCI move list of a snapshot to a fresh position, at most
// maxMoves of them (-1 for all). Returns the number of moves applied, or -1
// when a move was illegal.
int replayMoves(const std::string& moves, chess::Position& out, chess::Move* lastMove, int maxMoves = -1);
// Same, starting from base instead of the start position.
int replayMovesFrom(const chess::Position& base, const std::string& moves, chess::Position& out,
                    chess::Move* lastMove, int maxMoves = -1);
// Number of moves in a UCI move list.
int countMoves(const std::string& moves);

// Copies a C string into a fixed buffer with truncation and termination.
void copyStr(char* dst, size_t dstSize, const char* src);

}  // namespace lichess
