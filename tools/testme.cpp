#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "resolume/composition.h"
#include "resolume/protocol.h"
#include "resolume/ws_client.h"

static void drain_and_print(resolume::ResolumeWsClient& client) {
  auto msgs = client.poll();
  if (msgs.empty()) {
    printf("  (no messages)\n");
    return;
  }
  for (auto& m : msgs) {
    if (auto* pu = std::get_if<resolume::ParameterUpdate>(&m)) {
      printf("  [update] id=%lld path=%s valuetype=%s value=%s\n",
             (long long)pu->id, pu->path.c_str(), pu->valuetype.c_str(),
             pu->value.dump().c_str());
    } else if (auto* ps = std::get_if<resolume::ParameterSubscribed>(&m)) {
      printf("  [subscribed] id=%lld path=%s valuetype=%s value=%s\n",
             (long long)ps->id, ps->path.c_str(), ps->valuetype.c_str(),
             ps->value.dump().c_str());
    } else if (auto* err = std::get_if<resolume::ErrorMessage>(&m)) {
      printf("  [error] %s", err->error.c_str());
      if (err->path) printf(" (path=%s)", err->path->c_str());
      printf("\n");
    } else if (std::get_if<resolume::CompositionState>(&m)) {
      printf("  [composition state]\n");
    }
  }
}

int main(int argc, char* argv[]) {
  std::string url = "ws://127.0.0.1:8080/api/v1";
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--url" && i + 1 < argc) {
      url = argv[++i];
    }
  }

  printf("Connecting to %s...\n", url.c_str());

  resolume::ResolumeWsClient client;
  client.connect(url);

  for (int i = 0; i < 40; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (client.is_connected()) break;
  }
  if (!client.is_connected()) {
    fprintf(stderr, "Failed to connect to %s\n", url.c_str());
    return 1;
  }
  printf("Connected!\n");

  resolume::Composition comp;
  for (int i = 0; i < 40; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    auto messages = client.poll();
    for (auto& msg : messages) {
      if (auto* cs = std::get_if<resolume::CompositionState>(&msg)) {
        comp = resolume::parse_composition(cs->data);
        printf("Received composition: \"%s\"\n", comp.name.c_str());
        printf("  Layers: %zu\n", comp.layers.size());
        goto got_state;
      }
    }
  }
  fprintf(stderr, "Timed out waiting for initial state\n");
  return 1;

got_state:
  for (size_t li = 0; li < comp.layers.size(); ++li) {
    auto& layer = comp.layers[li];
    printf("  Layer %zu: \"%s\" (%zu clips)\n", li + 1, layer.name.c_str(),
           layer.clips.size());
    for (size_t ci = 0; ci < layer.clips.size(); ++ci) {
      auto& clip = layer.clips[ci];
      printf("    Clip %zu: \"%s\" [%s] (id=%lld, connected_id=%lld)\n", ci + 1,
             clip.name.c_str(), clip.connected_state.c_str(),
             (long long)clip.id, (long long)clip.connected_id);
    }
  }

  for (auto& layer : comp.layers) {
    for (size_t ci = 0; ci < layer.clips.size(); ++ci) {
      auto& clip = layer.clips[ci];
      if (clip.connected_state == "Disconnected") {
        printf("\nFound clip \"%s\" on layer \"%s\"\n",
               clip.name.c_str(), layer.name.c_str());

        // Subscribe to the connected param to receive updates
        printf("\n[1] Subscribe to connected_id=%lld\n",
               (long long)clip.connected_id);
        client.subscribe_by_id(clip.connected_id);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        drain_and_print(client);

        // Trigger using /composition/clips/by-id/<clip.id>/connect
        // Must send true then false (pulse)
        std::string trigger_path = "/composition/clips/by-id/" +
                                   std::to_string(clip.id) + "/connect";

        printf("\n[2] trigger true on %s\n", trigger_path.c_str());
        client.trigger(trigger_path, true);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        printf("[3] trigger false on %s\n", trigger_path.c_str());
        client.trigger(trigger_path, false);

        printf("  Waiting 5 seconds for updates...\n");
        std::this_thread::sleep_for(std::chrono::seconds(5));
        drain_and_print(client);

        goto done;
      }
    }
  }
  printf("\nNo disconnected clips found to trigger.\n");

done:
  printf("\nDone. Disconnecting.\n");
  client.disconnect();
  return 0;
}
