#pragma once

#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <ixwebsocket/IXWebSocket.h>

#include "protocol.h"

namespace resolume {

class ResolumeWsClient {
public:
  ResolumeWsClient();
  ~ResolumeWsClient();

  // Not copyable or movable (owns a background thread)
  ResolumeWsClient(const ResolumeWsClient&) = delete;
  ResolumeWsClient& operator=(const ResolumeWsClient&) = delete;

  void connect(const std::string& url);
  void disconnect();
  bool is_connected() const;

  // Send messages (thread-safe)
  void subscribe(const std::string& parameter_path);
  void subscribe_by_id(int64_t id);
  void set(const std::string& path, int64_t id, const nlohmann::json& value);
  void trigger(const std::string& path, bool value = true);

  // Convenience: trigger a clip connect by clip ID.
  // Sends the true/false pulse that Resolume requires.
  void trigger_clip(int64_t clip_id);

  // Poll for received messages (call from main thread).
  // Drains the internal queue and returns all pending messages.
  std::vector<IncomingMessage> poll();

  // Optional callback invoked on the WebSocket thread when a message arrives.
  // Use with care — this runs on the IXWebSocket background thread.
  void set_on_message(std::function<void(const IncomingMessage&)> callback);

private:
  void send_json(const nlohmann::json& j);

  ix::WebSocket ws_;
  mutable std::mutex inbox_mutex_;
  std::deque<IncomingMessage> inbox_;
  std::function<void(const IncomingMessage&)> on_message_;
};

} // namespace resolume
