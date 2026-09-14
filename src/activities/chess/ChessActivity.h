#pragma once
#include <ChessSettings.h>
#include <I18n.h>
#include <LichessProtocol.h>
#include <PuzzleStore.h>
#include <PuzzleThemes.h>
#include <StudyStore.h>

#include <string>
#include <vector>

#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "components/UiAppHost.h"

// Chess lobby: token setup, ongoing games, time-control cards, recent games,
// and puzzles. The radio belongs to this screen: it comes up on demand and
// goes down in onExit.
//
// Flow rule: the lobby works offline, and every action that needs Lichess
// (a seek, an engine game, a challenge, the games list, a puzzle download)
// connects on its own and then continues. The Wi-Fi picker appears only when
// no saved network answers.
class ChessActivity final : public Activity, private UiAppHost {
 public:
  ChessActivity(GfxRenderer& renderer, MappedInputManager& mappedInput);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Connecting and Loading are the transient states of connecting on demand.
  // Computer is the engine screen.
  enum class State : uint8_t { Lobby, Connecting, Loading, Seeking, Custom, Error, Themes, Stats };
  // What the custom dialog starts when confirmed.
  enum class DialogMode : uint8_t { Seek, Challenge, Computer };

  struct TimeControl {
    uint8_t minutes;
    uint8_t increment;
  };
  // People mode: the Lichess app presets that Lichess pairs from open seeks
  // (Rapid and Classical; Blitz seeks come back "Invalid time control").
  static constexpr TimeControl PEOPLE_CARDS[5] = {{10, 0}, {10, 5}, {15, 10}, {30, 0}, {30, 20}};
  static constexpr int PEOPLE_PRESETS = 5;
  // Computer mode: the full app preset list; the engine accepts any control.
  static constexpr TimeControl AI_CARDS[9] = {{3, 0},  {3, 2},   {5, 0},  {5, 3},  {10, 0},
                                              {10, 5}, {15, 10}, {30, 0}, {30, 20}};
  static constexpr int AI_PRESETS = 9;
  static constexpr int CARD_COUNT = 10;  // largest preset list plus Custom
  static constexpr int CARD_COLUMNS = 3;
  static constexpr int MAX_ONGOING_ROWS = 3;
  static constexpr int MAX_RECENT = 12;
  static constexpr int MAX_INCREMENT_S = 60;
  static constexpr int NOW_PLAYING_RETRIES = 5;
  static constexpr uint32_t NOW_PLAYING_RETRY_MS = 2000;

  State state = State::Lobby;
  bool wifiActivated = false;
  bool clientStarted = false;
  // The account lookup succeeded, or there is no token and the client runs anonymously.
  bool accountReady = false;
  bool connected() const { return clientStarted && accountReady; }
  // Connected with a token: games and seeks need this; puzzles only need connected().
  bool signedIn() const { return connected() && CHESS_SETTINGS.hasToken(); }

  // The action that asked for a connection. It runs as soon as the account is
  // ready (or, for puzzles, as soon as the link is up), so one tap does the job.
  enum class Pending : uint8_t {
    None,
    Seek,
    AiGame,
    Challenge,
    Games,
    Download,
    Puzzle,
    Studies,
    Study,
    Dashboard,
    Friends
  };
  Pending pendingAction = Pending::None;
  TimeControl pendingControl{10, 0};
  void connectThen(Pending action);
  void runPending();
  void afterConnect();
  // The once-per-connection result send and refill has run.
  bool autoSynced = false;

