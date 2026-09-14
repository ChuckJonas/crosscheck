#include "ChessGameActivity.h"

#include <ChessSettings.h>
#include <GfxRenderer.h>
#include <HalFrontlight.h>
#include <I18n.h>
#include <LichessClient.h>
#include <Logging.h>
#include <Pgn.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>

#include "components/UITheme.h"
#include "fontIds.h"
#include "util/QrUtils.h"

using chess::Move;
using chess::NoPiece;
using chess::NoSquare;
using chess::Piece;
using chess::PieceType;
using chess::Square;

ChessGameActivity::ChessGameActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* lichessGameId,
                                     int colorHint)
    : Activity("ChessGame", renderer, mappedInput) {
  if (lichessGameId && lichessGameId[0]) {
    online = true;
    lichess::copyStr(gameId, sizeof(gameId), lichessGameId);
    if (colorHint >= 0) board.setFlipped(colorHint == 1);
  }
}

ChessGameActivity::ChessGameActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const lichess::GameSummary& summary, std::string movesUci,
                                     std::vector<lichess::AnalysisPly> analysisIn)
    : Activity("ChessGame", renderer, mappedInput), review(true) {
  lichess::copyStr(gameId, sizeof(gameId), summary.id);
  lichess::copyStr(game.id, sizeof(game.id), summary.id);
  game.white = summary.white;
  game.black = summary.black;
  game.myColor = summary.myColor;
  game.rated = summary.rated;
  lichess::copyStr(game.speed, sizeof(game.speed), summary.speed);
  game.initialMs = summary.initialMs;
  game.incrementMs = summary.incrementMs;
  game.status = summary.status;
  game.hasWinner = summary.hasWinner;
  game.winner = summary.winner;
  localMoves = std::move(movesUci);
  analysis = std::move(analysisIn);
  haveGame = true;
  gameOver = true;
}

ChessGameActivity::ChessGameActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const lichess::Puzzle& puzzleData)
    : Activity("ChessGame", renderer, mappedInput), puzzle(true), puzzleData(puzzleData) {
  solutionCount = lichess::countMoves(puzzleData.solution);
}

void ChessGameActivity::onEnter() {
  Activity::onEnter();
  layoutBoard();
  if (review) {
    Move last;
    lichess::replayMoves(localMoves, position, &last);
    marks.lastFrom = last.from;
    marks.lastTo = last.to;
    board.setFlipped(game.myColor == chess::Color::Black);
    refreshLegalMoves();
    forceFullRefresh = true;
    requestUpdate();
    return;
  }
  if (puzzle) {
    // The puzzle position is the position after the game's moves; the last
    // of them is the opponent's move that the highlight shows.
    std::string uci;
    puzzleBase.setStartPosition();
    if (chess::replaySan(puzzleData.pgn.c_str(), puzzleBase, &uci) < 0) {
      LOG_ERR("CHESS", "Puzzle %s: could not replay the game moves", puzzleData.id);
    }
    const size_t space = uci.rfind(' ');
    Move last;
    if (chess::moveFromUci(uci.c_str() + (space == std::string::npos ? 0 : space + 1), last)) puzzleLastMove = last;
    position = puzzleBase;
    solverColor = position.sideToMove();
    board.setFlipped(solverColor == chess::Color::Black);
    marks.lastFrom = puzzleLastMove.from;
    marks.lastTo = puzzleLastMove.to;
    refreshLegalMoves();
    forceFullRefresh = true;
    requestUpdate();
    return;
  }
  if (online) {
    // Modem sleep stalls the stream and shows up as HTTP timeouts.
    WiFi.setSleep(false);
    position.setStartPosition();
    refreshLegalMoves();
    LICHESS.streamGame(gameId);
    LOG_INF("CHESS", "Joining game %s, heap %u", gameId, ESP.getFreeHeap());
    forceFullRefresh = true;
    requestUpdate();
  } else {
    newLocalGame();
  }
}

void ChessGameActivity::onExit() {
  // The next screen draws into the framebuffer at once; finish our refresh first.
  renderer.waitRefreshComplete();
  if (online) {
    LICHESS.setKeepAlive(false);
    LICHESS.stopStream();
    WiFi.setSleep(true);
  }
  if (pulseUntil) {
    pulseUntil = 0;
    if (pulseChangedOn) {
      Frontlight.setOn(false);
    } else {
      Frontlight.setBrightness(pulseSavedBrightness);
    }
  }
  Activity::onExit();
}

void ChessGameActivity::newLocalGame() {
  position.setStartPosition();
  marks = ChessBoardView::Marks{};
  localMoves.clear();
  viewPly = -1;
  premove = Move{};
  gameOver = false;
  inCheck = false;
  refreshLegalMoves();
  forceFullRefresh = true;
  requestUpdate();
}

void ChessGameActivity::layoutBoard() {
  auto& theme = UITheme::getInstance();
  const auto& metrics = theme.getMetrics();
  const Rect safe = theme.getScreenSafeArea(renderer, false, false);
  const int top = safe.y + metrics.topPadding + metrics.headerHeight + CLOCK_ROW_HEIGHT;
  const int availableHeight = safe.y + safe.height - top - CLOCK_ROW_HEIGHT - BAR_HEIGHT;
  const int side = safe.width < availableHeight ? safe.width : availableHeight;
  int squareSize = side / 8;
  // With analysis the chart needs the room under the board.
  if (review && !analysis.empty() && squareSize > ANALYSIS_SQUARE) squareSize = ANALYSIS_SQUARE;
  const int x = safe.x + (safe.width - squareSize * 8) / 2;
  board.setLayout(x, top, squareSize);
}

void ChessGameActivity::refreshLegalMoves() {
  legalCount = position.generateLegalMoves(legalMoves, chess::MAX_MOVES);
  inCheck = position.inCheck();
  marks.checkedKing = inCheck ? position.kingSquare(position.sideToMove()) : NoSquare;
  if (!online && !review && !puzzle && legalCount == 0) gameOver = true;
}

chess::Color ChessGameActivity::myColor() const { return online ? game.myColor : position.sideToMove(); }

bool ChessGameActivity::myTurn() const {
  if (review) return false;
  if (puzzle) return !puzzleSolved && replyAt == 0 && position.sideToMove() == solverColor;
  if (!online) return true;
  return haveGame && !gameOver && !moveInFlight && position.sideToMove() == game.myColor;
}

void ChessGameActivity::clearSelection() {
  marks.selected = NoSquare;
  for (bool& t : marks.target) t = false;
}

void ChessGameActivity::selectSquare(Square square) {
  clearSelection();
  marks.selected = square;
  for (int i = 0; i < legalCount; ++i) {
    if (legalMoves[i].from == square) marks.target[legalMoves[i].to] = true;
  }
}

