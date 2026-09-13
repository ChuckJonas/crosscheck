#pragma once
// Chess board state and legal move generation. This library has no Arduino
// or display includes so it also builds in the native test environment.
#include <cstdint>
#include <string>

namespace chess {

enum class Color : uint8_t { White = 0, Black = 1 };
inline Color opposite(Color c) { return c == Color::White ? Color::Black : Color::White; }

// Piece codes. The low 3 bits hold the type and bit 3 holds the color.
enum Piece : uint8_t {
  NoPiece = 0,
  WhitePawn = 1,
  WhiteKnight = 2,
  WhiteBishop = 3,
  WhiteRook = 4,
  WhiteQueen = 5,
  WhiteKing = 6,
  BlackPawn = 9,
  BlackKnight = 10,
  BlackBishop = 11,
  BlackRook = 12,
  BlackQueen = 13,
  BlackKing = 14,
};

enum class PieceType : uint8_t { None = 0, Pawn = 1, Knight = 2, Bishop = 3, Rook = 4, Queen = 5, King = 6 };

inline PieceType typeOf(Piece p) { return static_cast<PieceType>(p & 7); }
inline Color colorOf(Piece p) { return (p & 8) ? Color::Black : Color::White; }
inline Piece makePiece(Color c, PieceType t) {
  return static_cast<Piece>(static_cast<uint8_t>(t) | (c == Color::Black ? 8 : 0));
}

// Squares are 0..63 with a1 = 0, b1 = 1, ..., h8 = 63.
using Square = int8_t;
constexpr Square NoSquare = -1;
inline Square makeSquare(int file, int rank) { return static_cast<Square>(rank * 8 + file); }
inline int fileOf(Square s) { return s & 7; }
inline int rankOf(Square s) { return s >> 3; }

// Castling rights bit flags.
enum Castling : uint8_t {
  CastleNone = 0,
  WhiteKingSide = 1,
  WhiteQueenSide = 2,
  BlackKingSide = 4,
  BlackQueenSide = 8,
};

struct Move {
  Square from = NoSquare;
  Square to = NoSquare;
  PieceType promotion = PieceType::None;

  bool isNull() const { return from == NoSquare; }
  bool operator==(const Move& o) const { return from == o.from && to == o.to && promotion == o.promotion; }
};

// Upper bound on legal moves in any reachable position.
constexpr int MAX_MOVES = 256;

enum class GameStatus : uint8_t { Ongoing, Checkmate, Stalemate };

class Position {
 public:
  Position();

  void setStartPosition();
  // Parses a FEN string. Returns false and leaves the position unchanged on error.
  bool setFromFen(const char* fen);
  // Writes the FEN string for the position. buf must hold at least 90 bytes.
  void toFen(char* buf, int bufSize) const;

  Piece pieceAt(Square s) const { return board[s]; }
  void setPiece(Square s, Piece p) { board[s] = p; }
  Color sideToMove() const { return side; }
  // Hands the move to the other side without a move, for premove targets.
  void setSideToMove(Color c) {
    side = c;
    epSquare = NoSquare;
  }
  uint8_t castlingRights() const { return castling; }
  Square enPassantSquare() const { return epSquare; }
  int halfmoveClock() const { return halfmove; }
  int fullmoveNumber() const { return fullmove; }

  // Writes up to maxCount legal moves into out and returns the count.
  int generateLegalMoves(Move* out, int maxCount) const;
  bool isLegal(const Move& m) const;
  // Applies a move. The caller must pass a legal move.
  void makeMove(const Move& m);
  // Applies a UCI move such as "e2e4" or "e7e8q". Returns false when the
  // move is not legal in this position.
  bool makeUciMove(const char* uci);

  Square kingSquare(Color c) const;
  bool isSquareAttacked(Square s, Color by) const;
  bool inCheck() const { return isSquareAttacked(kingSquare(side), opposite(side)); }
  GameStatus status() const;

  // Counts leaf nodes of the move tree to the given depth. Used by tests.
  uint64_t perft(int depth) const;

 private:
  Piece board[64];
  Color side;
  uint8_t castling;
  Square epSquare;
  int halfmove;
  int fullmove;

  int generatePseudoLegal(Move* out, int maxCount) const;
  bool leavesKingInCheck(const Move& m) const;
};

// Converts a square to two characters like "e4". buf must hold 3 bytes.
void squareToString(Square s, char* buf);
// Parses two characters like "e4". Returns NoSquare on error.
Square squareFromString(const char* text);
// Returns the FEN letter of a piece, or ' ' for NoPiece.
char pieceToChar(Piece p);
Piece pieceFromChar(char c);
// UCI move text such as "e2e4" or "e7e8q". buf must hold 6 bytes.
void moveToUci(const Move& m, char* buf);
// Parses UCI move text. Returns false on malformed input. Legality is not checked.
bool moveFromUci(const char* text, Move& out);

}  // namespace chess

namespace chess {

// Parses one move in standard algebraic notation ("Nf3", "exd5", "O-O",
// "e8=Q+", "R1e2") against the legal moves of pos. Returns false when the
// text matches no legal move or more than one.
bool moveFromSan(const Position& pos, const char* san, Move& out);

// Applies a space-separated SAN move list, skipping move numbers such as
// "12." and result tokens. Appends the UCI form of each move to uciOut when
// given. Returns the number of moves applied, or -1 at the first bad move.
int replaySan(const char* text, Position& pos, std::string* uciOut);

}  // namespace chess
