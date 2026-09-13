#include <gtest/gtest.h>

#include "Position.h"

using namespace chess;

TEST(Position, StartPositionRoundTrip) {
  Position p;
  char fen[96];
  p.toFen(fen, sizeof(fen));
  EXPECT_STREQ(fen, "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
  EXPECT_EQ(p.pieceAt(squareFromString("e1")), WhiteKing);
  EXPECT_EQ(p.pieceAt(squareFromString("d8")), BlackQueen);
  EXPECT_EQ(p.pieceAt(squareFromString("e4")), NoPiece);
  EXPECT_EQ(p.sideToMove(), Color::White);
}

TEST(Position, ParsesMidGameFen) {
  Position p;
  const char* fen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R b Kq e3 5 12";
  ASSERT_TRUE(p.setFromFen(fen));
  EXPECT_EQ(p.sideToMove(), Color::Black);
  EXPECT_EQ(p.castlingRights(), WhiteKingSide | BlackQueenSide);
  EXPECT_EQ(p.enPassantSquare(), squareFromString("e3"));
  EXPECT_EQ(p.halfmoveClock(), 5);
  EXPECT_EQ(p.fullmoveNumber(), 12);
  char out[96];
  p.toFen(out, sizeof(out));
  EXPECT_STREQ(out, fen);
}

TEST(Position, RejectsBadFenAndKeepsState) {
  Position p;
  EXPECT_FALSE(p.setFromFen("rnbqkbnr/pppppppp/9/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"));
  EXPECT_FALSE(p.setFromFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR x KQkq - 0 1"));
  EXPECT_FALSE(p.setFromFen(nullptr));
  EXPECT_EQ(p.pieceAt(squareFromString("a1")), WhiteRook);
}

TEST(Position, SquareNames) {
  char buf[3];
  squareToString(makeSquare(0, 0), buf);
  EXPECT_STREQ(buf, "a1");
  squareToString(makeSquare(7, 7), buf);
  EXPECT_STREQ(buf, "h8");
  EXPECT_EQ(squareFromString("z9"), NoSquare);
  EXPECT_EQ(fileOf(squareFromString("c6")), 2);
  EXPECT_EQ(rankOf(squareFromString("c6")), 5);
}

// Perft node counts from the Chess Programming Wiki.
struct PerftCase {
  const char* fen;
  uint64_t d1;
  uint64_t d2;
  uint64_t d3;
};

TEST(Position, Perft) {
  const PerftCase cases[] = {
      {"rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 20, 400, 8902},
      {"r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 48, 2039, 97862},
      {"8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 14, 191, 2812},
      {"r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 6, 264, 9467},
      {"rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 44, 1486, 62379},
      {"r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 46, 2079, 89890},
  };
  for (const auto& c : cases) {
    Position p;
    ASSERT_TRUE(p.setFromFen(c.fen)) << c.fen;
    EXPECT_EQ(p.perft(1), c.d1) << c.fen;
    EXPECT_EQ(p.perft(2), c.d2) << c.fen;
    EXPECT_EQ(p.perft(3), c.d3) << c.fen;
  }
}

TEST(Position, UciMovesAndSpecialRules) {
  Position p;
  ASSERT_TRUE(p.makeUciMove("e2e4"));
  EXPECT_EQ(p.enPassantSquare(), squareFromString("e3"));
  ASSERT_TRUE(p.makeUciMove("d7d5"));
  ASSERT_TRUE(p.makeUciMove("e4e5"));
  ASSERT_TRUE(p.makeUciMove("f7f5"));
  // En passant capture removes the f5 pawn.
  ASSERT_TRUE(p.makeUciMove("e5f6"));
  EXPECT_EQ(p.pieceAt(squareFromString("f5")), NoPiece);
  EXPECT_EQ(p.pieceAt(squareFromString("f6")), WhitePawn);
  EXPECT_FALSE(p.makeUciMove("e1e3"));

  // Castling moves the rook too.
  Position c;
  ASSERT_TRUE(c.setFromFen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1"));
  ASSERT_TRUE(c.makeUciMove("e1g1"));
  EXPECT_EQ(c.pieceAt(squareFromString("f1")), WhiteRook);
  EXPECT_EQ(c.pieceAt(squareFromString("h1")), NoPiece);
  EXPECT_EQ(c.castlingRights(), BlackKingSide | BlackQueenSide);
  ASSERT_TRUE(c.makeUciMove("e8c8"));
  EXPECT_EQ(c.pieceAt(squareFromString("d8")), BlackRook);
  EXPECT_EQ(c.castlingRights(), CastleNone);

  // Promotion.
  Position q;
  ASSERT_TRUE(q.setFromFen("8/P6k/8/8/8/8/8/K7 w - - 0 1"));
  ASSERT_TRUE(q.makeUciMove("a7a8q"));
  EXPECT_EQ(q.pieceAt(squareFromString("a8")), WhiteQueen);
  char uci[6];
  moveToUci(Move{squareFromString("a7"), squareFromString("a8"), PieceType::Knight}, uci);
  EXPECT_STREQ(uci, "a7a8n");
}

TEST(Position, StatusDetection) {
  Position mate;
  ASSERT_TRUE(mate.setFromFen("rnb1kbnr/pppp1ppp/8/4p3/6Pq/5P2/PPPPP2P/RNBQKBNR w KQkq - 1 3"));
  EXPECT_TRUE(mate.inCheck());
  EXPECT_EQ(mate.status(), GameStatus::Checkmate);
  Position stale;
  ASSERT_TRUE(stale.setFromFen("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1"));
  EXPECT_FALSE(stale.inCheck());
  EXPECT_EQ(stale.status(), GameStatus::Stalemate);
  Position start;
  EXPECT_EQ(start.status(), GameStatus::Ongoing);
}

TEST(San, ReplaysAGameToMate) {
  Position p;
  std::string uci;
  const int n = chess::replaySan("e4 d5 exd5 Nf6 Nc3 Nxd5 Qf3 e6 Bc4 Bc5 Nxd5 exd5 Bxd5 c6 Qxf7#", p, &uci);
  EXPECT_EQ(n, 15);
  EXPECT_EQ(p.status(), GameStatus::Checkmate);
  EXPECT_EQ(uci.substr(0, 14), "e2e4 d7d5 e4d5");
}

TEST(San, CastlingDisambiguationAndPromotion) {
  Position p;
  ASSERT_TRUE(p.setFromFen("r3k2r/1P6/8/8/8/8/8/R3K2R w KQkq - 0 1"));
  Move m;
  ASSERT_TRUE(chess::moveFromSan(p, "O-O-O", m));
  EXPECT_EQ(m.to, squareFromString("c1"));
  ASSERT_TRUE(chess::moveFromSan(p, "Rad1", m));
  EXPECT_EQ(m.from, squareFromString("a1"));
  ASSERT_TRUE(chess::moveFromSan(p, "bxa8=Q+", m));
  EXPECT_EQ(m.to, squareFromString("a8"));
  EXPECT_EQ(m.promotion, PieceType::Queen);
  ASSERT_TRUE(chess::moveFromSan(p, "b8N", m));
  EXPECT_EQ(m.promotion, PieceType::Knight);
  // With the king off the first rank, "Rd1" is ambiguous between the rooks.
  Position two;
  ASSERT_TRUE(two.setFromFen("r3k2r/1P6/8/8/8/8/4K3/R6R w kq - 0 1"));
  EXPECT_FALSE(chess::moveFromSan(two, "Rd1", m));
  ASSERT_TRUE(chess::moveFromSan(two, "Rhd1", m));
  EXPECT_EQ(m.from, squareFromString("h1"));
  // Move numbers and results are skipped.
  Position q;
  EXPECT_EQ(chess::replaySan("1. e4 e5 2. Nf3 1-0", q, nullptr), 3);
  Position z;
  ASSERT_TRUE(z.setFromFen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1"));
  EXPECT_EQ(chess::replaySan("0-0 0-0-0", z, nullptr), 2);
  EXPECT_EQ(chess::replaySan("1. e4 e5 2. Nf9", q, nullptr), -1);
}
