#include "ChessSettings.h"

#include <ObfuscationUtils.h>

namespace {
// Bumped when a stored default must be replaced on load.
}  // namespace

void ChessSettings::toJson(JsonDocument& doc) const {
  doc["token_obf"] = obfuscation::obfuscateToBase64(token);
  doc["username"] = username;
  doc["userId"] = userId;
  doc["rated"] = rated;
  doc["customMinutes"] = customMinutes;
  doc["customIncrement"] = customIncrement;
  doc["pulseMs"] = pulseMs;
  doc["fullRefreshEveryMove"] = fullRefreshEveryMove;
  doc["clockRefreshSec"] = clockRefreshSec;
  doc["aiLevel"] = aiLevel;
  doc["lobbyTab"] = lobbyTab;
  doc["puzzleDifficulty"] = puzzleDifficulty;
  doc["playTab"] = playTab;
  doc["puzzleThemes"] = puzzleThemes;
}

bool ChessSettings::fromJson(JsonVariantConst doc) {
  // A plain "token" field lets the user write the file by hand. It is
  // rewritten in obfuscated form on the next save.
  const char* plain = doc["token"] | "";
  if (plain[0] != '\0') {
    token = plain;
    requestResave();
  } else {
    const char* obf = doc["token_obf"] | "";
    bool ok = false;
    token = obf[0] ? obfuscation::deobfuscateFromBase64(obf, &ok) : "";
    if (!ok) token.clear();
  }
  username = doc["username"] | "";
  userId = doc["userId"] | "";
  rated = doc["rated"] | true;
  customMinutes = doc["customMinutes"] | (uint8_t)10;
  customIncrement = doc["customIncrement"] | (uint8_t)0;
  pulseMs = doc["pulseMs"] | (uint16_t)0;
  fullRefreshEveryMove = doc["fullRefreshEveryMove"] | false;
  clockRefreshSec = doc["clockRefreshSec"] | (uint8_t)5;
  if (clockRefreshSec != 1 && clockRefreshSec != 2 && clockRefreshSec != 5 && clockRefreshSec != 10) {
    clockRefreshSec = 5;
  }
  aiLevel = doc["aiLevel"] | (uint8_t)3;
  if (aiLevel < 1 || aiLevel > 8) aiLevel = 3;
  lobbyTab = doc["lobbyTab"] | (uint8_t)0;
  if (lobbyTab > 3) lobbyTab = 0;
  puzzleDifficulty = doc["puzzleDifficulty"] | (uint8_t)2;
  playTab = doc["playTab"] | (uint8_t)0;
  if (playTab > 3) playTab = 0;
  if (puzzleDifficulty > 4) puzzleDifficulty = 2;
  puzzleThemes = doc["puzzleThemes"] | "";
  return true;
}
