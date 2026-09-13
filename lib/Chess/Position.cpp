#include "Position.h"

#include <cstdio>
#include <cstring>

namespace chess {

namespace {
constexpr const char* START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";

struct Step {
  int8_t df;
  int8_t dr;
};
constexpr Step KNIGHT_STEPS[8] = {{1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
constexpr Step KING_STEPS[8] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1}, {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
constexpr Step BISHOP_STEPS[4] = {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}};
constexpr Step ROOK_STEPS[4] = {{1, 0}, {0, 1}, {-1, 0}, {0, -1}};

// Returns the square reached from s by one step, or NoSquare when off the board.
inline Square stepFrom(Square s, Step st) {
  const int f = fileOf(s) + st.df;
  const int r = rankOf(s) + st.dr;
  if (f < 0 || f > 7 || r < 0 || r > 7) return NoSquare;
  return makeSquare(f, r);
}

inline int pawnDirection(Color c) { return c == Color::White ? 1 : -1; }

constexpr Square A1 = 0, C1 = 2, D1 = 3, E1 = 4, F1 = 5, G1 = 6, H1 = 7;
constexpr Square A8 = 56, C8 = 58, D8 = 59, E8 = 60, F8 = 61, G8 = 62, H8 = 63;
}  // namespace

char pieceToChar(Piece p) {
  static constexpr char WHITE[] = " PNBRQK";
  if (p == NoPiece) return ' ';
  const char c = WHITE[static_cast<uint8_t>(typeOf(p))];
  return colorOf(p) == Color::White ? c : static_cast<char>(c + ('a' - 'A'));
}

Piece pieceFromChar(char c) {
  const bool black = c >= 'a' && c <= 'z';
  const char u = black ? static_cast<char>(c - ('a' - 'A')) : c;
  PieceType t = PieceType::None;
  switch (u) {
    case 'P':
      t = PieceType::Pawn;
      break;
    case 'N':
      t = PieceType::Knight;
      break;
    case 'B':
      t = PieceType::Bishop;
      break;
    case 'R':
      t = PieceType::Rook;
      break;
    case 'Q':
      t = PieceType::Queen;
      break;
    case 'K':
      t = PieceType::King;
      break;
    default:
      return NoPiece;
  }
  return makePiece(black ? Color::Black : Color::White, t);
}

void squareToString(Square s, char* buf) {
  if (s < 0 || s > 63) {
    buf[0] = '-';
    buf[1] = '\0';
    return;
  }
  buf[0] = static_cast<char>('a' + fileOf(s));
  buf[1] = static_cast<char>('1' + rankOf(s));
  buf[2] = '\0';
}

Square squareFromString(const char* text) {
  if (!text || text[0] < 'a' || text[0] > 'h' || text[1] < '1' || text[1] > '8') return NoSquare;
  return makeSquare(text[0] - 'a', text[1] - '1');
}

void moveToUci(const Move& m, char* buf) {
  squareToString(m.from, buf);
  squareToString(m.to, buf + 2);
  int n = 4;
  if (m.promotion != PieceType::None) {
    static constexpr char PROMO[] = "  nbrq ";
    buf[n++] = PROMO[static_cast<uint8_t>(m.promotion)];
  }
  buf[n] = '\0';
}

bool moveFromUci(const char* text, Move& out) {
  if (!text || strlen(text) < 4) return false;
  const Square from = squareFromString(text);
  const Square to = squareFromString(text + 2);
  if (from == NoSquare || to == NoSquare) return false;
  PieceType promo = PieceType::None;
  switch (text[4]) {
    case '\0':
      break;
    case 'n':
      promo = PieceType::Knight;
      break;
    case 'b':
      promo = PieceType::Bishop;
      break;
    case 'r':
      promo = PieceType::Rook;
      break;
    case 'q':
      promo = PieceType::Queen;
      break;
    default:
      return false;
  }
  out.from = from;
  out.to = to;
  out.promotion = promo;
  return true;
}

Position::Position() { setStartPosition(); }

void Position::setStartPosition() { setFromFen(START_FEN); }

bool Position::setFromFen(const char* fen) {
  if (!fen) return false;
  Piece newBoard[64];
  memset(newBoard, NoPiece, sizeof(newBoard));

  // Piece placement, from rank 8 down to rank 1.
  const char* p = fen;
  int rank = 7;
  int file = 0;
  for (; *p && *p != ' '; ++p) {
    if (*p == '/') {
      if (file != 8 || rank == 0) return false;
      --rank;
      file = 0;
    } else if (*p >= '1' && *p <= '8') {
      file += *p - '0';
      if (file > 8) return false;
    } else {
      const Piece piece = pieceFromChar(*p);
      if (piece == NoPiece || file > 7) return false;
      newBoard[makeSquare(file, rank)] = piece;
      ++file;
    }
  }
  if (rank != 0 || file != 8) return false;

  // Side to move.
  while (*p == ' ') ++p;
  Color newSide;
  if (*p == 'w') {
    newSide = Color::White;
  } else if (*p == 'b') {
    newSide = Color::Black;
  } else {
    return false;
  }
  ++p;

  // Castling rights.
  while (*p == ' ') ++p;
  uint8_t newCastling = CastleNone;
  if (*p == '-') {
    ++p;
  } else {
    for (; *p && *p != ' '; ++p) {
      switch (*p) {
        case 'K':
          newCastling |= WhiteKingSide;
          break;
        case 'Q':
          newCastling |= WhiteQueenSide;
          break;
        case 'k':
          newCastling |= BlackKingSide;
          break;
        case 'q':
          newCastling |= BlackQueenSide;
          break;
        default:
          return false;
      }
    }
  }

  // En passant square.
  while (*p == ' ') ++p;
  Square newEp = NoSquare;
  if (*p == '-') {
    ++p;
  } else if (*p) {
    newEp = squareFromString(p);
    if (newEp == NoSquare) return false;
    p += 2;
  }

  // Move counters are optional.
  int newHalfmove = 0;
  int newFullmove = 1;
  while (*p == ' ') ++p;
  if (*p) {
    newHalfmove = 0;
    while (*p >= '0' && *p <= '9') newHalfmove = newHalfmove * 10 + (*p++ - '0');
    while (*p == ' ') ++p;
    if (*p) {
      newFullmove = 0;
      while (*p >= '0' && *p <= '9') newFullmove = newFullmove * 10 + (*p++ - '0');
      if (newFullmove < 1) newFullmove = 1;
    }
  }

  memcpy(board, newBoard, sizeof(board));
  side = newSide;
  castling = newCastling;
  epSquare = newEp;
  halfmove = newHalfmove;
  fullmove = newFullmove;
  return true;
}

void Position::toFen(char* buf, int bufSize) const {
  int n = 0;
  auto put = [&](char c) {
    if (n < bufSize - 1) buf[n++] = c;
  };
  for (int rank = 7; rank >= 0; --rank) {
    int empty = 0;
    for (int file = 0; file < 8; ++file) {
      const Piece piece = board[makeSquare(file, rank)];
      if (piece == NoPiece) {
        ++empty;
        continue;
      }
      if (empty) put(static_cast<char>('0' + empty));
      empty = 0;
      put(pieceToChar(piece));
    }
    if (empty) put(static_cast<char>('0' + empty));
    if (rank) put('/');
  }
  put(' ');
  put(side == Color::White ? 'w' : 'b');
  put(' ');
  if (castling == CastleNone) {
    put('-');
  } else {
    if (castling & WhiteKingSide) put('K');
    if (castling & WhiteQueenSide) put('Q');
    if (castling & BlackKingSide) put('k');
    if (castling & BlackQueenSide) put('q');
  }
  put(' ');
  if (epSquare == NoSquare) {
    put('-');
  } else {
    char sq[3];
    squareToString(epSquare, sq);
    put(sq[0]);
    put(sq[1]);
  }
  put(' ');
  char num[12];
  snprintf(num, sizeof(num), "%d %d", halfmove, fullmove);
  for (const char* c = num; *c; ++c) put(*c);
  buf[n] = '\0';
}

Square Position::kingSquare(Color c) const {
  const Piece king = makePiece(c, PieceType::King);
  for (Square s = 0; s < 64; ++s) {
    if (board[s] == king) return s;
  }
  return NoSquare;
}

bool Position::isSquareAttacked(Square s, Color by) const {
  if (s == NoSquare) return false;
  // Pawns attack diagonally forward, so look one rank back from s.
  const int dir = pawnDirection(by);
  for (int df = -1; df <= 1; df += 2) {
    const Square from = stepFrom(s, Step{static_cast<int8_t>(df), static_cast<int8_t>(-dir)});
    if (from != NoSquare && board[from] == makePiece(by, PieceType::Pawn)) return true;
  }
  for (const Step st : KNIGHT_STEPS) {
    const Square from = stepFrom(s, st);
    if (from != NoSquare && board[from] == makePiece(by, PieceType::Knight)) return true;
  }
  for (const Step st : KING_STEPS) {
    const Square from = stepFrom(s, st);
    if (from != NoSquare && board[from] == makePiece(by, PieceType::King)) return true;
  }
  const Piece queen = makePiece(by, PieceType::Queen);
  const Piece bishop = makePiece(by, PieceType::Bishop);
  const Piece rook = makePiece(by, PieceType::Rook);
  for (const Step st : BISHOP_STEPS) {
    for (Square sq = stepFrom(s, st); sq != NoSquare; sq = stepFrom(sq, st)) {
      const Piece p = board[sq];
      if (p == NoPiece) continue;
      if (p == queen || p == bishop) return true;
      break;
    }
  }
  for (const Step st : ROOK_STEPS) {
    for (Square sq = stepFrom(s, st); sq != NoSquare; sq = stepFrom(sq, st)) {
      const Piece p = board[sq];
      if (p == NoPiece) continue;
      if (p == queen || p == rook) return true;
      break;
    }
  }
  return false;
}

int Position::generatePseudoLegal(Move* out, int maxCount) const {
  int n = 0;
  auto add = [&](Square from, Square to, PieceType promo = PieceType::None) {
    if (n < maxCount) out[n++] = Move{from, to, promo};
  };
  auto addPawnMove = [&](Square from, Square to) {
    const int toRank = rankOf(to);
    if (toRank == 0 || toRank == 7) {
      add(from, to, PieceType::Queen);
      add(from, to, PieceType::Rook);
      add(from, to, PieceType::Bishop);
      add(from, to, PieceType::Knight);
    } else {
      add(from, to);
    }
  };

  const Color us = side;
  const Color them = opposite(us);
  const int dir = pawnDirection(us);
  const int startRank = us == Color::White ? 1 : 6;

  for (Square from = 0; from < 64; ++from) {
    const Piece piece = board[from];
    if (piece == NoPiece || colorOf(piece) != us) continue;
    switch (typeOf(piece)) {
      case PieceType::Pawn: {
        const Square one = stepFrom(from, Step{0, static_cast<int8_t>(dir)});
        if (one != NoSquare && board[one] == NoPiece) {
          addPawnMove(from, one);
          if (rankOf(from) == startRank) {
            const Square two = stepFrom(one, Step{0, static_cast<int8_t>(dir)});
            if (two != NoSquare && board[two] == NoPiece) add(from, two);
          }
        }
        for (int df = -1; df <= 1; df += 2) {
          const Square cap = stepFrom(from, Step{static_cast<int8_t>(df), static_cast<int8_t>(dir)});
          if (cap == NoSquare) continue;
          const Piece target = board[cap];
          if ((target != NoPiece && colorOf(target) == them) || cap == epSquare) addPawnMove(from, cap);
        }
        break;
      }
      case PieceType::Knight:
        for (const Step st : KNIGHT_STEPS) {
          const Square to = stepFrom(from, st);
          if (to != NoSquare && (board[to] == NoPiece || colorOf(board[to]) == them)) add(from, to);
        }
        break;
      case PieceType::King:
        for (const Step st : KING_STEPS) {
          const Square to = stepFrom(from, st);
          if (to != NoSquare && (board[to] == NoPiece || colorOf(board[to]) == them)) add(from, to);
        }
        break;
      case PieceType::Bishop:
      case PieceType::Rook:
      case PieceType::Queen: {
        const PieceType t = typeOf(piece);
        const bool diagonal = t != PieceType::Rook;
        const bool straight = t != PieceType::Bishop;
        for (int i = 0; i < 8; ++i) {
          const bool isDiagonal = i < 4;
          if (isDiagonal && !diagonal) continue;
          if (!isDiagonal && !straight) continue;
          const Step st = isDiagonal ? BISHOP_STEPS[i] : ROOK_STEPS[i - 4];
          for (Square to = stepFrom(from, st); to != NoSquare; to = stepFrom(to, st)) {
            const Piece target = board[to];
            if (target == NoPiece) {
              add(from, to);
              continue;
            }
            if (colorOf(target) == them) add(from, to);
            break;
          }
        }
        break;
      }
      default:
        break;
    }
  }

  // Castling. The king must not be in check and must not pass through an
  // attacked square. The destination square is checked by the legality filter.
  const Square kingFrom = us == Color::White ? E1 : E8;
  if (board[kingFrom] == makePiece(us, PieceType::King) && !isSquareAttacked(kingFrom, them)) {
    const uint8_t kingSide = us == Color::White ? WhiteKingSide : BlackKingSide;
    const uint8_t queenSide = us == Color::White ? WhiteQueenSide : BlackQueenSide;
    const Piece rook = makePiece(us, PieceType::Rook);
    if ((castling & kingSide) && board[kingFrom + 1] == NoPiece && board[kingFrom + 2] == NoPiece &&
        board[kingFrom + 3] == rook && !isSquareAttacked(kingFrom + 1, them)) {
      add(kingFrom, kingFrom + 2);
    }
    if ((castling & queenSide) && board[kingFrom - 1] == NoPiece && board[kingFrom - 2] == NoPiece &&
        board[kingFrom - 3] == NoPiece && board[kingFrom - 4] == rook && !isSquareAttacked(kingFrom - 1, them)) {
      add(kingFrom, kingFrom - 2);
    }
  }
  return n;
}

bool Position::leavesKingInCheck(const Move& m) const {
  Position next = *this;
  next.makeMove(m);
  return next.isSquareAttacked(next.kingSquare(side), opposite(side));
}

int Position::generateLegalMoves(Move* out, int maxCount) const {
  Move pseudo[MAX_MOVES];
  const int count = generatePseudoLegal(pseudo, MAX_MOVES);
  int n = 0;
  for (int i = 0; i < count && n < maxCount; ++i) {
    if (!leavesKingInCheck(pseudo[i])) out[n++] = pseudo[i];
  }
  return n;
}

bool Position::isLegal(const Move& m) const {
  Move moves[MAX_MOVES];
  const int count = generateLegalMoves(moves, MAX_MOVES);
  for (int i = 0; i < count; ++i) {
    if (moves[i] == m) return true;
  }
  return false;
}

void Position::makeMove(const Move& m) {
  const Piece piece = board[m.from];
  const Piece captured = board[m.to];
  const Color us = colorOf(piece);
  const PieceType type = typeOf(piece);

  // En passant capture removes the pawn beside the destination.
  if (type == PieceType::Pawn && m.to == epSquare && captured == NoPiece) {
    board[makeSquare(fileOf(m.to), rankOf(m.from))] = NoPiece;
  }

  // Castling also moves the rook.
  if (type == PieceType::King && fileOf(m.to) - fileOf(m.from) == 2) {
    board[m.to - 1] = board[m.to + 1];
    board[m.to + 1] = NoPiece;
  } else if (type == PieceType::King && fileOf(m.from) - fileOf(m.to) == 2) {
    board[m.to + 1] = board[m.to - 2];
    board[m.to - 2] = NoPiece;
  }

  board[m.to] = m.promotion == PieceType::None ? piece : makePiece(us, m.promotion);
  board[m.from] = NoPiece;

  // Castling rights end when a king or rook moves, or a rook is captured.
  if (type == PieceType::King) {
    castling &= us == Color::White ? ~(WhiteKingSide | WhiteQueenSide) : ~(BlackKingSide | BlackQueenSide);
  }
  auto dropRookRight = [&](Square s) {
    if (s == H1) castling &= ~WhiteKingSide;
    if (s == A1) castling &= ~WhiteQueenSide;
    if (s == H8) castling &= ~BlackKingSide;
    if (s == A8) castling &= ~BlackQueenSide;
  };
  dropRookRight(m.from);
  dropRookRight(m.to);

  epSquare = NoSquare;
  if (type == PieceType::Pawn && (rankOf(m.to) - rankOf(m.from) == 2 || rankOf(m.from) - rankOf(m.to) == 2)) {
    epSquare = makeSquare(fileOf(m.from), (rankOf(m.from) + rankOf(m.to)) / 2);
  }

  if (type == PieceType::Pawn || captured != NoPiece) {
    halfmove = 0;
  } else {
    ++halfmove;
  }
  if (us == Color::Black) ++fullmove;
  side = opposite(us);
}

bool Position::makeUciMove(const char* uci) {
  Move m;
  if (!moveFromUci(uci, m)) return false;
  if (!isLegal(m)) return false;
  makeMove(m);
  return true;
}

GameStatus Position::status() const {
  Move moves[MAX_MOVES];
  if (generateLegalMoves(moves, MAX_MOVES) > 0) return GameStatus::Ongoing;
  return inCheck() ? GameStatus::Checkmate : GameStatus::Stalemate;
}

uint64_t Position::perft(int depth) const {
  Move moves[MAX_MOVES];
  const int count = generateLegalMoves(moves, MAX_MOVES);
  if (depth <= 1) return static_cast<uint64_t>(count);
  uint64_t nodes = 0;
  for (int i = 0; i < count; ++i) {
    Position next = *this;
    next.makeMove(moves[i]);
    nodes += next.perft(depth - 1);
  }
  return nodes;
}

}  // namespace chess

namespace chess {

namespace {
PieceType pieceLetter(char c) {
  switch (c) {
    case 'N':
      return PieceType::Knight;
    case 'B':
      return PieceType::Bishop;
    case 'R':
      return PieceType::Rook;
    case 'Q':
      return PieceType::Queen;
    case 'K':
      return PieceType::King;
    default:
      return PieceType::None;
  }
}
}  // namespace

bool moveFromSan(const Position& pos, const char* san, Move& out) {
  if (!san || !san[0]) return false;
  // Copy without check, mate, and annotation marks.
  char text[16];
  int n = 0;
  for (const char* c = san; *c && n < 15; ++c) {
    if (*c == '+' || *c == '#' || *c == '!' || *c == '?') continue;
    text[n++] = *c;
  }
  text[n] = '\0';
  if (n == 0) return false;

  Move moves[MAX_MOVES];
  const int count = pos.generateLegalMoves(moves, MAX_MOVES);
  const Color us = pos.sideToMove();
  const int homeRank = us == Color::White ? 0 : 7;

  // Castling: the king moves two files.
  if (strcmp(text, "O-O") == 0 || strcmp(text, "0-0") == 0 || strcmp(text, "O-O-O") == 0 ||
      strcmp(text, "0-0-0") == 0) {
    const int toFile = strlen(text) == 3 ? 6 : 2;
    for (int i = 0; i < count; ++i) {
      const Move& m = moves[i];
      if (typeOf(pos.pieceAt(m.from)) == PieceType::King && m.from == makeSquare(4, homeRank) &&
          m.to == makeSquare(toFile, homeRank)) {
        out = m;
        return true;
      }
    }
    return false;
  }

  // Promotion suffix: "=Q" or a bare "Q".
  PieceType promotion = PieceType::None;
  if (n >= 2 && text[n - 2] == '=') {
    promotion = pieceLetter(text[n - 1]);
    if (promotion == PieceType::None) return false;
    n -= 2;
    text[n] = '\0';
  } else if (n >= 3 && text[0] >= 'a' && text[0] <= 'h' && pieceLetter(text[n - 1]) != PieceType::None &&
             text[n - 2] >= '1' && text[n - 2] <= '8') {
    promotion = pieceLetter(text[n - 1]);
    --n;
    text[n] = '\0';
  }
  if (n < 2) return false;

  // Destination is always the last two characters.
  const Square to = squareFromString(text + n - 2);
  if (to == NoSquare) return false;
  n -= 2;
  text[n] = '\0';

  // Optional capture mark, then the leading piece letter and disambiguation.
  if (n > 0 && text[n - 1] == 'x') text[--n] = '\0';
  PieceType type = PieceType::Pawn;
  int p = 0;
  if (n > 0 && pieceLetter(text[0]) != PieceType::None) {
    type = pieceLetter(text[0]);
    p = 1;
  }
  int fromFile = -1;
  int fromRank = -1;
  for (; p < n; ++p) {
    if (text[p] >= 'a' && text[p] <= 'h') {
      fromFile = text[p] - 'a';
    } else if (text[p] >= '1' && text[p] <= '8') {
      fromRank = text[p] - '1';
    } else {
      return false;
    }
  }

  int matches = 0;
  for (int i = 0; i < count; ++i) {
    const Move& m = moves[i];
    if (m.to != to) continue;
    if (typeOf(pos.pieceAt(m.from)) != type) continue;
    if (fromFile >= 0 && fileOf(m.from) != fromFile) continue;
    if (fromRank >= 0 && rankOf(m.from) != fromRank) continue;
    if (m.promotion != promotion) {
      // A pawn reaching the last rank without a suffix means a queen.
      if (!(promotion == PieceType::None && m.promotion == PieceType::Queen)) continue;
    }
    out = m;
    ++matches;
  }
  return matches == 1;
}

int replaySan(const char* text, Position& pos, std::string* uciOut) {
  if (!text) return 0;
  int count = 0;
  const char* c = text;
  while (*c) {
    while (*c == ' ' || *c == '\n' || *c == '\t') ++c;
    if (!*c) break;
    char token[16];
    int n = 0;
    while (*c && *c != ' ' && *c != '\n' && *c != '\t') {
      if (n < 15) token[n++] = *c;
      ++c;
    }
    token[n] = '\0';
    // Move numbers ("12." or "12...") and result tokens are skipped; "0-0"
    // castling spelled with zeros is a move.
    const bool zeroCastle = strcmp(token, "0-0") == 0 || strcmp(token, "0-0-0") == 0;
    if (!zeroCastle && token[0] >= '0' && token[0] <= '9') continue;
    if (token[0] == '*') continue;
    Move m;
    if (!moveFromSan(pos, token, m)) return -1;
    pos.makeMove(m);
    if (uciOut) {
      char uci[6];
      moveToUci(m, uci);
      if (!uciOut->empty()) uciOut->push_back(' ');
      uciOut->append(uci);
    }
    ++count;
  }
  return count;
}

}  // namespace chess
