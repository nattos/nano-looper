# NanoLooper

A 16-step looper for Resolume Arena. Trigger clips in rhythm, build up patterns, and perform live — all from within Resolume.

NanoLooper is an FFGL plugin that connects back to Resolume's own WebSocket API to trigger clips. It overlays a step sequencer grid on top of your composition and lets you record, loop, mute, and delete patterns in real time.

## What's in the box

NanoLooper ships as two FFGL plugin bundles. Both are required:

| Bundle | Purpose |
|--------|---------|
| **NanoLooper.bundle** | Main looper — sequencer logic, overlay UI, WebSocket connection |
| **NanoLooperCh.bundle** | Channel tag — lightweight effect you drop on clips to assign them to looper channels 1–4 |

## Setup

### 1. Install

Copy both `.bundle` files to Resolume's extra effects folder:

```
~/Documents/Resolume Arena/Extra Effects/
```

Restart Resolume. Both effects will appear in the effects list.

### 2. Tag your clips

Drop **"NanoLooper Ch"** as an effect on each clip you want the looper to control. Set the **Channel** dropdown to 1, 2, 3, or 4.

- You can tag clips on any layer — they don't need to be on the same one
- Multiple clips can share a channel — they'll all trigger together
- The first clip found for each channel provides the thumbnail and name shown in the overlay

### 3. Add the looper

Drop **"NanoLooper"** as an effect on any clip (it draws the overlay on top of whatever that clip is showing). The looper will connect to Resolume's WebSocket API at `ws://127.0.0.1:8080/api/v1`, discover your tagged clips, and display the sequencer grid.

The overlay shows a connection status indicator:
- Blue pulsing dot — connecting
- Green solid dot — connected
- Red pulsing dot — can't connect (check that Resolume's webserver is enabled in Preferences)

### 4. Map controls

In Resolume, map the looper's parameters to your MIDI controller, keyboard, or OSC:

| Parameter | Type | Description |
|-----------|------|-------------|
| Trigger 1–4 | Boolean | Fire channel (hold = key down, release = key up) |
| Delete | Boolean | Hold + release = clear all; hold + trigger 1–4 = clear channel |
| Mute | Boolean | Hold + hold trigger 1–4 = mute that channel (momentary) |
| Undo / Redo | Boolean | Step backward/forward through edit history |
| Record | Boolean | Hold to destructive-record (erases steps as playhead passes) |
| Show Overlay | Boolean | Toggle the visual overlay on/off |

All trigger parameters are **piano-key style**: press = true, release = false.

## How to play

### Simple: one-shot pattern

1. Press **1**, **2**, **3**, **4** on the beat to trigger clips and record steps
2. The looper records each press into the grid, quantized to the nearest 16th note
3. After one bar (16 steps), the pattern loops and triggers your clips automatically
4. Press **Delete** (tap and release) to clear everything and start over

### Building up: layered recording

1. Record a kick pattern on channel 1: press **1** on beats 1 and 3
2. Let it loop — you'll see the steps light up as they play
3. Add a snare on channel 2: press **2** on beats 2 and 4
4. Layer in hi-hats on channel 3: press **3** on every 16th note
5. Use **Undo** to step back if you make a mistake
6. Use **Delete + 3** to clear just the hi-hats and re-record them

### Live performance: mute and unmute

1. Build up a full 4-channel pattern
2. Hold **Mute + 1** to temporarily silence channel 1 — release to bring it back
3. Hold **Mute + 2 + 3** to mute multiple channels at once
4. Use muting to create breakdowns and builds during a performance
5. Muting is purely momentary — channels play again the instant you release

### Advanced: destructive re-recording

