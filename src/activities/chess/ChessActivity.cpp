#include "ChessActivity.h"

#include <ChessFiles.h>
#include <ChessSettings.h>
#include <GfxRenderer.h>
#include <HalPowerManager.h>
#include <I18n.h>
#include <LichessClient.h>
#include <Logging.h>
#include <PuzzleStore.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

#include "ChessGameActivity.h"
#include "ChessStudyActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "util/QrUtils.h"

namespace fui = freeink::ui;

namespace {
// The app host holds six handlers, so plain buttons share one action id
// and carry their meaning in the event value.
constexpr fui::ActionId ACTION_BUTTON = 1;
constexpr fui::ActionId ACTION_CARD = 2;
constexpr fui::ActionId ACTION_ROW = 3;
constexpr fui::ActionId ACTION_RATED = 4;
constexpr fui::ActionId ACTION_STEP = 5;
constexpr fui::ActionId ACTION_TAB = 6;
constexpr fui::ActionId ACTION_LAST = ACTION_TAB;
constexpr int16_t BTN_TOKEN = 1;
constexpr int16_t BTN_LOCAL = 2;
constexpr int16_t BTN_CANCEL = 3;
constexpr int16_t BTN_RETRY = 4;
constexpr int16_t BTN_DIALOG_OK = 5;
constexpr int16_t BTN_DIALOG_CANCEL = 6;
constexpr int16_t BTN_BACK = 7;
constexpr int16_t BTN_REFRESH_GAMES = 8;
constexpr int16_t BTN_NEXT_PUZZLE = 9;
constexpr int16_t BTN_DOWNLOAD = 10;
constexpr int16_t BTN_STOP_DOWNLOAD = 11;
constexpr int16_t BTN_REFRESH_STUDIES = 12;
constexpr int16_t BTN_STUDY_BY_ID = 13;
constexpr int16_t BTN_THEME = 14;
constexpr int16_t BTN_STATS = 15;
constexpr int16_t BTN_LOAD_ONGOING = 16;
constexpr int16_t BTN_STUDIES_BY_USER = 17;
constexpr int16_t BTN_DOWNLOAD_STUDIES = 18;
constexpr int16_t BTN_CHALLENGE_NAME = 19;
constexpr int16_t BTN_LOAD_FRIENDS = 20;
// Stepper values: +-1 minutes, +-10 increment, +-100 AI level.
constexpr int16_t STEP_INCREMENT_UNIT = 10;
constexpr int16_t STEP_LEVEL_UNIT = 100;
constexpr int16_t STEP_DIFF_UNIT = 1000;
constexpr const char* DIFFICULTY_NAMES[5] = {"easiest", "easier", "normal", "harder", "hardest"};
constexpr StrId DIFFICULTY_LABELS[5] = {StrId::STR_CHESS_DIFF_EASIEST, StrId::STR_CHESS_DIFF_EASIER,
                                        StrId::STR_CHESS_DIFF_NORMAL, StrId::STR_CHESS_DIFF_HARDER,
                                        StrId::STR_CHESS_DIFF_HARDEST};
// The token page with every scope the app uses ticked, shown as a QR code.
constexpr const char* TOKEN_URL =
    "https://lichess.org/account/oauth/token/"
    "create?scopes[]=board:play&scopes[]=challenge:write&scopes[]=challenge:read&scopes[]=puzzle:read&scopes[]=puzzle:"
    "write&scopes[]=study:read&scopes[]=follow:read&description=crosscheck+X4+Pro";
constexpr int QR_SIZE = 220;
// Approximate ratings of the Lichess engine levels 1..8.
constexpr int AI_RATING[8] = {800, 1100, 1400, 1700, 2000, 2300, 2700, 3000};

// Lichess speed category from the estimated game length in seconds.
// Tab highlight for both tab levels: the selected tab is inverted.
fui::StyleSet tabStyles(const fui::ThemeTokens& theme) {
  fui::StyleSet styles;
  styles.explicitlySet = true;
  styles.normal.foreground = fui::Paint::solid(fui::Color::Black);
  styles.selected.background = fui::Paint::solid(fui::Color::Black);
  styles.selected.foreground = fui::Paint::solid(fui::Color::White);
  styles.selected.radius = theme.listRowRadius;
  return styles;
}

StrId categoryFor(int minutes, int increment) {
  const int estimate = minutes * 60 + increment * 40;
  if (estimate < 180) return StrId::STR_CHESS_BULLET;
  if (estimate < 480) return StrId::STR_CHESS_BLITZ;
  if (estimate < 1500) return StrId::STR_CHESS_RAPID;
  return StrId::STR_CHESS_CLASSICAL;
}
}  // namespace

constexpr ChessActivity::TimeControl ChessActivity::PEOPLE_CARDS[5];
constexpr ChessActivity::TimeControl ChessActivity::AI_CARDS[9];
constexpr int ChessActivity::DOWNLOAD_CHOICES[5];

ChessActivity::ChessActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("Chess", renderer, mappedInput), UiAppHost(renderer) {}

void ChessActivity::onEnter() {
  Activity::onEnter();
  LOG_INF("CHESS", "Enter lobby: heap %u, max alloc %u, battery %u%%", ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
          powerManager.getBatteryPercentage());
  resetUi();
  for (fui::ActionId a = ACTION_BUTTON; a <= ACTION_LAST; ++a) app.on(a, &ChessActivity::onAction, this);
  app.setScreen(&ChessActivity::screenTrampoline, this);

  CHESS_SETTINGS.loadFromFile();
  PUZZLE_STORE.load();
  STUDY_STORE.load();
  rebuildStudyRows();
  seekRated = CHESS_SETTINGS.getRated();
  customMinutes = CHESS_SETTINGS.getCustomMinutes();
  customIncrement = CHESS_SETTINGS.getCustomIncrement();
  aiLevel = CHESS_SETTINGS.getAiLevel();
  tab = CHESS_SETTINGS.getLobbyTab();
  playTab = CHESS_SETTINGS.getPlayTab();
  puzzleDifficulty = CHESS_SETTINGS.getPuzzleDifficulty();
  loadThemeSelection();
  puzzleLine[0] = '\0';
  ratingLine[0] = '\0';
  lichess::copyStr(account.username, sizeof(account.username), CHESS_SETTINGS.getUsername().c_str());
  rebuildCardLabels();

  // The lobby opens offline; the first action that needs Lichess connects.
  state = State::Lobby;
  requestUpdate();
}

void ChessActivity::onExit() {
  if (!resultsInFlight.empty()) {
    // A send still in flight: keep the results for next time.
    PUZZLE_STORE.restoreResults(resultsInFlight);
    resultsInFlight.clear();
    PUZZLE_STORE.saveToFile();
  }
  PUZZLE_STORE.clearAll();  // everything is on the SD card; free the RAM
  STUDY_STORE.clearAll();
  std::vector<lichess::ThemeStat>().swap(dashboard.themes);
  if (clientStarted) {
    LICHESS.end();
    clientStarted = false;
  }
  if (wifiActivated) {
    // WIFI_OFF stops the driver through the Arduino layer, so the next
    // WiFi.mode(WIFI_STA) starts it again. A bare esp_wifi_stop() leaves the
    // layer believing the driver is up and every later connect fails.
    WiFi.disconnect(true);
    delay(30);
    WiFi.mode(WIFI_OFF);
    wifiActivated = false;
  }
  LOG_INF("CHESS", "Exit lobby: heap %u, max alloc %u, battery %u%%", ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
          powerManager.getBatteryPercentage());
  Activity::onExit();
}

// --- connection --------------------------------------------------------------

void ChessActivity::startWifi() {
  wifiActivated = true;
  state = State::Connecting;
  if (WiFi.status() == WL_CONNECTED) {
    onWifiResult(true);
    return;
  }
  requestUpdate();
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) {
                           RenderLock lock;
                           onWifiResult(!result.isCancelled);
                         });
}

void ChessActivity::onWifiResult(bool connected) {
  if (!connected) {
    // The picker was cancelled: back to the offline lobby, the action dropped.
    pendingAction = Pending::None;
    pendingPuzzleOpen = false;
    state = State::Lobby;
    requestUpdate();
    return;
  }
  // The lobby tolerates latency, so modem sleep stays on here.
  WiFi.setSleep(true);
  startClient();
}

void ChessActivity::startClient() {
  if (!clientStarted) {
    if (!LICHESS.begin(CHESS_SETTINGS.getToken(), CHESS_SETTINGS.getUserId())) {
      showError(StrId::STR_CHESS_ERROR_NETWORK, 0);
      return;
    }
    clientStarted = true;
  }
  if (!CHESS_SETTINGS.hasToken()) {
    // Anonymous: puzzles download, nothing else needs an account.
    accountReady = true;
    state = State::Lobby;
    afterConnect();
    requestUpdate();
    return;
  }
  state = State::Loading;
  LICHESS.fetchAccount();
  requestUpdate();
}

void ChessActivity::afterConnect() {
  // The action that asked for the connection runs first. Then, once per
  // connection, pending puzzle results go out and a low store is topped up.
  runPending();
  if (autoSynced) return;
  autoSynced = true;
  const int have = readyCount();
  if (PUZZLE_STORE.pendingResults() > 0 || have < PuzzleStore::REFILL_BELOW) {
    const int topUp = PuzzleStore::TOP_UP - have;
    syncPuzzles(false, topUp > 0 ? topUp : 0);
  }
}

void ChessActivity::connectThen(Pending action) {
  app.clearTapFlash();
  pendingAction = action;
  const bool needsAccount = action != Pending::Download && action != Pending::Puzzle && action != Pending::Study &&
                            !(action == Pending::Studies && studyUser[0]);
  if (needsAccount ? signedIn() : connected()) {
    runPending();
  } else if (needsAccount && !CHESS_SETTINGS.hasToken()) {
    enterToken();  // its handler connects, and the account event runs the action
  } else if (state != State::Connecting && state != State::Loading) {
    startWifi();
  }
}

void ChessActivity::runPending() {
  const Pending action = pendingAction;
  pendingAction = Pending::None;
  if (action == Pending::None) return;
  const bool needsAccount = action != Pending::Download && action != Pending::Puzzle && action != Pending::Study &&
                            !(action == Pending::Studies && studyUser[0]);
  if (needsAccount ? !signedIn() : !connected()) return;
  switch (action) {
    case Pending::Seek:
      startSeek(pendingControl);
      break;
    case Pending::AiGame:
      startAiGame(pendingAiControl);
      break;
    case Pending::Challenge:
      sendChallenge();
      break;
    case Pending::Games:
      if (!recentLoading) {
        recentLoading = true;
        LICHESS.fetchRecentGames(MAX_RECENT);
      }
      requestUpdate();
      break;
    case Pending::Download:
      syncPuzzles(false, nextBatch());
      break;
    case Pending::Puzzle:
      syncPuzzles(true, PuzzleStore::TOP_UP);
      break;
    case Pending::Studies:
      if (!studiesLoading) {
        studiesLoading = true;
        LICHESS.fetchStudies(studyUser);
      }
      requestUpdate();
      break;
    case Pending::Study:
      loadingText = StrId::STR_CHESS_DOWNLOADING_STUDY;
      state = State::Loading;
      LICHESS.fetchStudy(pendingStudyId);
      requestUpdate();
      break;
    case Pending::Dashboard:
      if (!dashboardLoading) {
        dashboardLoading = true;
        LICHESS.fetchPuzzleDashboard(30);
      }
      state = State::Stats;
      requestUpdate();
      break;
    case Pending::Friends:
      friendsStatus = 0;
      if (!friendsLoading) {
        friendsLoading = true;
        LICHESS.fetchFriends();
      }
      tab = 0;
      playTab = PLAY_CHALLENGE;
      requestUpdate();
      break;
    default:
      break;
  }
}

