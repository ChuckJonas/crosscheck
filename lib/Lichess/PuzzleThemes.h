#pragma once
#include <I18n.h>

// Puzzle themes offered for downloads: the Lichess angle key and its name.
struct PuzzleTheme {
  const char* key;
  StrId name;
};

constexpr PuzzleTheme PUZZLE_THEMES[] = {
    {"mix", StrId::STR_CHESS_THEME_MIX},
    {"opening", StrId::STR_CHESS_THEME_OPENING},
    {"middlegame", StrId::STR_CHESS_THEME_MIDDLEGAME},
    {"endgame", StrId::STR_CHESS_THEME_ENDGAME},
    {"rookEndgame", StrId::STR_CHESS_THEME_ROOK_ENDGAME},
    {"pawnEndgame", StrId::STR_CHESS_THEME_PAWN_ENDGAME},
    {"mateIn1", StrId::STR_CHESS_THEME_MATE_IN_1},
    {"mateIn2", StrId::STR_CHESS_THEME_MATE_IN_2},
    {"mateIn3", StrId::STR_CHESS_THEME_MATE_IN_3},
    {"mate", StrId::STR_CHESS_THEME_MATE},
    {"backRankMate", StrId::STR_CHESS_THEME_BACK_RANK},
    {"smotheredMate", StrId::STR_CHESS_THEME_SMOTHERED},
    {"fork", StrId::STR_CHESS_THEME_FORK},
    {"pin", StrId::STR_CHESS_THEME_PIN},
    {"skewer", StrId::STR_CHESS_THEME_SKEWER},
    {"discoveredAttack", StrId::STR_CHESS_THEME_DISCOVERED},
    {"doubleCheck", StrId::STR_CHESS_THEME_DOUBLE_CHECK},
    {"hangingPiece", StrId::STR_CHESS_THEME_HANGING},
    {"trappedPiece", StrId::STR_CHESS_THEME_TRAPPED},
    {"sacrifice", StrId::STR_CHESS_THEME_SACRIFICE},
    {"attraction", StrId::STR_CHESS_THEME_ATTRACTION},
    {"deflection", StrId::STR_CHESS_THEME_DEFLECTION},
    {"clearance", StrId::STR_CHESS_THEME_CLEARANCE},
    {"intermezzo", StrId::STR_CHESS_THEME_INTERMEZZO},
    {"quietMove", StrId::STR_CHESS_THEME_QUIET},
    {"defensiveMove", StrId::STR_CHESS_THEME_DEFENSIVE},
    {"zugzwang", StrId::STR_CHESS_THEME_ZUGZWANG},
    {"advancedPawn", StrId::STR_CHESS_THEME_ADVANCED_PAWN},
    {"promotion", StrId::STR_CHESS_THEME_PROMOTION},
    {"kingsideAttack", StrId::STR_CHESS_THEME_KINGSIDE},
};
constexpr int PUZZLE_THEME_COUNT = sizeof(PUZZLE_THEMES) / sizeof(PUZZLE_THEMES[0]);

// The index of a theme key, 0 (mix) when unknown.
inline int puzzleThemeIndex(const char* key) {
  for (int i = 0; i < PUZZLE_THEME_COUNT; ++i) {
    if (strcmp(PUZZLE_THEMES[i].key, key) == 0) return i;
  }
  return 0;
}
