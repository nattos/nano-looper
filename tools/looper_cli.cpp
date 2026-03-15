#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "looper/core.h"
#include "resolume/composition.h"
#include "resolume/protocol.h"
#include "resolume/ws_client.h"

static constexpr int NUM_CHANNELS = 4;
static constexpr int NUM_STEPS = 16;
static constexpr int TARGET_LAYER = 1; // 0-indexed → "Layer 2"

// Timeout for detecting key release (no keyup events in terminal).
// When a held key stops repeating for this long, we treat it as released.
static constexpr double HOLD_TIMEOUT = 0.5;

// --- Terminal raw mode (RAII) ---

struct RawTerminal {
  termios original;

  RawTerminal() {
    tcgetattr(STDIN_FILENO, &original);
    termios raw = original;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    printf("\033[?25l"); // hide cursor
    printf("\033[2J");   // clear screen
    fflush(stdout);
  }

  ~RawTerminal() {
    printf("\033[?25h\033[0m\n"); // show cursor, reset colors
    tcsetattr(STDIN_FILENO, TCSANOW, &original);
    fflush(stdout);
  }
};

static int read_key() {
  char c;
  return (read(STDIN_FILENO, &c, 1) == 1) ? (unsigned char)c : -1;
}

// --- ANSI color helpers ---

static const char* ch_color(int ch) {
  static const char* c[] = {"\033[91m", "\033[92m", "\033[93m", "\033[96m"};
  return c[ch & 3];
}

static const char* ch_color_bold(int ch) {
  static const char* c[] = {"\033[91;1m", "\033[92;1m", "\033[93;1m", "\033[96;1m"};
  return c[ch & 3];
}

// --- App state ---

struct ClipInfo {
  int64_t clip_id = 0;
  int64_t connected_id = 0;
  std::string name;
  std::string connected_state = "Empty";
};

struct App {
  resolume::ResolumeWsClient client;
  looper::LooperCore looper{(double)NUM_STEPS, NUM_CHANNELS};

  ClipInfo clips[NUM_CHANNELS];
  double bpm = 120.0;
  int64_t tempo_id = 0;

  double phase = 0.0;
  std::chrono::steady_clock::time_point last_tick;

  // Momentary modifier keys (held detection via timeout)
  bool delete_held = false;
  double delete_timer = 0.0;
  bool delete_acted = false; // true if 1-4 pressed during this hold

  bool mute_held = false;
  double mute_timer = 0.0;

  bool muted[NUM_CHANNELS] = {};
  double flash[NUM_CHANNELS] = {};
  bool running = true;
  bool ws_connected = false;
};

// --- Composition parsing ---

static void setup_from_state(App& app, const nlohmann::json& state) {
  auto comp = resolume::parse_composition(state);

  for (int i = 0; i < NUM_CHANNELS; ++i) app.clips[i] = {};

  if ((int)comp.layers.size() > TARGET_LAYER) {
    auto& layer = comp.layers[TARGET_LAYER];
    for (int i = 0; i < NUM_CHANNELS && i < (int)layer.clips.size(); ++i) {
      app.clips[i].clip_id = layer.clips[i].id;
      app.clips[i].connected_id = layer.clips[i].connected_id;
      app.clips[i].name = layer.clips[i].name;
      app.clips[i].connected_state = layer.clips[i].connected_state;
    }
  }

  if (state.contains("tempocontroller") &&
      state["tempocontroller"].contains("tempo")) {
    auto& tempo = state["tempocontroller"]["tempo"];
    if (tempo.contains("id")) app.tempo_id = tempo["id"].get<int64_t>();
    if (tempo.contains("value")) app.bpm = tempo["value"].get<double>();
  }
}

static void subscribe_params(App& app) {
  for (int i = 0; i < NUM_CHANNELS; ++i) {
    if (app.clips[i].connected_id != 0)
      app.client.subscribe_by_id(app.clips[i].connected_id);
  }
  if (app.tempo_id != 0)
    app.client.subscribe_by_id(app.tempo_id);
}

