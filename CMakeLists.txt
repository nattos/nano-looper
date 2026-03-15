cmake_minimum_required(VERSION 3.16)
project(resolume-client LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_STANDARD 17)

# Silence macOS OpenGL deprecation warnings
add_compile_definitions(GL_SILENCE_DEPRECATION)

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

add_executable(test_utf8 tests/test_utf8.cpp)
target_include_directories(test_utf8 PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(test_utf8 PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(test_utf8)

# --- Tools ---
add_executable(testme tools/testme.cpp)
target_link_libraries(testme PRIVATE resolume_client)

add_executable(looper_cli tools/looper_cli.cpp)
target_link_libraries(looper_cli PRIVATE looper_core resolume_client)

# --- FFGL SDK ---
set(FFGL_BUILD_EXAMPLE_PLUGINS OFF CACHE BOOL "" FORCE)
add_subdirectory(modules/ffgl)

# --- Channel Tag Plugin (auxiliary, lightweight) ---
add_library(channel_tag_plugin STATIC
  src/plugin/channel_tag_plugin.cpp
)
target_include_directories(channel_tag_plugin PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(channel_tag_plugin PUBLIC ffgl::sdk)

# --- Looper Plugin (static library) ---
add_library(looper_plugin STATIC
  src/plugin/looper_plugin.cpp
  src/plugin/overlay_renderer.cpp
  src/plugin/text_renderer.mm
  src/plugin/image_loader.mm
)
target_include_directories(looper_plugin PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(looper_plugin PUBLIC
  ffgl::sdk
  looper_core
  resolume_client
)
if(APPLE)
  target_link_libraries(looper_plugin PUBLIC
    "-framework OpenGL"
    "-framework CoreText"
    "-framework CoreGraphics"
    "-framework Foundation"
  )
endif()

# --- FFGL Bundles (macOS .bundle for Resolume) ---
if(APPLE)
  # Common FFGL bundle linker flags
  set(FFGL_BUNDLE_LINK_FLAGS "-undefined dynamic_lookup")

  # NanoLooper main plugin bundle
  add_library(NanoLooper MODULE
    src/plugin/looper_plugin.cpp
    src/plugin/overlay_renderer.cpp
    src/plugin/text_renderer.mm
    src/plugin/image_loader.mm
  )
  target_include_directories(NanoLooper PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(NanoLooper PRIVATE
    ffgl::sdk looper_core resolume_client
    "-framework OpenGL"
    "-framework CoreText"
    "-framework CoreGraphics"
    "-framework Foundation"
  )
  set_target_properties(NanoLooper PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION "bundle"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/plugin/Info.plist.in"
    LINK_FLAGS "${FFGL_BUNDLE_LINK_FLAGS}"
  )

  # NanoLooper Channel tag plugin bundle
  add_library(NanoLooperCh MODULE
    src/plugin/channel_tag_plugin.cpp
  )
  target_include_directories(NanoLooperCh PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(NanoLooperCh PRIVATE
    ffgl::sdk
    "-framework OpenGL"
  )
  set_target_properties(NanoLooperCh PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION "bundle"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/src/plugin/Info.plist.in"
    LINK_FLAGS "${FFGL_BUNDLE_LINK_FLAGS}"
  )
endif()

# --- Test Harness (macOS only) ---
if(APPLE)
  add_executable(looper_harness tools/harness/main.mm)
  target_link_libraries(looper_harness PRIVATE
    looper_plugin
    "-framework Cocoa"
    "-framework OpenGL"
  )
  set_target_properties(looper_harness PROPERTIES
    MACOSX_BUNDLE FALSE
  )
endif()
