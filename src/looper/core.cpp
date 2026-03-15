#include "core.h"

#include <algorithm>
#include <cmath>

namespace looper {

LooperCore::LooperCore(double loop_length, int num_channels)
    : loop_length_(loop_length), num_channels_(num_channels) {}

void LooperCore::set_quantize(std::optional<double> step) {
  quantize_step_ = step;
}

double LooperCore::wrap(double time) const {
  double t = std::fmod(time, loop_length_);
  if (t < 0) t += loop_length_;
  return t;
}

double LooperCore::quantize(double time) const {
  double t = wrap(time);
  if (quantize_step_ && *quantize_step_ > 0) {
    double step = *quantize_step_;
    t = std::floor(t / step) * step;
  }
  return t;
}

void LooperCore::push_undo() {
  undo_stack_.push_back(events_);
  redo_stack_.clear();
}

bool LooperCore::trigger(int channel, double current_time) {
  double t = quantize(current_time);

  // Skip if an event already exists at this time/channel
  for (const auto& e : events_) {
    if (e.channel == channel && std::abs(e.time - t) < kEpsilon)
      return false;
  }

  if (!destructive_recording_) {
    push_undo();
  }

  events_.push_back({t, channel});
  return true;
}

std::vector<int> LooperCore::advance(double prev_time, double new_time) const {
  std::vector<int> fired;

  for (const auto& e : events_) {
    bool in_range;
    if (new_time >= prev_time) {
      // Normal: no wrap
      in_range = e.time >= prev_time - kEpsilon &&
                 e.time < new_time - kEpsilon;
    } else {
      // Wrapped: [prev_time, loop_length) ∪ [0, new_time)
      in_range = (e.time >= prev_time - kEpsilon) ||
                 (e.time < new_time - kEpsilon);
    }
    if (in_range) {
      fired.push_back(e.channel);
    }
  }

  return fired;
}

void LooperCore::clear_channel(int channel) {
  push_undo();
  events_.erase(
      std::remove_if(events_.begin(), events_.end(),
                     [channel](const Event& e) { return e.channel == channel; }),
      events_.end());
}

void LooperCore::clear_all() {
  push_undo();
  events_.clear();
}

void LooperCore::begin_destructive_record() {
  pre_record_snapshot_ = events_;
  events_.clear();
  destructive_recording_ = true;
}

void LooperCore::end_destructive_record() {
  if (!destructive_recording_) return;
  destructive_recording_ = false;
  // The pre-record snapshot becomes one undo step
  undo_stack_.push_back(std::move(pre_record_snapshot_));
  redo_stack_.clear();
}

void LooperCore::undo() {
  if (undo_stack_.empty()) return;
  redo_stack_.push_back(std::move(events_));
  events_ = std::move(undo_stack_.back());
  undo_stack_.pop_back();
}

void LooperCore::redo() {
  if (redo_stack_.empty()) return;
  undo_stack_.push_back(std::move(events_));
  events_ = std::move(redo_stack_.back());
  redo_stack_.pop_back();
}

std::vector<Event> LooperCore::events_for_channel(int channel) const {
  std::vector<Event> result;
  for (const auto& e : events_) {
    if (e.channel == channel) result.push_back(e);
  }
  return result;
}

} // namespace looper
