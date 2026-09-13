#include "ChessStudyActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <cstdio>
#include <cstring>

#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;
using chess::Move;
using chess::NoSquare;
using chess::Square;

namespace {
constexpr fui::ActionId ACTION_BUTTON = 1;
constexpr fui::ActionId ACTION_ROW = 2;
constexpr fui::ActionId ACTION_VARIATION = 3;
constexpr fui::ActionId ACTION_LAST = ACTION_VARIATION;
constexpr int16_t BTN_PREV = 1;
constexpr int16_t BTN_NEXT = 2;
constexpr int16_t BTN_MENU = 3;
constexpr int16_t BTN_SHOW = 4;
constexpr int16_t BTN_BOARD = 5;
constexpr int COMMENT_FONT = UI_12_FONT_ID;
constexpr int TEXT_FONT = NOTOSERIF_16_FONT_ID;
constexpr int PREVIEW_LINES = 2;
}  // namespace

ChessStudyActivity::ChessStudyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* id,
                                       const char* name)
    : Activity("ChessStudy", renderer, mappedInput), UiAppHost(renderer) {
  lichess::copyStr(studyId, sizeof(studyId), id);
  lichess::copyStr(studyName, sizeof(studyName), name);
}

void ChessStudyActivity::onEnter() {
  Activity::onEnter();
  resetUi();
  for (fui::ActionId a = ACTION_BUTTON; a <= ACTION_LAST; ++a) app.on(a, &ChessStudyActivity::onAction, this);
  app.setScreen(&ChessStudyActivity::screenTrampoline, this);

  auto& theme = UITheme::getInstance();
  const auto& metrics = theme.getMetrics();
  const Rect safe = theme.getScreenSafeArea(renderer, false, false);
  const int top = safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int squareSize = safe.width / 8;
  board.setLayout(safe.x + (safe.width - squareSize * 8) / 2, top, squareSize);

  int last = 0;
  STUDY_STORE.loadChapters(studyId, chapters, last);
  chapterRows.resize(chapters.size());
  for (size_t i = 0; i < chapters.size(); ++i) {
    chapterRows[i] = fui::ListItem{};
    chapterRows[i].label = chapters[i].name;
    chapterRows[i].actionValue = static_cast<int16_t>(i);
  }
  chapterNav.reset(last);
  LOG_INF("CHESS", "Study %s: %d chapters, heap %u", studyId, static_cast<int>(chapters.size()), ESP.getFreeHeap());
  if (!chapters.empty()) {
    openChapter(last);
  } else {
    requestUpdate();
  }
}

void ChessStudyActivity::onExit() {
  std::vector<chess::pgn::Node>().swap(tree.nodes);
  std::vector<std::string>().swap(commentLines);
  std::vector<std::string>().swap(textLines);
  Activity::onExit();
}

// --- chapter ------------------------------------------------------------------

void ChessStudyActivity::openChapter(int index) {
  if (index < 0 || index >= static_cast<int>(chapters.size())) return;
  ChapterSource src;
  if (!src.open(studyId, chapters[index].at, chapters[index].len)) {
    LOG_ERR("CHESS", "Cannot read chapter %d of %s", index, studyId);
    return;
  }
  chess::pgn::parse(src, tree);
  chapter = index;
  STUDY_STORE.setLastChapter(studyId, index);
  practice = false;
  lineDone = false;
  replyAt = 0;
  wrongUntil = 0;
  truncatedNoted = false;
  board.setFlipped(strcmp(tree.tags.orientation, "black") == 0);
  LOG_INF("CHESS", "Chapter %d: %d nodes%s, heap %u", index, static_cast<int>(tree.nodes.size()),
          tree.truncated ? " (truncated)" : "", ESP.getFreeHeap());
  state = State::Reading;
  goTo(0);
}

void ChessStudyActivity::goTo(uint16_t node) {
  if (node >= tree.nodes.size()) return;
  cur = node;
  chess::pgn::positionAt(tree, cur, pos);
  clearSelection();
  const Move& m = tree.nodes[cur].move;
  marks.lastFrom = m.from;
  marks.lastTo = m.to;
  marks.wrongSquare = NoSquare;
  marks.checkedKing = pos.inCheck() ? pos.kingSquare(pos.sideToMove()) : NoSquare;
  refreshLegal();
  loadComment();
  updateVariations();
  updateMoveText();
  requestUpdate();
}

