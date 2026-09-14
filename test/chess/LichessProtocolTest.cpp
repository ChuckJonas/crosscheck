#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "LichessProtocol.h"

using namespace lichess;

namespace {
struct Collector {
  std::vector<std::string> lines;
  int heartbeats = 0;
};
void onLine(void* ctx, const char* line, size_t len) { static_cast<Collector*>(ctx)->lines.emplace_back(line, len); }
void onBeat(void* ctx) { static_cast<Collector*>(ctx)->heartbeats++; }
}  // namespace

TEST(Ndjson, SplitsAcrossChunksAndCountsHeartbeats) {
  Collector c;
  NdjsonSplitter s;
  s.setCallbacks(&c, onLine, onBeat);
  const char* a = "{\"type\":\"a\"}\n\n{\"ty";
  const char* b = "pe\":\"b\"}\r\n\n";
  s.feed(reinterpret_cast<const uint8_t*>(a), strlen(a));
  s.feed(reinterpret_cast<const uint8_t*>(b), strlen(b));
  ASSERT_EQ(c.lines.size(), 2u);
  EXPECT_EQ(c.lines[0], "{\"type\":\"a\"}");
  EXPECT_EQ(c.lines[1], "{\"type\":\"b\"}");
  EXPECT_EQ(c.heartbeats, 2);
  const char* tail = "{\"type\":\"c\"}";
  s.feed(reinterpret_cast<const uint8_t*>(tail), strlen(tail));
  s.flush();
  ASSERT_EQ(c.lines.size(), 3u);
  EXPECT_EQ(c.lines[2], "{\"type\":\"c\"}");
}

TEST(Protocol, ParsesGameFullAndState) {
  const char* full =
      "{\"type\":\"gameFull\",\"id\":\"abcdefgh\",\"rated\":true,\"speed\":\"blitz\","
      "\"clock\":{\"initial\":180000,\"increment\":2000},"
      "\"white\":{\"id\":\"alice\",\"name\":\"Alice\",\"rating\":1500},"
      "\"black\":{\"id\":\"bob\",\"name\":\"Bob\",\"rating\":1600,\"provisional\":true},"
      "\"state\":{\"type\":\"gameState\",\"moves\":\"e2e4 e7e5\",\"wtime\":170000,\"btime\":175000,"
      "\"winc\":2000,\"binc\":2000,\"status\":\"started\"}}";
  GameSnapshot g;
  EXPECT_EQ(parseGameLine(full, strlen(full), g, "bob"), GameLineKind::GameFull);
  EXPECT_STREQ(g.id, "abcdefgh");
  EXPECT_EQ(g.myColor, chess::Color::Black);
  EXPECT_EQ(g.initialMs, 180000u);
  EXPECT_EQ(g.incrementMs, 2000u);
  EXPECT_EQ(g.moves, "e2e4 e7e5");
  EXPECT_EQ(g.status, GameStatus::Started);
  EXPECT_TRUE(g.black.provisional);
  EXPECT_EQ(g.black.aiLevel, 0);

  const char* ai =
      "{\"type\":\"gameFull\",\"id\":\"aiai1234\",\"rated\":false,\"speed\":\"blitz\",\"clock\":{\"initial\":300000,"
      "\"increment\":0},\"white\":{\"aiLevel\":3},\"black\":{\"id\":\"bob\",\"name\":\"Bob\",\"rating\":1600},"
      "\"state\":{\"type\":\"gameState\",\"moves\":\"\",\"wtime\":300000,\"btime\":300000,\"status\":\"started\"}}";
  lichess::GameSnapshot a;
  EXPECT_EQ(parseGameLine(ai, strlen(ai), a, "bob"), GameLineKind::GameFull);
  EXPECT_EQ(a.white.aiLevel, 3);
  EXPECT_EQ(a.myColor, chess::Color::Black);

  const char* state =
      "{\"type\":\"gameState\",\"moves\":\"e2e4 e7e5 g1f3\",\"wtime\":160000,\"btime\":175000,"
      "\"winc\":2000,\"binc\":2000,\"status\":\"resign\",\"winner\":\"black\",\"wdraw\":false,\"bdraw\":true}";
  EXPECT_EQ(parseGameLine(state, strlen(state), g, "bob"), GameLineKind::GameState);
  EXPECT_EQ(g.moves, "e2e4 e7e5 g1f3");
  EXPECT_EQ(g.wtimeMs, 160000u);
  EXPECT_EQ(g.status, GameStatus::Resign);
  EXPECT_TRUE(g.hasWinner);
  EXPECT_EQ(g.winner, chess::Color::Black);
  EXPECT_TRUE(g.blackOffersDraw);
  EXPECT_TRUE(isFinished(g.status));

  chess::Position p;
  chess::Move last;
  EXPECT_EQ(replayMoves(g.moves, p, &last), 3);
  EXPECT_EQ(p.sideToMove(), chess::Color::Black);
  char uci[6];
  chess::moveToUci(last, uci);
  EXPECT_STREQ(uci, "g1f3");
  EXPECT_EQ(replayMoves("e2e4 e2e4", p, nullptr), -1);
}

