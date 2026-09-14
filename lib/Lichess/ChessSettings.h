#pragma once
#include <ArduinoJson.h>
#include <ChessFiles.h>
#include <PersistableStore.h>

#include <cstdint>
#include <string>

// Chess preferences and the Lichess token, stored on the SD card.
class ChessSettings : public PersistableStore<ChessSettings> {
 private:
  std::string token;
  std::string username;
  std::string userId;
  bool rated = true;
  uint8_t customMinutes = 10;
  uint8_t customIncrement = 0;
  // Frontlight pulse length in milliseconds on an opponent move. 0 disables it.
  uint16_t pulseMs = 0;
  bool fullRefreshEveryMove = false;
  // Seconds between clock redraws during a live game: 1, 2, 5, or 10. Every
  // redraw is a full-frame fast refresh, so 1 keeps the panel busy half the time.
  uint8_t clockRefreshSec = 5;
  uint8_t aiLevel = 3;
  // Lobby tab: 0 play, 1 games, 2 puzzles, 3 studies.
  uint8_t lobbyTab = 0;
  // Play sub-tab: 0 match, 1 computer, 2 challenge, 3 local.
  uint8_t playTab = 0;
  // Puzzle difficulty index: easiest, easier, normal, harder, hardest.
  uint8_t puzzleDifficulty = 2;
  // Selected puzzle themes, comma separated keys; empty means all themes.
  std::string puzzleThemes;

  ChessSettings() = default;
  ~ChessSettings() = default;
  friend class PersistableStore<ChessSettings>;

 public:
  static const char* getFilePath() { return chessfiles::SETTINGS_PATH; }
  void toJson(JsonDocument& doc) const;
  bool fromJson(JsonVariantConst doc);

  bool hasToken() const { return !token.empty(); }
  const std::string& getToken() const { return token; }
  void setToken(const std::string& value) { token = value; }
  const std::string& getUsername() const { return username; }
  const std::string& getUserId() const { return userId; }
  void setAccount(const std::string& name, const std::string& id) {
    username = name;
    userId = id;
  }
  bool getRated() const { return rated; }
  void setRated(bool value) { rated = value; }
  int getCustomMinutes() const { return customMinutes; }
  int getCustomIncrement() const { return customIncrement; }
  void setCustom(int minutes, int increment) {
    customMinutes = static_cast<uint8_t>(minutes);
    customIncrement = static_cast<uint8_t>(increment);
  }
  int getPulseMs() const { return pulseMs; }
  void setPulseMs(int value) { pulseMs = static_cast<uint16_t>(value); }
  bool getFullRefreshEveryMove() const { return fullRefreshEveryMove; }
  int getClockRefreshSec() const { return clockRefreshSec; }
  int getAiLevel() const { return aiLevel; }
  int getLobbyTab() const { return lobbyTab; }
  void setLobbyTab(int value) { lobbyTab = static_cast<uint8_t>(value); }
  int getPlayTab() const { return playTab; }
  void setPlayTab(int value) { playTab = static_cast<uint8_t>(value); }
  int getPuzzleDifficulty() const { return puzzleDifficulty; }
  void setPuzzleDifficulty(int value) { puzzleDifficulty = static_cast<uint8_t>(value); }
  const std::string& getPuzzleThemes() const { return puzzleThemes; }
  void setPuzzleThemes(const std::string& value) { puzzleThemes = value; }
  void setAiLevel(int value) { aiLevel = static_cast<uint8_t>(value); }
  void setClockRefreshSec(int value) { clockRefreshSec = static_cast<uint8_t>(value); }
  void setFullRefreshEveryMove(bool value) { fullRefreshEveryMove = value; }
};

#define CHESS_SETTINGS ChessSettings::getInstance()