void ChessActivity::syncPuzzles(bool openAfter, int need) {
  if (!connected() || puzzleLoading) return;
  // Results leave the store now and come back only if the send fails, so a
  // reply lost to another screen cannot make them go out twice.
  std::string results;
  if (PUZZLE_STORE.pendingResults() > 0) {
    PUZZLE_STORE.takeResults(resultsInFlight);
    PuzzleStore::resultsJson(resultsInFlight, results);
    PUZZLE_STORE.saveToFile();
  }
  const int room = PuzzleStore::MAX_PUZZLES - PUZZLE_STORE.count();
  if (need > room) need = room;
  if (need <= 0 && results.empty()) {
    if (openAfter) requestPuzzle();
    return;
  }
  if (need <= 0) need = 1;  // the batch endpoint wants at least one
  puzzleLoading = true;
  pendingPuzzleOpen = openAfter;
  lichess::copyStr(downloadTheme, sizeof(downloadTheme), downloadThemeFor(downloadBatchIndex++));
  LICHESS.fetchPuzzleBatch(downloadTheme, DIFFICULTY_NAMES[puzzleDifficulty], need, results);
  requestUpdate();
}

void ChessActivity::showError(StrId text, int code, const char* detail) {
  errorText = text;
  errorCode = code;
  lichess::copyStr(errorDetail, sizeof(errorDetail), detail ? detail : "");
  state = State::Error;
  requestUpdate();
}

void ChessActivity::handleClientEvents() {
  LichessClient::Event ev;
  while (LICHESS.pollEvent(ev)) {
    switch (ev.type) {
      case LichessClient::EventType::AccountReady:
        LICHESS.copyAccount(account);
        LICHESS.setUserId(account.id);
        CHESS_SETTINGS.setAccount(account.username, account.id);
        CHESS_SETTINGS.saveToFile();
        snprintf(ratingLine, sizeof(ratingLine), "%s %d  %s %d  %s %d", tr(STR_CHESS_BLITZ), account.blitz,
                 tr(STR_CHESS_RAPID), account.rapid, tr(STR_CHESS_CLASSICAL), account.classical);
        // Connected means: account known, ongoing games listed, event stream
        // open. The list comes first so the stream's replay of existing games
        // can be told from a new game.
        LICHESS.fetchNowPlaying();
        requestUpdate();
        break;
      case LichessClient::EventType::RecentGamesReady:
        LICHESS.copyRecentGames(recent);
        rebuildRecentRows();
        recentLoaded = true;
        recentLoading = false;
        recentNav.reset();
        requestUpdate();
        break;
      case LichessClient::EventType::RecentGamesFailed:
        recentLoading = false;
        showError(StrId::STR_CHESS_ERROR_GAMES, ev.code);
        break;
      case LichessClient::EventType::AnalysisReady:
      case LichessClient::EventType::AnalysisFailed: {
        // Without evaluations the review still opens, plain.
        std::vector<lichess::AnalysisPly> a;
        if (ev.type == LichessClient::EventType::AnalysisReady) LICHESS.copyAnalysis(a);
        loadingText = StrId::STR_CHESS_CONNECTING;
        if (state == State::Loading) state = State::Lobby;
        const int index = reviewIndex;
        reviewIndex = -1;
        startReview(index, std::move(a));
        break;
      }
      case LichessClient::EventType::PuzzleBatchReady: {
        puzzleLoading = false;
        resultsInFlight.clear();  // recorded by Lichess
        std::vector<lichess::Puzzle> more;
        LICHESS.copyPuzzleBatch(more);
        PUZZLE_STORE.add(downloadTheme, more);
        if (downloadTarget > 0) {
          downloadDone += static_cast<int>(more.size());
          if (downloadDone >= downloadTarget || PUZZLE_STORE.count() >= PuzzleStore::MAX_PUZZLES) downloadTarget = 0;
        }
        const bool open = pendingPuzzleOpen;
        pendingPuzzleOpen = false;
        if (open && readyCount() > 0) {
          requestPuzzle();
        } else {
          requestUpdate();
        }
        if (downloadTarget > 0) {
          syncPuzzles(false, nextBatch());  // the next batch of the same download
        } else if (PUZZLE_STORE.pendingResults() > 0) {
          syncPuzzles(false, 0);  // more results than one post carries
        }
        break;
      }
      case LichessClient::EventType::PuzzleBatchFailed:
        puzzleLoading = false;
        pendingPuzzleOpen = false;
        downloadTarget = 0;
        if (!resultsInFlight.empty()) {
          PUZZLE_STORE.restoreResults(resultsInFlight);
          resultsInFlight.clear();
          PUZZLE_STORE.saveToFile();
        }
        showError(StrId::STR_CHESS_ERROR_PUZZLE, ev.code, ev.detail);
        break;
      case LichessClient::EventType::StudiesReady: {
        std::vector<lichess::StudyInfo> list;
        LICHESS.copyStudies(list);
        STUDY_STORE.merge(list);
        rebuildStudyRows();
        studiesLoading = false;
        studiesLoaded = true;
        studyNav.reset();
        requestUpdate();
        break;
      }
      case LichessClient::EventType::StudiesFailed:
        studiesLoading = false;
        showError(StrId::STR_CHESS_ERROR_STUDIES, ev.code);
        break;
      case LichessClient::EventType::StudyReady: {
        loadingText = StrId::STR_CHESS_CONNECTING;
        loadingLine[0] = '\0';
        char name[64];
        const int chapters = STUDY_STORE.indexStudy(ev.gameId, name, sizeof(name));
        if (chapters <= 0) {
          // Not a PGN (a private study answers with a web page): keep nothing.
          STUDY_STORE.remove(ev.gameId);
          rebuildStudyRows();
          studyQueue.clear();
          studyQueueTotal = 0;
          showError(StrId::STR_CHESS_ERROR_STUDY, -3);
          break;
        }
        if (name[0]) {
          lichess::StudyInfo info;
          lichess::copyStr(info.id, sizeof(info.id), ev.gameId);
          lichess::copyStr(info.name, sizeof(info.name), name);
          STUDY_STORE.addOrUpdate(info);
        }
        rebuildStudyRows();
        if (!studyQueue.empty()) {
          nextQueuedStudy();  // a "download all" in progress: no opening
          break;
        }
        if (state == State::Loading) state = State::Lobby;
        if (studyQueueTotal > 0) {
          studyQueueTotal = 0;  // the last of a batch: stay on the list
          requestUpdate();
        } else {
          openStudy(ev.gameId);
        }
        break;
      }
      case LichessClient::EventType::StudyFailed:
        loadingText = StrId::STR_CHESS_CONNECTING;
        loadingLine[0] = '\0';
        studyQueue.clear();
        studyQueueTotal = 0;
        showError(StrId::STR_CHESS_ERROR_STUDY, ev.code);
        break;
      case LichessClient::EventType::DashboardReady:
        LICHESS.copyDashboard(dashboard);
        dashboardLoading = false;
        rebuildStatRows();
        statNav.reset();
        requestUpdate();
        break;
      case LichessClient::EventType::DashboardFailed:
        dashboardLoading = false;
        showError(StrId::STR_CHESS_ERROR_DASHBOARD, ev.code);
        break;
      case LichessClient::EventType::FriendsReady:
        LICHESS.copyFriends(friends);
        rebuildFriendRows();
        friendsLoading = false;
        friendNav.reset();
        requestUpdate();
        break;
      case LichessClient::EventType::FriendsFailed:
        // The screen stays up with the reason and the typed-name option.
        friendsLoading = false;
        friendsStatus = ev.code;
        requestUpdate();
        break;
      case LichessClient::EventType::AccountFailed:
        showError(ev.code == 401 ? StrId::STR_CHESS_ERROR_TOKEN : StrId::STR_CHESS_ERROR_NETWORK, ev.code);
        break;
      case LichessClient::EventType::NowPlayingReady: {
        LICHESS.copyNowPlaying(ongoing);
        rebuildOngoingRows();
        ongoingLoaded = true;
        // Presence on Lichess comes from the event stream; keep it open all session.
        LICHESS.streamEvents();
        if (!accountReady) {
          accountReady = true;
          if (state == State::Loading) state = State::Lobby;
          afterConnect();
          if (tab == 1 && !recentLoaded && !recentLoading) {
            recentLoading = true;
            LICHESS.fetchRecentGames(MAX_RECENT);
          }
        }
        if (state == State::Seeking && !LICHESS.seeking()) {
          // The seek or challenge produced a game but the event stream did not
          // report it: open the game that was not there before.
          const lichess::OngoingGame* fresh = nullptr;
          for (const auto& g : ongoing) {
            bool known = false;
            for (const auto& id : preSeekGameIds) known = known || id == g.gameId;
            if (!known) {
              fresh = &g;
              break;
            }
          }
          if (fresh) {
            openGame(fresh->gameId, fresh->myColor == chess::Color::Black ? 1 : 0);
          } else if (!waitingForChallenge && nowPlayingRetries < NOW_PLAYING_RETRIES) {
            ++nowPlayingRetries;
            nowPlayingRetryAt = millis() + NOW_PLAYING_RETRY_MS;
          } else if (!waitingForChallenge) {
            showError(StrId::STR_CHESS_ERROR_SEEK, 0);
          }
        }
        requestUpdate();
        break;
      }
      case LichessClient::EventType::NowPlayingFailed:
        if (!accountReady) showError(StrId::STR_CHESS_ERROR_NETWORK, ev.code);
        break;
      case LichessClient::EventType::GameStart: {
        // The stream replays existing games when it opens; only a game that is
        // not in the ongoing list is new. Outside a seek a new game still opens
        // (a challenge accepted late, or a seek a cancel did not reach), so its
        // clock does not run unseen.
        bool known = false;
        for (const auto& g : ongoing) known = known || strcmp(g.gameId, ev.gameId) == 0;
        const bool idle = state == State::Lobby || state == State::Custom;
        if (state == State::Seeking || (idle && ongoingLoaded && !known)) openGame(ev.gameId, ev.color);
        break;
      }
      case LichessClient::EventType::SeekEnded:
        if (state != State::Seeking) break;
        if (ev.code == -2) {
          state = State::Lobby;
          requestUpdate();
        } else if (ev.code == 200) {
          nowPlayingRetries = 0;
          LICHESS.fetchNowPlaying();
        } else {
          showError(StrId::STR_CHESS_ERROR_SEEK, ev.code, ev.detail);
        }
        break;
      case LichessClient::EventType::ChallengeDeclined:
        if (state == State::Seeking && waitingForChallenge) showError(StrId::STR_CHESS_CHALLENGE_DECLINED, 0);
        break;
      case LichessClient::EventType::EngineGameReady:
        // The engine game exists on Lichess now; open it even after a cancel
        // that came too late, so its clock does not run unseen.
        if (ev.gameId[0] && state != State::Error) openGame(ev.gameId, -1);
        break;
      case LichessClient::EventType::CommandFailed:
        if (state == State::Seeking && waitingForChallenge) {
          showError(StrId::STR_CHESS_ERROR_CHALLENGE, ev.code, ev.detail);
        }
        break;
      default:
        break;
    }
  }
  if (state == State::Seeking && nowPlayingRetryAt && millis() >= nowPlayingRetryAt) {
    nowPlayingRetryAt = 0;
    LICHESS.fetchNowPlaying();
  }
}