void ChessStudyActivity::stepPrev() {
  if (cur == 0) return;
  replyAt = 0;
  goTo(tree.nodes[cur].parent);
}

void ChessStudyActivity::stepNext() {
  const uint16_t child = tree.nodes[cur].firstChild;
  if (child) goTo(child);
}

void ChessStudyActivity::loadComment() {
  commentLines.clear();
  textLines.clear();
  commentText.clear();
  textPage = 0;
  marks.arrowCount = 0;
  marks.circleCount = 0;
  const chess::pgn::Node& n = tree.nodes[cur];
  std::string text;
  if (n.commentLen > 0) {
    std::string raw;
    if (STUDY_STORE.readSpan(studyId, chapters[chapter].at + n.commentAt, n.commentLen, raw)) {
      chess::pgn::Highlight marksFound[ChessBoardView::Marks::MAX_ARROWS];
      const int count =
          chess::pgn::cleanComment(raw.c_str(), raw.size(), text, marksFound, ChessBoardView::Marks::MAX_ARROWS);
      for (int i = 0; i < count; ++i) {
        if (marksFound[i].to != NoSquare) {
          marks.arrows[marks.arrowCount++] = {marksFound[i].from, marksFound[i].to};
        } else {
          marks.circles[marks.circleCount++] = marksFound[i].from;
        }
      }
    }
  }
  if (practice && !lineDone && pos.sideToMove() == practiceSide) {
    // The comment of a position often names the move: keep it for after the move.
    text.clear();
  }
  if (cur == 0 && tree.truncated && !truncatedNoted) {
    if (!text.empty()) text.append(" ");
    text.append(tr(STR_CHESS_CHAPTER_TRUNCATED));
  }
  if (cur == 0 && !tree.tags.standard) text = tr(STR_CHESS_VARIANT_UNSUPPORTED);
  commentText.swap(text);
  if (!commentText.empty()) {
    // The preview needs one line more than it shows, to know there is more.
    commentLines = renderer.wrappedText(COMMENT_FONT, commentText.c_str(), board.size() - 8, PREVIEW_LINES + 1);
  }
}

void ChessStudyActivity::openText() {
  if (practice || commentText.empty()) return;
  state = State::Text;
  textPage = 0;
  requestUpdate();
}

void ChessStudyActivity::pageText(int delta) {
  if (textPages <= 1) return;
  textPage = (textPage + delta + textPages) % textPages;
  requestUpdate();
}

void ChessStudyActivity::updateVariations() {
  variationCount = 0;
  if (practice) return;
  uint16_t c = tree.nodes[cur].firstChild;
  const uint16_t count = static_cast<uint16_t>(chess::pgn::childCount(tree, cur));
  if (count < 2) return;
  while (c && variationCount < MAX_VARIATIONS) {
    char san[12];
    chess::pgn::moveToSan(pos, tree.nodes[c].move, san, sizeof(san));
    snprintf(variationLabels[variationCount], sizeof(variationLabels[0]), "%s", san);
    ++variationCount;
    c = tree.nodes[c].nextSibling;
  }
}

void ChessStudyActivity::updateMoveText() {
  if (practice) {
    if (lineDone) {
      snprintf(moveText, sizeof(moveText), tr(STR_CHESS_LINE_DONE), found, missed);
    } else if (marks.wrongSquare != NoSquare) {
      snprintf(moveText, sizeof(moveText), "%s", tr(STR_CHESS_PUZZLE_WRONG));
    } else if (pos.sideToMove() == practiceSide) {
      snprintf(moveText, sizeof(moveText), "%s",
               practiceSide == chess::Color::White ? tr(STR_CHESS_FIND_MOVE_WHITE) : tr(STR_CHESS_FIND_MOVE_BLACK));
    } else {
      snprintf(moveText, sizeof(moveText), "...");
    }
    return;
  }
  if (cur == 0) {
    snprintf(moveText, sizeof(moveText), "%s", tr(STR_CHESS_START));
    return;
  }
  chess::Position before;
  chess::pgn::positionAt(tree, tree.nodes[cur].parent, before);
  char san[12];
  chess::pgn::moveToSan(before, tree.nodes[cur].move, san, sizeof(san));
  const int number = before.fullmoveNumber();
  const bool white = before.sideToMove() == chess::Color::White;
  snprintf(moveText, sizeof(moveText), "%d.%s %s%s", number, white ? "" : "..", san,
           chess::pgn::nagText(tree.nodes[cur].nag));
  if (chess::pgn::childCount(tree, cur) == 0) {
    const size_t used = strlen(moveText);
    snprintf(moveText + used, sizeof(moveText) - used, "  (%s)", tr(STR_CHESS_END_OF_LINE));
  }
}

