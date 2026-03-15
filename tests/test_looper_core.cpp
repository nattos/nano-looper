#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_vector.hpp>

#include "looper/core.h"

using namespace looper;
using Catch::Matchers::UnorderedEquals;

// --- Basic recording ---

TEST_CASE("Trigger adds an event", "[looper]") {
  LooperCore lp(16.0, 4);
  bool added = lp.trigger(0, 1.0);
  REQUIRE(added);
  REQUIRE(lp.events().size() == 1);
  REQUIRE(lp.events()[0].channel == 0);
  REQUIRE(lp.events()[0].time == Catch::Approx(1.0));
}

TEST_CASE("Duplicate trigger at same position is a no-op", "[looper]") {
  LooperCore lp(16.0, 4);
  REQUIRE(lp.trigger(0, 1.0));
  REQUIRE_FALSE(lp.trigger(0, 1.0)); // already exists
  REQUIRE(lp.events().size() == 1);
}

TEST_CASE("Duplicate does not push undo step", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 1.0);
  REQUIRE(lp.can_undo());
  lp.undo(); // back to empty
  REQUIRE(lp.events().empty());

  lp.redo(); // back to 1 event
  lp.trigger(0, 1.0); // duplicate — no-op
  // Undo should go back to empty (the redo state), not to something else
  lp.undo();
  REQUIRE(lp.events().empty());
}

TEST_CASE("Same position different channels are both recorded", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 1.0);
  lp.trigger(1, 1.0);
  REQUIRE(lp.events().size() == 2);
}

TEST_CASE("Multiple events on same channel at different times", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 0.0);
  lp.trigger(0, 1.0);
  lp.trigger(0, 2.0);
  REQUIRE(lp.events().size() == 3);
}

TEST_CASE("events_for_channel filters correctly", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 0.0);
  lp.trigger(1, 1.0);
  lp.trigger(0, 2.0);
  lp.trigger(2, 3.0);

  REQUIRE(lp.events_for_channel(0).size() == 2);
  REQUIRE(lp.events_for_channel(1).size() == 1);
  REQUIRE(lp.events_for_channel(3).empty());
}

// --- Time wrapping ---

TEST_CASE("Time wraps around loop length", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 17.0); // wraps to 1.0
  REQUIRE(lp.events()[0].time == Catch::Approx(1.0));
}

TEST_CASE("Negative time wraps correctly", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, -1.0); // wraps to 15.0
  REQUIRE(lp.events()[0].time == Catch::Approx(15.0));
}

// --- Quantization (floor to step) ---

TEST_CASE("Quantization floors to grid", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.set_quantize(1.0);

  lp.trigger(0, 0.3); // floors to 0.0
  lp.trigger(1, 0.7); // floors to 0.0
  lp.trigger(2, 1.9); // floors to 1.0
  lp.trigger(3, 3.8); // floors to 3.0

  REQUIRE(lp.events().size() == 4);
  REQUIRE(lp.events()[0].time == Catch::Approx(0.0));
  REQUIRE(lp.events()[1].time == Catch::Approx(0.0));
  REQUIRE(lp.events()[2].time == Catch::Approx(1.0));
  REQUIRE(lp.events()[3].time == Catch::Approx(3.0));
}

TEST_CASE("Quantize deduplicates within same step", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.set_quantize(1.0);

  REQUIRE(lp.trigger(0, 0.3));  // floors to 0.0, added
  REQUIRE_FALSE(lp.trigger(0, 0.7)); // also floors to 0.0, duplicate
  REQUIRE(lp.events().size() == 1);
}

TEST_CASE("Retroactive recording: trigger after entering step records to current step", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.set_quantize(1.0);

  // Phase is 5.7 — we're well into step 5 — should still record at 5.0
  lp.trigger(0, 5.7);
  REQUIRE(lp.events()[0].time == Catch::Approx(5.0));
}

