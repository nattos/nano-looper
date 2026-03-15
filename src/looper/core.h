#pragma once

#include <cmath>
#include <optional>
#include <vector>

namespace looper {

struct Event {
  double time;  // position in loop [0, loop_length)
  int channel;
};

class LooperCore {
public:
  explicit LooperCore(double loop_length, int num_channels = 4);

  // --- Configuration ---
  double loop_length() const { return loop_length_; }
  int num_channels() const { return num_channels_; }
  void set_quantize(std::optional<double> step);
  std::optional<double> quantize_step() const { return quantize_step_; }

  // --- Recording ---
  // Add an event at the given time for the given channel.
  // Quantizes the time (floor to step) if quantization is set.
  // If an event already exists at that position, this is a no-op.
  // Returns true if an event was added, false if it already existed.
  bool trigger(int channel, double current_time);

  // --- Playback ---
  // Returns channels that have events in the half-open range [prev_time, new_time),
  // handling loop wrap-around. Both times should be in [0, loop_length).
  std::vector<int> advance(double prev_time, double new_time) const;

  // --- Editing ---
  void clear_channel(int channel);
  void clear_all();

  // --- Destructive record ---
  // Saves current state, clears all events, enters record mode.
  // Individual trigger() calls during this mode do NOT push undo steps.
  // When end_destructive_record() is called, the pre-record state becomes
  // one undo step (so undo restores everything before the destructive record).
  void begin_destructive_record();
  void end_destructive_record();
  bool is_destructive_recording() const { return destructive_recording_; }

  // --- Undo/Redo ---
  void undo();
  void redo();
  bool can_undo() const { return !undo_stack_.empty(); }
  bool can_redo() const { return !redo_stack_.empty(); }

  // --- Inspection ---
  const std::vector<Event>& events() const { return events_; }
  std::vector<Event> events_for_channel(int channel) const;

private:
  double loop_length_;
  int num_channels_;
  std::optional<double> quantize_step_;

  std::vector<Event> events_;

  // Undo/redo
  std::vector<std::vector<Event>> undo_stack_;
  std::vector<std::vector<Event>> redo_stack_;

  // Destructive record state
  bool destructive_recording_ = false;
  std::vector<Event> pre_record_snapshot_;

  void push_undo();
  double quantize(double time) const;
  double wrap(double time) const;

  static constexpr double kEpsilon = 1e-6;
};

} // namespace looper
