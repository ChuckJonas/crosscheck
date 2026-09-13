FetchContent_Declare(
  ArduinoJson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_MakeAvailable(ArduinoJson)

add_executable(ChessPositionTest
  PositionTest.cpp
  LichessProtocolTest.cpp
  PgnTest.cpp
  ${REPO_ROOT}/lib/Chess/Position.cpp
  ${REPO_ROOT}/lib/Chess/LichessProtocol.cpp
  ${REPO_ROOT}/lib/Chess/Pgn.cpp
)

target_include_directories(ChessPositionTest PRIVATE
  ${REPO_ROOT}/lib/Chess
)

target_link_libraries(ChessPositionTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(ChessPositionTest)