void ChessStudyActivity::refreshLegal() { legalCount = pos.generateLegalMoves(legal, chess::MAX_MOVES); }

// --- board input ---------------------------------------------------------------

void ChessStudyActivity::selectSquare(Square square) {
  clearSelection();
  marks.selected = square;
  for (int i = 0; i < legalCount; ++i) {
    if (legal[i].from == square) marks.target[legal[i].to] = true;
  }
}

void ChessStudyActivity::clearSelection() {
  marks.selected = NoSquare;
  memset(marks.target, 0, sizeof(marks.target));
}

uint16_t ChessStudyActivity::childFor(const Move& m) const {
  for (uint16_t c = tree.nodes[cur].firstChild; c; c = tree.nodes[c].nextSibling) {
    const Move& cm = tree.nodes[c].move;
    if (cm.from == m.from && cm.to == m.to) return c;
  }
  return 0;
}

void ChessStudyActivity::onSquareTapped(Square square) {
  if (practice && !userToMove()) return;
  const chess::Piece piece = pos.pieceAt(square);
  const bool own = piece != chess::NoPiece && chess::colorOf(piece) == pos.sideToMove();
  if (marks.selected == NoSquare) {
    if (own) selectSquare(square);
  } else if (square == marks.selected) {
    clearSelection();
  } else if (marks.target[square]) {
    tryMove(Move{marks.selected, square, chess::PieceType::None});
  } else if (own) {
    selectSquare(square);
  } else {
    clearSelection();
  }
  requestUpdate();
}

void ChessStudyActivity::tryMove(const Move& m) {
  const uint16_t child = childFor(m);
  if (child) {
    if (practice) ++found;
    missedHere = false;
    goTo(child);
    if (practice) {
      lineDone = chess::pgn::childCount(tree, cur) == 0;
      if (!lineDone) replyAt = millis() + REPLY_DELAY_MS;
      updateMoveText();
    }
    return;
  }
  clearSelection();
  if (practice) {
    if (!missedHere) ++missed;
    missedHere = true;
    marks.wrongSquare = m.to;
    wrongUntil = millis() + WRONG_MS;
    updateMoveText();
  }
}

// --- practice ------------------------------------------------------------------

void ChessStudyActivity::startPractice() {
  practice = true;
  found = 0;
  missed = 0;
  missedHere = false;
  practiceSide = strcmp(tree.tags.orientation, "black") == 0   ? chess::Color::Black
                 : strcmp(tree.tags.orientation, "white") == 0 ? chess::Color::White
                                                               : tree.root.sideToMove();
  board.setFlipped(practiceSide == chess::Color::Black);
  lineDone = chess::pgn::childCount(tree, cur) == 0;
  goTo(cur);
  if (!lineDone && pos.sideToMove() != practiceSide) replyAt = millis() + REPLY_DELAY_MS;
}

void ChessStudyActivity::playReply() {
  const int count = chess::pgn::childCount(tree, cur);
  if (count == 0) {
    lineDone = true;
    updateMoveText();
    requestUpdate();
    return;
  }
  // Several replies in the tree: any of them, so repeated runs cover the branches.
  const uint16_t child = chess::pgn::childAt(tree, cur, count > 1 ? static_cast<int>(random(count)) : 0);
  missedHere = false;
  goTo(child);
  lineDone = chess::pgn::childCount(tree, cur) == 0;
  updateMoveText();
}