TEST_CASE("Sub-beat quantization (sixteenth notes within a beat)", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.set_quantize(0.25);

  lp.trigger(0, 0.13); // floors to 0.0
  lp.trigger(1, 0.30); // floors to 0.25
  lp.trigger(2, 0.63); // floors to 0.5

  REQUIRE(lp.events()[0].time == Catch::Approx(0.0));
  REQUIRE(lp.events()[1].time == Catch::Approx(0.25));
  REQUIRE(lp.events()[2].time == Catch::Approx(0.5));
}

TEST_CASE("No quantization uses raw time", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 1.234);
  REQUIRE(lp.events()[0].time == Catch::Approx(1.234));
}

// --- Playback (advance) ---

TEST_CASE("Advance fires events in range", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 1.0);
  lp.trigger(1, 2.0);
  lp.trigger(2, 3.0);

  auto fired = lp.advance(0.5, 1.5);
  REQUIRE(fired.size() == 1);
  REQUIRE(fired[0] == 0);
}

TEST_CASE("Advance fires multiple events", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 1.0);
  lp.trigger(1, 1.5);

  auto fired = lp.advance(0.5, 2.0);
  REQUIRE_THAT(fired, UnorderedEquals(std::vector<int>{0, 1}));
}

TEST_CASE("Advance handles wrap-around", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 0.5);
  lp.trigger(1, 15.5);

  auto fired = lp.advance(15.0, 1.0);
  REQUIRE_THAT(fired, UnorderedEquals(std::vector<int>{0, 1}));
}

TEST_CASE("Advance does not fire events outside range", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 2.0);
  REQUIRE(lp.advance(0.0, 1.0).empty());
}

TEST_CASE("Advance with no events returns empty", "[looper]") {
  LooperCore lp(16.0, 4);
  REQUIRE(lp.advance(0.0, 2.0).empty());
}

// --- Clear ---

TEST_CASE("Clear channel removes only that channel", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 1.0);
  lp.trigger(1, 2.0);
  lp.trigger(0, 3.0);

  lp.clear_channel(0);
  REQUIRE(lp.events().size() == 1);
  REQUIRE(lp.events()[0].channel == 1);
}

TEST_CASE("Clear all removes everything", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 1.0);
  lp.trigger(1, 2.0);
  lp.trigger(2, 3.0);

  lp.clear_all();
  REQUIRE(lp.events().empty());
}

// --- Undo/Redo ---

TEST_CASE("Undo reverses trigger", "[looper][undo]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 1.0);
  REQUIRE(lp.events().size() == 1);

  lp.undo();
  REQUIRE(lp.events().empty());
}

TEST_CASE("Redo re-applies trigger", "[looper][undo]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 1.0);
  lp.undo();
  REQUIRE(lp.events().empty());

  lp.redo();
  REQUIRE(lp.events().size() == 1);
  REQUIRE(lp.events()[0].channel == 0);
}

TEST_CASE("Undo reverses clear_channel", "[looper][undo]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 1.0);
  lp.trigger(1, 2.0);
  lp.trigger(0, 3.0);
  lp.clear_channel(0);
  REQUIRE(lp.events().size() == 1);

  lp.undo();
  REQUIRE(lp.events().size() == 3);
}

TEST_CASE("Undo reverses clear_all", "[looper][undo]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 1.0);
  lp.trigger(1, 2.0);
  lp.clear_all();
  REQUIRE(lp.events().empty());

  lp.undo();
  REQUIRE(lp.events().size() == 2);
}

TEST_CASE("Multiple undos in sequence", "[looper][undo]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 1.0);
  lp.trigger(1, 2.0);
  lp.trigger(2, 3.0);

  lp.undo();
  REQUIRE(lp.events().size() == 2);
  lp.undo();
  REQUIRE(lp.events().size() == 1);
  lp.undo();
  REQUIRE(lp.events().empty());
  lp.undo(); // no-op
  REQUIRE(lp.events().empty());
}

