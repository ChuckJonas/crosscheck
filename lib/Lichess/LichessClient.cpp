#include "LichessClient.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstring>
#include <memory>

#include "StudyStore.h"

namespace {
constexpr const char* BASE_URL = "https://lichess.org";
constexpr const char* USER_AGENT = "crosscheck/0.1 (CrossPoint X4 Pro)";
constexpr uint32_t STREAM_TIMEOUT_MS = 15000;  // heartbeats arrive every ~6 s
constexpr uint32_t COMMAND_TIMEOUT_MS = 12000;
constexpr uint32_t SEEK_TIMEOUT_MS = 60000;    // seek bodies only carry heartbeats
constexpr uint32_t STREAM_TASK_STACK = 12288;  // each of the two stream tasks
constexpr uint32_t COMMAND_TASK_STACK = 12288;
constexpr int COMMAND_QUEUE_LEN = 8;
constexpr int EVENT_QUEUE_LEN = 16;
constexpr uint32_t RECONNECT_MAX_MS = 30000;
constexpr int RECONNECT_ATTEMPTS = 8;
// Lichess drops an idle keep-alive connection after roughly ten seconds.
constexpr uint32_t KEEP_ALIVE_MS = 5000;

class MutexLock {
 public:
  explicit MutexLock(SemaphoreHandle_t m) : m(m) { xSemaphoreTake(m, portMAX_DELAY); }
  ~MutexLock() { xSemaphoreGive(m); }

 private:
  SemaphoreHandle_t m;
};
}  // namespace