void ChessStudyActivity::showAnswer() {
  if (!userToMove()) return;
  if (!missedHere) ++missed;
  const uint16_t child = tree.nodes[cur].firstChild;
  if (!child) return;
  missedHere = false;
  goTo(child);
  lineDone = chess::pgn::childCount(tree, cur) == 0;
  if (!lineDone) replyAt = millis() + REPLY_DELAY_MS;
  updateMoveText();
}

// --- menu ----------------------------------------------------------------------

void ChessStudyActivity::openMenu() {
  static const char* labels[static_cast<int>(MenuItem::Count)];
  menuCount = 0;
  auto add = [&](MenuItem item, StrId label) {
    menuItems[menuCount] = item;
    labels[menuCount] = I18N.get(label);
    ++menuCount;
  };
  if (state != State::Chapters) {
    add(MenuItem::Practice, practice ? StrId::STR_CHESS_STOP_PRACTICE : StrId::STR_CHESS_PRACTICE);
    add(MenuItem::Flip, StrId::STR_CHESS_FLIP_BOARD);
    if (chapter + 1 < static_cast<int>(chapters.size())) add(MenuItem::NextChapter, StrId::STR_CHESS_NEXT_CHAPTER);
    if (chapter > 0) add(MenuItem::PrevChapter, StrId::STR_CHESS_PREV_CHAPTER);
    add(MenuItem::Chapters, StrId::STR_CHESS_CHAPTER_LIST);
  }
  add(MenuItem::Delete, StrId::STR_CHESS_DELETE_STUDY);
  add(MenuItem::Back, StrId::STR_CHESS_BACK_TO_STUDIES);
  confirmingDelete = false;
  menuChoice = -1;
  menuPopup.show(studyName, labels, menuCount, 0, [this](const int idx) { menuChoice = idx; });
  requestUpdate();
}

void ChessStudyActivity::runMenuItem(MenuItem item) {
  switch (item) {
    case MenuItem::Practice:
      if (practice) {
        practice = false;
        replyAt = 0;
        goTo(cur);
      } else {
        startPractice();
      }
      break;
    case MenuItem::Flip:
      board.setFlipped(!board.isFlipped());
      requestUpdate();
      break;
    case MenuItem::NextChapter:
      openChapter(chapter + 1);
      break;
    case MenuItem::PrevChapter:
      openChapter(chapter - 1);
      break;
    case MenuItem::Chapters:
      state = State::Chapters;
      chapterNav.reset(chapter < 0 ? 0 : chapter);
      requestUpdate();
      break;
    case MenuItem::Delete: {
      static const char* yesNo[2];
      yesNo[0] = tr(STR_DELETE);
      yesNo[1] = tr(STR_CANCEL);
      confirmingDelete = true;
      menuChoice = -1;
      menuPopup.show(tr(STR_CHESS_DELETE_STUDY_Q), yesNo, 2, 1, [this](const int idx) { menuChoice = idx; });
      requestUpdate();
      break;
    }
    case MenuItem::Back:
      finish();
      break;
    default:
      break;
  }
}

// --- screen --------------------------------------------------------------------

void ChessStudyActivity::screenTrampoline(UiScreen& screen, void* user) {
  static_cast<ChessStudyActivity*>(user)->buildScreen(screen);
}

void ChessStudyActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, false, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.verticalSpacing * 2),
      static_cast<int16_t>(safe.x)});
  if (state == State::Chapters) {
    buildChapters(screen);
  } else if (state == State::Text) {
    buildText(screen);
  } else {
    buildReading(screen);
  }
}

void ChessStudyActivity::buildChapters(UiScreen& screen) {
  const auto& theme = screen.theme();
  if (chapters.empty()) {
    screen.centeredText(tr(STR_CHESS_NO_CHAPTERS));
    return;
  }
  const int count = static_cast<int>(chapters.size());
  fui::ListProps props;
  props.items = chapterRows.data();
  props.count = static_cast<uint16_t>(count);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.selectedIndex = -1;
  props.labelText = theme.bodyText;
  const int16_t rowH = static_cast<int16_t>(theme.rowHeight + theme.spaceSm);
  chapterNav.syncToProps(screen.body(), rowH, theme.listRowGap, count, props);
  props.selectedIndex = -1;
  screen.list(props);
}