// --- Message processing ---

static void process_messages(App& app) {
  for (auto& msg : app.client.poll()) {
    if (auto* cs = std::get_if<resolume::CompositionState>(&msg)) {
      setup_from_state(app, cs->data);
      subscribe_params(app);
    } else if (auto* pu = std::get_if<resolume::ParameterUpdate>(&msg)) {
      if (pu->id == app.tempo_id && pu->value.is_number())
        app.bpm = pu->value.get<double>();
      for (int i = 0; i < NUM_CHANNELS; ++i) {
        if (pu->id == app.clips[i].connected_id && pu->value.is_string())
          app.clips[i].connected_state = pu->value.get<std::string>();
      }
    }
  }
}

// --- Input ---

static void process_input(App& app, double dt) {
  // Drain all available keys this frame
  bool saw_d = false, saw_m = false;
  std::vector<int> keys;

  for (;;) {
    int ch = read_key();
    if (ch < 0) break;
    if (ch == 'd')
      saw_d = true;
    else if (ch == 'm')
      saw_m = true;
    else
      keys.push_back(ch);
  }

  // --- Update delete hold state ---
  if (saw_d) {
    if (!app.delete_held) {
      app.delete_held = true;
      app.delete_acted = false;
    }
    app.delete_timer = 0.0;
  }
  if (app.delete_held) {
    app.delete_timer += dt;
    if (app.delete_timer > HOLD_TIMEOUT) {
      // Key released — if nothing was acted on, clear all
      if (!app.delete_acted)
        app.looper.clear_all();
      app.delete_held = false;
    }
  }

  // --- Update mute hold state ---
  if (saw_m) {
    if (!app.mute_held) app.mute_held = true;
    app.mute_timer = 0.0;
  }
  if (app.mute_held) {
    app.mute_timer += dt;
    if (app.mute_timer > HOLD_TIMEOUT)
      app.mute_held = false;
  }

  // --- Process other keys ---
  for (int ch : keys) {
    if (ch == 'q' || ch == 27) {
      app.running = false;
      return;
    }

    if (ch >= '1' && ch <= '4') {
      int channel = ch - '1';

      if (app.delete_held) {
        app.looper.clear_channel(channel);
        app.delete_acted = true;
      } else if (app.mute_held) {
        app.muted[channel] = !app.muted[channel];
      } else {
        // Normal trigger
        app.looper.trigger(channel, app.phase);
        if (app.clips[channel].clip_id != 0 && !app.muted[channel])
          app.client.trigger_clip(app.clips[channel].clip_id);
        app.flash[channel] = 0.25;
      }
    }

    if (ch == 'z') app.looper.undo();
    if (ch == 'x') app.looper.redo();

    if (ch == 'r') {
      if (app.looper.is_destructive_recording())
        app.looper.end_destructive_record();
      else
        app.looper.begin_destructive_record();
    }
  }
}

// --- Render ---

