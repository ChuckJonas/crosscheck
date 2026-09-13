#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "Pgn.h"

using namespace chess;
using namespace chess::pgn;

namespace {
const char* SAMPLE =
    "[Event \"Study: Chapter 1\"]\n"
    "[ChapterName \"Italian\"]\n"
    "[Result \"*\"]\n"
    "[Orientation \"black\"]\n"
    "\n"
    "{ Intro text. } 1. e4 { [%anno \"x\", y] Kings pawn. } 1... e5 2. Nf3 Nc6 3. Bc4 { The Italian ♞ } "
    "{ [%cal Gc4f7,Rd2d4] [%csl Gd4] } 3... Nf6!? (3... Bc5 { Other main line. } 4. c3) 4. d3 $1 Be7 *\n";

bool parseText(const char* text, Tree& tree) {
  StringSource src(text, strlen(text));
  return parse(src, tree);
}
}  // namespace

TEST(Pgn, ParsesTagsAndMainLine) {
  Tree tree;
  ASSERT_TRUE(parseText(SAMPLE, tree));
  EXPECT_STREQ(tree.tags.chapterName, "Italian");
  EXPECT_STREQ(tree.tags.orientation, "black");
  EXPECT_FALSE(tree.truncated);
  // Root + e4 e5 Nf3 Nc6 Bc4 Nf6 d3 Be7 + variation Bc5 c3 = 11 nodes.
  ASSERT_EQ(tree.nodes.size(), 11u);
  uint16_t path[16];
  // The main line ends with Be7.
  uint16_t node = 0;
  while (tree.nodes[node].firstChild) node = tree.nodes[node].firstChild;
  EXPECT_EQ(pathTo(tree, node, path, 16), 8);
  Position pos;
  positionAt(tree, node, pos);
  char fen[100];
  pos.toFen(fen, sizeof(fen));
  EXPECT_STREQ(fen, "r1bqk2r/ppppbppp/2n2n2/4p3/2B1P3/3P1N2/PPP2PPP/RNBQK2R w KQkq - 1 5");
}

TEST(Pgn, VariationIsSiblingOfReplacedMove) {
  Tree tree;
  ASSERT_TRUE(parseText(SAMPLE, tree));
  // After 3. Bc4 the node has two children: Nf6 (main) and Bc5.
  uint16_t bc4 = 0;
  for (int i = 0; i < 5; ++i) bc4 = tree.nodes[bc4].firstChild;
  EXPECT_EQ(childCount(tree, bc4), 2);
  const uint16_t nf6 = childAt(tree, bc4, 0);
  const uint16_t bc5 = childAt(tree, bc4, 1);
  Position pos;
  positionAt(tree, bc4, pos);
  char san[12];
  moveToSan(pos, tree.nodes[nf6].move, san, sizeof(san));
  EXPECT_STREQ(san, "Nf6");
  EXPECT_EQ(tree.nodes[nf6].nag, 5);  // !?
  moveToSan(pos, tree.nodes[bc5].move, san, sizeof(san));
  EXPECT_STREQ(san, "Bc5");
  // Bc5 has one child (c3), and it is not on the main line.
  EXPECT_EQ(childCount(tree, bc5), 1);
  EXPECT_EQ(depthOf(tree, childAt(tree, bc5, 0)), 7);
  // d3 carries the $1 glyph.
  const uint16_t d3 = tree.nodes[nf6].firstChild;
  EXPECT_EQ(tree.nodes[d3].nag, 1);
}

