#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <nlohmann/json.hpp>

#include "resolume/composition.h"

using namespace resolume;

// Helper to load the fixture
static nlohmann::json load_fixture() {
  std::ifstream f(FIXTURE_PATH);
  REQUIRE(f.is_open());
  return nlohmann::json::parse(f);
}

TEST_CASE("Parse composition from fixture", "[composition]") {
  auto state = load_fixture();
  auto comp = parse_composition(state);

  REQUIRE(comp.name == "Probe Test");
  REQUIRE(comp.layers.size() == 3);
}

TEST_CASE("Parse layer details", "[composition]") {
  auto state = load_fixture();
  auto comp = parse_composition(state);

  auto& layer = comp.layers[0];
  REQUIRE(layer.clips.size() == 9);
  REQUIRE(layer.video_opacity.valuetype == "ParamRange");
  REQUIRE(layer.video_opacity.id == 1763903991101);
}

TEST_CASE("Parse clip details", "[composition]") {
  auto state = load_fixture();
  auto comp = parse_composition(state);

  auto& clip = comp.layers[0].clips[0];
  REQUIRE(clip.connected_state == "Empty");
  REQUIRE(clip.connected_id == 1763903990340);
}

TEST_CASE("Parse layer 3 with name", "[composition]") {
  auto state = load_fixture();
  auto comp = parse_composition(state);

  auto& layer = comp.layers[2];
  REQUIRE(layer.name == "Layer 3 With Name");

  auto& clip = layer.clips[0];
  REQUIRE(clip.connected_state == "Disconnected");
}

TEST_CASE("Parse parameter", "[composition]") {
  auto j = nlohmann::json{
      {"id", 12345},
      {"valuetype", "ParamRange"},
      {"value", 0.5},
      {"min", 0.0},
      {"max", 1.0},
  };
  auto p = parse_parameter(j);
  REQUIRE(p.id == 12345);
  REQUIRE(p.valuetype == "ParamRange");
  REQUIRE(p.value == 0.5);
  REQUIRE(p.min.value() == 0.0);
  REQUIRE(p.max.value() == 1.0);
}

TEST_CASE("Parse ParamChoice parameter", "[composition]") {
  auto j = nlohmann::json{
      {"id", 99},
      {"valuetype", "ParamChoice"},
      {"value", "Normal"},
      {"index", 0},
      {"options", {"Normal", "Piano", "Toggle"}},
  };
  auto p = parse_parameter(j);
  REQUIRE(p.valuetype == "ParamChoice");
  REQUIRE(p.options.size() == 3);
  REQUIRE(p.options[0] == "Normal");
}