bool ChessGameActivity::puzzleAccepts(const Move& move) const {
  // The scripted move, or any move that mates, counts as correct.
  char uci[6];
  chess::moveToUci(move, uci);
  char expected[8] = {};
  int index = 0;
  size_t i = 0;
  while (i < puzzleData.solution.size()) {
    while (i < puzzleData.solution.size() && puzzleData.solution[i] == ' ') ++i;
    size_t n = 0;
    while (i < puzzleData.solution.size() && puzzleData.solution[i] != ' ') {
      if (n < sizeof(expected) - 1) expected[n++] = puzzleData.solution[i];
      ++i;
    }
    expected[n] = '\0';
    if (index == solveIndex) break;
    ++index;
  }
  if (strcmp(uci, expected) == 0) return true;
  chess::Position after = position;
  after.makeMove(move);
  return after.status() == chess::GameStatus::Checkmate;
}

void ChessGameActivity::finishPuzzle(bool next) {
  // "puzzle:<w|l|n>:<next|back>": a win is a clean solve, a loss any wrong
  // move or a shown solution, n an abandoned puzzle.
  const char outcome = puzzleSolved ? (puzzleFailed ? 'l' : 'w') : (puzzleFailed ? 'l' : 'n');
  char text[24];
  snprintf(text, sizeof(text), "puzzle:%c:%s", outcome, next ? "next" : "back");
  setResult(KeyboardResult{text});
  finish();
}

void ChessGameActivity::playSolutionMove() {
  // Applies the next scripted move, for the opponent's reply or a shown solution.
  char expected[8] = {};
  int index = 0;
  size_t i = 0;
  while (i < puzzleData.solution.size()) {
    while (i < puzzleData.solution.size() && puzzleData.solution[i] == ' ') ++i;
    size_t n = 0;
    while (i < puzzleData.solution.size() && puzzleData.solution[i] != ' ') {
      if (n < sizeof(expected) - 1) expected[n++] = puzzleData.solution[i];
      ++i;
    }
    expected[n] = '\0';
    if (index == solveIndex) break;
    ++index;
  }
  Move m;
  if (!chess::moveFromUci(expected, m) || !position.isLegal(m)) {
    puzzleSolved = true;  // a broken script ends the puzzle rather than blocking it
    requestUpdate();
    return;
  }
  position.makeMove(m);
  if (!localMoves.empty()) localMoves.push_back(' ');
  localMoves.append(expected);
  clearSelection();
  marks.lastFrom = m.from;
  marks.lastTo = m.to;
  refreshLegalMoves();
  ++solveIndex;
  if (solveIndex >= solutionCount) {
    puzzleSolved = true;
    forceFullRefresh = true;
  } else if (position.sideToMove() != solverColor) {
    replyAt = millis() + 600;
  }
  requestUpdate();
}

void ChessGameActivity::playMove(const Move& move) {
  char uci[6];
  chess::moveToUci(move, uci);
  if (puzzle) {
    if (!puzzleAccepts(move)) {
      puzzleWrong = true;
      puzzleFailed = true;
      clearSelection();
      marks.wrongSquare = move.to;
      wrongUntil = millis() + 1200;
      requestUpdate();
      return;
    }
    puzzleWrong = false;
    marks.wrongSquare = NoSquare;
  }
  LOG_INF("CHESS", "Move %s", uci);
  position.makeMove(move);
  clearSelection();
  premove = Move{};
  marks.premoveFrom = NoSquare;
  marks.premoveTo = NoSquare;
  marks.lastFrom = move.from;
  marks.lastTo = move.to;
  refreshLegalMoves();
  if (!online) {
    if (!localMoves.empty()) localMoves.push_back(' ');
    localMoves.append(uci);
  }
  if (puzzle) {
    ++solveIndex;
    if (solveIndex >= solutionCount) {
      puzzleSolved = true;
      forceFullRefresh = true;
    } else {
      replyAt = millis() + 600;
    }
    return;
  }
  if (online) {
    // Optimistic: the board shows the move now, the stream confirms it.
    moveInFlight = true;
    ++knownMoveCount;
    if (!LICHESS.sendMove(gameId, uci)) {
      LOG_ERR("CHESS", "Could not queue move; resyncing");
      moveInFlight = false;
      applySnapshot();
    }
  } else if (gameOver) {
    forceFullRefresh = true;
  }
}

void ChessGameActivity::askPromotion(Square from, Square to) {
  pendingPromotion = Move{from, to, PieceType::None};
  static constexpr StrId OPTIONS[] = {StrId::STR_CHESS_QUEEN, StrId::STR_CHESS_ROOK, StrId::STR_CHESS_BISHOP,
                                      StrId::STR_CHESS_KNIGHT};
  static constexpr PieceType PIECES[] = {PieceType::Queen, PieceType::Rook, PieceType::Bishop, PieceType::Knight};
  promotionChoice = -1;
  promotionPopup.show(StrId::STR_CHESS_PROMOTE, OPTIONS, 4, 0, [this](const int idx) { promotionChoice = idx; });
}

void ChessGameActivity::applyPopupChoices() {
  static constexpr PieceType PIECES[] = {PieceType::Queen, PieceType::Rook, PieceType::Bishop, PieceType::Knight};
  if (promotionChoice >= 0) {
    Move m = pendingPromotion;
    m.promotion = PIECES[promotionChoice];
    promotionChoice = -1;
    pendingPromotion = Move{};
    // The position may have changed while the popup was open (a resync or
    // the game ending), so the move is checked again.
    if (position.isLegal(m)) {
      playMove(m);
    } else {
      clearSelection();
    }
    requestUpdate();
  }
  if (menuChoice >= 0) {
    const int idx = menuChoice;
    menuChoice = -1;
    if (idx < menuCount) runMenuItem(menuItems[idx]);
    requestUpdate();
  }
  if (confirmChoice >= 0) {
    const int idx = confirmChoice;
    confirmChoice = -1;
    if (idx == 1) {
      if (confirmAction == MenuItem::Resign) LICHESS.resign(gameId);
      if (confirmAction == MenuItem::Abort) LICHESS.abortGame(gameId);
    }
    requestUpdate();
  }
}

void ChessGameActivity::onSquareTapped(Square square) {
  if (!online && !review && !puzzle && gameOver) {
    newLocalGame();
    return;
  }
  if (browsing()) return;
  if (!myTurn()) {
    if (online && haveGame && !gameOver && position.sideToMove() != game.myColor) selectPremoveSquare(square);
    return;
  }

  const Piece piece = position.pieceAt(square);
  const bool ownPiece = piece != NoPiece && chess::colorOf(piece) == position.sideToMove();

  if (marks.selected == NoSquare) {
    if (ownPiece) selectSquare(square);
  } else if (square == marks.selected) {
    clearSelection();
  } else if (marks.target[square]) {
    Move chosen;
    for (int i = 0; i < legalCount; ++i) {
      const Move& m = legalMoves[i];
      if (m.from == marks.selected && m.to == square) {
        chosen = m;
        break;
      }
    }
    if (chosen.promotion != PieceType::None) {
      askPromotion(chosen.from, chosen.to);
    } else {
      playMove(chosen);
    }
  } else if (ownPiece) {
    selectSquare(square);
  } else {
    clearSelection();
  }
  requestUpdate();
}

// --- premove -----------------------------------------------------------------