TEST(Pgn, CommentsCleanAndMarks) {
  Tree tree;
  ASSERT_TRUE(parseText(SAMPLE, tree));
  // The root comment is the intro.
  const Node& root = tree.nodes[0];
  ASSERT_GT(root.commentLen, 0);
  std::string text;
  Highlight marks[8];
  int n = cleanComment(SAMPLE + root.commentAt, root.commentLen, text, marks, 8);
  EXPECT_EQ(text, "Intro text.");
  EXPECT_EQ(n, 0);
  // e4: the [%anno] command is dropped.
  const Node& e4 = tree.nodes[tree.nodes[0].firstChild];
  cleanComment(SAMPLE + e4.commentAt, e4.commentLen, text, marks, 8);
  EXPECT_EQ(text, "Kings pawn.");
  // Bc4 has two comment blocks: text plus arrows and a circle; the figurine becomes a letter.
  uint16_t bc4 = 0;
  for (int i = 0; i < 5; ++i) bc4 = tree.nodes[bc4].firstChild;
  const Node& b = tree.nodes[bc4];
  n = cleanComment(SAMPLE + b.commentAt, b.commentLen, text, marks, 8);
  EXPECT_EQ(text, "The Italian N");
  ASSERT_EQ(n, 3);
  EXPECT_EQ(marks[0].from, makeSquare(2, 3));
  EXPECT_EQ(marks[0].to, makeSquare(5, 6));
  EXPECT_EQ(marks[2].from, makeSquare(3, 3));
  EXPECT_EQ(marks[2].to, NoSquare);
}

TEST(Pgn, FenTagAndBadMoveSkipsVariation) {
  const char* text =
      "[FEN \"8/8/8/8/8/4k3/8/R3K3 w Q - 0 1\"]\n\n"
      "1. O-O-O (1. Rz9 Kd3 2. Kd1) 1... Kf3 2. Kb1 *\n";
  Tree tree;
  ASSERT_TRUE(parseText(text, tree));
  EXPECT_TRUE(tree.truncated);  // the bad variation was skipped
  // Main line survives: O-O-O Kf3 Kb1.
  uint16_t node = 0;
  int depth = 0;
  while (tree.nodes[node].firstChild) {
    node = tree.nodes[node].firstChild;
    ++depth;
  }
  EXPECT_EQ(depth, 3);
  Position pos;
  positionAt(tree, tree.nodes[0].firstChild, pos);
  EXPECT_EQ(pos.pieceAt(makeSquare(2, 0)), WhiteKing);
}

TEST(Pgn, SanDisambiguationAndMate) {
  Position pos;
  ASSERT_TRUE(pos.setFromFen("7k/8/8/8/8/R7/8/R6K w - - 0 1"));
  char san[12];
  // Two rooks on the a-file: the rank tells them apart.
  moveToSan(pos, Move{makeSquare(0, 0), makeSquare(0, 1), PieceType::None}, san, sizeof(san));
  EXPECT_STREQ(san, "R1a2");
  ASSERT_TRUE(pos.setFromFen("7k/8/8/8/8/8/R7/R6K w - - 0 1"));
  moveToSan(pos, Move{makeSquare(0, 1), makeSquare(7, 1), PieceType::None}, san, sizeof(san));
  EXPECT_STREQ(san, "Rh2+");
  ASSERT_TRUE(pos.setFromFen("7k/8/8/8/8/8/8/R2R3K w - - 0 1"));
  moveToSan(pos, Move{makeSquare(0, 0), makeSquare(1, 0), PieceType::None}, san, sizeof(san));
  EXPECT_STREQ(san, "Rab1");
  ASSERT_TRUE(pos.setFromFen("r5k1/4Pppp/8/8/8/8/8/6K1 w - - 0 1"));
  moveToSan(pos, Move{makeSquare(4, 6), makeSquare(4, 7), PieceType::Queen}, san, sizeof(san));
  EXPECT_STREQ(san, "e8=Q+");
  ASSERT_TRUE(pos.setFromFen("6k1/4Pppp/8/8/8/8/8/6K1 w - - 0 1"));
  moveToSan(pos, Move{makeSquare(4, 6), makeSquare(4, 7), PieceType::Queen}, san, sizeof(san));
  EXPECT_STREQ(san, "e8=Q#");
  ASSERT_TRUE(pos.setFromFen("6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1"));
  moveToSan(pos, Move{makeSquare(0, 0), makeSquare(0, 7), PieceType::None}, san, sizeof(san));
  EXPECT_STREQ(san, "Ra8#");
}
