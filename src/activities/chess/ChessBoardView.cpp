#include "ChessBoardView.h"

#include <GfxRenderer.h>

#include "ChessPieceBitmaps.h"
#include "fontIds.h"

using chess::fileOf;
using chess::makeSquare;
using chess::NoPiece;
using chess::NoSquare;
using chess::Piece;
using chess::Position;
using chess::rankOf;
using chess::Square;
using chess::typeOf;

void ChessBoardView::setLayout(int x, int y, int squareSize) {
  originX = x;
  originY = y;
  squarePx = squareSize;
}

void ChessBoardView::squareOrigin(Square s, int& outX, int& outY) const {
  const int file = flipped ? 7 - fileOf(s) : fileOf(s);
  const int rank = flipped ? rankOf(s) : 7 - rankOf(s);
  outX = originX + file * squarePx;
  outY = originY + rank * squarePx;
}

Square ChessBoardView::squareAt(int px, int py) const {
  const int relX = px - originX;
  const int relY = py - originY;
  const int boardPx = squarePx * 8;
  if (relX < 0 || relY < 0 || relX >= boardPx || relY >= boardPx) return NoSquare;
  const int inX = relX % squarePx;
  const int inY = relY % squarePx;
  if (inX < DEAD_ZONE || inY < DEAD_ZONE || inX >= squarePx - DEAD_ZONE || inY >= squarePx - DEAD_ZONE) {
    return NoSquare;
  }
  const int col = relX / squarePx;
  const int row = relY / squarePx;
  const int file = flipped ? 7 - col : col;
  const int rank = flipped ? row : 7 - row;
  return makeSquare(file, rank);
}

void ChessBoardView::drawPiece(const GfxRenderer& renderer, Piece piece, int sx, int sy) const {
  const bool white = chess::colorOf(piece) == chess::Color::White;
  const int type = static_cast<int>(typeOf(piece));
  if (squarePx == chesspieces::SIZE) {
    // Bitmap pieces: paint every opaque pixel so white pieces stay solid on
    // the dithered dark squares. drawPixel applies the orientation transform.
    const uint8_t* shape = white ? chesspieces::WHITE_SHAPE[type] : chesspieces::BLACK_SHAPE[type];
    const uint8_t* ink = white ? chesspieces::WHITE_INK[type] : chesspieces::BLACK_INK[type];
    for (int y = 0; y < chesspieces::SIZE; ++y) {
      for (int x = 0; x < chesspieces::SIZE; ++x) {
        const int i = y * chesspieces::SIZE + x;
        const uint8_t bit = 0x80 >> (i & 7);
        if (!(shape[i >> 3] & bit)) continue;
        renderer.drawPixel(sx + x, sy + y, (ink[i >> 3] & bit) != 0);
      }
    }
    return;
  }
  // Fallback for other square sizes: a letter on a disc.
  static constexpr char LETTERS[] = " PNBRQK";
  const char letter[2] = {LETTERS[type], '\0'};
  const int radius = squarePx / 2 - 6;
  const int cx = sx + squarePx / 2;
  const int cy = sy + squarePx / 2;
  renderer.fillRoundedRect(cx - radius, cy - radius, radius * 2, radius * 2, radius, white ? White : Black);
  if (white) renderer.drawRoundedRect(cx - radius, cy - radius, radius * 2, radius * 2, 2, radius, true);
  const int fontId = NOTOSANS_18_FONT_ID;
  const int tw = renderer.getTextWidth(fontId, letter, EpdFontFamily::BOLD);
  const int th = renderer.getTextHeight(fontId);
  renderer.drawText(fontId, cx - tw / 2, cy - th / 2, letter, white, EpdFontFamily::BOLD);
}