void ChessGameActivity::selectPremoveSquare(Square square) {
  const Piece piece = position.pieceAt(square);
  const bool ownPiece = piece != NoPiece && chess::colorOf(piece) == game.myColor;
  if (marks.selected == NoSquare) {
    // Any tap cancels a waiting premove; a tap on our own piece also selects it.
    premove = Move{};
    marks.premoveFrom = NoSquare;
    marks.premoveTo = NoSquare;
    if (ownPiece) {
      // Targets as if it were our move already; the real check happens when
      // the opponent has moved.
      chess::Position mine = position;
      mine.setSideToMove(game.myColor);
      Move moves[chess::MAX_MOVES];
      const int count = mine.generateLegalMoves(moves, chess::MAX_MOVES);
      clearSelection();
      marks.selected = square;
      for (int i = 0; i < count; ++i) {
        if (moves[i].from == square) marks.target[moves[i].to] = true;
      }
    }
  } else if (square == marks.selected) {
    clearSelection();
  } else if (marks.target[square]) {
    premove = Move{marks.selected, square, PieceType::None};
    marks.premoveFrom = marks.selected;
    marks.premoveTo = square;
    clearSelection();
  } else if (ownPiece) {
    clearSelection();
    selectPremoveSquare(square);
    return;
  } else {
    clearSelection();
  }
  requestUpdate();
}

void ChessGameActivity::tryPremove() {
  if (premove.isNull()) return;
  const Move wanted = premove;
  premove = Move{};
  marks.premoveFrom = NoSquare;
  marks.premoveTo = NoSquare;
  for (int i = 0; i < legalCount; ++i) {
    const Move& m = legalMoves[i];
    if (m.from != wanted.from || m.to != wanted.to) continue;
    if (m.promotion != PieceType::None && m.promotion != PieceType::Queen) continue;
    playMove(m);
    return;
  }
}

// --- history -----------------------------------------------------------------

chess::Position ChessGameActivity::basePosition() const {
  if (puzzle) return puzzleBase;
  chess::Position base;
  return base;
}

void ChessGameActivity::showPly(int ply) {
  const int total = lichess::countMoves(moveList());
  if (ply < 0) ply = 0;
  if (ply >= total) {
    goLive();
    return;
  }
  viewPly = ply;
  lichess::replayMovesFrom(basePosition(), moveList(), viewPosition, &viewLastMove, ply);
  if (puzzle && ply == 0) viewLastMove = puzzleLastMove;  // the opponent's move before the puzzle
  requestUpdate();
}

void ChessGameActivity::stepView(int delta) {
  const int total = lichess::countMoves(moveList());
  const int current = browsing() ? viewPly : total;
  showPly(current + delta);
}

void ChessGameActivity::goLive() {
  viewPly = -1;
  requestUpdate();
}

int ChessGameActivity::material(const chess::Position& p, chess::Color c) {
  static constexpr int VALUE[7] = {0, 1, 3, 3, 5, 9, 0};
  int sum = 0;
  for (Square s = 0; s < 64; ++s) {
    const Piece piece = p.pieceAt(s);
    if (piece != NoPiece && chess::colorOf(piece) == c) sum += VALUE[static_cast<int>(chess::typeOf(piece))];
  }
  return sum;
}

// --- menu --------------------------------------------------------------------