void ChessStudyActivity::buildReading(UiScreen& screen) {
  const auto& theme = screen.theme();
  screen.takeTop(static_cast<int16_t>(board.size() + theme.spaceSm), 0);  // the board is drawn after the UI

  // Navigation strip: Prev, the move (or the practice prompt), Next or Show, Menu.
  const fui::Rect band = screen.takeTop(theme.rowHeight, theme.spaceSm);
  const int16_t gap = theme.spaceSm;
  const int16_t navW = 64;
  const int16_t menuW = 64;
  auto button = [&](const char* label, int16_t value, fui::Rect rect) {
    fui::ButtonProps b;
    b.label = label;
    b.action = ACTION_BUTTON;
    b.value = value;
    b.inputMask = fui::InputTouch;
    b.text = theme.smallText;
    b.text.align = fui::TextAlign::Center;
    screen.button(b, rect);
  };
  button("<", BTN_PREV, fui::Rect{band.x, band.y, navW, band.height});
  const int16_t nextX = static_cast<int16_t>(band.x + band.width - menuW - gap - navW);
  if (userToMove()) {
    button(tr(STR_CHESS_SHOW), BTN_SHOW, fui::Rect{nextX, band.y, navW, band.height});
  } else {
    button(">", BTN_NEXT, fui::Rect{nextX, band.y, navW, band.height});
  }
  button(tr(STR_CHESS_MENU), BTN_MENU,
         fui::Rect{static_cast<int16_t>(band.x + band.width - menuW), band.y, menuW, band.height});
  moveRect = fui::Rect{static_cast<int16_t>(band.x + navW + gap), band.y,
                       static_cast<int16_t>(nextX - band.x - navW - gap * 2), band.height};

  // Alternatives to the next move, as buttons.
  if (variationCount > 1) {
    const fui::Rect row = screen.takeTop(theme.rowHeight, theme.spaceSm);
    const int16_t w = static_cast<int16_t>((row.width - gap * (variationCount - 1)) / variationCount);
    for (int i = 0; i < variationCount; ++i) {
      fui::ButtonProps b;
      b.label = variationLabels[i];
      b.action = ACTION_VARIATION;
      b.value = static_cast<int16_t>(i);
      b.inputMask = fui::InputTouch;
      b.text = theme.smallText;
      b.text.align = fui::TextAlign::Center;
      b.radius = theme.controlRadius / 2;
      screen.button(b, fui::Rect{static_cast<int16_t>(row.x + i * (w + gap)), row.y, w, row.height});
    }
  }
  commentRect = screen.body();
}

void ChessStudyActivity::buildText(UiScreen& screen) {
  const auto& theme = screen.theme();
  // Bottom: back to the board, and the previous or next move without leaving the text.
  const fui::Rect band = screen.takeBottom(theme.rowHeight, theme.spaceSm);
  const int16_t gap = theme.spaceSm;
  const int16_t navW = 72;
  auto button = [&](const char* label, int16_t value, fui::Rect rect) {
    fui::ButtonProps b;
    b.label = label;
    b.action = ACTION_BUTTON;
    b.value = value;
    b.inputMask = fui::InputTouch;
    b.text = theme.smallText;
    b.text.align = fui::TextAlign::Center;
    screen.button(b, rect);
  };
  button(tr(STR_CHESS_BOARD), BTN_BOARD,
         fui::Rect{band.x, band.y, static_cast<int16_t>(band.width - (navW + gap) * 2), band.height});
  button("<", BTN_PREV,
         fui::Rect{static_cast<int16_t>(band.x + band.width - navW * 2 - gap), band.y, navW, band.height});
  button(">", BTN_NEXT, fui::Rect{static_cast<int16_t>(band.x + band.width - navW), band.y, navW, band.height});
  textRect = screen.body();
}

void ChessStudyActivity::onAction(const fui::ActionEvent& event, void* user) {
  static_cast<ChessStudyActivity*>(user)->handleAction(event);
}