// --- lobby actions -----------------------------------------------------------

void ChessActivity::openGame(const char* gameId, int colorHint) {
  LICHESS.cancelSeek();
  waitingForChallenge = false;
  nowPlayingRetryAt = 0;
  state = State::Lobby;
  app.clearTapFlash();
  char id[lichess::GAME_ID_LEN];
  lichess::copyStr(id, sizeof(id), gameId);
  startActivityForResult(std::make_unique<ChessGameActivity>(renderer, mappedInput, id, colorHint),
                         [this](const ActivityResult& result) {
                           RenderLock lock;
                           if (!result.isCancelled && std::holds_alternative<KeyboardResult>(result.data)) {
                             // A rematch is out. Against a person the event stream reports the
                             // accept; against the engine the command result carries the game.
                             const bool ai = std::get<KeyboardResult>(result.data).text == "rematch-ai";
                             preSeekGameIds.clear();
                             for (const auto& g : ongoing) preSeekGameIds.emplace_back(g.gameId);
                             waitingForChallenge = true;
                             dialogMode = ai ? DialogMode::Computer : DialogMode::Challenge;
                             state = State::Seeking;
                           }
                           // Replies to these may have been consumed by the game screen; a
                           // batch that arrived meanwhile is still in the client.
                           puzzleLoading = false;
                           recentLoading = false;
                           std::vector<lichess::Puzzle> more;
                           LICHESS.copyPuzzleBatch(more);
                           if (!more.empty()) {
                             resultsInFlight.clear();
                             PUZZLE_STORE.add(downloadTheme, more);
                           }
                           LICHESS.fetchNowPlaying();
                           requestUpdate();
                         });
}

void ChessActivity::startSeek(TimeControl control) {
  seekControl = control;
  waitingForChallenge = false;
  nowPlayingRetries = 0;
  nowPlayingRetryAt = 0;
  preSeekGameIds.clear();
  for (const auto& g : ongoing) preSeekGameIds.emplace_back(g.gameId);
  state = State::Seeking;
  LICHESS.streamEvents();  // normally already open; harmless
  LICHESS.seek(control.minutes, control.increment, seekRated);
  requestUpdate();
}

void ChessActivity::startAiGame(TimeControl control) {
  seekControl = control;
  preSeekGameIds.clear();
  for (const auto& g : ongoing) preSeekGameIds.emplace_back(g.gameId);
  waitingForChallenge = true;  // no seek socket; the CommandDone event opens the game
  dialogMode = DialogMode::Computer;
  state = State::Seeking;
  LICHESS.challengeAi(aiLevel, control.minutes, control.increment);
  requestUpdate();
}

void ChessActivity::sendChallenge() {
  preSeekGameIds.clear();
  for (const auto& g : ongoing) preSeekGameIds.emplace_back(g.gameId);
  waitingForChallenge = true;
  nowPlayingRetryAt = 0;
  dialogMode = DialogMode::Challenge;
  state = State::Seeking;
  LICHESS.challenge(challengeUser, seekControl.minutes, seekControl.increment, seekRated);
  requestUpdate();
}

void ChessActivity::askAccount() {
  static const char* labels[3];
  labels[0] = tr(STR_CHESS_NEW_TOKEN);
  labels[1] = tr(STR_CHESS_REMOVE_TOKEN);
  labels[2] = tr(STR_CANCEL);
  levelChoice = -1;
  popupKind = PopupKind::Account;
  levelPopup.show(tr(STR_CHESS_ACCOUNT), labels, 3, 0, [this](const int idx) { levelChoice = idx; });
  requestUpdate();
}

void ChessActivity::askDownloadCount() {
  static const char* labels[5];
  for (int i = 0; i < 5; ++i) {
    snprintf(countLabels[i], sizeof(countLabels[i]), tr(STR_CHESS_N_PUZZLES), DOWNLOAD_CHOICES[i]);
    labels[i] = countLabels[i];
  }
  levelChoice = -1;
  popupKind = PopupKind::Count;
  downloadBatchIndex = 0;
  levelPopup.show(tr(STR_CHESS_DOWNLOAD_COUNT), labels, 5, 0, [this](const int idx) { levelChoice = idx; });
  requestUpdate();
}

int ChessActivity::nextBatch() const {
  const int left = downloadTarget > 0 ? downloadTarget - downloadDone : PuzzleStore::BATCH_SIZE;
  int batch = left < PuzzleStore::BATCH_SIZE ? left : PuzzleStore::BATCH_SIZE;
  // Several themes share a download: each batch takes its part, ten at least.
  const int themes = selectedThemeCount();
  if (themes > 1) {
    int share = (left + themes - 1) / themes;
    if (share < 10) share = 10;
    if (share < batch) batch = share;
  }
  return batch;
}

void ChessActivity::askLevel(TimeControl control) {
  pendingAiControl = control;
  static const char* labels[8];
  for (int i = 0; i < 8; ++i) {
    snprintf(levelLabels[i], sizeof(levelLabels[i]), tr(STR_CHESS_LEVEL_ABOUT), i + 1, AI_RATING[i]);
    labels[i] = levelLabels[i];
  }
  levelChoice = -1;
  popupKind = PopupKind::Level;
  levelPopup.show(tr(STR_CHESS_LEVEL), labels, 8, aiLevel - 1, [this](const int idx) { levelChoice = idx; });
  requestUpdate();
}

void ChessActivity::cancelSeek() {
  LICHESS.cancelSeek();
  waitingForChallenge = false;
  nowPlayingRetryAt = 0;
  state = State::Lobby;
  requestUpdate();
}

void ChessActivity::enterToken() {
  app.clearTapFlash();
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_CHESS_TOKEN),
                                                                 CHESS_SETTINGS.getToken(), 64, InputType::Text),
                         [this](const ActivityResult& result) {
                           if (!result.isCancelled) {
                             const auto& kb = std::get<KeyboardResult>(result.data);
                             CHESS_SETTINGS.setToken(kb.text);
                             CHESS_SETTINGS.saveToFile();
                             // Stopping the client can take seconds; keep the render lock free.
                             if (clientStarted) {
                               LICHESS.end();
                               clientStarted = false;
                             }
                           }
                           RenderLock lock;
                           if (result.isCancelled) {
                             pendingAction = Pending::None;
                             requestUpdate();
                             return;
                           }
                           accountReady = false;
                           if (!CHESS_SETTINGS.hasToken()) {
                             pendingAction = Pending::None;
                             state = State::Lobby;
                             requestUpdate();
                             return;
                           }
                           if (wifiActivated) {
                             startClient();
                           } else {
                             startWifi();
                           }
                         });
}

void ChessActivity::enterChallengeUsername() {
  app.clearTapFlash();
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_CHESS_CHALLENGE_USER),
                                                                 challengeUser, 30, InputType::Text),
                         [this](const ActivityResult& result) {
                           RenderLock lock;
                           if (!result.isCancelled) {
                             const auto& kb = std::get<KeyboardResult>(result.data);
                             lichess::copyStr(challengeUser, sizeof(challengeUser), kb.text.c_str());
                             if (challengeUser[0]) {
                               dialogMode = DialogMode::Challenge;
                               state = State::Custom;
                             }
                           }
                           requestUpdate();
                         });
}

void ChessActivity::confirmDialog() {
  const TimeControl control{static_cast<uint8_t>(customMinutes), static_cast<uint8_t>(customIncrement)};
  CHESS_SETTINGS.setCustom(customMinutes, customIncrement);
  CHESS_SETTINGS.saveToFile();
  preSeekGameIds.clear();
  for (const auto& g : ongoing) preSeekGameIds.emplace_back(g.gameId);
  switch (dialogMode) {
    case DialogMode::Seek:
      pendingControl = control;
      connectThen(Pending::Seek);
      break;
    case DialogMode::Challenge:
      seekControl = control;
      connectThen(Pending::Challenge);
      break;
    case DialogMode::Computer:
      app.clearTapFlash();
      askLevel(control);
      break;
  }
}

void ChessActivity::rebuildRecentRows() {
  const int count = recent.size() < MAX_RECENT ? static_cast<int>(recent.size()) : MAX_RECENT;
  for (int i = 0; i < count; ++i) {
    const lichess::GameSummary& g = recent[i];
    const lichess::Player& opp = g.myColor == chess::Color::White ? g.black : g.white;
    const char* tag = g.analysed ? tr(STR_CHESS_ANALYSED) : "";
    if (opp.aiLevel > 0) {
      char engine[32];
      snprintf(engine, sizeof(engine), tr(STR_CHESS_STOCKFISH), opp.aiLevel);
      snprintf(recentLabels[i], sizeof(recentLabels[i]), "%s%s", engine, tag);
    } else {
      snprintf(recentLabels[i], sizeof(recentLabels[i]), "%s (%d)%s", opp.name, opp.rating, tag);
    }
    const char* result = !g.hasWinner            ? tr(STR_CHESS_DRAW)
                         : g.winner == g.myColor ? tr(STR_CHESS_WON)
                                                 : tr(STR_CHESS_LOST);
    const char* how = "";
    switch (g.status) {
      case lichess::GameStatus::Mate:
        how = tr(STR_CHESS_BY_CHECKMATE);
        break;
      case lichess::GameStatus::Resign:
        how = tr(STR_CHESS_BY_RESIGNATION);
        break;
      case lichess::GameStatus::Timeout:
      case lichess::GameStatus::OutOfTime:
        how = tr(STR_CHESS_ON_TIME);
        break;
      case lichess::GameStatus::Aborted:
        result = tr(STR_CHESS_GAME_ABORTED);
        break;
      default:
        break;
    }
    snprintf(recentSubtitles[i], sizeof(recentSubtitles[i]), "%s %s, %lu+%lu %s", result, how,
             static_cast<unsigned long>(g.initialMs / 60000), static_cast<unsigned long>(g.incrementMs / 1000),
             g.speed);
    recentRows[i] = fui::ListItem{};
    recentRows[i].label = recentLabels[i];
    recentRows[i].subtitle = recentSubtitles[i];
    recentRows[i].value = g.myColor == chess::Color::White ? tr(STR_CHESS_WHITE) : tr(STR_CHESS_BLACK);
    recentRows[i].actionValue = static_cast<int16_t>(i);
  }
}

void ChessActivity::openReview(int index) {
  if (index < 0 || index >= static_cast<int>(recent.size())) return;
  if (recent[index].analysed && signedIn()) {
    // The evaluations come with the game export; the review opens when they land.
    reviewIndex = index;
    loadingText = StrId::STR_CHESS_LOADING_ANALYSIS;
    state = State::Loading;
    LICHESS.fetchAnalysis(recent[index].id);
    requestUpdate();
    return;
  }
  startReview(index, {});
}

