#pragma once
#include <LichessProtocol.h>
#include <Position.h>

#include <string>

#include "ChessBoardView.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"

// The game screen. Without a game id it plays a local two-player game on one
// device. With a game id it streams a Lichess game and sends moves.
class ChessGameActivity final : public Activity {
 public:
  // colorHint: 0 white, 1 black, -1 unknown. It orients the board before the
  // first game message arrives.
  ChessGameActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* lichessGameId = nullptr,
                    int colorHint = -1);
  // Review of a finished game: the players from the export, moves in UCI.
  ChessGameActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const lichess::GameSummary& summary,
                    std::string movesUci, std::vector<lichess::AnalysisPly> analysisIn = {});
  // Puzzle: the solver plays the side to move in the puzzle position.
  ChessGameActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const lichess::Puzzle& puzzleData);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  // Only a running real-time game keeps the device awake; correspondence
  // games can sit open for days.
  bool preventAutoSleep() override { return online && haveGame && !gameOver && game.initialMs > 0; }

 private:
  static constexpr int CLOCK_ROW_HEIGHT = 44;
  static constexpr uint32_t CLOCK_REDRAW_FAST_MS = 1000;
  static constexpr uint32_t CLOCK_FAST_BELOW_MS = 20000;
  static constexpr int BAR_HEIGHT = 44;
  static constexpr int BAR_BUTTONS = 3;

  enum class MenuItem : uint8_t {
    Resign,
    Draw,
    Abort,
    ClaimWin,
    Flip,
    ClockRate,
    FullRefresh,
    Pulse,
    RefreshScreen,
    Prev,
    Next,
    Live,
    NextPuzzle,
    ShowSolution,
    NextMistake,
    PrevMistake,
    RequestAnalysis,
    NewGame,
    Rematch,
    Back,
    Count
  };

  bool online = false;
  bool review = false;
  bool puzzle = false;
  char gameId[lichess::GAME_ID_LEN] = {};

  chess::Position position;
  ChessBoardView board;
  ChessBoardView::Marks marks;
  chess::Move legalMoves[chess::MAX_MOVES];
  int legalCount = 0;
  bool gameOver = false;
  bool inCheck = false;

  // Online state, copied from the client on each GameUpdated event.
  lichess::GameSnapshot game;
  bool haveGame = false;
  int knownMoveCount = 0;
  bool moveInFlight = false;
  bool reconnecting = false;
  int reconnectAttempt = 0;
  bool connectionLost = false;
  bool ratingKnown = false;
  int ratingDiff = 0;
  uint32_t lastClockRedraw = 0;
  // A pending redraw that only advances the clocks: fast refresh, and it does
  // not count toward the ghost-clearing full refresh.
  bool clockTickPending = false;
  char clockRateLabel[32] = {};
  char fullRefreshLabel[48] = {};
  char pulseLabel[40] = {};

  // Frontlight pulse on an opponent move.
  uint32_t pulseUntil = 0;
  bool pulseChangedOn = false;
  uint8_t pulseSavedBrightness = 0;

  // Popup callbacks run inside handleInput() without the render lock, so
  // they only record a choice; loop() applies it under the lock.
  OptionPopup promotionPopup;
  chess::Move pendingPromotion;
  int promotionChoice = -1;
  OptionPopup menuPopup;
  MenuItem menuItems[static_cast<int>(MenuItem::Count)];
  int menuCount = 0;
  int menuChoice = -1;
  OptionPopup confirmPopup;
  MenuItem confirmAction = MenuItem::Back;
  int confirmChoice = -1;

  bool forceFullRefresh = true;

  // Move history. viewPly is -1 for the live position, else the number of
  // plies shown; viewPosition holds that position.
  int viewPly = -1;
  chess::Position viewPosition;
  chess::Move viewLastMove;
  std::string localMoves;  // UCI list for local games
  // Server analysis of a reviewed game, one entry per ply, or empty.
  std::vector<lichess::AnalysisPly> analysis;
  const lichess::AnalysisPly* analysisAt(int ply) const;
  int shownPly() const;
  void jumpToJudged(int direction);
  void drawAnalysis(int y);
  // The evaluation chart under the status lines: a tap jumps to that ply.
  static constexpr int ANALYSIS_SQUARE = 52;
  int chartX = 0;
  int chartY = 0;
  int chartW = 0;
  int chartH = 0;
  void drawEvalChart();
  bool tapChart(int tx, int ty);
  // The request-analysis code is up over the board until a tap. Its button
  // asks Lichess for the analysis without leaving the game.
  bool showQr = false;
  bool analysisPending = false;
  char analysisNote[80] = {};
  int qrButtonY = 0;
  int qrButtonH = 0;
  void checkForAnalysis();
  // A move chosen during the opponent's turn, sent as soon as it is legal.
  chess::Move premove;

  // Puzzle state: the solution alternates solver and opponent moves.
  lichess::Puzzle puzzleData;
  chess::Move puzzleLastMove;  // the opponent's move that led to the puzzle position
  int solveIndex = 0;
  int solutionCount = 0;
  bool puzzleSolved = false;
  bool puzzleWrong = false;
  bool puzzleFailed = false;  // a wrong move or the solution was used
  void finishPuzzle(bool next);
  chess::Color solverColor = chess::Color::White;
  chess::Position puzzleBase;  // the position to solve, built once in onEnter
  uint32_t replyAt = 0;        // when the opponent's scripted reply plays
  uint32_t wrongUntil = 0;     // the refused-move cross stays until then
  bool puzzleAccepts(const chess::Move& move) const;
  void playSolutionMove();

  bool browsing() const { return viewPly >= 0; }
  const std::string& moveList() const { return online ? game.moves : localMoves; }
  void showPly(int ply);
  // Start position of the move list: the puzzle position, else the initial position.
  chess::Position basePosition() const;
  void stepView(int delta);
  void goLive();
  static int material(const chess::Position& p, chess::Color c);
  void selectPremoveSquare(chess::Square square);
  void tryPremove();

  // Touch is polled by the main loop, so the loop must never wait for the
  // render lock: a refresh holds it for half a second and taps made in that
  // window would be missed. Input is captured every pass and applied when
  // the lock is free.
  struct PendingInput {
    bool tap = false;
    int tx = 0;
    int ty = 0;
    bool back = false;
    bool menu = false;
    bool prev = false;
    bool next = false;
  };
  static constexpr int PENDING_MAX = 6;
  PendingInput pending[PENDING_MAX];
  int pendingCount = 0;
  void captureInput();
  void applyPending();

  void newLocalGame();
  void layoutBoard();
  void refreshLegalMoves();
  void onSquareTapped(chess::Square square);
  void selectSquare(chess::Square square);
  void clearSelection();
  void playMove(const chess::Move& move);
  void askPromotion(chess::Square from, chess::Square to);
  void openMenu();
  void runMenuItem(MenuItem item);
  void handleClientEvents();
  void applyPopupChoices();
  void applySnapshot();
  bool myTurn() const;
  chess::Color myColor() const;
  uint32_t remainingMs(chess::Color side) const;
  void startPulse();
  void endPulseIfDue();
  void updateClockRedraw();
  const char* statusText(char* buf, size_t size) const;
  void drawClockRow(int y, chess::Color side) const;
  void drawPuzzleRow(int y) const;
  void drawStatusLine();
  // The action bar under the status line: three buttons with fixed rects.
  struct BarButton {
    MenuItem item;
    StrId label;
  };
  int barButtons(BarButton* out) const;
  int barY() const;
  void drawActionBar();
  bool tapActionBar(int tx, int ty);
};