void ChessGameActivity::openMenu() {
  static const char* labels[static_cast<int>(MenuItem::Count)];
  menuCount = 0;
  auto add = [&](MenuItem item, StrId label) {
    menuItems[menuCount] = item;
    labels[menuCount] = I18N.get(label);
    ++menuCount;
  };
  if (online) {
    if (!gameOver && haveGame) {
      const int moveCount = knownMoveCount;
      if (moveCount < 2) add(MenuItem::Abort, StrId::STR_CHESS_ABORT);
      add(MenuItem::Resign, StrId::STR_CHESS_RESIGN);
      const bool opponentOffers = game.myColor == chess::Color::White ? game.blackOffersDraw : game.whiteOffersDraw;
      add(MenuItem::Draw, opponentOffers ? StrId::STR_CHESS_ACCEPT_DRAW : StrId::STR_CHESS_OFFER_DRAW);
      if (game.opponentGone && game.claimWinInSeconds == 0) add(MenuItem::ClaimWin, StrId::STR_CHESS_CLAIM_WIN);
    }
    if (gameOver) add(MenuItem::Rematch, StrId::STR_CHESS_REMATCH);
  } else if (!review && !puzzle) {
    add(MenuItem::NewGame, StrId::STR_CHESS_NEW_GAME);
  }
  if (review) {
    if (analysis.empty()) {
      add(MenuItem::RequestAnalysis, StrId::STR_CHESS_REQUEST_ANALYSIS);
    } else {
      add(MenuItem::NextMistake, StrId::STR_CHESS_NEXT_MISTAKE);
      add(MenuItem::PrevMistake, StrId::STR_CHESS_PREV_MISTAKE);
    }
  }
  add(MenuItem::Flip, StrId::STR_CHESS_FLIP_BOARD);
  if (online) {
    snprintf(clockRateLabel, sizeof(clockRateLabel), "%s: %d s", tr(STR_CHESS_CLOCK_REFRESH),
             CHESS_SETTINGS.getClockRefreshSec());
    menuItems[menuCount] = MenuItem::ClockRate;
    labels[menuCount] = clockRateLabel;
    ++menuCount;
    snprintf(fullRefreshLabel, sizeof(fullRefreshLabel), "%s: %s", tr(STR_CHESS_FULL_REFRESH_MOVE),
             CHESS_SETTINGS.getFullRefreshEveryMove() ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
    menuItems[menuCount] = MenuItem::FullRefresh;
    labels[menuCount] = fullRefreshLabel;
    ++menuCount;
    snprintf(pulseLabel, sizeof(pulseLabel), "%s: %s", tr(STR_CHESS_PULSE),
             CHESS_SETTINGS.getPulseMs() > 0 ? tr(STR_STATE_ON) : tr(STR_STATE_OFF));
    menuItems[menuCount] = MenuItem::Pulse;
    labels[menuCount] = pulseLabel;
    ++menuCount;
  }
  add(MenuItem::RefreshScreen, StrId::STR_CHESS_REFRESH_SCREEN);
  add(MenuItem::Back, StrId::STR_CHESS_BACK_TO_LOBBY);
  menuChoice = -1;
  menuPopup.show(tr(STR_CHESS), labels, menuCount, 0, [this](const int idx) { menuChoice = idx; });
}

void ChessGameActivity::runMenuItem(MenuItem item) {
  switch (item) {
    case MenuItem::Resign:
    case MenuItem::Abort: {
      confirmAction = item;
      static const char* yesNo[2];
      yesNo[0] = tr(STR_NO);
      yesNo[1] = tr(STR_YES);
      confirmChoice = -1;
      confirmPopup.show(item == MenuItem::Resign ? tr(STR_CHESS_RESIGN_CONFIRM) : tr(STR_CHESS_ABORT_CONFIRM), yesNo, 2,
                        0, [this](const int idx) { confirmChoice = idx; });
      break;
    }
    case MenuItem::Draw:
      LICHESS.offerDraw(gameId, true);
      break;
    case MenuItem::ClaimWin:
      LICHESS.claimVictory(gameId);
      break;
    case MenuItem::Flip:
      board.setFlipped(!board.isFlipped());
      forceFullRefresh = true;
      break;
    case MenuItem::FullRefresh:
      CHESS_SETTINGS.setFullRefreshEveryMove(!CHESS_SETTINGS.getFullRefreshEveryMove());
      CHESS_SETTINGS.saveToFile();
      openMenu();  // show the new value
      break;
    case MenuItem::Pulse:
      CHESS_SETTINGS.setPulseMs(CHESS_SETTINGS.getPulseMs() > 0 ? 0 : 300);
      CHESS_SETTINGS.saveToFile();
      openMenu();
      break;
    case MenuItem::RefreshScreen:
      forceFullRefresh = true;
      break;
    case MenuItem::Prev:
      stepView(-1);
      break;
    case MenuItem::Next:
      stepView(1);
      break;
    case MenuItem::Live:
      goLive();
      break;
    case MenuItem::NextMistake:
      jumpToJudged(1);
      break;
    case MenuItem::PrevMistake:
      jumpToJudged(-1);
      break;
    case MenuItem::RequestAnalysis:
      showQr = true;
      forceFullRefresh = true;
      requestUpdate();
      break;
    case MenuItem::NextPuzzle:
      finishPuzzle(true);
      break;
    case MenuItem::ShowSolution:
      if (puzzle && !puzzleSolved && replyAt == 0) {
        puzzleWrong = false;
        puzzleFailed = true;
        playSolutionMove();
      }
      break;
    case MenuItem::ClockRate: {
      // Cycle 1 -> 2 -> 5 -> 10 -> 1 seconds.
      const int current = CHESS_SETTINGS.getClockRefreshSec();
      CHESS_SETTINGS.setClockRefreshSec(current == 1 ? 2 : current == 2 ? 5 : current == 5 ? 10 : 1);
      CHESS_SETTINGS.saveToFile();
      openMenu();
      break;
    }
    case MenuItem::NewGame:
      newLocalGame();
      break;
    case MenuItem::Rematch: {
      const lichess::Player& opp = game.myColor == chess::Color::White ? game.black : game.white;
      const int minutes = static_cast<int>(game.initialMs / 60000);
      const int increment = static_cast<int>(game.incrementMs / 1000);
      if (opp.aiLevel > 0) {
        LICHESS.challengeAi(opp.aiLevel, minutes, increment);
      } else {
        LICHESS.challenge(opp.name, minutes, increment, game.rated);
      }
      // The lobby waits for the new game to start.
      setResult(KeyboardResult{opp.aiLevel > 0 ? "rematch-ai" : "rematch"});
      finish();
      break;
    }
    case MenuItem::Back:
      if (puzzle) {
        finishPuzzle(false);
      } else {
        finish();
      }
      break;
    default:
      break;
  }
}

// --- online events -----------------------------------------------------------

void ChessGameActivity::applySnapshot() {
  LICHESS.copyGame(game);
  if (game.id[0] == '\0') return;
  // Orient the board once; a later Flip must survive the next stream line.
  if (!haveGame) board.setFlipped(game.myColor == chess::Color::Black);
  haveGame = true;

  // A stale snapshot (an opponentGone line, say) must not undo the move that
  // is still on its way to the server.
  int pending = 0;
  for (size_t i = 0; i < game.moves.size(); ++i) {
    if (game.moves[i] == ' ') ++pending;
  }
  if (!game.moves.empty()) ++pending;
  if (moveInFlight && pending < knownMoveCount) {
    gameOver = lichess::isFinished(game.status);
    if (gameOver) moveInFlight = false;
    requestUpdate();
    return;
  }

  Move last;
  const int count = lichess::replayMoves(game.moves, position, &last);
  if (count < 0) {
    LOG_ERR("CHESS", "Could not replay moves: %s", game.moves.c_str());
    return;
  }
  const bool opponentMoved = count > knownMoveCount && position.sideToMove() == game.myColor;
  if (count >= knownMoveCount) moveInFlight = false;
  knownMoveCount = count;
  // Keep the piece the user has selected; its targets are recomputed below.
  const Square keepSelected = marks.selected;
  clearSelection();
  marks.lastFrom = last.from;
  marks.lastTo = last.to;
  refreshLegalMoves();
  const bool wasOver = gameOver;
  gameOver = lichess::isFinished(game.status);
  reconnecting = false;
  connectionLost = false;
  // Correspondence games have no live clock, so modem sleep can stay on.
  if (game.initialMs == 0) WiFi.setSleep(true);
  // Keep the move connection warm only while a live clock runs.
  LICHESS.setKeepAlive(!gameOver && game.initialMs > 0);
  if (gameOver && !wasOver && game.rated) LICHESS.fetchRating(gameId, game.myColor);
  if (opponentMoved && !gameOver) startPulse();
  if ((gameOver && !wasOver) || (opponentMoved && CHESS_SETTINGS.getFullRefreshEveryMove())) forceFullRefresh = true;
  lastClockRedraw = millis();
  if (gameOver) {
    premove = Move{};
    marks.premoveFrom = NoSquare;
    marks.premoveTo = NoSquare;
  } else if (myTurn()) {
    tryPremove();
  }
  if (!gameOver && keepSelected != NoSquare && marks.selected == NoSquare) {
    const Piece kept = position.pieceAt(keepSelected);
    if (kept != NoPiece && chess::colorOf(kept) == game.myColor) {
      if (myTurn()) {
        selectSquare(keepSelected);
      } else if (premove.isNull()) {
        selectPremoveSquare(keepSelected);
      }
    }
  }
  requestUpdate();
}

void ChessGameActivity::checkForAnalysis() {
  if (analysisPending) return;
  if (!LICHESS.running() || !LICHESS.fetchAnalysis(gameId)) {
    snprintf(analysisNote, sizeof(analysisNote), "%s", tr(STR_CHESS_OFFLINE));
    requestUpdate();
    return;
  }
  analysisPending = true;
  snprintf(analysisNote, sizeof(analysisNote), "%s", tr(STR_CHESS_CHECKING_ANALYSIS));
  requestUpdate();
}

void ChessGameActivity::handleClientEvents() {
  LichessClient::Event ev;
  while (LICHESS.pollEvent(ev)) {
    if (review) {
      // Only the analysis reply matters here; the lobby owns everything else.
      if (ev.type == LichessClient::EventType::AnalysisReady && strcmp(ev.gameId, gameId) == 0) {
        LICHESS.copyAnalysis(analysis);
        analysisPending = false;
        if (!analysis.empty()) {
          setResult(KeyboardResult{"analysed"});  // the lobby tags the row
          showQr = false;
          layoutBoard();
          forceFullRefresh = true;
        }
        requestUpdate();
      } else if (ev.type == LichessClient::EventType::AnalysisFailed) {
        analysisPending = false;
        snprintf(analysisNote, sizeof(analysisNote), "%s", tr(STR_CHESS_NO_ANALYSIS_YET));
        requestUpdate();
      }
      continue;
    }
    switch (ev.type) {
      case LichessClient::EventType::GameUpdated:
        applySnapshot();
        break;
      case LichessClient::EventType::MoveFailed:
        LOG_ERR("CHESS", "Server rejected move (%d); resyncing", ev.code);
        moveInFlight = false;
        applySnapshot();
        break;
      case LichessClient::EventType::StreamReconnecting:
        reconnecting = true;
        reconnectAttempt = ev.code;
        requestUpdate();
        break;
      case LichessClient::EventType::StreamFailed:
        if (ev.gameId[0] == '\0') break;  // the event stream, not this game
        reconnecting = false;
        connectionLost = true;
        requestUpdate();
        break;
      case LichessClient::EventType::RatingReady:
        if (strcmp(ev.gameId, gameId) == 0) {
          ratingKnown = true;
          ratingDiff = ev.code;
          requestUpdate();
        }
        break;
      default:
        break;
    }
  }
}

uint32_t ChessGameActivity::remainingMs(chess::Color side) const {
  const uint32_t base = side == chess::Color::White ? game.wtimeMs : game.btimeMs;
  // The clock only runs once both sides have moved, and only for the side to move.
  if (gameOver || knownMoveCount < 2 || position.sideToMove() != side) return base;
  const uint32_t elapsed = millis() - LICHESS.lastGameEventMillis();
  return elapsed >= base ? 0 : base - elapsed;
}

void ChessGameActivity::updateClockRedraw() {
  if (!online || !haveGame || gameOver || game.initialMs == 0 || knownMoveCount < 2) return;
  const uint32_t mine = remainingMs(game.myColor);
  const uint32_t theirs = remainingMs(chess::opposite(game.myColor));
  const uint32_t interval = (mine < CLOCK_FAST_BELOW_MS || theirs < CLOCK_FAST_BELOW_MS)
                                ? CLOCK_REDRAW_FAST_MS
                                : static_cast<uint32_t>(CHESS_SETTINGS.getClockRefreshSec()) * 1000;
  if (millis() - lastClockRedraw >= interval) {
    lastClockRedraw = millis();
    clockTickPending = true;
    requestUpdate();
  }
}

void ChessGameActivity::startPulse() {
  const int pulseMs = CHESS_SETTINGS.getPulseMs();
  if (pulseMs <= 0 || !Frontlight.present() || pulseUntil) return;
  if (Frontlight.isOn()) {
    pulseSavedBrightness = Frontlight.brightness();
    pulseChangedOn = false;
    Frontlight.setBrightness(pulseSavedBrightness > 40 ? pulseSavedBrightness / 4 : 100);
  } else {
    pulseChangedOn = true;
    Frontlight.setOn(true);
  }
  pulseUntil = millis() + pulseMs;
}

void ChessGameActivity::endPulseIfDue() {
  if (!pulseUntil || millis() < pulseUntil) return;
  pulseUntil = 0;
  if (pulseChangedOn) {
    Frontlight.setOn(false);
  } else {
    Frontlight.setBrightness(pulseSavedBrightness);
  }
}

// --- loop --------------------------------------------------------------------

void ChessGameActivity::captureInput() {
  PendingInput in;
  in.tap = mappedInput.wasScreenTapped(in.tx, in.ty);
  in.back = mappedInput.wasReleased(MappedInputManager::Button::Back);
  in.menu = mappedInput.wasMenuGesture() || mappedInput.wasReleased(MappedInputManager::Button::Confirm);
  const auto swipe = mappedInput.wasSwipe();
  // A swipe turns pages: left goes forward through the moves, right goes back.
  in.prev = swipe == MappedInputManager::SwipeDir::Right || mappedInput.wasReleased(MappedInputManager::Button::Left) ||
            mappedInput.wasReleased(MappedInputManager::Button::PageBack);
  in.next = swipe == MappedInputManager::SwipeDir::Left || mappedInput.wasReleased(MappedInputManager::Button::Right) ||
            mappedInput.wasReleased(MappedInputManager::Button::PageForward);
  if (!(in.tap || in.back || in.menu || in.prev || in.next)) return;
  if (pendingCount < PENDING_MAX) pending[pendingCount++] = in;
}

void ChessGameActivity::applyPending() {
  const int count = pendingCount;
  pendingCount = 0;
  for (int i = 0; i < count; ++i) {
    const PendingInput& in = pending[i];
    if (in.back) {
      if (browsing()) {
        goLive();
        continue;
      }
      if (puzzle) {
        finishPuzzle(false);
      } else {
        finish();
      }
      return;
    }
    if (in.prev || in.next) {
      stepView(in.prev ? -1 : 1);
      continue;
    }
    if (in.menu) {
      openMenu();
      requestUpdate();
      return;  // the popup takes the input from here
    }
    if (showQr) {
      // The button asks for the analysis; any other input puts the code away.
      if (in.tap && in.ty >= qrButtonY && in.ty < qrButtonY + qrButtonH) {
        checkForAnalysis();
        continue;
      }
      showQr = false;
      forceFullRefresh = true;
      requestUpdate();
      continue;
    }
    if (in.tap) {
      // The board has 64 hit targets, more than the FreeInkUI host can
      // register, so it maps taps to squares itself like the reader page does.
      const Square square = board.squareAt(in.tx, in.ty);
      if (square != NoSquare) {
        onSquareTapped(square);
      } else if (review && tapChart(in.tx, in.ty)) {
        // jumped to the tapped ply
      } else if (!tapActionBar(in.tx, in.ty) && in.ty > board.y() + board.size() + CLOCK_ROW_HEIGHT) {
        openMenu();
        requestUpdate();
        return;
      }
    }
  }
}

void ChessGameActivity::loop() {
  // Modal popups read the input themselves.
  if (!pendingPromotion.isNull()) {
    if (promotionPopup.handleInput(mappedInput, [this] { requestUpdate(); })) {
      RenderLock lock;
      applyPopupChoices();
      return;
    }
    // Dismissed without a choice: keep the piece selected so the user can retry.
    RenderLock lock;
    pendingPromotion = Move{};
    requestUpdate();
    return;
  }
  if (confirmPopup.isActive()) {
    confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); });
    RenderLock lock;
    applyPopupChoices();
    return;
  }
  if (menuPopup.isActive()) {
    menuPopup.handleInput(mappedInput, [this] { requestUpdate(); });
    RenderLock lock;
    applyPopupChoices();
    return;
  }

  captureInput();
  if (RenderLock::peek()) return;  // a refresh is running; keep polling input
  RenderLock lock;
  if (online || (review && analysisPending)) handleClientEvents();
  if (puzzle && replyAt && millis() >= replyAt) {
    replyAt = 0;
    playSolutionMove();
  }
  if (wrongUntil && millis() >= wrongUntil) {
    wrongUntil = 0;
    marks.wrongSquare = NoSquare;
    requestUpdate();
  }
  endPulseIfDue();
  applyPopupChoices();
  applyPending();
  updateClockRedraw();
}