void ChessActivity::startReview(int index, std::vector<lichess::AnalysisPly> analysis) {
  if (index < 0 || index >= static_cast<int>(recent.size())) return;
  // The export carries standard notation; the board wants coordinate moves.
  chess::Position pos;
  std::string uci;
  if (chess::replaySan(recent[index].movesSan.c_str(), pos, &uci) < 0) {
    LOG_ERR("CHESS", "Could not read all moves of game %s", recent[index].id);
  }
  app.clearTapFlash();
  startActivityForResult(
      std::make_unique<ChessGameActivity>(renderer, mappedInput, recent[index], std::move(uci), std::move(analysis)),
      [this, index](const ActivityResult& result) {
        RenderLock lock;
        // The review fetched the analysis itself: the row shows it now.
        if (!result.isCancelled && std::holds_alternative<KeyboardResult>(result.data) &&
            std::get<KeyboardResult>(result.data).text == "analysed" && index < static_cast<int>(recent.size())) {
          recent[index].analysed = true;
          rebuildRecentRows();
        }
        requestUpdate();
      });
}

// --- studies -----------------------------------------------------------------

void ChessActivity::rebuildStudyRows() {
  const auto& list = STUDY_STORE.entries();
  studyRows.resize(list.size());
  for (size_t i = 0; i < list.size() && i < static_cast<size_t>(StudyStore::MAX_STUDIES); ++i) {
    if (list[i].saved) {
      snprintf(studySubtitles[i], sizeof(studySubtitles[i]), tr(STR_CHESS_ON_DEVICE), list[i].chapters);
    } else {
      snprintf(studySubtitles[i], sizeof(studySubtitles[i]), "%s", tr(STR_CHESS_NOT_ON_DEVICE));
    }
    studyRows[i] = fui::ListItem{};
    studyRows[i].label = list[i].info.name;
    studyRows[i].subtitle = studySubtitles[i];
    studyRows[i].actionValue = static_cast<int16_t>(i);
  }
}

void ChessActivity::openStudy(const char* id) {
  const StudyStore::Entry* e = STUDY_STORE.find(id);
  char studyId[16];
  lichess::copyStr(studyId, sizeof(studyId), id);
  app.clearTapFlash();
  startActivityForResult(std::make_unique<ChessStudyActivity>(renderer, mappedInput, studyId, e ? e->info.name : id),
                         [this](const ActivityResult&) {
                           RenderLock lock;
                           rebuildStudyRows();
                           requestUpdate();
                         });
}

void ChessActivity::queueMissingStudies() {
  studyQueue.clear();
  for (const auto& e : STUDY_STORE.entries()) {
    if (!e.saved) studyQueue.emplace_back(e.info.id);
  }
  studyQueueTotal = static_cast<int>(studyQueue.size());
  if (studyQueue.empty()) return;
  // The first one goes through the connect-then-continue path; the rest follow
  // from the StudyReady events.
  lichess::copyStr(pendingStudyId, sizeof(pendingStudyId), studyQueue.front().c_str());
  studyQueue.erase(studyQueue.begin());
  snprintf(loadingLine, sizeof(loadingLine), tr(STR_CHESS_DOWNLOADING_N_OF), 1, studyQueueTotal);
  connectThen(Pending::Study);
}

void ChessActivity::nextQueuedStudy() {
  lichess::copyStr(pendingStudyId, sizeof(pendingStudyId), studyQueue.front().c_str());
  studyQueue.erase(studyQueue.begin());
  snprintf(loadingLine, sizeof(loadingLine), tr(STR_CHESS_DOWNLOADING_N_OF),
           studyQueueTotal - static_cast<int>(studyQueue.size()), studyQueueTotal);
  loadingText = StrId::STR_CHESS_DOWNLOADING_STUDY;
  state = State::Loading;
  LICHESS.fetchStudy(pendingStudyId);
  requestUpdate();
}

void ChessActivity::enterStudyUser() {
  app.clearTapFlash();
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_CHESS_STUDY_USER),
                                                                 studyUser, 30, InputType::Text),
                         [this](const ActivityResult& result) {
                           RenderLock lock;
                           if (result.isCancelled || !std::holds_alternative<KeyboardResult>(result.data)) {
                             requestUpdate();
                             return;
                           }
                           std::string name = std::get<KeyboardResult>(result.data).text;
                           while (!name.empty() && name.back() == ' ') name.pop_back();
                           const size_t at = name.find("@/");
                           if (at != std::string::npos) name = name.substr(at + 2);
                           if (name.empty()) {
                             requestUpdate();
                             return;
                           }
                           lichess::copyStr(studyUser, sizeof(studyUser), name.c_str());
                           if (!studiesLoading) connectThen(Pending::Studies);
                         });
}

void ChessActivity::enterStudyId() {
  app.clearTapFlash();
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_CHESS_STUDY_ID), "", 80, InputType::Text),
      [this](const ActivityResult& result) {
        RenderLock lock;
        if (result.isCancelled || !std::holds_alternative<KeyboardResult>(result.data)) {
          requestUpdate();
          return;
        }
        // An id, or a lichess.org/study/<id> link.
        std::string text = std::get<KeyboardResult>(result.data).text;
        const size_t at = text.find("study/");
        if (at != std::string::npos) text = text.substr(at + 6);
        std::string id;
        for (char c : text) {
          if (isalnum(static_cast<unsigned char>(c)))
            id.push_back(c);
          else if (!id.empty())
            break;
          if (id.size() == 8) break;
        }
        if (id.size() != 8) {
          showError(StrId::STR_CHESS_ERROR_STUDY, 0);
          return;
        }
        lichess::copyStr(pendingStudyId, sizeof(pendingStudyId), id.c_str());
        lichess::StudyInfo info;
        lichess::copyStr(info.id, sizeof(info.id), id.c_str());
        lichess::copyStr(info.name, sizeof(info.name), id.c_str());
        if (!STUDY_STORE.find(info.id)) STUDY_STORE.addOrUpdate(info);
        rebuildStudyRows();
        connectThen(Pending::Study);
      });
}

void ChessActivity::rebuildStatRows() {
  const int count =
      dashboard.themes.size() < static_cast<size_t>(MAX_STATS) ? static_cast<int>(dashboard.themes.size()) : MAX_STATS;
  for (int i = 0; i < count; ++i) {
    const lichess::ThemeStat& t = dashboard.themes[i];
    snprintf(statLabels[i], sizeof(statLabels[i]), "%s", t.name);
    snprintf(statSubtitles[i], sizeof(statSubtitles[i]), tr(STR_CHESS_STAT_LINE), t.played, t.wins);
    snprintf(statValues[i], sizeof(statValues[i]), "%d", t.performance);
    statRows[i] = fui::ListItem{};
    statRows[i].label = statLabels[i];
    statRows[i].subtitle = statSubtitles[i];
    statRows[i].value = statValues[i];
    statRows[i].actionValue = static_cast<int16_t>(i);
  }
  if (dashboard.played > 0) {
    snprintf(statsLine, sizeof(statsLine), tr(STR_CHESS_RESULTS_GLOBAL), dashboard.played, dashboard.wins,
             dashboard.performance);
  } else {
    snprintf(statsLine, sizeof(statsLine), "%s", tr(STR_CHESS_NO_RESULTS));
  }
}

void ChessActivity::requestPuzzle() {
  lichess::Puzzle saved;
  if (takeSelected(saved)) {
    openPuzzle(saved);
    return;
  }
  connectThen(Pending::Puzzle);  // a download first; the puzzle opens when it lands
}

void ChessActivity::openPuzzle(const lichess::Puzzle& puzzle) {
  lastPuzzleRating = puzzle.rating;
  lichess::copyStr(lastPuzzleId, sizeof(lastPuzzleId), puzzle.id);
  app.clearTapFlash();
  startActivityForResult(std::make_unique<ChessGameActivity>(renderer, mappedInput, puzzle),
                         [this](const ActivityResult& result) {
                           RenderLock lock;
                           // The game screen reports "puzzle:<w|l|n>:<next|back>".
                           std::string text;
                           if (!result.isCancelled && std::holds_alternative<KeyboardResult>(result.data)) {
                             text = std::get<KeyboardResult>(result.data).text;
                           }
                           if (text.rfind("puzzle:", 0) == 0 && text.size() >= 9) {
                             const char outcome = text[7];
                             if (outcome == 'w' || outcome == 'l') {
                               PUZZLE_STORE.addResult(lastPuzzleId, outcome == 'w');
                               PUZZLE_STORE.saveToFile();
                             }
                             if (text.compare(9, 4, "next") == 0) {
                               requestPuzzle();
                               return;
                             }
                           }
                           // A batch that arrived while the puzzle screen was up is still in the client.
                           puzzleLoading = false;
                           if (clientStarted) {
                             std::vector<lichess::Puzzle> more;
                             LICHESS.copyPuzzleBatch(more);
                             if (!more.empty()) {
                               resultsInFlight.clear();
                               PUZZLE_STORE.add(downloadTheme, more);
                             }
                           }
                           // Results go out in small groups while a connection is up.
                           if (connected() && PUZZLE_STORE.pendingResults() >= 5) syncPuzzles(false, 0);
                           requestUpdate();
                         });
}

void ChessActivity::onAction(const fui::ActionEvent& event, void* user) {
  static_cast<ChessActivity*>(user)->handleAction(event);
}