1. You have a pattern you mostly like, but want to redo channel 2
2. Hold **Delete + 2** to clear channel 2
3. Hold **Record** — the playhead now erases steps as it passes through them
4. While holding Record, press **2** to lay down new steps in real time
5. Release **Record** to stop erasing
6. If you don't like it, **Undo** restores everything from before you started recording

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│  Resolume Arena                                          │
│                                                          │
│  ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐     │
│  │ Clip     │ │ Clip     │ │ Clip     │ │ Clip     │     │
│  │ NanoLoop │ │ NanoLoop │ │ NanoLoop │ │ NanoLoop │     │
│  │ Ch: 1    │ │ Ch: 2    │ │ Ch: 3    │ │ Ch: 4    │     │
│  └──────────┘ └──────────┘ └──────────┘ └──────────┘     │
│                                                          │
│  ┌──────────────────────────────────────────────────┐    │
│  │ Any clip with NanoLooper effect                  │    │
│  │ ┌──────────────────────────────────────────────┐ │    │
│  │ │ Overlay: grid, clip cards, status            │ │    │
│  │ └──────────────────────────────────────────────┘ │    │
│  └──────────────────────────────────────────────────┘    │
│                                                          │
│  WebSocket API ◄──────────────────────────────────────┐  │
│  ws://127.0.0.1:8080/api/v1                           │  │
└───────────────────────────────────────────────────────┼──┘
                                                        │
  NanoLooper plugin internals:                          │
  ┌─────────────────────────────────────────────────────┼┐
  │ LooperCore          ResolumeWsClient                ││
  │ (events, undo,      (subscribe, trigger, ───────────┘│
  │  quantize)           gate on/off)                    │
  │                                                      │
  │ OverlayRenderer     TextRenderer                     │
  │ (grid, cards,        (Core Text,                     │
  │  status)              glyph atlas)                   │
  └──────────────────────────────────────────────────────┘
```

**LooperCore** — Pure C++ step sequencer. 16 steps, 4 channels, quantized to 16th notes. Add-only recording, undo/redo stack, destructive record mode with progressive step clearing and user-added step protection.

**ResolumeWsClient** — WebSocket client (IXWebSocket) that connects to Resolume's API. Discovers composition structure, subscribes to clip state and tempo, triggers clips via gate-style on/off (held for one 16th note).

**Channel Tag Plugin** — Lightweight FFGL effect (always bypasses rendering). Exposes a Channel dropdown. The main plugin scans the composition JSON for these effects to discover which clips belong to which channels.

**OverlayRenderer** — GL 3.2 Core Profile. Colored quad batching + Core Text font rendering with automatic Unicode fallback. Draws clip cards, step grid, beat markers, and status indicators.

**Gate model** — Clips are held "connected" for the duration of one 16th note, supporting Resolume's Piano trigger style. Consecutive steps retrigger (off-on). Muting or deleting while a gate is open sends an immediate off.

## Build from source

Requires CMake 3.16+, a C++17 compiler, and macOS (for Core Text and the FFGL host).

```bash
./build.sh                          # Build everything
./run_tests.sh                      # Run all tests (Python mock server + C++ unit/integration)
```

Build outputs:

| Target | Description |
|--------|-------------|
| `build/NanoLooper.bundle` | Main FFGL plugin bundle |
| `build/NanoLooperCh.bundle` | Channel tag FFGL plugin bundle |
| `build/looper_harness` | macOS test harness (hosts plugin in a window, keyboard input) |
| `build/looper_cli` | Terminal-based looper (legacy, no FFGL) |
| `build/testme` | Manual WebSocket test tool |

Dependencies are fetched automatically via CMake FetchContent:

| Library | Purpose |
|---------|---------|
| IXWebSocket | WebSocket client |
| nlohmann/json | JSON parsing |
| Catch2 | C++ test framework |
| FFGL SDK | FreeFrame GL plugin API (included in `tmp/native/modules/ffgl/`) |

### Development with the test harness

The test harness hosts the FFGL plugin directly in a native macOS window, mapping keyboard keys to plugin parameters:

| Key | Action |
|-----|--------|
| 1–4 | Trigger channels (piano-key) |
| d | Delete (hold) |
| m | Mute (hold) |
| z / x | Undo / Redo |
| r | Record (hold) |
| q | Quit |

Connect to the mock server (`python mock_server/resolume_mock.py`) or real Resolume running on localhost.