TEST(Protocol, ParsesGameSummaryAndPuzzle) {
  const char* line =
      "{\"id\":\"8bEx0oep\",\"rated\":true,\"speed\":\"rapid\",\"status\":\"mate\",\"winner\":\"white\","
      "\"players\":{\"white\":{\"user\":{\"name\":\"shomint\",\"id\":\"shomint\"},\"rating\":1349},"
      "\"black\":{\"user\":{\"name\":\"illbilly\",\"id\":\"illbilly\"},\"rating\":1409}},"
      "\"clock\":{\"initial\":300,\"increment\":5},\"moves\":\"e4 d5 exd5\"}";
  GameSummary g;
  ASSERT_TRUE(parseGameSummary(line, strlen(line), "illbilly", g));
  EXPECT_STREQ(g.id, "8bEx0oep");
  EXPECT_EQ(g.myColor, chess::Color::Black);
  EXPECT_EQ(g.initialMs, 300000u);
  EXPECT_EQ(g.status, GameStatus::Mate);
  EXPECT_TRUE(g.hasWinner);
  EXPECT_EQ(g.winner, chess::Color::White);
  EXPECT_EQ(g.movesSan, "e4 d5 exd5");
  EXPECT_STREQ(g.white.name, "shomint");
  const char* odd = "{\"id\":\"zzzzzzzz\",\"variant\":\"chess960\",\"status\":\"mate\",\"players\":{}}";
  GameSummary o;
  EXPECT_FALSE(parseGameSummary(odd, strlen(odd), "illbilly", o));

  const char* pz =
      "{\"puzzles\":[{\"game\":{\"id\":\"abc\",\"pgn\":\"e4 e5\"},\"puzzle\":{\"id\":\"PZ123\",\"rating\":1918,"
      "\"solution\":[\"g1g7\",\"g8g7\",\"e3h6\"],\"fen\":\"6k1/8/8/8/8/4B3/2Q3R1/6K1 w - - 0 1\","
      "\"lastMove\":\"h8g8\",\"initialPly\":55}}]}";
  std::vector<Puzzle> single;
  ASSERT_EQ(parsePuzzleBatch(pz, strlen(pz), single, 10), 1);
  const Puzzle& z = single[0];
  EXPECT_STREQ(z.id, "PZ123");
  EXPECT_EQ(z.rating, 1918);
  EXPECT_EQ(z.solution, "g1g7 g8g7 e3h6");

  // A batch puzzle carries no FEN: only the game moves, and the solver moves
  // next in the position after them.
  const char* next =
      "{\"puzzles\":[{\"game\":{\"id\":\"4ULyN7VZ\",\"pgn\":\"e4 e5 Nf3 Nc6 Bb5 a6\"},\"puzzle\":{\"id\":\"NX1\","
      "\"rating\":1500,\"solution\":[\"b5c6\",\"d7c6\"],\"initialPly\":5}}]}";
  std::vector<Puzzle> one;
  ASSERT_EQ(parsePuzzleBatch(next, strlen(next), one, 10), 1);
  const Puzzle& n = one[0];
  EXPECT_EQ(n.pgn, "e4 e5 Nf3 Nc6 Bb5 a6");
  chess::Position after;
  std::string uci;
  EXPECT_EQ(chess::replaySan(n.pgn.c_str(), after, &uci), 6);
  EXPECT_EQ(after.sideToMove(), chess::Color::White);
  EXPECT_TRUE(after.makeUciMove("b5c6"));

  const char* batch =
      "{\"puzzles\":[{\"game\":{\"id\":\"g1\",\"pgn\":\"e4 e5\",\"players\":[]},\"puzzle\":{\"id\":\"B1\","
      "\"rating\":1400,\"solution\":[\"g1f3\"],\"themes\":[\"x\"]}},{\"game\":{\"pgn\":\"d4\"},\"puzzle\":"
      "{\"id\":\"B2\",\"rating\":1600,\"solution\":[\"d7d5\",\"c2c4\"]}},{\"game\":{},\"puzzle\":{\"id\":\"bad\"}}]}";
  std::vector<Puzzle> many;
  EXPECT_EQ(parsePuzzleBatch(batch, strlen(batch), many, 10), 2);
  ASSERT_EQ(many.size(), 2u);
  EXPECT_STREQ(many[1].id, "B2");
  EXPECT_EQ(many[1].solution, "d7d5 c2c4");
  EXPECT_EQ(many[0].pgn, "e4 e5");
  EXPECT_EQ(many[0].themes, "x");
  EXPECT_EQ(many[1].themes, "");
}