void ChessActivity::handleAction(const fui::ActionEvent& event) {
  if (event.action == ACTION_BUTTON) {
    switch (event.value) {
      case BTN_TOKEN:
        enterToken();
        break;
      case BTN_LOCAL:
        app.clearTapFlash();
        startActivityForResult(std::make_unique<ChessGameActivity>(renderer, mappedInput),
                               [this](const ActivityResult&) { requestUpdate(); });
        break;
      case BTN_CHALLENGE_NAME:
        enterChallengeUsername();
        break;
      case BTN_LOAD_FRIENDS:
        connectThen(Pending::Friends);
        break;
      case BTN_BACK:
        pendingAction = Pending::None;
        state = State::Lobby;
        rebuildCardLabels();
        requestUpdate();
        break;
      case BTN_DOWNLOAD:
        askDownloadCount();
        break;
      case BTN_STOP_DOWNLOAD:
        downloadTarget = 0;  // the batch in flight still lands
        requestUpdate();
        break;
      case BTN_REFRESH_STUDIES:
        studyUser[0] = '\0';
        if (!studiesLoading) connectThen(Pending::Studies);
        break;
      case BTN_STUDIES_BY_USER:
        enterStudyUser();
        break;
      case BTN_LOAD_ONGOING:
        connectThen(Pending::None);  // connecting loads the list
        break;
      case BTN_DOWNLOAD_STUDIES:
        queueMissingStudies();
        break;
      case BTN_STUDY_BY_ID:
        enterStudyId();
        break;
      case BTN_THEME:
        state = State::Themes;
        themeNav.reset(0);
        requestUpdate();
        break;
      case BTN_STATS:
        connectThen(Pending::Dashboard);
        break;
      case BTN_REFRESH_GAMES:
        if (!recentLoading) {
          recentLoaded = false;
          connectThen(Pending::Games);
        }
        break;
      case BTN_NEXT_PUZZLE:
        requestPuzzle();
        break;
      case BTN_CANCEL:
        cancelSeek();
        break;
      case BTN_RETRY:
        if (errorText == StrId::STR_CHESS_ERROR_TOKEN) {
          enterToken();
        } else if (!wifiActivated || WiFi.status() != WL_CONNECTED) {
          startWifi();
        } else if (!accountReady) {
          startClient();
        } else {
          state = State::Lobby;
          requestUpdate();
        }
        break;
      case BTN_DIALOG_OK:
        confirmDialog();
        break;
      case BTN_DIALOG_CANCEL:
        state = State::Lobby;
        requestUpdate();
        break;
      default:
        break;
    }
    return;
  }
  switch (event.action) {
    case ACTION_CARD:
      if (event.value == customIndex()) {
        dialogMode = playTab == PLAY_COMPUTER ? DialogMode::Computer : DialogMode::Seek;
        state = State::Custom;
        requestUpdate();
      } else if (event.value >= 0 && event.value < customIndex()) {
        if (playTab == PLAY_COMPUTER) {
          app.clearTapFlash();
          askLevel(cards()[event.value]);
        } else {
          pendingControl = cards()[event.value];
          connectThen(Pending::Seek);
        }
      }
      break;
    case ACTION_TAB:
      // The tap feedback would paint the tapped tab in its pressed look on
      // top of the selected one, so the highlight looked gone until the
      // next repaint. The selected style is the feedback here.
      app.clearTapFlash();
      if (event.value >= 10 && event.value < 10 + PLAY_TAB_COUNT) {
        // A Play sub-tab. The challenge one lists the followed players, so it connects.
        playTab = event.value - 10;
        CHESS_SETTINGS.setPlayTab(playTab);
        CHESS_SETTINGS.saveToFile();
        if (playTab == PLAY_CHALLENGE && CHESS_SETTINGS.hasToken() && !friendsLoading) connectThen(Pending::Friends);
        requestUpdate();
        break;
      }
      if (event.value < 0 || event.value >= TAB_COUNT) break;
      tab = event.value;
      CHESS_SETTINGS.setLobbyTab(tab);
      CHESS_SETTINGS.saveToFile();
      // The games list needs Lichess: the tab connects on its own. So does an
      // empty study list, once.
      if (tab == 1 && !recentLoaded && !recentLoading && CHESS_SETTINGS.hasToken()) connectThen(Pending::Games);
      if (tab == 3 && !studiesLoaded && !studiesLoading && STUDY_STORE.count() == 0 && CHESS_SETTINGS.hasToken()) {
        connectThen(Pending::Studies);
      }
      requestUpdate();
      break;
    case ACTION_ROW:
      if (state == State::Themes) {
        // Row 0 is "all themes"; the others toggle one theme each. The list stays up.
        if (event.value == 0) {
          for (bool& on : themeSelected) on = false;
        } else if (event.value > 0 && event.value < PUZZLE_THEME_COUNT) {
          themeSelected[event.value] = !themeSelected[event.value];
        }
        saveThemeSelection();
        requestUpdate();
      } else if (state == State::Stats) {
        if (event.value >= 0 && event.value < static_cast<int>(dashboard.themes.size())) {
          for (bool& on : themeSelected) on = false;
          const int idx = puzzleThemeIndex(dashboard.themes[event.value].key);
          if (idx > 0 || strcmp(dashboard.themes[event.value].key, "mix") == 0) themeSelected[idx] = true;
          saveThemeSelection();
        }
        state = State::Lobby;
        tab = 2;
        requestUpdate();
      } else if (tab == 3) {
        const auto& list = STUDY_STORE.entries();
        if (event.value >= 0 && event.value < static_cast<int>(list.size())) {
          if (list[event.value].saved) {
            openStudy(list[event.value].info.id);
          } else {
            lichess::copyStr(pendingStudyId, sizeof(pendingStudyId), list[event.value].info.id);
            connectThen(Pending::Study);
          }
        }
      } else if (tab == 1) {
        openReview(event.value);
      } else if (playTab == PLAY_CHALLENGE) {
        if (event.value >= 0 && event.value < static_cast<int>(friends.size())) {
          // The time control dialog follows, as after a typed name.
          lichess::copyStr(challengeUser, sizeof(challengeUser), friends[event.value].name);
          dialogMode = DialogMode::Challenge;
          state = State::Custom;
        }
        requestUpdate();
      } else if (event.value >= 0 && event.value < static_cast<int>(ongoing.size())) {
        openGame(ongoing[event.value].gameId, ongoing[event.value].myColor == chess::Color::Black ? 1 : 0);
      }
      break;
    case ACTION_RATED:
      seekRated = !seekRated;
      CHESS_SETTINGS.setRated(seekRated);
      CHESS_SETTINGS.saveToFile();
      requestUpdate();
      break;
    case ACTION_STEP:
      if (event.value == 1 || event.value == -1) {
        customMinutes += event.value;
        if (customMinutes < 1) customMinutes = 1;
        if (customMinutes > 180) customMinutes = 180;
      } else if (event.value == STEP_DIFF_UNIT || event.value == -STEP_DIFF_UNIT) {
        puzzleDifficulty += event.value / STEP_DIFF_UNIT;
        if (puzzleDifficulty < 0) puzzleDifficulty = 0;
        if (puzzleDifficulty > 4) puzzleDifficulty = 4;
        CHESS_SETTINGS.setPuzzleDifficulty(puzzleDifficulty);
        CHESS_SETTINGS.saveToFile();
      } else if (event.value == STEP_INCREMENT_UNIT || event.value == -STEP_INCREMENT_UNIT) {
        customIncrement += event.value / STEP_INCREMENT_UNIT;
        if (customIncrement < 0) customIncrement = 0;
        if (customIncrement > MAX_INCREMENT_S) customIncrement = MAX_INCREMENT_S;
      } else {
        aiLevel += event.value / STEP_LEVEL_UNIT;
        if (aiLevel < 1) aiLevel = 1;
        if (aiLevel > 8) aiLevel = 8;
        CHESS_SETTINGS.setAiLevel(aiLevel);
        CHESS_SETTINGS.saveToFile();
      }
      requestUpdate();
      break;
    default:
      break;
  }
}

void ChessActivity::loop() {
  if (levelPopup.isActive()) {
    // The popup owns the input while it is up; the app must not see the taps.
    levelPopup.handleInput(mappedInput, [this] { requestUpdate(); });
    if (levelChoice >= 0) {
      RenderLock lock;
      const int idx = levelChoice;
      levelChoice = -1;
      if (popupKind == PopupKind::Count) {
        const int room = PuzzleStore::MAX_PUZZLES - PUZZLE_STORE.count();
        downloadTarget = DOWNLOAD_CHOICES[idx] < room ? DOWNLOAD_CHOICES[idx] : room;
        downloadDone = 0;
        if (downloadTarget > 0) connectThen(Pending::Download);
      } else if (popupKind == PopupKind::Account) {
        if (idx == 0) {
          enterToken();
        } else if (idx == 1) {
          // Sign out: the token goes, and so does the connection.
          CHESS_SETTINGS.setToken("");
          CHESS_SETTINGS.saveToFile();
          if (clientStarted) {
            LICHESS.end();
            clientStarted = false;
          }
          accountReady = false;
          ongoing.clear();
          ratingLine[0] = '\0';
          requestUpdate();
        } else {
          requestUpdate();
        }
      } else {
        aiLevel = idx + 1;
        CHESS_SETTINGS.setAiLevel(aiLevel);
        CHESS_SETTINGS.saveToFile();
        connectThen(Pending::AiGame);
      }
    }
    return;
  }
  {
    // The render task reads the same state; both handlers mutate it.
    RenderLock lock;
    if (clientStarted) handleClientEvents();
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;
    int tx = 0;
    int ty = 0;
    if (state == State::Lobby && CHESS_SETTINGS.hasToken() && mappedInput.wasScreenTapped(tx, ty) &&
        ty < headerBottom) {
      askAccount();  // the header shows the account; a tap on it manages the token
      return;
    }
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      // The scrolling lists: recent games, studies, themes, and results.
      const int dir = swipe == MappedInputManager::SwipeDir::Up ? 1 : -1;
      fui::ListNav* nav = nullptr;
      int count = 0;
      if (state == State::Lobby && tab == 1) {
        nav = &recentNav;
        count = static_cast<int>(recent.size());
      } else if (state == State::Lobby && tab == 3) {
        nav = &studyNav;
        count = STUDY_STORE.count();
      } else if (state == State::Themes) {
        nav = &themeNav;
        count = PUZZLE_THEME_COUNT;
      } else if (state == State::Stats) {
        nav = &statNav;
        count = static_cast<int>(dashboard.themes.size());
      } else if (state == State::Lobby && tab == 0 && playTab == PLAY_CHALLENGE) {
        nav = &friendNav;
        count = static_cast<int>(friends.size());
      }
      if (nav && count > 0 && nav->scrollBy(dir * nav->visibleRows, count)) requestUpdate();
    }
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    RenderLock lock;
    if (state == State::Seeking) {
      cancelSeek();
    } else if (state == State::Custom || state == State::Error || state == State::Themes || state == State::Stats) {
      pendingAction = Pending::None;
      state = State::Lobby;
      rebuildCardLabels();
      requestUpdate();
    } else {
      finish();
    }
  }
}

// --- screen ------------------------------------------------------------------

void ChessActivity::rebuildCardLabels() {
  const TimeControl* set = cards();
  for (int i = 0; i < presetCount(); ++i) {
    snprintf(cardLabels[i], sizeof(cardLabels[i]), "%d+%d", set[i].minutes, set[i].increment);
  }
  snprintf(cardLabels[customIndex()], sizeof(cardLabels[customIndex()]), "%s", tr(STR_CHESS_CUSTOM));
}

void ChessActivity::rebuildOngoingRows() {
  const int count = ongoing.size() < MAX_ONGOING_ROWS ? static_cast<int>(ongoing.size()) : MAX_ONGOING_ROWS;
  for (int i = 0; i < count; ++i) {
    const lichess::OngoingGame& g = ongoing[i];
    snprintf(ongoingLabels[i], sizeof(ongoingLabels[i]), "%s (%d)", g.opponent, g.opponentRating);
    snprintf(ongoingSubtitles[i], sizeof(ongoingSubtitles[i]), "%s, %s, %s", g.speed,
             g.myColor == chess::Color::White ? tr(STR_CHESS_WHITE) : tr(STR_CHESS_BLACK),
             g.isMyTurn ? tr(STR_CHESS_YOUR_MOVE) : tr(STR_CHESS_WAITING));
    ongoingRows[i] = fui::ListItem{};
    ongoingRows[i].label = ongoingLabels[i];
    ongoingRows[i].subtitle = ongoingSubtitles[i];
    ongoingRows[i].value = g.isMyTurn ? "!" : "";
    ongoingRows[i].actionValue = static_cast<int16_t>(i);
  }
}

void ChessActivity::screenTrampoline(UiScreen& screen, void* user) {
  static_cast<ChessActivity*>(user)->buildScreen(screen);
}

void ChessActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  headerBottom = static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing);
  screen.setContentMarginFromScreen(fui::Insets{
      headerBottom, static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.verticalSpacing * 2),
      static_cast<int16_t>(safe.x)});
  const auto& theme = screen.theme();

  switch (state) {
    case State::Connecting:
      screen.centeredText(tr(STR_CHESS_CONNECTING_WIFI));
      break;
    case State::Loading:
      screen.centeredText(loadingLine[0] ? loadingLine : I18N.get(loadingText));
      break;
    case State::Themes:
      buildThemes(screen);
      break;
    case State::Stats:
      buildStats(screen);
      break;
    case State::Error: {
      char msg[96];
      if (errorCode > 0) {
        snprintf(msg, sizeof(msg), "%s (%d)", I18N.get(errorText), errorCode);
      } else {
        snprintf(msg, sizeof(msg), "%s", I18N.get(errorText));
      }
      fui::TextStyle body = theme.bodyText;
      body.maxLines = 4;
      body.align = fui::TextAlign::Center;
      const fui::Rect text = screen.takeTop(static_cast<int16_t>(theme.rowHeight * 3), theme.spaceSm);
      fui::drawText(screen.target(), text, msg, body);
      if (errorDetail[0]) {
        // The server's own words, for example why a seek was refused.
        fui::TextStyle small = theme.smallText;
        small.maxLines = 3;
        small.align = fui::TextAlign::Center;
        fui::drawText(screen.target(), screen.takeTop(static_cast<int16_t>(theme.rowHeight * 2), theme.spaceLg),
                      errorDetail, small);
      } else {
        screen.spacer(theme.spaceLg);
      }
      screen.button(tr(STR_BACK), ACTION_BUTTON, BTN_BACK);
      screen.button(tr(STR_RETRY), ACTION_BUTTON, BTN_RETRY);
      if (errorText == StrId::STR_CHESS_ERROR_TOKEN) screen.button(tr(STR_CHESS_ENTER_TOKEN), ACTION_BUTTON, BTN_TOKEN);
      break;
    }
    case State::Seeking: {
      char msg[80];
      if (waitingForChallenge) {
        snprintf(msg, sizeof(msg), "%s",
                 dialogMode == DialogMode::Computer ? tr(STR_CHESS_STARTING_AI) : tr(STR_CHESS_WAITING_CHALLENGE));
      } else {
        snprintf(msg, sizeof(msg), "%s %d+%d %s", tr(STR_CHESS_SEARCHING), seekControl.minutes, seekControl.increment,
                 seekRated ? tr(STR_CHESS_RATED) : tr(STR_CHESS_CASUAL));
      }
      screen.centeredText(msg);
      screen.button(tr(STR_CANCEL), ACTION_BUTTON, BTN_CANCEL, fui::StateNormal, fui::LayoutAnchor::Bottom);
      break;
    }
    case State::Custom:
      buildCustomDialog(screen);
      break;
    case State::Lobby:
      buildTabBar(screen);
      if (tab == 1) {
        buildGames(screen);
      } else if (tab == 2) {
        buildPuzzles(screen);
      } else if (tab == 3) {
        buildStudies(screen);
      } else {
        buildPlay(screen);
      }
      break;
  }
}

void ChessActivity::buildPlay(UiScreen& screen) {
  rebuildCardLabels();  // the card set follows the sub-tab
  buildSubTabs(screen);
  switch (playTab) {
    case PLAY_COMPUTER:
      buildComputer(screen);
      break;
    case PLAY_CHALLENGE:
      buildChallenge(screen);
      break;
    case PLAY_LOCAL:
      buildLocal(screen);
      break;
    default:
      buildMatch(screen);
      break;
  }
}

void ChessActivity::buildSubTabs(UiScreen& screen) {
  const auto& theme = screen.theme();
  fui::TabItem tabs[PLAY_TAB_COUNT];
  const StrId labels[PLAY_TAB_COUNT] = {StrId::STR_CHESS_SUB_MATCH, StrId::STR_CHESS_COMPUTER,
                                        StrId::STR_CHESS_CHALLENGE, StrId::STR_CHESS_SUB_LOCAL};
  for (int i = 0; i < PLAY_TAB_COUNT; ++i) {
    tabs[i].label = I18N.get(labels[i]);
    tabs[i].value = static_cast<int16_t>(10 + i);
    tabs[i].selected = playTab == i;
  }
  fui::TabBarProps props;
  props.tabs = tabs;
  props.count = PLAY_TAB_COUNT;
  props.action = ACTION_TAB;
  props.inputMask = fui::InputTouch;
  props.text = theme.smallText;
  props.divider = true;
  // The same inverted highlight as the main tabs, so both levels read alike.
  props.tabStyles = tabStyles(theme);
  const fui::Rect band = screen.takeTop(theme.rowHeight, theme.spaceSm);
  fui::tabBar(screen.frame(), band, props);
}

void ChessActivity::buildMatch(UiScreen& screen) {
  const auto& theme = screen.theme();
  qrRect = fui::Rect{};
  if (!CHESS_SETTINGS.hasToken()) {
    // No token yet: a QR code opens the token page with the scopes ticked.
    // The code is drawn after the UI, in render().
    fui::TextStyle body = theme.smallText;
    body.maxLines = 4;
    body.align = fui::TextAlign::Center;
    fui::drawText(screen.target(), screen.takeTop(static_cast<int16_t>(theme.rowHeight * 2), theme.spaceSm),
                  tr(STR_CHESS_TOKEN_HELP), body);
    qrRect = screen.takeTop(static_cast<int16_t>(QR_SIZE), theme.spaceSm);
    screen.button(tr(STR_CHESS_ENTER_TOKEN), ACTION_BUTTON, BTN_TOKEN);
    return;
  }
  // Ongoing games, up to three rows. Offline they load on a tap.
  if (!signedIn()) {
    fui::ButtonProps b;
    b.label = tr(STR_CHESS_LOAD_ONGOING);
    b.action = ACTION_BUTTON;
    b.value = BTN_LOAD_ONGOING;
    b.inputMask = fui::InputTouch;
    b.text = theme.smallText;
    screen.button(b);
  }
  const int rows = ongoing.size() < MAX_ONGOING_ROWS ? static_cast<int>(ongoing.size()) : MAX_ONGOING_ROWS;
  if (rows > 0) {
    fui::ListProps props;
    props.items = ongoingRows;
    props.count = static_cast<uint16_t>(rows);
    props.action = ACTION_ROW;
    props.inputMask = fui::InputTouch;
    props.selectedIndex = -1;
    props.labelText = theme.bodyText;
    props.subtitleText = theme.smallText;
    const int16_t rowH = static_cast<int16_t>(theme.rowHeight + theme.spaceMd);
    screen.list(props, static_cast<int16_t>(rowH * rows));
    screen.spacer(theme.spaceSm);
  }
  // The Rated switch sits right above the cards it applies to.
  fui::ToggleRowProps rated;
  rated.row.label = tr(STR_CHESS_RATED);
  rated.checked = seekRated;
  rated.toggleAction = ACTION_RATED;
  rated.row.inputMask = fui::InputTouch;
  screen.toggleRow(rated);
  screen.spacer(theme.spaceSm);
  buildCards(screen);
}

void ChessActivity::buildComputer(UiScreen& screen) {
  const auto& theme = screen.theme();
  fui::TextStyle small = theme.smallText;
  small.align = fui::TextAlign::Center;
  fui::drawText(screen.target(), screen.takeTop(theme.rowHeight, theme.spaceSm), tr(STR_CHESS_COMPUTER_HINT), small);
  buildCards(screen);
}

void ChessActivity::buildLocal(UiScreen& screen) {
  const auto& theme = screen.theme();
  fui::TextStyle hint = theme.smallText;
  hint.maxLines = 3;
  hint.align = fui::TextAlign::Center;
  fui::drawText(screen.target(), screen.takeTop(static_cast<int16_t>(theme.rowHeight * 2), theme.spaceLg),
                tr(STR_CHESS_LOCAL_HINT), hint);
  screen.button(tr(STR_CHESS_START_LOCAL), ACTION_BUTTON, BTN_LOCAL);
}

void ChessActivity::buildChallenge(UiScreen& screen) {
  const auto& theme = screen.theme();
  // Bottom: a typed name for anyone not in the list.
  screen.button(tr(STR_CHESS_TYPE_NAME), ACTION_BUTTON, BTN_CHALLENGE_NAME, fui::StateNormal,
                fui::LayoutAnchor::Bottom);
  fui::TextStyle hint = theme.smallText;
  hint.maxLines = 3;
  hint.align = fui::TextAlign::Center;
  if (!CHESS_SETTINGS.hasToken()) {
    fui::drawText(screen.target(), screen.takeTop(static_cast<int16_t>(theme.rowHeight * 2), theme.spaceSm),
                  tr(STR_CHESS_NEED_TOKEN_HINT), hint);
    return;
  }
  fui::ToggleRowProps rated;
  rated.row.label = tr(STR_CHESS_RATED);
  rated.checked = seekRated;
  rated.toggleAction = ACTION_RATED;
  rated.row.inputMask = fui::InputTouch;
  screen.toggleRow(rated);
  screen.spacer(theme.spaceSm);
  if (friendsLoading) {
    screen.centeredText(tr(STR_CHESS_LOADING));
    return;
  }
  if (!signedIn()) {
    screen.button(tr(STR_CHESS_LOAD_FRIENDS), ACTION_BUTTON, BTN_LOAD_FRIENDS);
    return;
  }
  if (friendsStatus == 401 || friendsStatus == 403) {
    fui::drawText(screen.target(), screen.takeTop(static_cast<int16_t>(theme.rowHeight * 2), theme.spaceSm),
                  tr(STR_CHESS_FRIENDS_SCOPE), hint);
    return;
  }
  if (friendsStatus != 0) {
    char msg[64];
    snprintf(msg, sizeof(msg), "%s (%d)", tr(STR_CHESS_ERROR_NETWORK), friendsStatus);
    screen.centeredText(msg);
    return;
  }
  const int count = friends.size() < static_cast<size_t>(MAX_FRIENDS) ? static_cast<int>(friends.size()) : MAX_FRIENDS;
  if (count == 0) {
    fui::drawText(screen.target(), screen.takeTop(static_cast<int16_t>(theme.rowHeight * 2), theme.spaceSm),
                  tr(STR_CHESS_NO_FRIENDS), hint);
    return;
  }
  fui::ListProps props;
  props.items = friendRows;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.selectedIndex = -1;
  props.labelText = theme.bodyText;
  props.valueText = theme.smallText;
  const int16_t rowH = static_cast<int16_t>(theme.rowHeight + theme.spaceSm);
  friendNav.syncToProps(screen.body(), rowH, theme.listRowGap, count, props);
  props.selectedIndex = -1;
  screen.list(props);
}

