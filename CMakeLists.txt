cmake_minimum_required(VERSION 3.16)
project(resolume-client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- Dependencies via FetchContent ---
include(FetchContent)

# nlohmann/json
FetchContent_Declare(
  nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.11.3
  GIT_SHALLOW TRUE
)

# IXWebSocket
FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG v11.4.5
  GIT_SHALLOW TRUE
)

# Catch2 (for tests)
FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.5.2
  GIT_SHALLOW TRUE
)

# Disable IXWebSocket TLS to avoid OpenSSL dependency for now
set(USE_TLS OFF CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(nlohmann_json ixwebsocket Catch2)

# --- Resolume client library ---
add_library(resolume_client
  src/resolume/protocol.cpp
  src/resolume/composition.cpp
  src/resolume/ws_client.cpp
)
target_include_directories(resolume_client PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(resolume_client PUBLIC nlohmann_json::nlohmann_json ixwebsocket)

# Path to fixture for tests
target_compile_definitions(resolume_client PUBLIC
  FIXTURE_PATH="${CMAKE_SOURCE_DIR}/mock_server/fixtures/resolume-ws-initial-state.json"
)

# --- Looper core library ---
add_library(looper_core
  src/looper/core.cpp
)
target_include_directories(looper_core PUBLIC ${CMAKE_SOURCE_DIR}/src)

# --- Tests ---
add_executable(test_protocol tests/test_protocol.cpp)
target_link_libraries(test_protocol PRIVATE resolume_client Catch2::Catch2WithMain)

add_executable(test_composition tests/test_composition.cpp)
target_link_libraries(test_composition PRIVATE resolume_client Catch2::Catch2WithMain)

add_executable(test_ws_client tests/test_ws_client.cpp)
target_link_libraries(test_ws_client PRIVATE resolume_client Catch2::Catch2WithMain)

# Register tests with CTest
include(CTest)
include(Catch)
catch_discover_tests(test_protocol)
catch_discover_tests(test_composition)
catch_discover_tests(test_ws_client PROPERTIES LABELS "integration")

add_executable(test_looper_core tests/test_looper_core.cpp)
target_link_libraries(test_looper_core PRIVATE looper_core Catch2::Catch2WithMain)
catch_discover_tests(test_looper_core)

# --- Tools ---
add_executable(testme tools/testme.cpp)
target_link_libraries(testme PRIVATE resolume_client)

add_executable(looper_cli tools/looper_cli.cpp)
target_link_libraries(looper_cli PRIVATE looper_core resolume_client)
