#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>

#include "resolume/composition.h"
#include "resolume/ws_client.h"

using namespace resolume;

// These tests require the mock server to be running on localhost:8080.
// Run: cd mock_server && python resolume_mock.py
// Then: ctest -R ws_client

static bool mock_server_available() {
  ResolumeWsClient client;
  client.connect("ws://127.0.0.1:8080/api/v1");
  // Wait briefly for connection
  for (int i = 0; i < 20; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (client.is_connected()) return true;
  }
  return false;
}

TEST_CASE("Connect and receive initial state", "[ws_client][integration]") {
  if (!mock_server_available()) {
    SKIP("Mock server not running");
  }

  ResolumeWsClient client;
  client.connect("ws://127.0.0.1:8080/api/v1");

  // Wait for connection + initial state
  std::vector<IncomingMessage> messages;
  for (int i = 0; i < 40; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto batch = client.poll();
    messages.insert(messages.end(),
                    std::make_move_iterator(batch.begin()),
                    std::make_move_iterator(batch.end()));
    if (!messages.empty()) break;
  }

  REQUIRE(!messages.empty());
  REQUIRE(std::holds_alternative<CompositionState>(messages[0]));

  auto& cs = std::get<CompositionState>(messages[0]);
  auto comp = parse_composition(cs.data);
  REQUIRE(comp.name == "Probe Test");
  REQUIRE(comp.layers.size() == 3);
}

TEST_CASE("Subscribe and receive confirmation", "[ws_client][integration]") {
  if (!mock_server_available()) {
    SKIP("Mock server not running");
  }

  ResolumeWsClient client;
  client.connect("ws://127.0.0.1:8080/api/v1");

  // Wait for initial state
  for (int i = 0; i < 40; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!client.poll().empty()) break;
  }

  // Subscribe
  client.subscribe_by_id(1763903991101);

  // Wait for response
  std::vector<IncomingMessage> messages;
  for (int i = 0; i < 40; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto batch = client.poll();
    messages.insert(messages.end(),
                    std::make_move_iterator(batch.begin()),
                    std::make_move_iterator(batch.end()));
    if (!messages.empty()) break;
  }

  REQUIRE(!messages.empty());
  REQUIRE(std::holds_alternative<ParameterSubscribed>(messages[0]));

  auto& ps = std::get<ParameterSubscribed>(messages[0]);
  REQUIRE(ps.id == 1763903991101);
  REQUIRE(ps.valuetype == "ParamRange");
  REQUIRE(ps.path == "/composition/layers/1/video/opacity");
}

TEST_CASE("Set parameter and receive update", "[ws_client][integration]") {
  if (!mock_server_available()) {
    SKIP("Mock server not running");
  }

  ResolumeWsClient client;
  client.connect("ws://127.0.0.1:8080/api/v1");

  // Wait for initial state
  for (int i = 0; i < 40; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (!client.poll().empty()) break;
  }

  // Subscribe then set
  client.subscribe_by_id(1763903991101);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  client.poll(); // drain subscribe response

  client.set("/composition/layers/1/video/opacity", 1763903991101, 0.42);

  std::vector<IncomingMessage> messages;
  for (int i = 0; i < 40; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto batch = client.poll();
    messages.insert(messages.end(),
                    std::make_move_iterator(batch.begin()),
                    std::make_move_iterator(batch.end()));
    if (!messages.empty()) break;
  }

  REQUIRE(!messages.empty());
  REQUIRE(std::holds_alternative<ParameterUpdate>(messages[0]));
  auto& pu = std::get<ParameterUpdate>(messages[0]);
  REQUIRE(pu.value == 0.42);
}
