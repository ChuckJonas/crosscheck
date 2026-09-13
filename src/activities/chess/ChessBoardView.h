#pragma once
#include <Position.h>

class GfxRenderer;

// Draws a chess board into the frame buffer and maps taps to squares.
class ChessBoardView {
 public:
  // Highlights drawn on top of the pieces.
  struct Marks {
    chess::Square selected = chess::NoSquare;
    chess::Square lastFrom = chess::NoSquare;
    chess::Square lastTo = chess::NoSquare;
    chess::Square checkedKing = chess::NoSquare;
    // A refused move: a cross on the square that was tapped.
    chess::Square wrongSquare = chess::NoSquare;
    // A premove waiting for the opponent, drawn with a double border.
    chess::Square premoveFrom = chess::NoSquare;
    chess::Square premoveTo = chess::NoSquare;
    // One flag per square: legal destinations of the selected piece.
    bool target[64] = {};
    // Annotation arrows and circles from a study comment.
    static constexpr int MAX_ARROWS = 8;
    struct Arrow {
      chess::Square from = chess::NoSquare;
      chess::Square to = chess::NoSquare;
    };
    Arrow arrows[MAX_ARROWS];
    int arrowCount = 0;
    chess::Square circles[MAX_ARROWS] = {};
    int circleCount = 0;
  };

  // Places the board at (x, y) with the given square size in pixels.
  void setLayout(int x, int y, int squareSize);
  int x() const { return originX; }
  int y() const { return originY; }
  int size() const { return squarePx * 8; }

  // When flipped is true, rank 8 is at the bottom (black point of view).
  void setFlipped(bool value) { flipped = value; }
  bool isFlipped() const { return flipped; }

  void draw(const GfxRenderer& renderer, const chess::Position& position, const Marks& marks) const;

  // Returns the square under a screen point, or NoSquare when the point is
  // outside the board or inside the dead zone at a square edge.
  chess::Square squareAt(int px, int py) const;

 private:
  static constexpr int DEAD_ZONE = 4;
  int originX = 0;
  int originY = 0;
  int squarePx = 60;
  bool flipped = false;

  void squareOrigin(chess::Square s, int& outX, int& outY) const;
  void drawArrow(const GfxRenderer& renderer, chess::Square from, chess::Square to) const;
  void drawPiece(const GfxRenderer& renderer, chess::Piece piece, int sx, int sy) const;
};