void ChessStudyActivity::handleAction(const fui::ActionEvent& event) {
  switch (event.action) {
    case ACTION_BUTTON:
      app.clearTapFlash();
      if (event.value == BTN_PREV) {
        stepPrev();
      } else if (event.value == BTN_NEXT) {
        if (!practice) stepNext();
      } else if (event.value == BTN_SHOW) {
        showAnswer();
      } else if (event.value == BTN_MENU) {
        openMenu();
      } else if (event.value == BTN_BOARD) {
        state = State::Reading;
        requestUpdate();
      }
      break;
    case ACTION_ROW:
      app.clearTapFlash();
      openChapter(event.value);
      break;
    case ACTION_VARIATION: {
      app.clearTapFlash();
      const uint16_t child = chess::pgn::childAt(tree, cur, event.value);
      if (child) goTo(child);
      break;
    }
    default:
      break;
  }
}

// --- loop and render -----------------------------------------------------------

void ChessStudyActivity::loop() {
  if (menuPopup.isActive()) {
    menuPopup.handleInput(mappedInput, [this] { requestUpdate(); });
    if (menuChoice >= 0) {
      RenderLock lock;
      const int idx = menuChoice;
      menuChoice = -1;
      if (confirmingDelete) {
        confirmingDelete = false;
        if (idx == 0) {
          STUDY_STORE.remove(studyId);
          setResult(KeyboardResult{"deleted"});
          finish();
          return;
        }
        requestUpdate();
      } else if (idx < menuCount) {
        runMenuItem(menuItems[idx]);
      }
    }
    return;
  }
  {
    RenderLock lock;
    if (practice && replyAt && millis() >= replyAt) {
      replyAt = 0;
      playReply();
    }
    if (wrongUntil && millis() >= wrongUntil) {
      wrongUntil = 0;
      marks.wrongSquare = NoSquare;
      updateMoveText();
      requestUpdate();
    }
    const auto route = routeTouch(mappedInput);
    if (route.routed && app.invalidated()) requestUpdate();
    if (route) return;
    int tx = 0;
    int ty = 0;
    if (state == State::Reading && mappedInput.wasScreenTapped(tx, ty)) {
      const Square square = board.squareAt(tx, ty);
      if (square != NoSquare) {
        onSquareTapped(square);
        return;
      }
      if (ty >= commentRect.y && ty < commentRect.bottom()) {
        openText();
        return;
      }
    } else if (state == State::Text && mappedInput.wasScreenTapped(tx, ty)) {
      if (ty >= textRect.y && ty < textRect.bottom()) {
        pageText(1);
        return;
      }
    }
    const auto swipe = mappedInput.wasSwipe();
    if (state == State::Reading || state == State::Text) {
      const bool prev = swipe == MappedInputManager::SwipeDir::Left ||
                        mappedInput.wasReleased(MappedInputManager::Button::Left) ||
                        mappedInput.wasReleased(MappedInputManager::Button::PageBack);
      const bool next = swipe == MappedInputManager::SwipeDir::Right ||
                        mappedInput.wasReleased(MappedInputManager::Button::Right) ||
                        mappedInput.wasReleased(MappedInputManager::Button::PageForward);
      if (prev) {
        stepPrev();
      } else if (next && !practice) {
        stepNext();
      } else if (swipe == MappedInputManager::SwipeDir::Up) {
        if (state == State::Text)
          pageText(1);
        else
          openText();
      } else if (swipe == MappedInputManager::SwipeDir::Down) {
        if (state == State::Text) pageText(-1);
      } else if (mappedInput.wasMenuGesture() || mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        openMenu();
      }
    } else if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      const int delta = swipe == MappedInputManager::SwipeDir::Up ? chapterNav.visibleRows : -chapterNav.visibleRows;
      if (chapterNav.scrollBy(delta, static_cast<int>(chapters.size()))) requestUpdate();
    }
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    RenderLock lock;
    if (state == State::Text) {
      state = State::Reading;
      requestUpdate();
    } else if (state == State::Reading && chapters.size() > 1) {
      state = State::Chapters;
      chapterNav.reset(chapter < 0 ? 0 : chapter);
      requestUpdate();
    } else {
      finish();
    }
  }
}