TEST_CASE("New action clears redo stack", "[looper][undo]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 1.0);
  lp.trigger(1, 2.0);
  lp.undo();
  REQUIRE(lp.can_redo());

  lp.trigger(2, 3.0);
  REQUIRE_FALSE(lp.can_redo());
}

TEST_CASE("can_undo and can_redo report correctly", "[looper][undo]") {
  LooperCore lp(16.0, 4);
  REQUIRE_FALSE(lp.can_undo());
  REQUIRE_FALSE(lp.can_redo());

  lp.trigger(0, 1.0);
  REQUIRE(lp.can_undo());
  REQUIRE_FALSE(lp.can_redo());

  lp.undo();
  REQUIRE_FALSE(lp.can_undo());
  REQUIRE(lp.can_redo());
}

// --- Destructive record ---

TEST_CASE("Destructive record clears events and records new ones", "[looper][destructive]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 1.0);
  lp.trigger(1, 2.0);
  REQUIRE(lp.events().size() == 2);

  lp.begin_destructive_record();
  REQUIRE(lp.events().empty());
  REQUIRE(lp.is_destructive_recording());

  lp.trigger(2, 0.5);
  lp.trigger(3, 1.5);
  REQUIRE(lp.events().size() == 2);

  lp.end_destructive_record();
  REQUIRE_FALSE(lp.is_destructive_recording());
  REQUIRE(lp.events().size() == 2);
  REQUIRE(lp.events()[0].channel == 2);
}

TEST_CASE("Undo destructive record restores pre-record state", "[looper][destructive]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 1.0);
  lp.trigger(1, 2.0);

  lp.begin_destructive_record();
  lp.trigger(2, 0.5);
  lp.trigger(3, 1.5);
  lp.end_destructive_record();

  lp.undo();
  REQUIRE(lp.events().size() == 2);
  REQUIRE(lp.events()[0].channel == 0);
  REQUIRE(lp.events()[1].channel == 1);
}

TEST_CASE("Redo after undo destructive record restores recorded events", "[looper][destructive]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 1.0);

  lp.begin_destructive_record();
  lp.trigger(2, 0.5);
  lp.end_destructive_record();

  lp.undo();
  REQUIRE(lp.events().size() == 1);
  REQUIRE(lp.events()[0].channel == 0);

  lp.redo();
  REQUIRE(lp.events().size() == 1);
  REQUIRE(lp.events()[0].channel == 2);
}

TEST_CASE("Triggers during destructive record do not push individual undo steps", "[looper][destructive]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 1.0);

  lp.begin_destructive_record();
  lp.trigger(2, 0.5);
  lp.trigger(3, 1.0);
  lp.trigger(3, 2.0);
  lp.end_destructive_record();

  lp.undo(); // one step back → pre-record
  REQUIRE(lp.events().size() == 1);
  REQUIRE(lp.events()[0].channel == 0);

  lp.undo(); // undo the original trigger
  REQUIRE(lp.events().empty());
}

TEST_CASE("Destructive record with no prior events", "[looper][destructive]") {
  LooperCore lp(16.0, 4);

  lp.begin_destructive_record();
  lp.trigger(0, 1.0);
  lp.end_destructive_record();

  REQUIRE(lp.events().size() == 1);

  lp.undo();
  REQUIRE(lp.events().empty());
}

// --- Edge cases ---

TEST_CASE("Event at time 0", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.trigger(0, 0.0);
  REQUIRE(lp.events()[0].time == Catch::Approx(0.0));

  auto fired = lp.advance(15.5, 0.5); // wrap around, should fire
  REQUIRE(fired.size() == 1);
}

TEST_CASE("Event near end of loop stays in range", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.set_quantize(1.0);
  lp.trigger(0, 15.3); // floors to 15.0
  REQUIRE(lp.events()[0].time == Catch::Approx(15.0));
}

TEST_CASE("Empty looper operations are safe", "[looper]") {
  LooperCore lp(16.0, 4);
  lp.clear_all();
  lp.clear_channel(0);
  lp.undo();
  lp.redo();
  REQUIRE(lp.events().empty());
}