// --- render ------------------------------------------------------------------

const char* ChessGameActivity::statusText(char* buf, size_t size) const {
  if (browsing()) {
    snprintf(buf, size, tr(STR_CHESS_MOVE_OF), viewPly, lichess::countMoves(moveList()));
    return buf;
  }
  if (puzzle) {
    if (puzzleSolved) return tr(STR_CHESS_PUZZLE_SOLVED);
    if (replyAt) return "";
    return tr(STR_CHESS_PUZZLE_FIND);
  }
  if (online || review) {
    if (connectionLost) return tr(STR_CHESS_CONNECTION_LOST);
    if (reconnecting) {
      snprintf(buf, size, "%s (%d)", tr(STR_CHESS_RECONNECTING), reconnectAttempt);
      return buf;
    }
    if (!haveGame) return tr(STR_CHESS_CONNECTING);
    if (gameOver) {
      const char* how = nullptr;
      switch (game.status) {
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
          return tr(STR_CHESS_GAME_ABORTED);
        default:
          break;
      }
      char rating[16] = "";
      if (ratingKnown) snprintf(rating, sizeof(rating), " (%+d)", ratingDiff);
      if (!game.hasWinner) {
        snprintf(buf, size, "%s%s", tr(STR_CHESS_DRAW), rating);
        return buf;
      }
      const bool iWon = game.winner == game.myColor;
      snprintf(buf, size, "%s %s%s", iWon ? tr(STR_CHESS_YOU_WON) : tr(STR_CHESS_YOU_LOST), how ? how : "", rating);
      return buf;
    }
    if (game.opponentGone) return tr(STR_CHESS_OPPONENT_GONE);
    const bool opponentOffers = game.myColor == chess::Color::White ? game.blackOffersDraw : game.whiteOffersDraw;
    if (opponentOffers) return tr(STR_CHESS_DRAW_OFFERED);
    if (inCheck) return tr(STR_CHESS_CHECK);
    return position.sideToMove() == game.myColor ? tr(STR_CHESS_YOUR_MOVE) : tr(STR_CHESS_WAITING);
  }
  if (gameOver) {
    if (inCheck)
      return position.sideToMove() == chess::Color::White ? tr(STR_CHESS_BLACK_WINS) : tr(STR_CHESS_WHITE_WINS);
    return tr(STR_CHESS_STALEMATE);
  }
  if (inCheck) return tr(STR_CHESS_CHECK);
  return position.sideToMove() == chess::Color::White ? tr(STR_CHESS_WHITE_TO_MOVE) : tr(STR_CHESS_BLACK_TO_MOVE);
}