void ChessStudyActivity::render(RenderLock&&) {
  renderer.clearScreen();
  auto& theme = UITheme::getInstance();
  const auto& metrics = theme.getMetrics();
  const Rect safe = theme.getScreenSafeArea(renderer, false, false);
  char subtitle[64];
  if (state == State::Text) {
    snprintf(subtitle, sizeof(subtitle), "%s", moveText);
  } else if (state == State::Reading && chapter >= 0) {
    snprintf(subtitle, sizeof(subtitle), "%s", chapters[chapter].name);
  } else {
    snprintf(subtitle, sizeof(subtitle), tr(STR_CHESS_CHAPTERS), static_cast<int>(chapters.size()));
  }
  GUI.drawHeader(renderer, Rect{safe.x, safe.y + metrics.topPadding, safe.width, metrics.headerHeight}, studyName,
                 subtitle);
  renderUi();
  if (state == State::Reading) {
    board.draw(renderer, pos, marks);
    // The move, or the practice prompt, centered in the strip.
    {
      const int th = renderer.getTextHeight(COMMENT_FONT);
      const int tw = renderer.getTextWidth(COMMENT_FONT, moveText);
      const int x = moveRect.x + (moveRect.width - tw) / 2;
      renderer.drawText(COMMENT_FONT, x < moveRect.x ? moveRect.x : x, moveRect.y + (moveRect.height - th) / 2,
                        moveText, true);
    }
    // The comment preview: two lines, cut with dots when there is more to read.
    const int lh = renderer.getLineHeight(COMMENT_FONT);
    int fit = commentRect.height / lh;
    if (fit > PREVIEW_LINES) fit = PREVIEW_LINES;
    const int lines = static_cast<int>(commentLines.size());
    const int shown = lines < fit ? lines : fit;
    for (int i = 0; i < shown; ++i) {
      std::string line = commentLines[i];
      if (i == shown - 1 && lines > shown) {
        if (line.size() > 4) line.erase(line.size() - 4);
        line.append(" ...");
      }
      renderer.drawText(COMMENT_FONT, commentRect.x + 4, commentRect.y + i * lh, line.c_str(), true);
    }
    if (lines > 0 && !practice) {
      const char* hint = tr(STR_CHESS_READ_MORE);
      const int tw = renderer.getTextWidth(UI_10_FONT_ID, hint);
      const int y = commentRect.y + shown * lh + 2;
      if (y + renderer.getLineHeight(UI_10_FONT_ID) <= commentRect.bottom()) {
        renderer.drawText(UI_10_FONT_ID, commentRect.right() - tw - 4, y, hint, true);
      }
    }
  } else if (state == State::Text) {
    // The full comment in the reading font, one page at a time.
    if (textLines.empty() && !commentText.empty()) {
      textLines = renderer.wrappedText(TEXT_FONT, commentText.c_str(), textRect.width - 8, MAX_COMMENT_LINES);
    }
    const int lh = renderer.getLineHeight(TEXT_FONT);
    const int pageLh = renderer.getLineHeight(UI_10_FONT_ID) + 2;
    textLinesPerPage = (textRect.height - pageLh) / lh;
    if (textLinesPerPage < 1) textLinesPerPage = 1;
    const int lines = static_cast<int>(textLines.size());
    textPages = lines > 0 ? (lines + textLinesPerPage - 1) / textLinesPerPage : 1;
    if (textPage >= textPages) textPage = textPages - 1;
    if (lines == 0) {
      renderer.drawCenteredText(TEXT_FONT, textRect.y + textRect.height / 2, tr(STR_CHESS_NO_COMMENT), true);
    }
    for (int i = 0; i < textLinesPerPage; ++i) {
      const int idx = textPage * textLinesPerPage + i;
      if (idx >= lines) break;
      renderer.drawText(TEXT_FONT, textRect.x + 4, textRect.y + i * lh, textLines[idx].c_str(), true);
    }
    if (textPages > 1) {
      char page[16];
      snprintf(page, sizeof(page), "%d/%d", textPage + 1, textPages);
      const int tw = renderer.getTextWidth(UI_10_FONT_ID, page);
      renderer.drawText(UI_10_FONT_ID, textRect.right() - tw - 4, textRect.bottom() - pageLh, page, true);
    }
  }
  if (menuPopup.processRender(renderer, mappedInput)) return;
  renderer.displayBuffer();
}
