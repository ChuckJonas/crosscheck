#include "Pgn.h"

#include <cstring>

namespace chess {
namespace pgn {

int StringSource::read(char* buf, size_t max) {
  if (pos >= len) return 0;
  size_t n = len - pos;
  if (n > max) n = max;
  memcpy(buf, text + pos, n);
  pos += n;
  return static_cast<int>(n);
}

namespace {

// Byte reader with one character of lookahead and the byte offset.
class Reader {
 public:
  explicit Reader(Source& src) : src(src) {}
  int peek() {
    if (!fill()) return -1;
    return static_cast<unsigned char>(buf[at]);
  }
  int get() {
    if (!fill()) return -1;
    ++offset;
    return static_cast<unsigned char>(buf[at++]);
  }
  size_t position() const { return offset; }

 private:
  bool fill() {
    if (at < count) return true;
    const int n = src.read(buf, sizeof(buf));
    if (n <= 0) return false;
    count = static_cast<size_t>(n);
    at = 0;
    return true;
  }
  Source& src;
  char buf[256];
  size_t at = 0;
  size_t count = 0;
  size_t offset = 0;
};

bool isSpace(int c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; }

void copyTag(char* dst, size_t size, const std::string& value) {
  size_t n = value.size();
  if (n >= size) n = size - 1;
  memcpy(dst, value.data(), n);
  dst[n] = '\0';
}

// Reads the tag pairs at the start of the chapter. Stops at the first line
// that does not start with '['.
void readTags(Reader& in, Tags& tags) {
  for (;;) {
    while (isSpace(in.peek())) in.get();
    if (in.peek() != '[') return;
    in.get();
    std::string key;
    std::string value;
    while (in.peek() != -1 && !isSpace(in.peek()) && in.peek() != ']') key.push_back(static_cast<char>(in.get()));
    while (isSpace(in.peek())) in.get();
    if (in.peek() == '"') {
      in.get();
      for (;;) {
        const int c = in.get();
        if (c == -1 || c == '"') break;
        if (c == '\\') {
          const int e = in.get();
          if (e == -1) break;
          value.push_back(static_cast<char>(e));
        } else {
          value.push_back(static_cast<char>(c));
        }
      }
    }
    while (in.peek() != -1 && in.peek() != ']') in.get();
    in.get();
    if (key == "Event")
      copyTag(tags.event, sizeof(tags.event), value);
    else if (key == "ChapterName")
      copyTag(tags.chapterName, sizeof(tags.chapterName), value);
    else if (key == "White")
      copyTag(tags.white, sizeof(tags.white), value);
    else if (key == "Black")
      copyTag(tags.black, sizeof(tags.black), value);
    else if (key == "Result")
      copyTag(tags.result, sizeof(tags.result), value);
    else if (key == "FEN")
      copyTag(tags.fen, sizeof(tags.fen), value);
    else if (key == "Orientation")
      copyTag(tags.orientation, sizeof(tags.orientation), value);
    else if (key == "Variant")
      tags.standard = value.empty() || value == "Standard" || value == "From Position";
  }
}

struct Context {
  uint16_t node;       // the last move made at this level, or the level's start node
  Position pos;        // position after `node`
  Position prevPos;    // position before `node`'s move
  bool moved = false;  // a move was made at this level
};

uint16_t addChild(Tree& tree, uint16_t parent, const Move& move) {
  Node n;
  n.move = move;
  n.parent = parent;
  const uint16_t index = static_cast<uint16_t>(tree.nodes.size());
  tree.nodes.push_back(n);
  uint16_t* link = &tree.nodes[parent].firstChild;
  while (*link) link = &tree.nodes[*link].nextSibling;
  *link = index;
  return index;
}

}  // namespace

bool parse(Source& src, Tree& out, uint16_t maxNodes) {
  out = Tree();
  out.nodes.reserve(maxNodes < 256 ? maxNodes : 256);
  out.nodes.push_back(Node());
  Reader in(src);
  readTags(in, out.tags);
  if (out.tags.fen[0]) {
    if (!out.root.setFromFen(out.tags.fen)) return false;
  } else {
    out.root.setStartPosition();
  }
  if (!out.tags.standard) return out.tags.event[0] != '\0';

  std::vector<Context> stack;
  stack.reserve(8);
  Context ctx;
  ctx.node = 0;
  ctx.pos = out.root;
  ctx.prevPos = out.root;
  // After a move that could not be read, the rest of its variation is
  // skipped: skipDepth counts the parentheses still to close.
  int skipDepth = -1;
  std::string token;
  for (;;) {
    int c = in.peek();
    if (c == -1) break;
    if (isSpace(c)) {
      in.get();
      continue;
    }
    if (c == '{') {
      const size_t start = in.position();
      in.get();
      while (in.peek() != -1 && in.peek() != '}') in.get();
      in.get();
      const size_t end = in.position();
      if (skipDepth < 0) {
        Node& n = out.nodes[ctx.node];
        if (n.commentLen == 0) {
          n.commentAt = static_cast<uint32_t>(start);
          n.commentLen = static_cast<uint16_t>(end - start > 65535 ? 65535 : end - start);
        } else if (end - n.commentAt <= 65535) {
          n.commentLen = static_cast<uint16_t>(end - n.commentAt);
        }
      }
      continue;
    }
    if (c == ';') {
      while (in.peek() != -1 && in.peek() != '\n') in.get();
      continue;
    }
    if (c == '(') {
      in.get();
      if (skipDepth >= 0) {
        ++skipDepth;
        continue;
      }
      // A variation replaces the last move of this level: it starts from the
      // position before that move, as a sibling of it.
      Context inner;
      inner.node = ctx.moved ? out.nodes[ctx.node].parent : ctx.node;
      inner.pos = ctx.moved ? ctx.prevPos : ctx.pos;
      inner.prevPos = inner.pos;
      stack.push_back(ctx);
      ctx = inner;
      continue;
    }
    if (c == ')') {
      in.get();
      if (skipDepth > 0) {
        --skipDepth;
        continue;
      }
      skipDepth = -1;
      if (stack.empty()) break;  // an unbalanced ')' ends the movetext
      ctx = stack.back();
      stack.pop_back();
      continue;
    }
    // A token: NAG, move number, result, or a move.
    token.clear();
    while (in.peek() != -1 && !isSpace(in.peek()) && in.peek() != '(' && in.peek() != ')' && in.peek() != '{') {
      if (token.size() < 32)
        token.push_back(static_cast<char>(in.get()));
      else
        in.get();
    }
    if (token.empty()) {
      in.get();
      continue;
    }
    if (skipDepth >= 0) continue;
    if (token[0] == '$') {
      Node& n = out.nodes[ctx.node];
      if (n.nag == 0) n.nag = static_cast<uint8_t>(atoi(token.c_str() + 1));
      continue;
    }
    if (token[0] == '*') continue;
    const bool zeroCastle = token == "0-0" || token == "0-0-0";
    if (!zeroCastle && token[0] >= '0' && token[0] <= '9') continue;  // move number or result
    if (token == "e.p.") continue;
    if (token == "!" || token == "?" || token == "!!" || token == "??" || token == "!?" || token == "?!") {
      static const char* const GLYPHS[6] = {"!", "?", "!!", "??", "!?", "?!"};
      Node& n = out.nodes[ctx.node];
      for (int i = 0; i < 6 && n.nag == 0; ++i) {
        if (token == GLYPHS[i]) n.nag = static_cast<uint8_t>(i + 1);
      }
      continue;
    }
    Move m;
    if (!moveFromSan(ctx.pos, token.c_str(), m)) {
      out.truncated = true;
      if (stack.empty()) break;
      skipDepth = 0;
      continue;
    }
    if (out.nodes.size() >= maxNodes) {
      out.truncated = true;
      break;
    }
    // A glyph attached to the move text ("Nf3!?") belongs to the new node.
    uint8_t nag = 0;
    const size_t bang = token.find_first_of("!?");
    if (bang != std::string::npos) {
      const std::string glyph = token.substr(bang);
      static const char* const GLYPHS[6] = {"!", "?", "!!", "??", "!?", "?!"};
      for (int i = 0; i < 6; ++i) {
        if (glyph == GLYPHS[i]) nag = static_cast<uint8_t>(i + 1);
      }
    }
    const uint16_t index = addChild(out, ctx.node, m);
    out.nodes[index].nag = nag;
    ctx.prevPos = ctx.pos;
    ctx.pos.makeMove(m);
    ctx.node = index;
    ctx.moved = true;
  }
  return out.nodes.size() > 1 || out.tags.event[0] != '\0' || out.tags.fen[0] != '\0';
}

int pathTo(const Tree& tree, uint16_t node, uint16_t* out, int max) {
  int depth = 0;
  for (uint16_t n = node; n != 0 && n < tree.nodes.size(); n = tree.nodes[n].parent) ++depth;
  const int count = depth < max ? depth : max;
  // Fill from the end so the first moves come first even when capped.
  int i = depth;
  for (uint16_t n = node; n != 0 && n < tree.nodes.size(); n = tree.nodes[n].parent) {
    --i;
    if (i < count) out[i] = n;
  }
  return count;
}

int depthOf(const Tree& tree, uint16_t node) {
  int depth = 0;
  for (uint16_t n = node; n != 0 && n < tree.nodes.size(); n = tree.nodes[n].parent) ++depth;
  return depth;
}

void positionAt(const Tree& tree, uint16_t node, Position& out) {
  out = tree.root;
  const int depth = depthOf(tree, node);
  if (depth == 0) return;
  std::vector<uint16_t> path(static_cast<size_t>(depth));
  pathTo(tree, node, path.data(), depth);
  for (uint16_t n : path) out.makeMove(tree.nodes[n].move);
}

int childCount(const Tree& tree, uint16_t node) {
  int count = 0;
  for (uint16_t c = tree.nodes[node].firstChild; c; c = tree.nodes[c].nextSibling) ++count;
  return count;
}

uint16_t childAt(const Tree& tree, uint16_t node, int index) {
  uint16_t c = tree.nodes[node].firstChild;
  while (c && index > 0) {
    c = tree.nodes[c].nextSibling;
    --index;
  }
  return c;
}

const char* nagText(uint8_t nag) {
  static const char* const GLYPHS[7] = {"", "!", "?", "!!", "??", "!?", "?!"};
  return nag < 7 ? GLYPHS[nag] : "";
}

void moveToSan(const Position& pos, const Move& m, char* out, size_t outSize) {
  if (outSize == 0) return;
  out[0] = '\0';
  const Piece piece = pos.pieceAt(m.from);
  if (piece == NoPiece) return;
  const PieceType type = typeOf(piece);
  char text[16];
  int n = 0;
  if (type == PieceType::King && fileOf(m.from) == 4 && (fileOf(m.to) == 6 || fileOf(m.to) == 2) &&
      rankOf(m.from) == rankOf(m.to)) {
    n = snprintf(text, sizeof(text), fileOf(m.to) == 6 ? "O-O" : "O-O-O");
  } else {
    const bool capture = pos.pieceAt(m.to) != NoPiece || (type == PieceType::Pawn && fileOf(m.from) != fileOf(m.to));
    if (type == PieceType::Pawn) {
      if (capture) text[n++] = static_cast<char>('a' + fileOf(m.from));
    } else {
      static const char LETTERS[7] = {' ', 'P', 'N', 'B', 'R', 'Q', 'K'};
      text[n++] = LETTERS[static_cast<int>(type)];
      // Disambiguation: another piece of the same type that can reach the square.
      Move moves[MAX_MOVES];
      const int count = pos.generateLegalMoves(moves, MAX_MOVES);
      bool sameFile = false;
      bool sameRank = false;
      bool other = false;
      for (int i = 0; i < count; ++i) {
        const Move& o = moves[i];
        if (o.to != m.to || o.from == m.from || typeOf(pos.pieceAt(o.from)) != type) continue;
        other = true;
        if (fileOf(o.from) == fileOf(m.from)) sameFile = true;
        if (rankOf(o.from) == rankOf(m.from)) sameRank = true;
      }
      if (other) {
        if (!sameFile) {
          text[n++] = static_cast<char>('a' + fileOf(m.from));
        } else if (!sameRank) {
          text[n++] = static_cast<char>('1' + rankOf(m.from));
        } else {
          text[n++] = static_cast<char>('a' + fileOf(m.from));
          text[n++] = static_cast<char>('1' + rankOf(m.from));
        }
      }
    }
    if (capture) text[n++] = 'x';
    text[n++] = static_cast<char>('a' + fileOf(m.to));
    text[n++] = static_cast<char>('1' + rankOf(m.to));
    if (m.promotion != PieceType::None) {
      static const char PROMO[7] = {' ', ' ', 'N', 'B', 'R', 'Q', ' '};
      text[n++] = '=';
      text[n++] = PROMO[static_cast<int>(m.promotion)];
    }
  }
  Position after = pos;
  after.makeMove(m);
  if (after.inCheck()) {
    Move replies[MAX_MOVES];
    text[n++] = after.generateLegalMoves(replies, MAX_MOVES) == 0 ? '#' : '+';
  }
  text[n] = '\0';
  snprintf(out, outSize, "%s", text);
}

namespace {
Square squareFrom(const char* s) {
  if (s[0] < 'a' || s[0] > 'h' || s[1] < '1' || s[1] > '8') return NoSquare;
  return makeSquare(s[0] - 'a', s[1] - '1');
}

// [%cal Gc4a2,Rd3f5] and [%csl Gd4]: the color letter is ignored.
void readMarks(const char* body, size_t len, bool arrows, Highlight* marks, int maxMarks, int& count) {
  size_t i = 0;
  while (i < len && count < maxMarks) {
    while (i < len && (body[i] == ' ' || body[i] == ',')) ++i;
    if (i >= len) break;
    size_t j = i;
    while (j < len && body[j] != ',' && body[j] != ' ') ++j;
    const size_t n = j - i;
    if ((arrows && n == 5) || (!arrows && n == 3)) {
      Highlight h;
      h.from = squareFrom(body + i + 1);
      h.to = arrows ? squareFrom(body + i + 3) : NoSquare;
      if (h.from != NoSquare && (!arrows || h.to != NoSquare)) marks[count++] = h;
    }
    i = j;
  }
}
}  // namespace

int cleanComment(const char* raw, size_t len, std::string& out, Highlight* marks, int maxMarks) {
  out.clear();
  int count = 0;
  bool inside = false;
  bool space = true;  // the last emitted character was a space (or nothing yet)
  size_t i = 0;
  while (i < len) {
    const unsigned char c = static_cast<unsigned char>(raw[i]);
    if (!inside) {
      if (c == '{') inside = true;
      ++i;
      continue;
    }
    if (c == '}') {
      inside = false;
      ++i;
      continue;
    }
    if (c == '[' && i + 1 < len && raw[i + 1] == '%') {
      // A command: [%name body]. Arrows and circles are kept, the rest dropped.
      size_t j = i + 2;
      size_t nameEnd = j;
      while (nameEnd < len && raw[nameEnd] != ' ' && raw[nameEnd] != ']') ++nameEnd;
      size_t end = nameEnd;
      while (end < len && raw[end] != ']') ++end;
      const size_t nameLen = nameEnd - j;
      if (marks && nameLen == 3 && (strncmp(raw + j, "cal", 3) == 0 || strncmp(raw + j, "csl", 3) == 0)) {
        const size_t bodyStart = nameEnd < end ? nameEnd + 1 : end;
        readMarks(raw + bodyStart, end - bodyStart, raw[j + 1] == 'a', marks, maxMarks, count);
      }
      i = end < len ? end + 1 : len;
      continue;
    }
    if (c == 0xE2 && i + 2 < len && static_cast<unsigned char>(raw[i + 1]) == 0x99) {
      // Figurines U+2654..U+265F: white and black K Q R B N P.
      const unsigned char k = static_cast<unsigned char>(raw[i + 2]);
      if (k >= 0x94 && k <= 0x9F) {
        static const char LETTERS[6] = {'K', 'Q', 'R', 'B', 'N', 'P'};
        out.push_back(LETTERS[(k - 0x94) % 6]);
        space = false;
        i += 3;
        continue;
      }
    }
    if (isSpace(c)) {
      if (!space) {
        out.push_back(' ');
        space = true;
      }
      ++i;
      continue;
    }
    out.push_back(static_cast<char>(c));
    space = false;
    ++i;
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return count;
}

}  // namespace pgn
}  // namespace chess