void ChessActivity::buildCards(UiScreen& screen) {
  // Time-control cards in a grid that fills what is left.
  const auto& theme = screen.theme();
  const int count = presetCount() + 1;
  const TimeControl* set = cards();
  const fui::Rect body = screen.body();
  const int16_t gap = theme.spaceMd;
  const int cardRows = (count + CARD_COLUMNS - 1) / CARD_COLUMNS;
  const int16_t cardW = static_cast<int16_t>((body.width - gap * (CARD_COLUMNS - 1)) / CARD_COLUMNS);
  int16_t cardH = static_cast<int16_t>((body.height - gap * (cardRows - 1)) / cardRows);
  if (cardH > cardW) cardH = cardW;
  if (cardH < theme.minTouchSize) cardH = theme.minTouchSize;
  for (int i = 0; i < count; ++i) {
    const int col = i % CARD_COLUMNS;
    const int row = i / CARD_COLUMNS;
    const fui::Rect rect{static_cast<int16_t>(body.x + col * (cardW + gap)),
                         static_cast<int16_t>(body.y + row * (cardH + gap)), cardW, cardH};
    fui::ButtonProps card;
    card.label = cardLabels[i];
    card.action = ACTION_CARD;
    card.value = static_cast<int16_t>(i);
    card.inputMask = fui::InputTouch;
    card.text = theme.titleText;
    card.text.align = fui::TextAlign::Center;
    card.radius = theme.controlRadius / 2;
    screen.button(card, rect);
    if (i < customIndex()) {
      fui::TextStyle small = theme.smallText;
      small.align = fui::TextAlign::Center;
      const int16_t lh = screen.target().lineHeight(small.font);
      fui::drawText(screen.target(),
                    fui::Rect{rect.x, static_cast<int16_t>(rect.y + rect.height - lh - theme.spaceSm), rect.width, lh},
                    I18N.get(categoryFor(set[i].minutes, set[i].increment)), small);
    }
  }
}

void ChessActivity::buildTabBar(UiScreen& screen) {
  const auto& theme = screen.theme();
  fui::TabItem tabs[TAB_COUNT];
  const StrId labels[TAB_COUNT] = {StrId::STR_CHESS_TAB_PLAY, StrId::STR_CHESS_TAB_GAMES, StrId::STR_CHESS_TAB_PUZZLES,
                                   StrId::STR_CHESS_TAB_STUDIES};
  for (int i = 0; i < TAB_COUNT; ++i) {
    tabs[i].label = I18N.get(labels[i]);
    tabs[i].value = static_cast<int16_t>(i);
    tabs[i].selected = tab == i;
  }
  fui::TabBarProps props;
  props.tabs = tabs;
  props.count = TAB_COUNT;
  props.action = ACTION_TAB;
  props.inputMask = fui::InputTouch;
  props.text = theme.bodyText;
  props.divider = true;
  props.tabStyles = tabStyles(theme);
  const fui::Rect band = screen.takeTop(theme.rowHeight, theme.spaceSm);
  fui::tabBar(screen.frame(), band, props);
}

void ChessActivity::buildGames(UiScreen& screen) {
  const auto& theme = screen.theme();
  if (!CHESS_SETTINGS.hasToken()) {
    screen.centeredText(tr(STR_CHESS_GAMES_NEED_TOKEN));
    return;
  }
  screen.button(tr(STR_CHESS_REFRESH), ACTION_BUTTON, BTN_REFRESH_GAMES, fui::StateNormal, fui::LayoutAnchor::Bottom);
  if (!signedIn()) {
    screen.centeredText(tr(STR_CHESS_OFFLINE_HINT));
    return;
  }
  if (recentLoading) {
    screen.centeredText(tr(STR_CHESS_LOADING));
    return;
  }
  if (recent.empty()) {
    screen.centeredText(tr(STR_CHESS_NO_GAMES));
    return;
  }
  const int count = recent.size() < MAX_RECENT ? static_cast<int>(recent.size()) : MAX_RECENT;
  fui::ListProps props;
  props.items = recentRows;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.selectedIndex = -1;
  props.labelText = theme.bodyText;
  props.subtitleText = theme.smallText;
  const int16_t rowH = static_cast<int16_t>(theme.rowHeight + theme.spaceMd);
  recentNav.syncToProps(screen.body(), rowH, theme.listRowGap, count, props);
  props.selectedIndex = -1;
  screen.list(props);
}

void ChessActivity::buildPuzzles(UiScreen& screen) {
  const auto& theme = screen.theme();
  fui::TextStyle small = theme.smallText;
  small.align = fui::TextAlign::Center;

  // The store: puzzles that fit the selected themes, and results that wait to go out.
  const int ready = readyCount();
  const int total = PUZZLE_STORE.count();
  if (total == 0) {
    snprintf(puzzleCountLine, sizeof(puzzleCountLine), "%s", tr(STR_CHESS_NO_PUZZLES_SAVED));
  } else if (selectedThemeCount() == 0) {
    snprintf(puzzleCountLine, sizeof(puzzleCountLine), tr(STR_CHESS_PUZZLES_SAVED), total);
  } else if (ready > 0) {
    snprintf(puzzleCountLine, sizeof(puzzleCountLine), tr(STR_CHESS_PUZZLES_MATCH), ready, total);
  } else {
    snprintf(puzzleCountLine, sizeof(puzzleCountLine), tr(STR_CHESS_PUZZLES_MATCH_NONE), total);
  }
  fui::drawText(screen.target(), screen.takeTop(theme.rowHeight, 0), puzzleCountLine, theme.bodyText);
  if (PUZZLE_STORE.pendingResults() > 0) {
    snprintf(puzzleResultsLine, sizeof(puzzleResultsLine), tr(STR_CHESS_RESULTS_TO_SEND),
             PUZZLE_STORE.pendingResults());
    fui::drawText(screen.target(), screen.takeTop(theme.rowHeight, 0), puzzleResultsLine, theme.smallText);
  }
  screen.spacer(theme.spaceSm);

  fui::StepperRowProps diff;
  diff.row.label = tr(STR_CHESS_DIFFICULTY);
  diff.value = I18N.get(DIFFICULTY_LABELS[puzzleDifficulty]);
  diff.decrement = ACTION_STEP;
  diff.increment = ACTION_STEP;
  diff.decrementValue = -STEP_DIFF_UNIT;
  diff.incrementValue = STEP_DIFF_UNIT;
  diff.row.inputMask = fui::InputTouch;
  screen.stepperRow(diff);
  buildThemeSummary();
  snprintf(themeLine, sizeof(themeLine), tr(STR_CHESS_THEMES_LINE), themeSummary);
  screen.button(themeLine, ACTION_BUTTON, BTN_THEME);
  screen.spacer(theme.spaceLg);

  if (puzzleLoading && downloadTarget > 0) {
    snprintf(progressLine, sizeof(progressLine), tr(STR_CHESS_DOWNLOAD_PROGRESS), downloadDone, downloadTarget);
    fui::drawText(screen.target(), screen.takeTop(theme.rowHeight, theme.spaceSm), progressLine, small);
    screen.button(tr(STR_CHESS_STOP), ACTION_BUTTON, BTN_STOP_DOWNLOAD);
  } else if (puzzleLoading) {
    fui::drawText(screen.target(), screen.takeTop(theme.rowHeight, theme.spaceSm), tr(STR_CHESS_DOWNLOADING), small);
  } else {
    // Next puzzle takes a saved one, or downloads a batch first when none is saved.
    screen.button(tr(STR_CHESS_NEXT_PUZZLE), ACTION_BUTTON, BTN_NEXT_PUZZLE);
    if (PUZZLE_STORE.count() < PuzzleStore::MAX_PUZZLES) {
      screen.button(tr(STR_CHESS_DOWNLOAD_PUZZLES), ACTION_BUTTON, BTN_DOWNLOAD);
    } else {
      fui::drawText(screen.target(), screen.takeTop(theme.rowHeight, theme.spaceSm), tr(STR_CHESS_STORE_FULL), small);
    }
    if (CHESS_SETTINGS.hasToken()) screen.button(tr(STR_CHESS_YOUR_RESULTS), ACTION_BUTTON, BTN_STATS);
  }
  if (lastPuzzleRating > 0) {
    snprintf(puzzleLine, sizeof(puzzleLine), tr(STR_CHESS_PUZZLE_TITLE), lastPuzzleRating);
    fui::drawText(screen.target(), screen.takeTop(theme.rowHeight, theme.spaceSm), puzzleLine, small);
  }
  fui::TextStyle help = theme.smallText;
  help.maxLines = 3;
  help.align = fui::TextAlign::Center;
  fui::drawText(screen.target(), screen.takeBottom(static_cast<int16_t>(theme.rowHeight * 2), 0),
                tr(STR_CHESS_PUZZLE_HELP), help);
}

void ChessActivity::buildStudies(UiScreen& screen) {
  const auto& theme = screen.theme();
  // Bottom: my studies, another user's studies, and a study by id.
  const fui::Rect band = screen.takeBottom(theme.rowHeight, theme.spaceSm);
  const int16_t third = static_cast<int16_t>((band.width - theme.spaceSm * 2) / 3);
  const StrId labels[3] = {StrId::STR_CHESS_MY_STUDIES, StrId::STR_CHESS_BY_USER, StrId::STR_CHESS_OPEN_BY_ID};
  const int16_t values[3] = {BTN_REFRESH_STUDIES, BTN_STUDIES_BY_USER, BTN_STUDY_BY_ID};
  for (int i = 0; i < 3; ++i) {
    fui::ButtonProps b;
    b.label = I18N.get(labels[i]);
    b.action = ACTION_BUTTON;
    b.value = values[i];
    b.inputMask = fui::InputTouch;
    b.text = theme.smallText;
    if (i == 0) b.state = CHESS_SETTINGS.hasToken() ? fui::StateNormal : fui::StateDisabled;
    screen.button(b, fui::Rect{static_cast<int16_t>(band.x + i * (third + theme.spaceSm)), band.y, third, band.height});
  }
  if (studiesLoading) {
    screen.centeredText(tr(STR_CHESS_LOADING));
    return;
  }
  const int count = STUDY_STORE.count();
  int missing = 0;
  for (const auto& e : STUDY_STORE.entries()) missing += e.saved ? 0 : 1;
  if (missing > 0) {
    snprintf(studyDownloadLabel, sizeof(studyDownloadLabel), tr(STR_CHESS_DOWNLOAD_STUDIES), missing);
    screen.button(studyDownloadLabel, ACTION_BUTTON, BTN_DOWNLOAD_STUDIES);
  }
  if (count > 0) {
    // Whose list this is: saved studies come first, then the last listing.
    if (studyUser[0]) {
      snprintf(studyListTitle, sizeof(studyListTitle), tr(STR_CHESS_STUDIES_OF), studyUser);
    } else {
      snprintf(studyListTitle, sizeof(studyListTitle), "%s", tr(STR_CHESS_YOUR_STUDIES));
    }
    fui::TextStyle small = theme.smallText;
    small.align = fui::TextAlign::Center;
    fui::drawText(screen.target(), screen.takeTop(theme.rowHeight, 0), studyListTitle, small);
  }
  if (count == 0) {
    fui::TextStyle hint = theme.smallText;
    hint.maxLines = 4;
    hint.align = fui::TextAlign::Center;
    fui::drawText(screen.target(), screen.takeTop(static_cast<int16_t>(theme.rowHeight * 3), theme.spaceSm),
                  tr(STR_CHESS_STUDIES_HINT), hint);
    return;
  }
  fui::ListProps props;
  props.items = studyRows.data();
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.selectedIndex = -1;
  props.labelText = theme.bodyText;
  props.subtitleText = theme.smallText;
  const int16_t rowH = static_cast<int16_t>(theme.rowHeight + theme.spaceMd);
  studyNav.syncToProps(screen.body(), rowH, theme.listRowGap, count, props);
  props.selectedIndex = -1;
  screen.list(props);
}