  StrId errorText = StrId::STR_CHESS_ERROR_NETWORK;
  int errorCode = 0;
  char errorDetail[64] = {};
  lichess::Account account;
  std::vector<lichess::OngoingGame> ongoing;
  // Row cache for the ongoing list, rebuilt only when `ongoing` changes.
  freeink::ui::ListItem ongoingRows[MAX_ONGOING_ROWS];
  char ongoingLabels[MAX_ONGOING_ROWS][48];
  char ongoingSubtitles[MAX_ONGOING_ROWS][48];
  char cardLabels[CARD_COUNT][20];
  char ratingLine[64];
  TimeControl seekControl{10, 0};
  bool seekRated = true;
  bool waitingForChallenge = false;
  // Games that existed before the seek, so a match can be told apart from them.
  std::vector<std::string> preSeekGameIds;
  int nowPlayingRetries = 0;
  uint32_t nowPlayingRetryAt = 0;
  DialogMode dialogMode = DialogMode::Seek;
  int customMinutes = 10;
  int customIncrement = 0;
  int aiLevel = 3;
  const TimeControl* cards() const { return playTab == PLAY_COMPUTER ? AI_CARDS : PEOPLE_CARDS; }
  int presetCount() const { return playTab == PLAY_COMPUTER ? AI_PRESETS : PEOPLE_PRESETS; }
  int customIndex() const { return presetCount(); }
  // Lobby tab: 0 play, 1 games, 2 puzzles, 3 studies. Remembered in the settings.
  int tab = 0;
  // Play sub-tab: match (open seeks), computer, challenge (friends), local game.
  static constexpr int PLAY_MATCH = 0;
  static constexpr int PLAY_COMPUTER = 1;
  static constexpr int PLAY_CHALLENGE = 2;
  static constexpr int PLAY_LOCAL = 3;
  static constexpr int PLAY_TAB_COUNT = 4;
  int playTab = 0;
  // Where the header ends: a tap on the header opens the account popup.
  int16_t headerBottom = 0;
  static constexpr int TAB_COUNT = 4;
  // What the Loading screen says: connecting, or a study download.
  StrId loadingText = StrId::STR_CHESS_CONNECTING;
  // Recent games for the Games tab, fetched once per session or on Refresh.
  std::vector<lichess::GameSummary> recent;
  bool recentLoaded = false;
  bool ongoingLoaded = false;
  bool recentLoading = false;
  freeink::ui::ListItem recentRows[MAX_RECENT];
  char recentLabels[MAX_RECENT][48];
  char recentSubtitles[MAX_RECENT][48];
  freeink::ui::ListNav recentNav;
  // Puzzles tab.
  bool puzzleLoading = false;
  // Set while a download runs for a puzzle that opens as soon as it arrives.
  bool pendingPuzzleOpen = false;
  // Results handed to a batch send; restored if the send fails.
  std::vector<PuzzleStore::Result> resultsInFlight;
  int puzzleDifficulty = 2;
  int lastPuzzleRating = 0;
  char lastPuzzleId[lichess::GAME_ID_LEN] = {};
  char puzzleCountLine[48];
  // The theme of the batch in flight; the reply goes to its lane.
  char downloadTheme[24] = {};
  char puzzleResultsLine[40];
  char puzzleLine[48];
  char themeLine[48];
  // Studies tab: rows over the study store's list.
  std::vector<freeink::ui::ListItem> studyRows;
  char studySubtitles[StudyStore::MAX_STUDIES][32];
  freeink::ui::ListNav studyNav;
  bool studiesLoading = false;
  bool studiesLoaded = false;
  char pendingStudyId[16] = {};
  // Whose studies the list shows: empty for the signed-in user.
  char studyUser[lichess::NAME_LEN] = {};
  char studyListTitle[64] = {};
  char studyDownloadLabel[48] = {};
  void askAccount();
  // Friends screen: the players the user follows, to challenge with one tap.
  static constexpr int MAX_FRIENDS = 60;
  std::vector<lichess::Friend> friends;
  bool friendsLoading = false;
  int friendsStatus = 0;  // the HTTP status of a failed list, for the scope hint
  freeink::ui::ListItem friendRows[MAX_FRIENDS];
  freeink::ui::ListNav friendNav;
  void rebuildFriendRows();
  // Where the token QR code goes on the Play tab while there is no token.
  freeink::ui::Rect qrRect{};
  void enterStudyUser();
  // "Download all": the listed studies not on the card, fetched one by one.
  std::vector<std::string> studyQueue;
  int studyQueueTotal = 0;
  char loadingLine[48] = {};
  void queueMissingStudies();
  void nextQueuedStudy();
  void rebuildStudyRows();
  void buildStudies(UiScreen& screen);
  void openStudy(const char* id);
  void enterStudyId();
  // Puzzle theme picker and the results screen.
  static constexpr int MAX_STATS = 24;
  // The theme picker: an "all" row, then one toggle row per real theme (the
  // mix entry is only a download angle) with the count of puzzles that have it.
  freeink::ui::ListItem themeRows[PUZZLE_THEME_COUNT];
  char themeRowLabels[PUZZLE_THEME_COUNT][40];
  bool themeSelected[PUZZLE_THEME_COUNT] = {};
  char themeSummary[64] = {};
  int downloadBatchIndex = 0;
  void loadThemeSelection();
  void saveThemeSelection();
  int selectedThemeCount() const;
  // Bit i for each selected theme i of PUZZLE_THEMES; 0 when all themes are selected.
  uint32_t selectionMask() const;
  // Unplayed puzzles that fit the selected themes.
  int readyCount() const;
  // The theme a download batch asks for: the selected themes in turn, else mix.
  const char* downloadThemeFor(int batch) const;
  // Takes a puzzle from the selected lanes, spread by how many each holds.
  bool takeSelected(lichess::Puzzle& out);
  void buildThemeSummary();
  freeink::ui::ListNav themeNav;
  lichess::PuzzleDashboard dashboard;
  bool dashboardLoading = false;
  freeink::ui::ListItem statRows[MAX_STATS];
  char statLabels[MAX_STATS][36];
  char statSubtitles[MAX_STATS][28];
  char statValues[MAX_STATS][8];
  char statsLine[72] = {};
  freeink::ui::ListNav statNav;
  void buildThemes(UiScreen& screen);
  void buildStats(UiScreen& screen);
  void rebuildStatRows();
  // A download of several batches: how many puzzles were asked for and how
  // many have arrived. Zero target means no download runs.
  static constexpr int DOWNLOAD_CHOICES[5] = {30, 60, 120, 240, 500};
  int downloadTarget = 0;
  int downloadDone = 0;
  char countLabels[5][20];
  char progressLine[40];
  void askDownloadCount();
  int nextBatch() const;
  void openPuzzle(const lichess::Puzzle& puzzle);
  // Sends pending results and asks for `need` more puzzles in one request.
  void syncPuzzles(bool openAfter, int need);
  void requestPuzzle();
  void rebuildRecentRows();
  void openReview(int index);
  // The analysed game whose evaluations are loading before its review opens.
  int reviewIndex = -1;
  void startReview(int index, std::vector<lichess::AnalysisPly> analysis);
  void buildTabBar(UiScreen& screen);
  void buildGames(UiScreen& screen);
  void buildPuzzles(UiScreen& screen);
  // One popup serves the level picker and the download count picker.
  enum class PopupKind : uint8_t { Level, Count, Account };
  OptionPopup levelPopup;
  PopupKind popupKind = PopupKind::Level;
  TimeControl pendingAiControl{5, 0};
  int levelChoice = -1;
  char levelLabels[8][28];
  void askLevel(TimeControl control);
  char challengeUser[lichess::NAME_LEN] = {};
  char customValues[3][28];

  void startWifi();
  void onWifiResult(bool connected);
  void startClient();
  void handleClientEvents();
  void rebuildOngoingRows();
  void rebuildCardLabels();
  void openGame(const char* gameId, int colorHint);
  void startSeek(TimeControl control);
  void startAiGame(TimeControl control);
  void sendChallenge();
  void cancelSeek();
  void enterToken();
  void enterChallengeUsername();
  void confirmDialog();
  void showError(StrId text, int code, const char* detail = nullptr);

  static void screenTrampoline(UiScreen& screen, void* user);
  void buildScreen(UiScreen& screen);
  void buildPlay(UiScreen& screen);
  void buildSubTabs(UiScreen& screen);
  void buildMatch(UiScreen& screen);
  void buildComputer(UiScreen& screen);
  void buildChallenge(UiScreen& screen);
  void buildLocal(UiScreen& screen);
  void buildCards(UiScreen& screen);
  void buildCustomDialog(UiScreen& screen);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void handleAction(const freeink::ui::ActionEvent& event);
};