static void render(const App& app) {
  printf("\033[H"); // cursor home

  // Title bar
  printf("  \033[1mLooper\033[0m");
  if (!app.ws_connected)
    printf("  \033[90m(connecting...)\033[0m");
  else if (app.looper.is_destructive_recording())
    printf("  \033[91;1m● REC\033[0m");
  printf("  \033[90m%.0f BPM\033[0m", app.bpm);
  printf("\033[K\n\n");

  // Clip info
  for (int i = 0; i < NUM_CHANNELS; ++i) {
    const char* color;
    const char* ind;
    if (app.clips[i].connected_state == "Connected") {
      color = "\033[92m"; ind = "●";
    } else if (app.clips[i].connected_state == "Disconnected") {
      color = "\033[93m"; ind = "○";
    } else {
      color = "\033[90m"; ind = "·";
    }

    std::string name = app.clips[i].name;
    if (name.empty()) name = "(empty)";
    if (name.length() > 24) name = name.substr(0, 22) + "..";

    printf("  %s%s %d: %-24s\033[0m", color, ind, i + 1, name.c_str());
    if (app.muted[i])
      printf(" \033[90;7m MUTE \033[0m");
    printf("\033[K\n");
  }

  int current_step = (int)std::floor(app.phase);
  if (current_step >= NUM_STEPS) current_step = 0;

  printf("\033[K\n");

  // Grid: 4 rows × 16 columns
  for (int ch = 0; ch < NUM_CHANNELS; ++ch) {
    bool is_muted = app.muted[ch];
    printf("  %s%d\033[0m ", is_muted ? "\033[90m" : ch_color(ch), ch + 1);

    auto events = app.looper.events_for_channel(ch);

    for (int s = 0; s < NUM_STEPS; ++s) {
      bool has_event = false;
      for (const auto& e : events) {
        if ((int)std::floor(e.time) == s) { has_event = true; break; }
      }

      bool cur = (s == current_step);

      if (has_event && cur) {
        if (is_muted)
          printf("\033[90;100m█\033[0m ");
        else
          printf("%s\033[105m█\033[0m ", ch_color_bold(ch));
      } else if (has_event) {
        if (is_muted)
          printf("\033[90m█\033[0m ");
        else
          printf("%s█\033[0m ", ch_color(ch));
      } else if (cur) {
        printf("\033[37;100m·\033[0m ");
      } else {
        printf("\033[90m·\033[0m ");
      }
    }
    printf("\033[K\n");
  }

  printf("\033[K\n");

  // Trigger row: channel indicators + modifiers
  printf("  ");
  for (int i = 0; i < NUM_CHANNELS; ++i) {
    if (app.flash[i] > 0)
      printf("%s%d\033[0m  ", ch_color_bold(i), i + 1);
    else if (app.muted[i])
      printf("\033[90;9m%d\033[0m  ", i + 1); // strikethrough
    else
      printf("\033[90m%d\033[0m  ", i + 1);
  }

  printf("   ");
  if (app.delete_held)
    printf("\033[91;1;7m D \033[0m");
  else
    printf("\033[90m D \033[0m");

  printf(" ");
  if (app.mute_held)
    printf("\033[93;1;7m M \033[0m");
  else
    printf("\033[90m M \033[0m");

  printf("\033[K\n");

  // Help
  printf("  \033[90mq=quit z=undo x=redo r=rec\033[0m\033[K\n");

  printf("\033[J"); // clear below
  fflush(stdout);
}

// --- Main ---

int main(int argc, char* argv[]) {
  std::string url = "ws://127.0.0.1:8080/api/v1";
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--url" && i + 1 < argc)
      url = argv[++i];
  }

  App app;
  app.looper.set_quantize(1.0);
  app.last_tick = std::chrono::steady_clock::now();

  RawTerminal term;

  app.client.connect(url);

  while (app.running) {
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - app.last_tick).count();
    app.last_tick = now;
    if (dt > 0.1) dt = 0.1;

    app.ws_connected = app.client.is_connected();

    // Process incoming WS messages
    process_messages(app);

    // Advance looper phase and fire playback events
    double prev = app.phase;
    double sixteenths_per_sec = (app.bpm / 60.0) * 4.0;
    app.phase = std::fmod(app.phase + sixteenths_per_sec * dt, (double)NUM_STEPS);
    for (int ch : app.looper.advance(prev, app.phase)) {
      if (ch >= 0 && ch < NUM_CHANNELS &&
          app.clips[ch].clip_id != 0 && !app.muted[ch])
        app.client.trigger_clip(app.clips[ch].clip_id);
    }

    // Process keyboard (after advance to avoid same-frame double-trigger)
    process_input(app, dt);

    // Decay flash timers
    for (int i = 0; i < NUM_CHANNELS; ++i)
      app.flash[i] = std::max(0.0, app.flash[i] - dt);

    render(app);

    std::this_thread::sleep_for(std::chrono::milliseconds(33));
  }

  return 0;
}