void ChessGameActivity::drawClockRow(int y, chess::Color side) const {
  const int x = board.x();
  const int w = board.size();
  const lichess::Player& player = side == chess::Color::White ? game.white : game.black;
  const bool active = haveGame && !gameOver && position.sideToMove() == side;
  // The side to move gets an inverted band so the state is legible at a glance.
  if (active) renderer.fillRect(x, y, w, CLOCK_ROW_HEIGHT, true);
  char name[56];
  const chess::Position& shown = browsing() ? viewPosition : position;
  const int lead = material(shown, side) - material(shown, chess::opposite(side));
  char diff[8] = "";
  if (lead > 0) snprintf(diff, sizeof(diff), " +%d", lead);
  if (player.aiLevel > 0) {
    char engine[32];
    snprintf(engine, sizeof(engine), tr(STR_CHESS_STOCKFISH), player.aiLevel);
    snprintf(name, sizeof(name), "%s%s", engine, diff);
  } else if (player.rating > 0) {
    snprintf(name, sizeof(name), "%s (%d%s)%s", player.name, player.rating, player.provisional ? "?" : "", diff);
  } else {
    snprintf(name, sizeof(name), "%s%s", player.name, diff);
  }
  const int nameH = renderer.getTextHeight(UI_12_FONT_ID);
  renderer.drawText(UI_12_FONT_ID, x + 6, y + (CLOCK_ROW_HEIGHT - nameH) / 2, name, !active);
  if (game.initialMs > 0 && !review) {
    const uint32_t ms = remainingMs(side);
    const uint32_t totalSec = ms / 1000;
    char clock[16];
    if (ms < CLOCK_FAST_BELOW_MS) {
      snprintf(clock, sizeof(clock), "%lu.%lu", static_cast<unsigned long>(totalSec),
               static_cast<unsigned long>((ms % 1000) / 100));
    } else {
      snprintf(clock, sizeof(clock), "%lu:%02lu", static_cast<unsigned long>(totalSec / 60),
               static_cast<unsigned long>(totalSec % 60));
    }
    const int tw = renderer.getTextWidth(NOTOSANS_18_FONT_ID, clock, EpdFontFamily::BOLD);
    const int th = renderer.getTextHeight(NOTOSANS_18_FONT_ID);
    renderer.drawText(NOTOSANS_18_FONT_ID, x + w - tw - 6, y + (CLOCK_ROW_HEIGHT - th) / 2, clock, !active,
                      EpdFontFamily::BOLD);
  }
}

void ChessGameActivity::drawPuzzleRow(int y) const {
  // Where a game shows the opponent: whose move it is, or the refusal.
  const int x = board.x();
  const int w = board.size();
  const bool refused = marks.wrongSquare != NoSquare;
  if (refused) renderer.fillRect(x, y, w, CLOCK_ROW_HEIGHT, true);
  const char* text = refused                                        ? tr(STR_CHESS_PUZZLE_WRONG)
                     : position.sideToMove() == chess::Color::White ? tr(STR_CHESS_WHITE_TO_MOVE)
                                                                    : tr(STR_CHESS_BLACK_TO_MOVE);
  const int th = renderer.getTextHeight(UI_12_FONT_ID);
  const int tw = renderer.getTextWidth(UI_12_FONT_ID, text);
  renderer.drawText(UI_12_FONT_ID, x + (w - tw) / 2, y + (CLOCK_ROW_HEIGHT - th) / 2, text, !refused);
}

const lichess::AnalysisPly* ChessGameActivity::analysisAt(int ply) const {
  if (ply < 1 || ply > static_cast<int>(analysis.size())) return nullptr;
  return &analysis[ply - 1];
}

int ChessGameActivity::shownPly() const { return browsing() ? viewPly : lichess::countMoves(moveList()); }

void ChessGameActivity::jumpToJudged(int direction) {
  const int total = static_cast<int>(analysis.size());
  int ply = shownPly() + direction;
  while (ply >= 1 && ply <= total) {
    if (analysis[ply - 1].judgment > 0) {
      showPly(ply);
      return;
    }
    ply += direction;
  }
}

