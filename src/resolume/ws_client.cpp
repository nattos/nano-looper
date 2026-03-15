#include "ws_client.h"

#include <nlohmann/json.hpp>

namespace resolume {

ResolumeWsClient::ResolumeWsClient() = default;

ResolumeWsClient::~ResolumeWsClient() { disconnect(); }

void ResolumeWsClient::connect(const std::string& url) {
  ws_.setUrl(url);
  ws_.setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
    if (msg->type == ix::WebSocketMessageType::Message) {
      try {
        auto j = nlohmann::json::parse(msg->str);
        auto incoming = parse_incoming(j);

        if (on_message_) {
          on_message_(incoming);
        }

        std::lock_guard<std::mutex> lock(inbox_mutex_);
        inbox_.push_back(std::move(incoming));
      } catch (const nlohmann::json::exception& e) {
        ErrorMessage err{"JSON parse error: " + std::string(e.what()), std::nullopt};
        std::lock_guard<std::mutex> lock(inbox_mutex_);
        inbox_.push_back(std::move(err));
      }
    }
  });
  ws_.start();
}

void ResolumeWsClient::disconnect() {
  ws_.stop();
}

bool ResolumeWsClient::is_connected() const {
  return ws_.getReadyState() == ix::ReadyState::Open;
}

void ResolumeWsClient::subscribe(const std::string& parameter_path) {
  send_json(to_json(SubscribeMessage{parameter_path}));
}

void ResolumeWsClient::subscribe_by_id(int64_t id) {
  subscribe("/parameter/by-id/" + std::to_string(id));
}

void ResolumeWsClient::set(const std::string& path, int64_t id,
                           const nlohmann::json& value) {
  send_json(to_json(SetMessage{path, id, value}));
}

void ResolumeWsClient::trigger(const std::string& path, bool value) {
  send_json(to_json(TriggerMessage{path, value}));
}

void ResolumeWsClient::trigger_clip(int64_t clip_id) {
  std::string path =
      "/composition/clips/by-id/" + std::to_string(clip_id) + "/connect";
  trigger(path, true);
  trigger(path, false);
}

std::vector<IncomingMessage> ResolumeWsClient::poll() {
  std::lock_guard<std::mutex> lock(inbox_mutex_);
  std::vector<IncomingMessage> result(
      std::make_move_iterator(inbox_.begin()),
      std::make_move_iterator(inbox_.end()));
  inbox_.clear();
  return result;
}

void ResolumeWsClient::set_on_message(
    std::function<void(const IncomingMessage&)> callback) {
  on_message_ = std::move(callback);
}

void ResolumeWsClient::send_json(const nlohmann::json& j) {
  ws_.send(j.dump());
}

} // namespace resolume