bool LichessClient::begin(const std::string& newToken, const std::string& newUserId) {
  if (started) return true;
  token = newToken;
  userId = newUserId;
  if (!dataMutex) dataMutex = xSemaphoreCreateMutex();
  if (!commandQueue) commandQueue = xQueueCreate(COMMAND_QUEUE_LEN, sizeof(Command));
  if (!eventQueue) eventQueue = xQueueCreate(EVENT_QUEUE_LEN, sizeof(Event));
  if (!dataMutex || !commandQueue || !eventQueue) {
    LOG_ERR("LICHESS", "OOM creating queues");
    return false;
  }
  xQueueReset(commandQueue);
  xQueueReset(eventQueue);
  stopRequested = false;
  seekAbort = false;
  seekActive = false;
  eventSlot.wanted = false;
  eventSlot.abort = false;
  gameSlot.wanted = false;
  gameSlot.abort = false;
  // Tasks from a previous session may still be closing a socket.
  const uint32_t waitUntil = millis() + 10000;
  while (tasksAlive > 0 && millis() < waitUntil) vTaskDelay(pdMS_TO_TICKS(20));
  if (tasksAlive > 0) {
    LOG_ERR("LICHESS", "Previous tasks still running; refusing to start");
    return false;
  }
  tasksAlive = 3;
  if (xTaskCreate(&eventTaskTrampoline, eventSlot.taskName, STREAM_TASK_STACK, this, 1, &eventSlot.task) != pdPASS) {
    LOG_ERR("LICHESS", "Failed to create event stream task");
    tasksAlive = 0;
    return false;
  }
  if (xTaskCreate(&gameTaskTrampoline, gameSlot.taskName, STREAM_TASK_STACK, this, 1, &gameSlot.task) != pdPASS) {
    LOG_ERR("LICHESS", "Failed to create game stream task");
    stopRequested = true;
    tasksAlive -= 2;
    return false;
  }
  if (xTaskCreate(&commandTaskTrampoline, "LichessCmd", COMMAND_TASK_STACK, this, 1, &commandTask) != pdPASS) {
    LOG_ERR("LICHESS", "Failed to create command task");
    stopRequested = true;
    tasksAlive--;
    return false;
  }
  started = true;
  LOG_INF("LICHESS", "Client started, free heap %u, max alloc %u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return true;
}

void LichessClient::end() {
  if (!started) return;
  stopRequested = true;
  eventSlot.abort = true;
  gameSlot.abort = true;
  seekAbort = true;
  // Wake the tasks so they observe the stop flag and delete themselves.
  if (eventSlot.task) xTaskNotifyGive(eventSlot.task);
  if (gameSlot.task) xTaskNotifyGive(gameSlot.task);
  Command wake;
  if (commandQueue) xQueueSend(commandQueue, &wake, 0);
  // Requests poll the abort flag every few milliseconds; only a TLS handshake
  // in progress can hold a task longer. Such a task finishes on its own and
  // begin() waits for it.
  const uint32_t deadline = millis() + 3000;
  while (tasksAlive > 0 && millis() < deadline) vTaskDelay(pdMS_TO_TICKS(20));
  if (tasksAlive > 0) LOG_ERR("LICHESS", "%d task(s) still closing", tasksAlive.load());
  started = false;
  // Release the session's data so the reader gets the heap back.
  if (dataMutex) {
    MutexLock lock(dataMutex);
    std::vector<lichess::GameSummary>().swap(recentGames);
    std::vector<lichess::OngoingGame>().swap(nowPlaying);
    std::string().swap(game.moves);
    std::vector<lichess::Puzzle>().swap(puzzleBatch);
    std::vector<lichess::StudyInfo>().swap(studies);
    std::vector<lichess::ThemeStat>().swap(dashboard.themes);
    std::vector<lichess::Friend>().swap(friends);
    std::vector<lichess::AnalysisPly>().swap(analysis);
    std::string().swap(pendingResults);
  }
  LOG_INF("LICHESS", "Client stopped, free heap %u, max alloc %u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

void LichessClient::eventTaskTrampoline(void* self) {
  auto* client = static_cast<LichessClient*>(self);
  client->streamTaskLoop(client->eventSlot);
  client->tasksAlive--;
  vTaskDelete(nullptr);
}

void LichessClient::gameTaskTrampoline(void* self) {
  auto* client = static_cast<LichessClient*>(self);
  client->streamTaskLoop(client->gameSlot);
  client->tasksAlive--;
  vTaskDelete(nullptr);
}

void LichessClient::commandTaskTrampoline(void* self) {
  auto* client = static_cast<LichessClient*>(self);
  client->commandTaskLoop();
  client->tasksAlive--;
  vTaskDelete(nullptr);
}

void LichessClient::setUserId(const std::string& id) {
  if (!dataMutex) {
    userId = id;
    return;
  }
  MutexLock lock(dataMutex);
  userId = id;
}

void LichessClient::postEvent(EventType type, int code, const char* gameId, int8_t color, const char* detail) {
  Event e;
  e.type = type;
  e.code = code;
  e.color = color;
  if (gameId) lichess::copyStr(e.gameId, sizeof(e.gameId), gameId);
  if (detail) lichess::copyStr(e.detail, sizeof(e.detail), detail);
  if (xQueueSend(eventQueue, &e, 0) != pdTRUE) LOG_ERR("LICHESS", "Event queue full, dropped %d", (int)type);
}

bool LichessClient::pollEvent(Event& out) { return eventQueue && xQueueReceive(eventQueue, &out, 0) == pdTRUE; }

// --- streams -----------------------------------------------------------------

void LichessClient::restartSlot(StreamSlot& slot) {
  slot.generation++;
  slot.abort = true;
  if (slot.task) xTaskNotifyGive(slot.task);
}

void LichessClient::streamEvents() {
  if (eventSlot.wanted) return;  // already open or reconnecting
  eventSlot.wanted = true;
  restartSlot(eventSlot);
}

void LichessClient::stopEvents() {
  eventSlot.wanted = false;
  restartSlot(eventSlot);
}

void LichessClient::streamGame(const char* gameId) {
  {
    MutexLock lock(dataMutex);
    lichess::copyStr(gameSlot.gameId, sizeof(gameSlot.gameId), gameId);
  }
  gameSlot.wanted = true;
  restartSlot(gameSlot);
}

void LichessClient::stopStream() {
  gameSlot.wanted = false;
  restartSlot(gameSlot);
}

void LichessClient::onGameLine(void* ctx, const char* line, size_t len) {
  auto* self = static_cast<LichessClient*>(ctx);
  lichess::GameLineKind kind;
  {
    MutexLock lock(self->dataMutex);
    kind = lichess::parseGameLine(line, len, self->game, self->userId.c_str());
  }
  if (kind == lichess::GameLineKind::GameFull || kind == lichess::GameLineKind::GameState) {
    // Only these lines carry clock values; the timestamp anchors the extrapolation.
    self->lastGameEventMs = millis();
  }
  if (kind == lichess::GameLineKind::GameFull || kind == lichess::GameLineKind::GameState ||
      kind == lichess::GameLineKind::OpponentGone) {
    self->postEvent(EventType::GameUpdated);
  }
}

void LichessClient::onRecentGameLine(void* ctx, const char* line, size_t len) {
  auto* out = static_cast<std::vector<lichess::GameSummary>*>(ctx);
  lichess::GameSummary g;
  if (lichess::parseGameSummary(line, len, LichessClient::getInstance().userId.c_str(), g)) out->push_back(g);
}

void LichessClient::onAnalysedLine(void* ctx, const char* line, size_t len) {
  auto* games = static_cast<std::vector<lichess::GameSummary>*>(ctx);
  char id[lichess::GAME_ID_LEN];
  if (!lichess::parseGameId(line, len, id, sizeof(id))) return;
  for (auto& g : *games) {
    if (strcmp(g.id, id) == 0) g.analysed = true;
  }
}

void LichessClient::onFollowingLine(void* ctx, const char* line, size_t len) {
  auto* out = static_cast<std::vector<lichess::Friend>*>(ctx);
  if (out->size() >= 60) return;
  lichess::Friend f;
  if (lichess::parseFollowingLine(line, len, f)) out->push_back(f);
}

void LichessClient::onStudyLine(void* ctx, const char* line, size_t len) {
  auto* out = static_cast<std::vector<lichess::StudyInfo>*>(ctx);
  if (out->size() >= 40) return;
  lichess::StudyInfo s;
  if (lichess::parseStudyLine(line, len, s)) out->push_back(s);
}

void LichessClient::onEventLine(void* ctx, const char* line, size_t len) {
  auto* self = static_cast<LichessClient*>(ctx);
  lichess::StreamEvent ev;
  if (!lichess::parseEventLine(line, len, ev)) return;
  switch (ev.kind) {
    case lichess::EventKind::GameStart:
      self->postEvent(EventType::GameStart, 0, ev.gameId, ev.hasColor ? (ev.color == chess::Color::Black ? 1 : 0) : -1);
      break;
    case lichess::EventKind::GameFinish:
      self->postEvent(EventType::GameFinish, 0, ev.gameId);
      break;
    case lichess::EventKind::ChallengeDeclined:
    case lichess::EventKind::ChallengeCanceled:
      self->postEvent(EventType::ChallengeDeclined, 0, ev.challengeId);
      break;
    default:
      break;
  }
}

void LichessClient::prepare(freeink::SecureHttpClient& http, const std::string& path, const char* accept,
                            bool withAuth) {
  http.setInsecure();
  http.setUserAgent(USER_AGENT);
  http.begin(std::string(BASE_URL) + path);
  if (withAuth && !token.empty()) http.addHeader("Authorization", "Bearer " + token);
  http.addHeader("Accept", accept);
}

void LichessClient::runStreamOnce(StreamSlot& slot, const char* gameId) {
  freeink::SecureHttpClient http;
  http.setReuse(false);
  http.setTimeout(STREAM_TIMEOUT_MS);
  const bool isGame = slot.kind == StreamKind::Game;
  std::string path = isGame ? std::string("/api/board/game/stream/") + gameId : "/api/stream/event";
  prepare(http, path, "application/x-ndjson");
  slot.splitter.reset();
  slot.splitter.setCallbacks(this, isGame ? &onGameLine : &onEventLine, nullptr);
  const uint32_t myGeneration = slot.generation;
  const auto onData = [this, &slot](const uint8_t* data, size_t len) {
    slot.splitter.feed(data, len);
    return !slot.abort.load();
  };
  const auto shouldAbort = [&slot]() { return slot.abort.load(); };
  LOG_INF("LICHESS", "Stream open: %s (heap %u)", path.c_str(), ESP.getFreeHeap());
  const int status = http.GET(onData, shouldAbort);
  http.end();
  LOG_INF("LICHESS", "Stream closed: %s status %d, complete %d, generation %u/%u", isGame ? "game" : "events", status,
          http.responseComplete(), myGeneration, slot.generation.load());
  if (myGeneration != slot.generation) return;
  if (isGame && status == 200 && http.responseComplete()) {
    // Lichess closes the game stream once the game is over.
    bool finished;
    {
      MutexLock lock(dataMutex);
      finished = lichess::isFinished(game.status);
    }
    if (finished) {
      postEvent(EventType::GameStreamEnded);
      slot.wanted = false;
    }
  }
  // A 4xx will not improve with retries. The game id tells the game screen
  // whether its own stream failed or only the event stream.
  if (status >= 400 && status < 500) {
    postEvent(EventType::StreamFailed, status, isGame ? gameId : nullptr);
    slot.wanted = false;
  }
}

void LichessClient::streamTaskLoop(StreamSlot& slot) {
  int attempt = 0;
  while (!stopRequested) {
    // Clear the abort flag before reading the request, so a request that
    // arrives after this point sets the flag and ends the run it targets.
    slot.abort = false;
    const uint32_t generation = slot.generation;
    if (!slot.wanted) {
      attempt = 0;
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
      continue;
    }
    char gameId[lichess::GAME_ID_LEN];
    {
      MutexLock lock(dataMutex);
      memcpy(gameId, slot.gameId, sizeof(gameId));
    }
    runStreamOnce(slot, gameId);
    if (stopRequested) break;
    if (generation != slot.generation) {
      attempt = 0;  // a new request replaced this stream
      continue;
    }
    if (!slot.wanted) continue;
    // Unexpected close: reconnect with backoff and replay from gameFull.
    if (attempt >= RECONNECT_ATTEMPTS) {
      postEvent(EventType::StreamFailed, -1, slot.kind == StreamKind::Game ? gameId : nullptr);
      slot.wanted = false;
      attempt = 0;
      continue;
    }
    uint32_t delayMs = 1000u << attempt;
    if (delayMs > RECONNECT_MAX_MS) delayMs = RECONNECT_MAX_MS;
    ++attempt;
    if (slot.kind == StreamKind::Game) postEvent(EventType::StreamReconnecting, attempt);
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
    const uint32_t until = millis() + delayMs;
    while (millis() < until && !stopRequested && generation == slot.generation) vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// --- commands ----------------------------------------------------------------

bool LichessClient::enqueue(const Command& c) {
  if (!commandQueue || !started) return false;
  if (xQueueSend(commandQueue, &c, 0) != pdTRUE) {
    LOG_ERR("LICHESS", "Command queue full");
    return false;
  }
  return true;
}

bool LichessClient::fetchAccount() {
  Command c;
  c.cmd = Cmd::FetchAccount;
  return enqueue(c);
}

bool LichessClient::fetchNowPlaying() {
  Command c;
  c.cmd = Cmd::FetchNowPlaying;
  return enqueue(c);
}

bool LichessClient::sendMove(const char* gameId, const char* uci) {
  Command c;
  c.cmd = Cmd::Move;
  lichess::copyStr(c.gameId, sizeof(c.gameId), gameId);
  lichess::copyStr(c.uci, sizeof(c.uci), uci);
  return enqueue(c);
}

bool LichessClient::resign(const char* gameId) {
  Command c;
  c.cmd = Cmd::Resign;
  lichess::copyStr(c.gameId, sizeof(c.gameId), gameId);
  return enqueue(c);
}

bool LichessClient::abortGame(const char* gameId) {
  Command c;
  c.cmd = Cmd::Abort;
  lichess::copyStr(c.gameId, sizeof(c.gameId), gameId);
  return enqueue(c);
}

bool LichessClient::offerDraw(const char* gameId, bool accept) {
  Command c;
  c.cmd = accept ? Cmd::DrawYes : Cmd::DrawNo;
  lichess::copyStr(c.gameId, sizeof(c.gameId), gameId);
  return enqueue(c);
}

bool LichessClient::claimVictory(const char* gameId) {
  Command c;
  c.cmd = Cmd::ClaimVictory;
  lichess::copyStr(c.gameId, sizeof(c.gameId), gameId);
  return enqueue(c);
}

bool LichessClient::fetchRecentGames(int max) {
  Command c;
  c.cmd = Cmd::FetchRecentGames;
  c.level = static_cast<int16_t>(max);
  return enqueue(c);
}

bool LichessClient::fetchPuzzleBatch(const char* angle, const char* difficulty, int count,
                                     const std::string& resultsJson) {
  Command c;
  c.cmd = Cmd::FetchPuzzleBatch;
  lichess::copyStr(c.user, sizeof(c.user), angle && angle[0] ? angle : "mix");
  lichess::copyStr(c.uci, sizeof(c.uci), difficulty ? difficulty : "normal");
  c.level = static_cast<int16_t>(count);
  {
    MutexLock lock(dataMutex);
    pendingResults = resultsJson;
  }
  return enqueue(c);
}

bool LichessClient::fetchStudies(const char* username) {
  Command c;
  c.cmd = Cmd::FetchStudies;
  lichess::copyStr(c.user, sizeof(c.user), username ? username : "");
  return enqueue(c);
}

bool LichessClient::fetchStudy(const char* studyId) {
  Command c;
  c.cmd = Cmd::FetchStudy;
  lichess::copyStr(c.gameId, sizeof(c.gameId), studyId);
  return enqueue(c);
}

bool LichessClient::fetchFriends() {
  Command c;
  c.cmd = Cmd::FetchFriends;
  return enqueue(c);
}

bool LichessClient::fetchPuzzleDashboard(int days) {
  Command c;
  c.cmd = Cmd::FetchPuzzleDashboard;
  c.level = static_cast<int16_t>(days);
  return enqueue(c);
}

bool LichessClient::fetchAnalysis(const char* gameId) {
  Command c;
  c.cmd = Cmd::FetchAnalysis;
  lichess::copyStr(c.gameId, sizeof(c.gameId), gameId);
  return enqueue(c);
}

bool LichessClient::fetchRating(const char* gameId, chess::Color myColor) {
  Command c;
  c.cmd = Cmd::FetchRating;
  lichess::copyStr(c.gameId, sizeof(c.gameId), gameId);
  c.level = myColor == chess::Color::Black ? 1 : 0;
  return enqueue(c);
}

bool LichessClient::seek(int minutes, int increment, bool rated) {
  Command c;
  c.cmd = Cmd::Seek;
  c.minutes = static_cast<int16_t>(minutes);
  c.increment = static_cast<int16_t>(increment);
  c.rated = rated;
  c.generation = seekGeneration;
  return enqueue(c);
}

bool LichessClient::challengeAi(int level, int minutes, int increment) {
  Command c;
  c.cmd = Cmd::ChallengeAi;
  c.level = static_cast<int16_t>(level);
  c.minutes = static_cast<int16_t>(minutes);
  c.increment = static_cast<int16_t>(increment);
  c.generation = seekGeneration;
  return enqueue(c);
}

void LichessClient::cancelSeek() {
  seekGeneration++;  // drops a seek or engine request still waiting in the queue
  seekAbort = true;
}

bool LichessClient::challenge(const char* username, int minutes, int increment, bool rated) {
  Command c;
  c.cmd = Cmd::Challenge;
  lichess::copyStr(c.user, sizeof(c.user), username);
  c.minutes = static_cast<int16_t>(minutes);
  c.increment = static_cast<int16_t>(increment);
  c.rated = rated;
  return enqueue(c);
}

int LichessClient::request(freeink::SecureHttpClient& http, const char* method, const std::string& path,
                           const std::string& form, const char* accept, std::string& body, bool withAuth) {
  prepare(http, path, accept, withAuth);
  if (strcmp(method, "POST") == 0) http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  body.clear();
  const auto onData = [&body](const uint8_t* data, size_t len) {
    if (body.size() < 65536) body.append(reinterpret_cast<const char*>(data), len);
    return true;
  };
  const auto shouldAbort = [this]() { return stopRequested.load(); };
  return http.sendRequest(method, reinterpret_cast<const uint8_t*>(form.data()), form.size(), onData, shouldAbort);
}

void LichessClient::runCommand(freeink::SecureHttpClient& http, const Command& c) {
  http.setTimeout(COMMAND_TIMEOUT_MS);
  const std::string gamePath = std::string("/api/board/game/") + c.gameId;
  std::string body;
  switch (c.cmd) {
    case Cmd::FetchAccount: {
      const int status = request(http, "GET", "/api/account", "", "application/json", body);
      lichess::Account parsed;
      const bool ok = status == 200 && lichess::parseAccount(body.c_str(), body.size(), parsed);
      if (ok) {
        MutexLock lock(dataMutex);
        account = parsed;
      }
      postEvent(ok ? EventType::AccountReady : EventType::AccountFailed, status);
      break;
    }
    case Cmd::FetchNowPlaying: {
      const int status = request(http, "GET", "/api/account/playing", "", "application/json", body);
      std::vector<lichess::OngoingGame> parsed;
      const bool ok = status == 200 && lichess::parseNowPlaying(body.c_str(), body.size(), parsed);
      if (ok) {
        MutexLock lock(dataMutex);
        nowPlaying.swap(parsed);
      }
      postEvent(ok ? EventType::NowPlayingReady : EventType::NowPlayingFailed, status);
      break;
    }
    case Cmd::Move: {
      const int status = request(http, "POST", gamePath + "/move/" + c.uci, "", "application/json", body);
      LOG_INF("LICHESS", "Move %s -> %d", c.uci, status);
      if (status != 200) postEvent(EventType::MoveFailed, status, c.gameId);
      break;
    }
    case Cmd::Resign:
    case Cmd::Abort:
    case Cmd::DrawYes:
    case Cmd::DrawNo:
    case Cmd::ClaimVictory: {
      const char* suffix = c.cmd == Cmd::Resign    ? "/resign"
                           : c.cmd == Cmd::Abort   ? "/abort"
                           : c.cmd == Cmd::DrawYes ? "/draw/yes"
                           : c.cmd == Cmd::DrawNo  ? "/draw/no"
                                                   : "/claim-victory";
      const int status = request(http, "POST", gamePath + suffix, "", "application/json", body);
      LOG_INF("LICHESS", "%s -> %d", suffix, status);
      postEvent(status == 200 ? EventType::CommandDone : EventType::CommandFailed, status, c.gameId);
      break;
    }
    case Cmd::Seek: {
      if (c.generation != seekGeneration) break;  // cancelled before it started; nothing to report
      char form[96];
      snprintf(form, sizeof(form), "time=%d&increment=%d&rated=%s&color=random&variant=standard", c.minutes,
               c.increment, c.rated ? "true" : "false");
      // The seek holds its socket open until matched, so it gets its own client.
      freeink::SecureHttpClient seekHttp;
      seekHttp.setTimeout(SEEK_TIMEOUT_MS);
      seekHttp.setReuse(false);
      prepare(seekHttp, "/api/board/seek", "text/plain");
      seekHttp.addHeader("Content-Type", "application/x-www-form-urlencoded");
      seekAbort = false;
      seekActive = true;
      // A refused seek explains itself in the body; keep the first bytes for the log.
      body.clear();
      const auto onData = [this, &body](const uint8_t* data, size_t len) {
        if (body.size() < 200) body.append(reinterpret_cast<const char*>(data), len);
        return !seekAbort.load();
      };
      const auto shouldAbort = [this]() { return seekAbort.load() || stopRequested.load(); };
      const int status =
          seekHttp.sendRequest("POST", reinterpret_cast<const uint8_t*>(form), strlen(form), onData, shouldAbort);
      seekHttp.end();
      seekActive = false;
      LOG_INF("LICHESS", "Seek ended -> %d (aborted %d) %s", status, seekAbort.load(),
              status == 200 ? "" : body.c_str());
      postEvent(EventType::SeekEnded, seekAbort ? -2 : status, nullptr, -1, status == 200 ? nullptr : body.c_str());
      break;
    }
    case Cmd::Challenge: {
      char form[96];
      snprintf(form, sizeof(form), "rated=%s&clock.limit=%d&clock.increment=%d&color=random",
               c.rated ? "true" : "false", c.minutes * 60, c.increment);
      const int status = request(http, "POST", std::string("/api/challenge/") + c.user, form, "application/json", body);
      LOG_INF("LICHESS", "Challenge %s -> %d %s", c.user, status, status == 200 ? "" : body.c_str());
      postEvent(status == 200 ? EventType::CommandDone : EventType::CommandFailed, status, nullptr, -1,
                status == 200 ? nullptr : body.c_str());
      break;
    }
    case Cmd::FetchAnalysis: {
      const int status = request(
          http, "GET", std::string("/game/export/") + c.gameId + "?evals=true&moves=false&clocks=false&opening=false",
          "", "application/json", body);
      std::vector<lichess::AnalysisPly> parsed;
      const bool ok = status == 200 && lichess::parseAnalysis(body.c_str(), body.size(), parsed);
      LOG_INF("LICHESS", "Analysis %s -> %d, %d plies", c.gameId, status, static_cast<int>(parsed.size()));
      if (ok) {
        MutexLock lock(dataMutex);
        analysis.swap(parsed);
      }
      postEvent(ok ? EventType::AnalysisReady : EventType::AnalysisFailed,
                ok ? static_cast<int>(analysis.size()) : status, c.gameId);
      break;
    }
    case Cmd::FetchRating: {
      const int status = request(http, "GET", std::string("/game/export/") + c.gameId, "", "application/json", body);
      int diff = 0;
      const chess::Color side = c.level == 1 ? chess::Color::Black : chess::Color::White;
      if (status == 200 && lichess::parseRatingDiff(body.c_str(), body.size(), side, diff)) {
        postEvent(EventType::RatingReady, diff, c.gameId);
      }
      break;
    }
    case Cmd::FetchRecentGames: {
      // One NDJSON line per game, parsed as it arrives so no whole-body buffer
      // is needed; long games would otherwise overflow it.
      char path[160];
      snprintf(path, sizeof(path), "/api/games/user/%s?max=%d&moves=true&opening=false&clocks=false", userId.c_str(),
               c.level);
      std::vector<lichess::GameSummary> parsed;
      parsed.reserve(c.level);
      lichess::NdjsonSplitter lines(8192);
      lines.setCallbacks(&parsed, &onRecentGameLine, nullptr);
      prepare(http, path, "application/x-ndjson");
      const auto onData = [&lines](const uint8_t* data, size_t len) {
        lines.feed(data, len);
        return true;
      };
      const auto shouldAbort = [this]() { return stopRequested.load(); };
      const int status = http.sendRequest("GET", nullptr, 0, onData, shouldAbort);
      lines.flush();
      if (status != 200) {
        postEvent(EventType::RecentGamesFailed, status);
        break;
      }
      if (!parsed.empty()) {
        // A second, small list of the analysed games marks the rows that have one.
        snprintf(path, sizeof(path), "/api/games/user/%s?max=%d&analysed=true&moves=false&opening=false&clocks=false",
                 userId.c_str(), c.level);
        lichess::NdjsonSplitter ids(2048);
        ids.setCallbacks(&parsed, &onAnalysedLine, nullptr);
        prepare(http, path, "application/x-ndjson");
        const auto onIds = [&ids](const uint8_t* data, size_t len) {
          ids.feed(data, len);
          return true;
        };
        const int st = http.sendRequest("GET", nullptr, 0, onIds, shouldAbort);
        ids.flush();
        int tagged = 0;
        for (const auto& g : parsed) tagged += g.analysed ? 1 : 0;
        LOG_INF("LICHESS", "Analysed list -> %d, %d of %d games tagged", st, tagged, static_cast<int>(parsed.size()));
      }
      const int count = static_cast<int>(parsed.size());
      {
        MutexLock lock(dataMutex);
        recentGames.swap(parsed);
      }
      postEvent(EventType::RecentGamesReady, count);
      break;
    }
    case Cmd::FetchPuzzleBatch: {
      std::string results;
      {
        MutexLock lock(dataMutex);
        results.swap(pendingResults);
      }
      char path[96];
      snprintf(path, sizeof(path), "/api/puzzle/batch/%s?nb=%d&difficulty=%s", c.user, c.level, c.uci);
      int status;
      if (results.empty()) {
        status = request(http, "GET", path, "", "application/json", body);
        if (status == 401 || status == 403) status = request(http, "GET", path, "", "application/json", body, false);
      } else {
        // The POST records the solved puzzles and returns a fresh batch.
        prepare(http, path, "application/json");
        http.addHeader("Content-Type", "application/json");
        body.clear();
        const auto onData = [&body](const uint8_t* data, size_t len) {
          if (body.size() < 65536) body.append(reinterpret_cast<const char*>(data), len);
          return true;
        };
        const auto shouldAbort = [this]() { return stopRequested.load(); };
        status = http.sendRequest("POST", reinterpret_cast<const uint8_t*>(results.data()), results.size(), onData,
                                  shouldAbort);
        if (status == 401 || status == 403) {
          // No puzzle:write scope: the results cannot be recorded, but the
          // batch still comes back for an anonymous post.
          LOG_ERR("LICHESS", "Puzzle results refused (%d); the token lacks puzzle:write", status);
          prepare(http, path, "application/json", false);
          http.addHeader("Content-Type", "application/json");
          body.clear();
          status = http.sendRequest("POST", reinterpret_cast<const uint8_t*>(results.data()), results.size(), onData,
                                    shouldAbort);
        }
      }
      std::vector<lichess::Puzzle> parsed;
      const int count = status == 200 ? lichess::parsePuzzleBatch(body.c_str(), body.size(), parsed, c.level) : 0;
      LOG_INF("LICHESS", "Puzzle batch -> %d, %d puzzles (%d results sent)", status, count, results.empty() ? 0 : 1);
      if (status != 200 || (count == 0 && c.level > 0)) {
        postEvent(EventType::PuzzleBatchFailed, status == 200 ? -3 : status, nullptr, -1,
                  status == 200 ? "empty or unreadable batch" : body.c_str());
        break;
      }
      {
        MutexLock lock(dataMutex);
        puzzleBatch.swap(parsed);
      }
      postEvent(EventType::PuzzleBatchReady, count);
      break;
    }
    case Cmd::FetchStudies: {
      char path[96];
      snprintf(path, sizeof(path), "/api/study/by/%s", c.user[0] ? c.user : userId.c_str());
      std::vector<lichess::StudyInfo> parsed;
      parsed.reserve(16);
      int status = 0;
      // A token without study:read is refused; the public studies still list anonymously.
      for (int attempt = 0; attempt < 2; ++attempt) {
        parsed.clear();
        lichess::NdjsonSplitter lines(2048);
        lines.setCallbacks(&parsed, &onStudyLine, nullptr);
        prepare(http, path, "application/x-ndjson", attempt == 0);
        const auto onData = [&lines](const uint8_t* data, size_t len) {
          lines.feed(data, len);
          return true;
        };
        const auto shouldAbort = [this]() { return stopRequested.load(); };
        status = http.sendRequest("GET", nullptr, 0, onData, shouldAbort);
        lines.flush();
        if (!(status == 401 || status == 403) || token.empty()) break;
        LOG_INF("LICHESS", "Study list refused with the token (%d); retrying anonymously", status);
      }
      LOG_INF("LICHESS", "Studies -> %d, %d studies", status, static_cast<int>(parsed.size()));
      if (status != 200) {
        postEvent(EventType::StudiesFailed, status);
        break;
      }
      const int count = static_cast<int>(parsed.size());
      {
        MutexLock lock(dataMutex);
        studies.swap(parsed);
      }
      postEvent(EventType::StudiesReady, count);
      break;
    }
    case Cmd::FetchStudy: {
      // The PGN streams straight to the card; a study can be hundreds of KB.
      char path[128];
      snprintf(path, sizeof(path), "/api/study/%s.pgn?clocks=false&comments=true&variations=true&orientation=true",
               c.gameId);
      char part[48];
      StudyStore::partPath(c.gameId, part, sizeof(part));
      Storage.mkdir(StudyStore::dirPath());
      int status = 0;
      size_t bytes = 0;
      for (int attempt = 0; attempt < 2; ++attempt) {
        bytes = 0;
        {
          HalFile out;
          if (!Storage.openFileForWrite("LICHESS", part, out)) {
            status = -1;
            break;
          }
          prepare(http, path, "application/x-chess-pgn", attempt == 0);
          const auto onData = [&out, &bytes](const uint8_t* data, size_t len) {
            out.write(data, len);
            bytes += len;
            return true;
          };
          const auto shouldAbort = [this]() { return stopRequested.load(); };
          status = http.sendRequest("GET", nullptr, 0, onData, shouldAbort);
          out.flush();
        }
        if (!(status == 401 || status == 403) || token.empty()) break;
        LOG_INF("LICHESS", "Study refused with the token (%d); retrying anonymously", status);
      }
      LOG_INF("LICHESS", "Study %s -> %d, %u bytes", c.gameId, status, static_cast<unsigned>(bytes));
      if (status == 200 && bytes > 0) {
        char final[48];
        StudyStore::pgnPath(c.gameId, final, sizeof(final));
        if (Storage.exists(final)) Storage.remove(final);
        if (!Storage.rename(part, final)) status = -2;
      }
      if (status != 200 || bytes == 0) {
        if (Storage.exists(part)) Storage.remove(part);
        postEvent(EventType::StudyFailed, status, c.gameId);
        break;
      }
      postEvent(EventType::StudyReady, static_cast<int>(bytes / 1024), c.gameId);
      break;
    }
    case Cmd::FetchFriends: {
      std::vector<lichess::Friend> parsed;
      parsed.reserve(24);
      lichess::NdjsonSplitter lines(8192);
      lines.setCallbacks(&parsed, &onFollowingLine, nullptr);
      prepare(http, "/api/rel/following", "application/x-ndjson");
      const auto onData = [&lines](const uint8_t* data, size_t len) {
        lines.feed(data, len);
        return true;
      };
      const auto shouldAbort = [this]() { return stopRequested.load(); };
      const int status = http.sendRequest("GET", nullptr, 0, onData, shouldAbort);
      lines.flush();
      LOG_INF("LICHESS", "Following -> %d, %d players", status, static_cast<int>(parsed.size()));
      if (status != 200) {
        postEvent(EventType::FriendsFailed, status);
        break;
      }
      if (!parsed.empty()) {
        // Presence comes from a second, public request for all ids at once.
        std::string ids;
        for (const auto& f : parsed) {
          if (!ids.empty()) ids.push_back(',');
          ids += f.id;
        }
        const int st = request(http, "GET", "/api/users/status?ids=" + ids, "", "application/json", body);
        if (st == 200) lichess::applyUsersStatus(body.c_str(), body.size(), parsed);
        // Online players first, then by name.
        auto before = [](const lichess::Friend& a, const lichess::Friend& b) {
          if (a.online != b.online) return a.online;
          return strcasecmp(a.name, b.name) < 0;
        };
        for (size_t i = 1; i < parsed.size(); ++i) {
          lichess::Friend key = parsed[i];
          size_t j = i;
          while (j > 0 && before(key, parsed[j - 1])) {
            parsed[j] = parsed[j - 1];
            --j;
          }
          parsed[j] = key;
        }
      }
      const int count = static_cast<int>(parsed.size());
      {
        MutexLock lock(dataMutex);
        friends.swap(parsed);
      }
      postEvent(EventType::FriendsReady, count);
      break;
    }
    case Cmd::FetchPuzzleDashboard: {
      char path[64];
      snprintf(path, sizeof(path), "/api/puzzle/dashboard/%d", c.level);
      const int status = request(http, "GET", path, "", "application/json", body);
      lichess::PuzzleDashboard parsed;
      const bool ok = status == 200 && lichess::parseDashboard(body.c_str(), body.size(), parsed);
      LOG_INF("LICHESS", "Puzzle dashboard -> %d, %d themes", status, static_cast<int>(parsed.themes.size()));
      if (ok) {
        MutexLock lock(dataMutex);
        dashboard = parsed;
      }
      postEvent(ok ? EventType::DashboardReady : EventType::DashboardFailed, status);
      break;
    }
    case Cmd::KeepAlive: {
      const int status = request(http, "GET", "/api/account/playing?nb=1", "", "application/json", body);
      LOG_DBG("LICHESS", "Keep-alive -> %d", status);
      break;
    }
    case Cmd::ChallengeAi: {
      if (c.generation != seekGeneration) break;  // cancelled before it started
      char form[96];
      snprintf(form, sizeof(form), "level=%d&clock.limit=%d&clock.increment=%d&color=random", c.level, c.minutes * 60,
               c.increment);
      const int status = request(http, "POST", "/api/challenge/ai", form, "application/json", body);
      // The response is the created game; its id opens the game stream.
      char id[lichess::GAME_ID_LEN] = {};
      if (status == 200 || status == 201) {
        const size_t at = body.find("\"id\":\"");
        if (at != std::string::npos) {
          const size_t start = at + 6;
          const size_t end = body.find('"', start);
          if (end != std::string::npos) lichess::copyStr(id, sizeof(id), body.substr(start, end - start).c_str());
        }
      }
      LOG_INF("LICHESS", "AI challenge -> %d, game %s", status, id);
      postEvent(id[0] ? EventType::EngineGameReady : EventType::CommandFailed, status, id);
      break;
    }
    default:
      break;
  }
}

void LichessClient::commandTaskLoop() {
  // One kept-alive client serves every short request, so a move costs a
  // round trip and not a TLS handshake.
  auto http = std::unique_ptr<freeink::SecureHttpClient>(new (std::nothrow) freeink::SecureHttpClient());
  if (!http) {
    LOG_ERR("LICHESS", "OOM: command client");
    return;
  }
  http->setReuse(true);
  Command c;
  uint32_t lastRequestMs = millis();
  while (!stopRequested) {
    if (xQueueReceive(commandQueue, &c, pdMS_TO_TICKS(500)) != pdTRUE) {
      if (keepAlive && millis() - lastRequestMs >= KEEP_ALIVE_MS) {
        c.cmd = Cmd::KeepAlive;
      } else {
        continue;
      }
    }
    if (stopRequested || c.cmd == Cmd::None) continue;
    runCommand(*http, c);
    lastRequestMs = millis();
  }
  http->end();
}

// --- snapshots ---------------------------------------------------------------

void LichessClient::copyGame(lichess::GameSnapshot& out) {
  MutexLock lock(dataMutex);
  out = game;
}

void LichessClient::copyAccount(lichess::Account& out) {
  MutexLock lock(dataMutex);
  out = account;
}

void LichessClient::copyNowPlaying(std::vector<lichess::OngoingGame>& out) {
  MutexLock lock(dataMutex);
  out = nowPlaying;
}

void LichessClient::copyAnalysis(std::vector<lichess::AnalysisPly>& out) {
  MutexLock lock(dataMutex);
  out = analysis;
}

void LichessClient::copyFriends(std::vector<lichess::Friend>& out) {
  MutexLock lock(dataMutex);
  out = friends;
}

void LichessClient::copyStudies(std::vector<lichess::StudyInfo>& out) {
  MutexLock lock(dataMutex);
  out = studies;
}

void LichessClient::copyDashboard(lichess::PuzzleDashboard& out) {
  MutexLock lock(dataMutex);
  out = dashboard;
}

void LichessClient::copyRecentGames(std::vector<lichess::GameSummary>& out) {
  MutexLock lock(dataMutex);
  out = recentGames;
}

void LichessClient::copyPuzzleBatch(std::vector<lichess::Puzzle>& out) {
  MutexLock lock(dataMutex);
  out.swap(puzzleBatch);
  puzzleBatch.clear();
}