void ChessActivity::buildThemes(UiScreen& screen) {
  const auto& theme = screen.theme();
  screen.button(tr(STR_BACK), ACTION_BUTTON, BTN_BACK, fui::StateNormal, fui::LayoutAnchor::Bottom);
  const int selected = selectedThemeCount();
  themeRows[0] = fui::ListItem{};
  themeRows[0].label = tr(STR_CHESS_ALL_THEMES);
  themeRows[0].toggle = true;
  themeRows[0].toggleChecked = selected == 0;
  themeRows[0].actionValue = 0;
  for (int i = 1; i < PUZZLE_THEME_COUNT; ++i) {
    snprintf(themeRowLabels[i], sizeof(themeRowLabels[i]), tr(STR_CHESS_THEME_WITH_COUNT),
             I18N.get(PUZZLE_THEMES[i].name), PUZZLE_STORE.count(1u << i));
    themeRows[i] = fui::ListItem{};
    themeRows[i].label = themeRowLabels[i];
    themeRows[i].toggle = true;
    themeRows[i].toggleChecked = themeSelected[i];
    themeRows[i].actionValue = static_cast<int16_t>(i);
  }
  fui::ListProps props;
  props.items = themeRows;
  props.count = PUZZLE_THEME_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.selectedIndex = -1;
  props.labelText = theme.bodyText;
  const int16_t rowH = static_cast<int16_t>(theme.rowHeight + theme.spaceSm);
  themeNav.syncToProps(screen.body(), rowH, theme.listRowGap, PUZZLE_THEME_COUNT, props);
  props.selectedIndex = -1;
  screen.list(props);
}

// --- puzzle themes ------------------------------------------------------------

void ChessActivity::loadThemeSelection() {
  for (bool& on : themeSelected) on = false;
  const std::string& keys = CHESS_SETTINGS.getPuzzleThemes();
  size_t start = 0;
  while (start < keys.size()) {
    size_t end = keys.find(',', start);
    if (end == std::string::npos) end = keys.size();
    const std::string key = keys.substr(start, end - start);
    for (int i = 1; i < PUZZLE_THEME_COUNT; ++i) {
      if (key == PUZZLE_THEMES[i].key) themeSelected[i] = true;
    }
    start = end + 1;
  }
}

void ChessActivity::saveThemeSelection() {
  std::string keys;
  for (int i = 0; i < PUZZLE_THEME_COUNT; ++i) {
    if (!themeSelected[i]) continue;
    if (!keys.empty()) keys.push_back(',');
    keys += PUZZLE_THEMES[i].key;
  }
  CHESS_SETTINGS.setPuzzleThemes(keys);
  CHESS_SETTINGS.saveToFile();
}

int ChessActivity::selectedThemeCount() const {
  int n = 0;
  for (const bool on : themeSelected) n += on ? 1 : 0;
  return n;
}

uint32_t ChessActivity::selectionMask() const {
  uint32_t mask = 0;
  for (int i = 1; i < PUZZLE_THEME_COUNT; ++i) {
    if (themeSelected[i]) mask |= 1u << i;
  }
  return mask;
}

int ChessActivity::readyCount() const { return PUZZLE_STORE.count(selectionMask()); }

const char* ChessActivity::downloadThemeFor(int batch) const {
  const int selected = selectedThemeCount();
  if (selected == 0) return "mix";
  int skip = batch % selected;
  for (int i = 1; i < PUZZLE_THEME_COUNT; ++i) {
    if (!themeSelected[i]) continue;
    if (skip == 0) return PUZZLE_THEMES[i].key;
    --skip;
  }
  return "mix";
}

bool ChessActivity::takeSelected(lichess::Puzzle& out) { return PUZZLE_STORE.takeNext(selectionMask(), out); }

void ChessActivity::buildThemeSummary() {
  const int selected = selectedThemeCount();
  if (selected == 0) {
    snprintf(themeSummary, sizeof(themeSummary), "%s", tr(STR_CHESS_ALL_THEMES));
    return;
  }
  themeSummary[0] = '\0';
  int shown = 0;
  for (int i = 0; i < PUZZLE_THEME_COUNT && shown < 2; ++i) {
    if (!themeSelected[i]) continue;
    const size_t used = strlen(themeSummary);
    snprintf(themeSummary + used, sizeof(themeSummary) - used, "%s%s", shown ? ", " : "",
             I18N.get(PUZZLE_THEMES[i].name));
    ++shown;
  }
  if (selected > shown) {
    const size_t used = strlen(themeSummary);
    snprintf(themeSummary + used, sizeof(themeSummary) - used, " +%d", selected - shown);
  }
}

void ChessActivity::rebuildFriendRows() {
  const int count = friends.size() < static_cast<size_t>(MAX_FRIENDS) ? static_cast<int>(friends.size()) : MAX_FRIENDS;
  for (int i = 0; i < count; ++i) {
    friendRows[i] = fui::ListItem{};
    friendRows[i].label = friends[i].name;
    friendRows[i].value = friends[i].playing ? tr(STR_CHESS_PLAYING) : friends[i].online ? tr(STR_CHESS_ONLINE) : "";
    friendRows[i].actionValue = static_cast<int16_t>(i);
  }
}

void ChessActivity::buildStats(UiScreen& screen) {
  const auto& theme = screen.theme();
  screen.button(tr(STR_BACK), ACTION_BUTTON, BTN_BACK, fui::StateNormal, fui::LayoutAnchor::Bottom);
  if (dashboardLoading) {
    screen.centeredText(tr(STR_CHESS_LOADING));
    return;
  }
  fui::TextStyle small = theme.smallText;
  small.maxLines = 2;
  small.align = fui::TextAlign::Center;
  fui::drawText(screen.target(), screen.takeTop(static_cast<int16_t>(theme.rowHeight), theme.spaceSm), statsLine,
                small);
  fui::drawText(screen.target(), screen.takeTop(static_cast<int16_t>(theme.rowHeight), theme.spaceSm),
                tr(STR_CHESS_STATS_HINT), small);
  const int count =
      dashboard.themes.size() < static_cast<size_t>(MAX_STATS) ? static_cast<int>(dashboard.themes.size()) : MAX_STATS;
  if (count == 0) return;
  fui::ListProps props;
  props.items = statRows;
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.selectedIndex = -1;
  props.labelText = theme.bodyText;
  props.subtitleText = theme.smallText;
  props.valueText = theme.bodyText;
  const int16_t rowH = static_cast<int16_t>(theme.rowHeight + theme.spaceMd);
  statNav.syncToProps(screen.body(), rowH, theme.listRowGap, count, props);
  props.selectedIndex = -1;
  screen.list(props);
}

void ChessActivity::buildCustomDialog(UiScreen& screen) {
  const auto& theme = screen.theme();
  snprintf(customValues[0], sizeof(customValues[0]), "%d %s", customMinutes, tr(STR_CHESS_MIN_SHORT));
  snprintf(customValues[1], sizeof(customValues[1]), "%d %s", customIncrement, tr(STR_CHESS_SEC_SHORT));
  snprintf(customValues[2], sizeof(customValues[2]), tr(STR_CHESS_LEVEL_ABOUT), aiLevel, AI_RATING[aiLevel - 1]);

  char title[80];
  switch (dialogMode) {
    case DialogMode::Challenge:
      snprintf(title, sizeof(title), "%s %s", tr(STR_CHESS_CHALLENGE), challengeUser);
      break;
    case DialogMode::Computer:
      snprintf(title, sizeof(title), "%s", tr(STR_CHESS_COMPUTER));
      break;
    default:
      snprintf(title, sizeof(title), "%s", tr(STR_CHESS_CUSTOM));
      break;
  }
  fui::TextStyle titleStyle = theme.titleText;
  titleStyle.align = fui::TextAlign::Center;
  fui::drawText(screen.target(), screen.takeTop(theme.rowHeight, theme.spaceSm), title, titleStyle);

  fui::StepperRowProps minutes;
  minutes.row.label = tr(STR_CHESS_MINUTES);
  minutes.value = customValues[0];
  minutes.decrement = ACTION_STEP;
  minutes.increment = ACTION_STEP;
  minutes.decrementValue = -1;
  minutes.incrementValue = 1;
  minutes.row.inputMask = fui::InputTouch;
  screen.stepperRow(minutes);

  fui::StepperRowProps inc;
  inc.row.label = tr(STR_CHESS_INCREMENT);
  inc.value = customValues[1];
  inc.decrement = ACTION_STEP;
  inc.increment = ACTION_STEP;
  inc.decrementValue = -STEP_INCREMENT_UNIT;
  inc.incrementValue = STEP_INCREMENT_UNIT;
  inc.row.inputMask = fui::InputTouch;
  screen.stepperRow(inc);

  if (dialogMode != DialogMode::Computer) {
    fui::ToggleRowProps rated;
    rated.row.label = tr(STR_CHESS_RATED);
    rated.checked = seekRated;
    rated.toggleAction = ACTION_RATED;
    rated.row.inputMask = fui::InputTouch;
    screen.toggleRow(rated);
  }

  screen.spacer(theme.spaceLg);
  screen.button(tr(STR_CHESS_PLAY), ACTION_BUTTON, BTN_DIALOG_OK);
  screen.button(tr(STR_CANCEL), ACTION_BUTTON, BTN_DIALOG_CANCEL);
}

void ChessActivity::render(RenderLock&&) {
  renderer.clearScreen();
  auto& theme = UITheme::getInstance();
  const auto& metrics = theme.getMetrics();
  const Rect safe = theme.getScreenSafeArea(renderer, false, false);
  const char* subtitle = nullptr;
  if (state == State::Themes) {
    subtitle = tr(STR_CHESS_THEME);
  } else if (state == State::Stats) {
    subtitle = tr(STR_CHESS_YOUR_RESULTS);
  } else if (state == State::Lobby || state == State::Seeking || state == State::Custom) {
    if (signedIn() && account.username[0]) {
      subtitle = account.username;
    } else if (CHESS_SETTINGS.hasToken()) {
      subtitle = tr(STR_CHESS_OFFLINE);
    }
  }
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight}, tr(STR_CHESS),
                 subtitle);
  renderUi();
  if (state == State::Lobby && tab == 0 && !CHESS_SETTINGS.hasToken() && qrRect.height > 0) {
    const int x = qrRect.x + (qrRect.width - QR_SIZE) / 2;
    QrUtils::drawQrCode(renderer, Rect{x, qrRect.y, QR_SIZE, QR_SIZE}, TOKEN_URL);
  }
  if (state == State::Lobby && tab == 0 && ratingLine[0]) {
    renderer.drawCenteredText(UI_10_FONT_ID, safe.y + safe.height - renderer.getLineHeight(UI_10_FONT_ID) - 2,
                              ratingLine, true);
  }
  if (levelPopup.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer();
}