void ChessBoardView::draw(const GfxRenderer& renderer, const Position& position, const Marks& marks) const {
  const int boardPx = squarePx * 8;
  renderer.fillRect(originX, originY, boardPx, boardPx, false);
  for (Square s = 0; s < 64; ++s) {
    int sx = 0;
    int sy = 0;
    squareOrigin(s, sx, sy);
    const bool dark = ((fileOf(s) + rankOf(s)) & 1) == 0;
    if (dark) renderer.fillRectDither(sx, sy, squarePx, squarePx, LightGray);
    if (s == marks.lastFrom || s == marks.lastTo) {
      renderer.drawRect(sx + 1, sy + 1, squarePx - 2, squarePx - 2, 2, true);
    }
    const Piece piece = position.pieceAt(s);
    if (piece != NoPiece) drawPiece(renderer, piece, sx, sy);
    if (marks.target[s]) {
      if (piece == NoPiece) {
        // Empty destination: a small dot in the middle of the square.
        const int r = squarePx / 8;
        renderer.fillRoundedRect(sx + squarePx / 2 - r, sy + squarePx / 2 - r, r * 2, r * 2, r, Black);
      } else {
        // Capture: a thick ring around the square.
        renderer.drawRect(sx + 2, sy + 2, squarePx - 4, squarePx - 4, 3, true);
      }
    }
    if (s == marks.checkedKing) renderer.drawRect(sx + 1, sy + 1, squarePx - 2, squarePx - 2, 3, true);
    if (s == marks.wrongSquare) {
      // Two thick diagonals across the square.
      renderer.drawLine(sx + 8, sy + 8, sx + squarePx - 9, sy + squarePx - 9, 5, true);
      renderer.drawLine(sx + squarePx - 9, sy + 8, sx + 8, sy + squarePx - 9, 5, true);
    }
    if (s == marks.premoveFrom || s == marks.premoveTo) {
      renderer.drawRect(sx + 1, sy + 1, squarePx - 2, squarePx - 2, 1, true);
      renderer.drawRect(sx + 4, sy + 4, squarePx - 8, squarePx - 8, 1, true);
    }
    if (s == marks.selected) renderer.drawRect(sx, sy, squarePx, squarePx, 4, true);
  }
  for (int i = 0; i < marks.circleCount && i < Marks::MAX_ARROWS; ++i) {
    int sx = 0;
    int sy = 0;
    squareOrigin(marks.circles[i], sx, sy);
    // A ring inside the square, with a white edge so it shows on dark squares.
    renderer.drawRoundedRect(sx + 2, sy + 2, squarePx - 4, squarePx - 4, 2, (squarePx - 4) / 2, false);
    renderer.drawRoundedRect(sx + 4, sy + 4, squarePx - 8, squarePx - 8, 3, (squarePx - 8) / 2, true);
  }
  for (int i = 0; i < marks.arrowCount && i < Marks::MAX_ARROWS; ++i) {
    drawArrow(renderer, marks.arrows[i].from, marks.arrows[i].to);
  }
  // The board spans the full screen width, so the frame sits inside the edge.
  renderer.drawRect(originX, originY, boardPx, boardPx, 1, true);

  // Coordinates: files along the bottom edge, ranks along the left edge.
  const int fontId = UI_10_FONT_ID;
  const int th = renderer.getTextHeight(fontId);
  for (int i = 0; i < 8; ++i) {
    const char fileLabel[2] = {static_cast<char>('a' + (flipped ? 7 - i : i)), '\0'};
    const char rankLabel[2] = {static_cast<char>('1' + (flipped ? i : 7 - i)), '\0'};
    renderer.drawText(fontId, originX + i * squarePx + squarePx - 9, originY + boardPx - th - 1, fileLabel, true);
    renderer.drawText(fontId, originX + 2, originY + i * squarePx + 1, rankLabel, true);
  }
}

void ChessBoardView::drawArrow(const GfxRenderer& renderer, Square from, Square to) const {
  if (from == NoSquare || to == NoSquare || from == to) return;
  int fx = 0;
  int fy = 0;
  int tx = 0;
  int ty = 0;
  squareOrigin(from, fx, fy);
  squareOrigin(to, tx, ty);
  const int half = squarePx / 2;
  const int x1 = fx + half;
  const int y1 = fy + half;
  const int x2 = tx + half;
  const int y2 = ty + half;
  // Unit direction in 1/256 steps, to place the head inside the target square.
  const int dx = x2 - x1;
  const int dy = y2 - y1;
  int length = 1;
  for (int i = 1; i * i < dx * dx + dy * dy; ++i) length = i + 1;
  const int ux = dx * 256 / length;
  const int uy = dy * 256 / length;
  const int headLen = squarePx / 3;
  const int headHalf = squarePx / 5;
  const int bx = x2 - ux * headLen / 256;  // base of the head
  const int by = y2 - uy * headLen / 256;
  // Shaft: a white border under a black line.
  renderer.drawLine(x1, y1, bx, by, 9, false);
  renderer.drawLine(x1, y1, bx, by, 5, true);
  // Head: a triangle; the white one is a little larger for the border.
  const int px = -uy;  // perpendicular
  const int py = ux;
  const int xs[3] = {x2 + ux * 2 / 256, bx + px * (headHalf + 2) / 256, bx - px * (headHalf + 2) / 256};
  const int ys[3] = {y2 + uy * 2 / 256, by + py * (headHalf + 2) / 256, by - py * (headHalf + 2) / 256};
  renderer.fillPolygon(xs, ys, 3, false);
  const int xk[3] = {x2, bx + px * headHalf / 256, bx - px * headHalf / 256};
  const int yk[3] = {y2, by + py * headHalf / 256, by - py * headHalf / 256};
  renderer.fillPolygon(xk, yk, 3, true);
}
