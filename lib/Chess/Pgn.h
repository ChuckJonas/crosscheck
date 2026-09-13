#pragma once
// PGN movetext as a tree: main line, variations, comments, and glyphs. No
// Arduino includes, so it builds in the native test environment.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "Position.h"

namespace chess {
namespace pgn {

// Bytes of one chapter, delivered in order.
class Source {
 public:
  virtual ~Source() = default;
  // Fills buf with up to max bytes. Returns the count, 0 at the end.
  virtual int read(char* buf, size_t max) = 0;
};

class StringSource final : public Source {
 public:
  StringSource(const char* text, size_t len) : text(text), len(len) {}
  int read(char* buf, size_t max) override;

 private:
  const char* text;
  size_t len;
  size_t pos = 0;
};

struct Tags {
  char event[64] = {};
  char chapterName[64] = {};
  char white[40] = {};
  char black[40] = {};
  char result[8] = {};
  char fen[96] = {};
  char orientation[8] = {};
  bool standard = true;  // no Variant tag, or "Standard"
};

// One move of the tree. Node 0 is the root and holds no move.
struct Node {
  Move move;
  uint16_t parent = 0;
  uint16_t firstChild = 0;  // 0 means none
  uint16_t nextSibling = 0;
  // The node's comments: from the first '{' through the last '}' of the
  // comment blocks that follow the move, as byte offsets into the chapter.
  uint32_t commentAt = 0;
  uint16_t commentLen = 0;
  uint8_t nag = 0;  // first numeric annotation glyph: 1 !, 2 ?, 3 !!, 4 ??, 5 !?, 6 ?!
};

struct Tree {
  Tags tags;
  Position root;
  std::vector<Node> nodes;
  // The node cap was reached or a move could not be read; the tree holds
  // what was read before that.
  bool truncated = false;
};

constexpr uint16_t MAX_NODES_DEFAULT = 1500;

// Reads one chapter (tags, then movetext). Returns false when no tags and no
// moves were found.
bool parse(Source& src, Tree& out, uint16_t maxNodes = MAX_NODES_DEFAULT);

// The position after the moves from the root to `node`.
void positionAt(const Tree& tree, uint16_t node, Position& out);
// Plies from the root to `node`.
int depthOf(const Tree& tree, uint16_t node);
// Nodes from the first move down to `node`. Returns the count, at most max.
int pathTo(const Tree& tree, uint16_t node, uint16_t* out, int max);
int childCount(const Tree& tree, uint16_t node);
// The n-th child (0-based), or 0 when there is none.
uint16_t childAt(const Tree& tree, uint16_t node, int index);

// Standard algebraic notation of a legal move, with + or # when it applies.
void moveToSan(const Position& pos, const Move& m, char* out, size_t outSize);
// The glyph text for a NAG code, "" when there is none.
const char* nagText(uint8_t nag);

// A square mark from a comment: an arrow when `to` is set, else a circle.
struct Highlight {
  Square from = NoSquare;
  Square to = NoSquare;
};

// Comment text for display: braces, [%...] commands, and clock or eval
// annotations removed, figurines mapped to letters, whitespace collapsed.
// Arrows and circles from [%cal] and [%csl] go to marks. Returns their count.
int cleanComment(const char* raw, size_t len, std::string& out, Highlight* marks, int maxMarks);

}  // namespace pgn
}  // namespace chess