void ChessGameActivity::drawAnalysis(int y) {
  // The evaluation of the shown position, the judgment of the move that led
  // to it, and a bar: the black share grows with Black's advantage.
  const lichess::AnalysisPly* a = analysisAt(shownPly());
  if (!a) return;
  char eval[24];
  if (a->mate != 0) {
    snprintf(eval, sizeof(eval), tr(STR_CHESS_MATE_IN), a->mate > 0 ? a->mate : -a->mate);
  } else {
    snprintf(eval, sizeof(eval), "%+.1f", a->cp / 100.0);
  }
  char line[96];
  if (a->judgment > 0) {
    const StrId names[3] = {StrId::STR_CHESS_INACCURACY, StrId::STR_CHESS_MISTAKE, StrId::STR_CHESS_BLUNDER};
    char best[12] = "";
    Move m;
    if (chess::moveFromUci(a->best, m)) {
      chess::Position before;
      lichess::replayMovesFrom(basePosition(), moveList(), before, nullptr, shownPly() - 1);
      chess::pgn::moveToSan(before, m, best, sizeof(best));
    }
    char bestText[32] = "";
    if (best[0]) snprintf(bestText, sizeof(bestText), tr(STR_CHESS_BEST_WAS), best);
    snprintf(line, sizeof(line), "%s   %s, %s", eval, I18N.get(names[a->judgment - 1]), bestText);
  } else {
    snprintf(line, sizeof(line), "%s", eval);
  }
  renderer.drawCenteredText(UI_12_FONT_ID, y, line, true);
  // The chart fills what is left above the action bar.
  chartX = board.x();
  chartW = board.size();
  chartY = y + renderer.getLineHeight(UI_12_FONT_ID) + 6;
  chartH = barY() - 8 - chartY;
  if (chartH >= 40) {
    drawEvalChart();
  } else {
    chartH = 0;
  }
}

namespace {
// The evaluation of a ply as a height in thousandths of the half chart,
// positive for White; a mate counts as five pawns.
int chartValue(const lichess::AnalysisPly& a) {
  if (a.mate != 0) return a.mate > 0 ? 1000 : -1000;
  int cp = a.cp;
  if (cp > 500) cp = 500;
  if (cp < -500) cp = -500;
  return cp * 2;
}
}  // namespace

void ChessGameActivity::drawEvalChart() {
  // An evaluation bar over time: each column is white on top for White's
  // share and black below, half and half when the game is level.
  const int n = static_cast<int>(analysis.size());
  if (n == 0 || chartW < 16) return;
  const int x0 = chartX + 1;
  const int innerW = chartW - 2;
  const int innerH = chartH - 2;
  renderer.drawRect(chartX, chartY, chartW, chartH, 1, true);
  auto valueAt = [&](int ply) { return ply <= 0 ? 0 : chartValue(analysis[ply - 1]); };
  for (int px = 0; px < innerW; ++px) {
    // The ply at this column, with a straight line between plies.
    const int num = px * n;
    const int p = num / innerW;
    const int frac = num % innerW;
    const int v = valueAt(p) + (valueAt(p + 1 > n ? n : p + 1) - valueAt(p)) * frac / innerW;
    const int whiteH = innerH * (1000 + v) / 2000;  // v is -1000..1000
    if (whiteH < innerH) renderer.fillRect(x0 + px, chartY + 1 + whiteH, 1, innerH - whiteH, true);
  }
  // The shown ply: a two-tone line that shows on both areas.
  const int shown = shownPly();
  if (shown >= 0 && shown <= n) {
    const int cx = x0 + shown * (innerW - 1) / n;
    renderer.fillRect(cx, chartY + 1, 1, innerH, false);
    renderer.fillRect(cx + 1, chartY + 1, 1, innerH, true);
  }
}

bool ChessGameActivity::tapChart(int tx, int ty) {
  const int n = static_cast<int>(analysis.size());
  if (chartH == 0 || n == 0) return false;
  if (tx < chartX || tx >= chartX + chartW || ty < chartY || ty >= chartY + chartH) return false;
  int ply = ((tx - chartX - 1) * n + (chartW - 2) / 2) / (chartW - 2);
  if (ply < 0) ply = 0;
  if (ply > n) ply = n;
  showPly(ply);
  return true;
}

void ChessGameActivity::drawStatusLine() {
  char buf[96];
  const char* text = statusText(buf, sizeof(buf));
  const int y = board.y() + board.size() + CLOCK_ROW_HEIGHT + 6;
  renderer.drawCenteredText(UI_12_FONT_ID, y, text, true);
  if (review && !analysis.empty()) drawAnalysis(y + renderer.getLineHeight(UI_12_FONT_ID) + 4);
  if (!online && !review && !puzzle && gameOver) {
    renderer.drawCenteredText(UI_10_FONT_ID, y + renderer.getLineHeight(UI_12_FONT_ID) + 4, tr(STR_CHESS_TAP_NEW_GAME),
                              true);
  }
}

int ChessGameActivity::barButtons(BarButton* out) const {
  int n = 0;
  if (browsing()) {
    out[n++] = {MenuItem::Prev, StrId::STR_CHESS_PREV};
    out[n++] = {MenuItem::Next, StrId::STR_CHESS_NEXT};
    out[n++] = {MenuItem::Live, review ? StrId::STR_CHESS_END : StrId::STR_CHESS_LIVE};
    return n;
  }
  if (review) {
    out[n++] = {MenuItem::Prev, StrId::STR_CHESS_PREV};
    out[n++] = {MenuItem::Back, StrId::STR_CHESS_LOBBY};
    out[n++] = {MenuItem::Count, StrId::STR_CHESS_MENU};
    return n;
  }
  if (puzzle) {
    out[n++] = puzzleSolved ? BarButton{MenuItem::NextPuzzle, StrId::STR_CHESS_NEXT_PUZZLE}
                            : BarButton{MenuItem::ShowSolution, StrId::STR_CHESS_SOLUTION};
    out[n++] = {MenuItem::Back, StrId::STR_CHESS_LOBBY};
    out[n++] = {MenuItem::Count, StrId::STR_CHESS_MENU};
    return n;
  }
  if (online) {
    if (gameOver) {
      out[n++] = {MenuItem::Rematch, StrId::STR_CHESS_REMATCH};
      out[n++] = {MenuItem::Back, StrId::STR_CHESS_LOBBY};
    } else {
      out[n++] = {MenuItem::Resign, StrId::STR_CHESS_RESIGN};
      const bool opponentOffers = game.myColor == chess::Color::White ? game.blackOffersDraw : game.whiteOffersDraw;
      out[n++] = {MenuItem::Draw, opponentOffers ? StrId::STR_CHESS_ACCEPT_DRAW : StrId::STR_CHESS_DRAW_SHORT};
    }
  } else {
    out[n++] = {MenuItem::NewGame, StrId::STR_CHESS_NEW_GAME};
    out[n++] = {MenuItem::Flip, StrId::STR_CHESS_FLIP_BOARD};
  }
  out[n++] = {MenuItem::Count, StrId::STR_CHESS_MENU};  // Count stands for "open the menu"
  return n;
}

int ChessGameActivity::barY() const {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  return safe.y + safe.height - BAR_HEIGHT - 4;
}

void ChessGameActivity::drawActionBar() {
  BarButton buttons[BAR_BUTTONS];
  const int count = barButtons(buttons);
  const int gap = 8;
  const int x0 = board.x();
  const int w = (board.size() - gap * (count - 1)) / count;
  const int y = barY();
  for (int i = 0; i < count; ++i) {
    const int x = x0 + i * (w + gap);
    renderer.drawRoundedRect(x, y, w, BAR_HEIGHT, 2, 8, true);
    const char* label = I18N.get(buttons[i].label);
    const int tw = renderer.getTextWidth(UI_12_FONT_ID, label);
    const int th = renderer.getTextHeight(UI_12_FONT_ID);
    renderer.drawText(UI_12_FONT_ID, x + (w - tw) / 2, y + (BAR_HEIGHT - th) / 2, label, true);
  }
}