TEST(Protocol, ParsesRatingDiff) {
  const char* json =
      "{\"id\":\"8bEx0oep\",\"status\":\"mate\",\"players\":{\"white\":{\"rating\":1349,\"ratingDiff\":14},"
      "\"black\":{\"rating\":1409,\"ratingDiff\":-60}}}";
  int diff = 0;
  ASSERT_TRUE(parseRatingDiff(json, strlen(json), chess::Color::Black, diff));
  EXPECT_EQ(diff, -60);
  ASSERT_TRUE(parseRatingDiff(json, strlen(json), chess::Color::White, diff));
  EXPECT_EQ(diff, 14);
  const char* casual = "{\"players\":{\"white\":{\"rating\":1349},\"black\":{\"rating\":1409}}}";
  EXPECT_FALSE(parseRatingDiff(casual, strlen(casual), chess::Color::White, diff));
}

TEST(Protocol, ParsesAccountNowPlayingAndEvents) {
  const char* acct =
      "{\"id\":\"alice\",\"username\":\"Alice\",\"perfs\":{\"blitz\":{\"rating\":1500},\"rapid\":{\"rating\":1550}}}";
  Account a;
  ASSERT_TRUE(parseAccount(acct, strlen(acct), a));
  EXPECT_STREQ(a.username, "Alice");
  EXPECT_EQ(a.blitz, 1500);
  EXPECT_EQ(a.rapid, 1550);
  EXPECT_EQ(a.classical, 0);

  const char* playing =
      "{\"nowPlaying\":[{\"gameId\":\"xyz12345\",\"color\":\"black\",\"isMyTurn\":true,\"speed\":\"correspondence\","
      "\"rated\":false,\"secondsLeft\":86400,\"lastMove\":\"e2e4\",\"opponent\":{\"id\":\"bob\",\"username\":\"Bob\","
      "\"rating\":1600},\"fen\":\"rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1\"}]}";
  std::vector<OngoingGame> games;
  ASSERT_TRUE(parseNowPlaying(playing, strlen(playing), games));
  ASSERT_EQ(games.size(), 1u);
  EXPECT_STREQ(games[0].gameId, "xyz12345");
  EXPECT_EQ(games[0].myColor, chess::Color::Black);
  EXPECT_TRUE(games[0].isMyTurn);
  EXPECT_STREQ(games[0].opponent, "Bob");
  EXPECT_EQ(games[0].opponentRating, 1600);

  const char* ev =
      "{\"type\":\"gameStart\",\"game\":{\"gameId\":\"xyz12345\",\"fullId\":\"xyz12345abcd\",\"color\":\"black\"}}";
  StreamEvent e;
  ASSERT_TRUE(parseEventLine(ev, strlen(ev), e));
  EXPECT_EQ(e.kind, EventKind::GameStart);
  EXPECT_STREQ(e.gameId, "xyz12345");
  EXPECT_TRUE(e.hasColor);
  EXPECT_EQ(e.color, chess::Color::Black);
  const char* ch = "{\"type\":\"challenge\",\"challenge\":{\"id\":\"chal1234\",\"challenger\":{\"name\":\"Carol\"}}}";
  ASSERT_TRUE(parseEventLine(ch, strlen(ch), e));
  EXPECT_EQ(e.kind, EventKind::Challenge);
  EXPECT_STREQ(e.challenger, "Carol");
}

TEST(Protocol, ParsesAnalysisAndGameId) {
  const char* json =
      "{\"id\":\"abcd1234\",\"analysis\":[{\"eval\":23},{\"eval\":-120,\"best\":\"g1f3\",\"variation\":\"Nf3 "
      "Nc6\",\"judgment\":{\"name\":\"Mistake\",\"comment\":\"Mistake. Nf3 was best.\"}},{\"mate\":-3}]}";
  std::vector<AnalysisPly> a;
  ASSERT_TRUE(parseAnalysis(json, strlen(json), a));
  ASSERT_EQ(a.size(), 3u);
  EXPECT_EQ(a[0].cp, 23);
  EXPECT_EQ(a[0].judgment, 0);
  EXPECT_EQ(a[1].cp, -120);
  EXPECT_EQ(a[1].judgment, 2);
  EXPECT_STREQ(a[1].best, "g1f3");
  EXPECT_EQ(a[2].mate, -3);
  char id[16];
  ASSERT_TRUE(parseGameId(json, strlen(json), id, sizeof(id)));
  EXPECT_STREQ(id, "abcd1234");
  const char* none = "{\"id\":\"x\"}";
  EXPECT_FALSE(parseAnalysis(none, strlen(none), a));
}
