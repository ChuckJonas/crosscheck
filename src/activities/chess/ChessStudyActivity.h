#pragma once
#include <Pgn.h>
#include <Position.h>
#include <StudyStore.h>

#include <string>
#include <vector>

#include "ChessBoardView.h"
#include "activities/Activity.h"
#include "components/OptionPopup.h"
#include "components/UiAppHost.h"

// Reads a study saved on the card: the chapter list, then a chapter on the
// board with its comments, glyphs, arrows, and variations. Practice mode
// hides the next move of the line and asks the reader to find it.
class ChessStudyActivity final : public Activity, private UiAppHost {
 public:
  ChessStudyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* studyId,
                     const char* studyName);

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  // Reading shows the board with a comment preview; Text is the full comment.
  enum class State : uint8_t { Chapters, Reading, Text };
  enum class MenuItem : uint8_t { Practice, Flip, NextChapter, PrevChapter, Chapters, Delete, Back, Count };
  static constexpr int MAX_VARIATIONS = 4;
  static constexpr int MAX_COMMENT_LINES = 120;
  static constexpr uint32_t REPLY_DELAY_MS = 700;
  static constexpr uint32_t WRONG_MS = 1200;

  char studyId[16] = {};
  char studyName[64] = {};
  State state = State::Chapters;
  std::vector<ChapterInfo> chapters;
  std::vector<freeink::ui::ListItem> chapterRows;
  freeink::ui::ListNav chapterNav;
  int chapter = -1;

  chess::pgn::Tree tree;
  uint16_t cur = 0;
  chess::Position pos;
  ChessBoardView board;
  ChessBoardView::Marks marks;
  chess::Move legal[chess::MAX_MOVES];
  int legalCount = 0;

  // The current node's comment: a short preview under the board, and the
  // full text on its own page in a larger font.
  std::string commentText;
  std::vector<std::string> commentLines;  // preview font
  std::vector<std::string> textLines;     // reading font, wrapped on demand
  int textPage = 0;
  int textPages = 1;
  int textLinesPerPage = 1;
  freeink::ui::Rect commentRect{};
  freeink::ui::Rect textRect{};
  freeink::ui::Rect moveRect{};
  char moveText[64] = {};
  char variationLabels[MAX_VARIATIONS][12] = {};
  int variationCount = 0;
  bool truncatedNoted = false;

  // Practice: the reader plays practiceSide; the other side's replies come
  // from the tree after a short delay.
  bool practice = false;
  chess::Color practiceSide = chess::Color::White;
  int found = 0;
  int missed = 0;
  bool missedHere = false;
  bool lineDone = false;
  uint32_t replyAt = 0;
  uint32_t wrongUntil = 0;

  OptionPopup menuPopup;
  MenuItem menuItems[static_cast<int>(MenuItem::Count)];
  int menuCount = 0;
  int menuChoice = -1;
  bool confirmingDelete = false;

  bool userToMove() const { return practice && !lineDone && pos.sideToMove() == practiceSide && replyAt == 0; }
  void openChapter(int index);
  void goTo(uint16_t node);
  void stepPrev();
  void stepNext();
  void loadComment();
  void openText();
  void pageText(int delta);
  void updateVariations();
  void updateMoveText();
  void refreshLegal();
  void onSquareTapped(chess::Square square);
  void selectSquare(chess::Square square);
  void clearSelection();
  uint16_t childFor(const chess::Move& m) const;
  void tryMove(const chess::Move& m);
  void startPractice();
  void playReply();
  void showAnswer();
  void openMenu();
  void runMenuItem(MenuItem item);

  static void screenTrampoline(UiScreen& screen, void* user);
  void buildScreen(UiScreen& screen);
  void buildChapters(UiScreen& screen);
  void buildReading(UiScreen& screen);
  void buildText(UiScreen& screen);
  static void onAction(const freeink::ui::ActionEvent& event, void* user);
  void handleAction(const freeink::ui::ActionEvent& event);
};
