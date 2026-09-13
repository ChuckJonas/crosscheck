#pragma once
// Lichess Board API client. One FreeRTOS task holds the long-lived NDJSON
// stream (event stream or game stream) and one task runs short requests
// (moves, seeks, account lookups). Results reach the UI loop as queued events.
#include <LichessProtocol.h>
#include <SecureHttpClient.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

class LichessClient {
 public:
  enum class EventType : uint8_t {
    AccountReady,
    AccountFailed,
    NowPlayingReady,
    NowPlayingFailed,
    GameUpdated,      // the game snapshot changed; read it with copyGame()
    GameStreamEnded,  // the game stream closed after the game finished
    StreamReconnecting,
    StreamFailed,  // gave up reconnecting
    GameStart,     // event stream: a game began (gameId set)
    GameFinish,
    ChallengeDeclined,
    SeekEnded,      // the seek request returned; code is the HTTP status
    RatingReady,    // code is our rating change for gameId
    AnalysisReady,  // code is the ply count; read with copyAnalysis()
    AnalysisFailed,
    RecentGamesReady,  // code is the number of games; read with copyRecentGames()
    RecentGamesFailed,
    PuzzleReady,  // read with copyPuzzle()
    PuzzleFailed,
    EngineGameReady,   // gameId is the engine game just created
    PuzzleBatchReady,  // code is the number of puzzles; read with copyPuzzleBatch()
    PuzzleBatchFailed,
    StudiesReady,  // code is the number of studies; read with copyStudies()
    StudiesFailed,
    StudyReady,  // gameId is the study id; its PGN is on the card
    StudyFailed,
    DashboardReady,  // read with copyDashboard()
    DashboardFailed,
    FriendsReady,  // code is the number of players; read with copyFriends()
    FriendsFailed,
    MoveFailed,  // the server rejected a move; resync from the snapshot
    CommandDone,
    CommandFailed,
  };

  struct Event {
    EventType type;
    int code = 0;
    char gameId[lichess::GAME_ID_LEN] = {};
    // Our color for GameStart: 0 white, 1 black, -1 unknown.
    int8_t color = -1;
    // Server reason text for a refused seek or challenge, when it sent one.
    char detail[64] = {};
  };

  enum class Cmd : uint8_t {
    None,
    FetchAccount,
    FetchNowPlaying,
    Move,
    Resign,
    Abort,
    DrawYes,
    DrawNo,
    ClaimVictory,
    Seek,
    Challenge,
    ChallengeAi,
    FetchRating,
    FetchAnalysis,
    KeepAlive,
    FetchRecentGames,
    FetchPuzzle,
    FetchPuzzleBatch,
    FetchStudies,
    FetchStudy,
    FetchPuzzleDashboard,
    FetchFriends,
  };

  static LichessClient& getInstance() {
    static LichessClient instance;
    return instance;
  }

  // Starts the tasks. userId is the lowercase Lichess id used to find our color.
  bool begin(const std::string& token, const std::string& userId);
  // Stops both tasks and closes the sockets. Safe to call twice.
  void end();
  bool running() const { return started; }
  // Lowercase Lichess id used to tell our color in a game stream.
  void setUserId(const std::string& id);

  // Two long-lived streams on their own tasks and connections: the event
  // stream stays open for the whole session so Lichess sees the player as
  // present; the game stream follows the open game.
  void streamEvents();
  void stopEvents();
  void streamGame(const char* gameId);
  void stopStream();

  // Queued commands. Return false when the queue is full.
  bool fetchAccount();
  bool fetchNowPlaying();
  bool sendMove(const char* gameId, const char* uci);
  bool resign(const char* gameId);
  bool abortGame(const char* gameId);
  bool offerDraw(const char* gameId, bool accept);
  bool claimVictory(const char* gameId);
  // Fetches up to max recent games of the signed-in user.
  bool fetchRecentGames(int max);
  // Fetches the next puzzle; difficulty is easiest, easier, normal, harder, or hardest.
  bool fetchPuzzle(const char* difficulty);
  // Fetches a batch of puzzles for offline solving. When resultsJson is not
  // empty it is posted first (needs the puzzle:write scope) and the reply
  // carries the new batch; otherwise the batch is fetched with GET.
  bool fetchPuzzleBatch(const char* angle, const char* difficulty, int count, const std::string& resultsJson);
  // Lists a user's studies (the signed-in user when username is empty);
  // StudiesReady carries the count. Public studies list without a token.
  bool fetchStudies(const char* username);
  // Downloads a study's PGN to the card (StudyStore paths); StudyReady follows.
  bool fetchStudy(const char* studyId);
  // Fetches the puzzle dashboard of the last `days` days.
  bool fetchPuzzleDashboard(int days);
  // Lists the players the user follows, online ones first (scope follow:read).
  bool fetchFriends();
  // Fetches our rating change for a finished game; RatingReady carries it.
  bool fetchRating(const char* gameId, chess::Color myColor);
  // Fetches the server analysis of a finished game; AnalysisReady follows.
  bool fetchAnalysis(const char* gameId);
  // While on, the command task sends a small request every few seconds so
  // the kept-alive connection survives between moves.
  void setKeepAlive(bool on) { keepAlive = on; }
  bool seek(int minutes, int increment, bool rated);
  // Closes the seek socket, which cancels the seek on the server.
  void cancelSeek();
  bool seeking() const { return seekActive; }
  bool challenge(const char* username, int minutes, int increment, bool rated);
  // Starts a game against the Lichess AI. CommandDone carries the game id.
  bool challengeAi(int level, int minutes, int increment);