bool ChessGameActivity::tapActionBar(int tx, int ty) {
  const int y = barY();
  if (ty < y || ty >= y + BAR_HEIGHT) return false;
  BarButton buttons[BAR_BUTTONS];
  const int count = barButtons(buttons);
  const int gap = 8;
  const int w = (board.size() - gap * (count - 1)) / count;
  const int index = (tx - board.x()) / (w + gap);
  if (tx < board.x() || index < 0 || index >= count) return false;
  if (buttons[index].item == MenuItem::Count) {
    openMenu();
  } else {
    runMenuItem(buttons[index].item);
  }
  requestUpdate();
  return true;
}

void ChessGameActivity::render(RenderLock&&) {
  // A previous asynchronous refresh must finish before the framebuffer
  // changes: its finish step copies the displayed frame into the panel's OLD
  // plane, and the next differential update diffs against that copy.
  renderer.waitRefreshComplete();
  renderer.clearScreen();
  auto& theme = UITheme::getInstance();
  const auto& metrics = theme.getMetrics();
  const Rect safe = theme.getScreenSafeArea(renderer, false, false);
  char title[48];
  if (puzzle) {
    snprintf(title, sizeof(title), tr(STR_CHESS_PUZZLE_TITLE), puzzleData.rating);
  } else if ((online || review) && haveGame) {
    snprintf(title, sizeof(title), "%s %lu+%lu", game.rated ? tr(STR_CHESS_RATED) : tr(STR_CHESS_CASUAL),
             static_cast<unsigned long>(game.initialMs / 60000), static_cast<unsigned long>(game.incrementMs / 1000));
  } else {
    snprintf(title, sizeof(title), "%s", tr(STR_CHESS));
  }
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight}, title);

  if (online && !haveGame) {
    // No game data yet: the board would show the wrong side and pieces.
    renderer.drawCenteredText(UI_12_FONT_ID, board.y() + board.size() / 2, tr(STR_CHESS_CONNECTING), true);
  } else {
    if (puzzle) {
      drawPuzzleRow(board.y() - CLOCK_ROW_HEIGHT);
    } else if (online || review) {
      const chess::Color top = board.isFlipped() ? chess::Color::White : chess::Color::Black;
      drawClockRow(board.y() - CLOCK_ROW_HEIGHT, top);
      drawClockRow(board.y() + board.size(), chess::opposite(top));
      // A border around the board marks that it is the user's move.
      if (myTurn() && !browsing()) renderer.drawRect(board.x(), board.y(), board.size(), board.size(), 3, true);
    }
    // In a reviewed game with analysis, the better move of a judged ply is an arrow.
    const lichess::AnalysisPly* judged = review ? analysisAt(shownPly()) : nullptr;
    Move better;
    const bool arrow = judged && judged->judgment > 0 && chess::moveFromUci(judged->best, better);
    if (browsing()) {
      ChessBoardView::Marks past;
      past.lastFrom = viewLastMove.from;
      past.lastTo = viewLastMove.to;
      past.checkedKing = viewPosition.inCheck() ? viewPosition.kingSquare(viewPosition.sideToMove()) : NoSquare;
      if (arrow) {
        // The better move starts from the position before the shown move.
        chess::Position before;
        lichess::replayMovesFrom(basePosition(), moveList(), before, nullptr, shownPly() - 1);
        past.arrows[0] = {better.from, better.to};
        past.arrowCount = 1;
        board.draw(renderer, before, past);
      } else {
        board.draw(renderer, viewPosition, past);
      }
    } else if (arrow) {
      ChessBoardView::Marks withArrow = marks;
      withArrow.arrows[0] = {better.from, better.to};
      withArrow.arrowCount = 1;
      chess::Position before;
      lichess::replayMovesFrom(basePosition(), moveList(), before, nullptr, shownPly() - 1);
      board.draw(renderer, before, withArrow);
    } else {
      board.draw(renderer, position, marks);
    }
    if (showQr) {
      // The game page on Lichess, where computer analysis is one tap away.
      const int size = board.size() >= 460 ? 260 : 220;
      const int qx = board.x() + (board.size() - size) / 2;
      const int qy = board.y() + 20;
      renderer.fillRect(board.x(), board.y(), board.size(), board.size(), false);
      renderer.drawRect(board.x(), board.y(), board.size(), board.size(), 1, true);
      char url[48];
      snprintf(url, sizeof(url), "https://lichess.org/%s", gameId);
      QrUtils::drawQrCode(renderer, Rect{qx, qy, size, size}, url);
      const std::vector<std::string> lines =
          renderer.wrappedText(UI_10_FONT_ID, tr(STR_CHESS_REQUEST_ANALYSIS_HELP), board.size() - 24, 4);
      int ty = qy + size + 12;
      for (const auto& l : lines) {
        renderer.drawCenteredText(UI_10_FONT_ID, ty, l.c_str(), true);
        ty += renderer.getLineHeight(UI_10_FONT_ID);
      }
      // The check button, then the last answer under it.
      qrButtonY = ty + 8;
      qrButtonH = BAR_HEIGHT;
      const int bw = board.size() - 80;
      const int bx = board.x() + 40;
      renderer.drawRoundedRect(bx, qrButtonY, bw, BAR_HEIGHT, 2, 8, true);
      const char* label = tr(STR_CHESS_CHECK_ANALYSIS);
      renderer.drawText(UI_12_FONT_ID, bx + (bw - renderer.getTextWidth(UI_12_FONT_ID, label)) / 2,
                        qrButtonY + (BAR_HEIGHT - renderer.getTextHeight(UI_12_FONT_ID)) / 2, label, true);
      if (analysisNote[0]) renderer.drawCenteredText(UI_10_FONT_ID, qrButtonY + BAR_HEIGHT + 6, analysisNote, true);
    }
    drawStatusLine();
  }
  drawActionBar();

  if (promotionPopup.processRender(renderer, mappedInput)) return;
  if (confirmPopup.processRender(renderer, mappedInput)) return;
  if (menuPopup.processRender(renderer, mappedInput)) return;

  // Fast refreshes only, except where a full one was asked for: game start,
  // game end, a board flip, the menu, or the opponent-move option. Clock
  // ticks never trigger one.
  clockTickPending = false;
  const bool full = forceFullRefresh;
  if (full) {
    renderer.displayBuffer(HalDisplay::FULL_REFRESH);
    forceFullRefresh = false;
  } else if (renderer.supportsAsyncRefresh()) {
    // Start the refresh and hold the render lock until the panel is done:
    // nothing may draw into the framebuffer before the finish step has copied
    // it into the panel's baseline, and only one task may run that step. The
    // loop keeps polling input meanwhile because it never waits for the lock.
    renderer.displayBufferAsync(HalDisplay::FAST_REFRESH);
    renderer.waitRefreshComplete();
  } else {
    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  }
}