  bool pollEvent(Event& out);

  // Copies of task-owned data, taken under the mutex.
  void copyGame(lichess::GameSnapshot& out);
  void copyAccount(lichess::Account& out);
  void copyNowPlaying(std::vector<lichess::OngoingGame>& out);
  void copyRecentGames(std::vector<lichess::GameSummary>& out);
  void copyPuzzle(lichess::Puzzle& out);
  void copyPuzzleBatch(std::vector<lichess::Puzzle>& out);
  void copyStudies(std::vector<lichess::StudyInfo>& out);
  void copyDashboard(lichess::PuzzleDashboard& out);
  void copyFriends(std::vector<lichess::Friend>& out);
  void copyAnalysis(std::vector<lichess::AnalysisPly>& out);
  // millis() of the last game stream line, for local clock extrapolation.
  uint32_t lastGameEventMillis() const { return lastGameEventMs; }
  uint32_t lastHeartbeatMillis() const { return lastHeartbeatMs; }

 private:
  enum class StreamKind : uint8_t { Game, Events };

  struct StreamSlot {
    StreamKind kind;
    const char* taskName;
    std::atomic<bool> wanted{false};
    std::atomic<uint32_t> generation{0};
    std::atomic<bool> abort{false};
    char gameId[lichess::GAME_ID_LEN] = {};  // game slot only, guarded by dataMutex
    TaskHandle_t task = nullptr;
    lichess::NdjsonSplitter splitter{8192};
    explicit StreamSlot(StreamKind kind, const char* taskName) : kind(kind), taskName(taskName) {}
  };

  struct Command {
    Cmd cmd = Cmd::None;
    char gameId[lichess::GAME_ID_LEN] = {};
    char uci[8] = {};
    char user[lichess::NAME_LEN] = {};
    int16_t minutes = 0;
    int16_t increment = 0;
    int16_t level = 0;
    bool rated = false;
    // Seek and engine requests carry the generation they were queued in; a
    // cancel bumps it so a not-yet-started request is dropped.
    uint32_t generation = 0;
  };

  LichessClient() = default;

  static void eventTaskTrampoline(void* self);
  static void gameTaskTrampoline(void* self);
  static void commandTaskTrampoline(void* self);
  void streamTaskLoop(StreamSlot& slot);
  void commandTaskLoop();
  void runStreamOnce(StreamSlot& slot, const char* gameId);
  void restartSlot(StreamSlot& slot);
  void runCommand(freeink::SecureHttpClient& http, const Command& c);
  // Runs one request and collects the body. Aborts when the client stops.
  int request(freeink::SecureHttpClient& http, const char* method, const std::string& path, const std::string& form,
              const char* accept, std::string& body, bool withAuth = true);
  void prepare(freeink::SecureHttpClient& http, const std::string& path, const char* accept, bool withAuth = true);
  void postEvent(EventType type, int code = 0, const char* gameId = nullptr, int8_t color = -1,
                 const char* detail = nullptr);
  bool enqueue(const Command& c);

  static void onGameLine(void* ctx, const char* line, size_t len);
  static void onRecentGameLine(void* ctx, const char* line, size_t len);
  static void onStudyLine(void* ctx, const char* line, size_t len);
  static void onFollowingLine(void* ctx, const char* line, size_t len);
  static void onAnalysedLine(void* ctx, const char* line, size_t len);
  static void onEventLine(void* ctx, const char* line, size_t len);
  static void onHeartbeat(void* ctx);

  std::string token;
  std::string userId;
  bool started = false;

  TaskHandle_t commandTask = nullptr;
  QueueHandle_t commandQueue = nullptr;
  QueueHandle_t eventQueue = nullptr;
  SemaphoreHandle_t dataMutex = nullptr;
  // Tasks that have not returned from their loop yet. end() never deletes a
  // task from outside: a killed task would leak its TLS session or hold the mutex.
  std::atomic<int> tasksAlive{0};
  std::atomic<bool> stopRequested{false};

  StreamSlot eventSlot{StreamKind::Events, "LichessEvents"};
  StreamSlot gameSlot{StreamKind::Game, "LichessGame"};

  std::atomic<bool> seekAbort{false};
  std::atomic<bool> seekActive{false};
  std::atomic<uint32_t> seekGeneration{0};
  std::atomic<bool> keepAlive{false};

  lichess::GameSnapshot game;
  lichess::Account account;
  std::vector<lichess::OngoingGame> nowPlaying;
  std::vector<lichess::GameSummary> recentGames;
  lichess::Puzzle puzzle;
  std::vector<lichess::Puzzle> puzzleBatch;
  std::vector<lichess::StudyInfo> studies;
  lichess::PuzzleDashboard dashboard;
  std::vector<lichess::Friend> friends;
  std::vector<lichess::AnalysisPly> analysis;
  std::string pendingResults;  // body of the next batch POST, guarded by dataMutex
  std::atomic<uint32_t> lastGameEventMs{0};
  std::atomic<uint32_t> lastHeartbeatMs{0};
};

#define LICHESS LichessClient::getInstance()
