# Most Distant — VCV Rack plugin

Ableton Link bridge modules for [VCV Rack 2](https://vcvrack.com), living in the WAIL
repo so they can reuse WAIL's proven Ableton Link + Link Audio C wrapper
(`../plugins/wail_link.{h,cpp}`, ADR-0007) and the pinned `../vendor/link`
(Link-4.0) submodule instead of re-vendoring the SDK.

## Modules

| Module | What it does |
| --- | --- |
| **Link Clock** | Joins the local Ableton Link session and turns its shared timeline into CV — a beat **Clock**, a bar **Reset**, and a **Run** gate. Knobs: pulses-per-beat, quantum (beats/bar). Edges are sample-accurate. The clock runs whenever Link does; the context menu can gate it with Link start/stop transport instead. The display shows tempo and peer count. |
| **Link Audio Receive** | Subscribes to a **Link Audio** channel and plays it into Rack (stereo, ±5 V). Pick the channel from the context menu, or leave it on **Auto** (the first WAIL room channel, `WAIL · …`) — with WAIL running, the room is audible with no setup. Playback follows the buffers' beat stamps, so WAIL room streams land on the grid. |
| **Link Audio Receive 16** | Every WAIL room stream at once (18 HP). Room channels (`WAIL · …`) fill sixteen stereo slots automatically, each with a two-line display (peer / stream) that dims while the stream is silent; jack tooltips name the stream too. A slot keeps its stream across the WAIL app's in-place renames, is freed ~3 s after the stream leaves, and no other slot moves, so patch cables keep hearing the same player. **ALL (POLY)** L/R carry every slot as one polyphonic cable (voice n = slot n). Same slot rules as WAIL Receive. |
| **Link Audio Send** | Publishes a Rack stereo input (polyphonic cables are summed; R normals to L) as a **Link Audio** channel that WAIL, Live, or another Receive can hear. Rename the channel from the context menu; the name is saved with the patch. The light is lit while a subscriber accepts the audio. |

Each module instance is its own LAN Link peer (as with WAIL Send / WAIL Receive).

## How timing works

Rack computes audio in bursts — a whole engine block per audio callback — so the wall
clock inside `process()` says little about which frame is being computed.
`FrameClock` maps engine frames to Link time from the one honest observation, the first
frame of each block (`Engine::getBlockFrame`), smoothed to absorb scheduling jitter and
follow clock drift. Every module then works in *play time*: frame time + one engine
block + a fixed device-latency constant (`kDeviceLatencyUs`, `src/LinkTime.hpp`).

- **Send** stamps each 48 kHz buffer with the beat at which its first frame is heard,
  compensating the sample-rate converter's group delay.
- **Link Clock** evaluates the beat at each frame's play time, so an edge lands on the
  frame where the beat boundary falls.
- **Receive** and **Receive 16** share one per-stream playout (`RecvSlot`); Receive 16's
  slot assignment is `RoomSlots`, a pure port of WAIL Receive's manager loop.
- **Receive** (`StampAligner`, a port of WAIL Receive's slot renderer) pads when early
  and skips when late against the stamps, with a 32-frame deadband. A channel stamped
  ahead (WAIL room channels) plays exactly on its stamp; a live channel stamped ~now
  arrives late, so the delay grows to cover it (max-hold, capped at 250 ms, reset on
  resubscribe). See `tradeoffs.md` → "VCV Rack Link Audio modules".

Nothing on the audio thread allocates or blocks: the session snapshot is preallocated
(`lb_state_create` + `lb_capture_into`), source pops reuse it (`lb_source_pop_at`), and
channel discovery + subscribe/unsubscribe run on Receive's manager thread.

Measured headless (44.1 kHz engine, `linkaudio-probe -onsets`): Link Clock edges sit
−0.04 ms off the beat (±0.01 ms); a Send → Receive → Send loop on one machine settles at
~30 ms delay — Link Audio's own LAN delivery jitter — with nothing skipped or lost.

## Build & test (macOS — Apple Silicon)

Needs the Rack SDK matching your Rack (2.6.x) and the Link submodule:

```sh
# 1. Toolchain: Xcode Command Line Tools
xcode-select --install

# 2. Rack SDK (match the Rack version you run; latest is the 2.6.x line)
cd ~/Downloads
curl -LO https://vcvrack.com/downloads/Rack-SDK-2.6.6-mac-arm64.zip   # confirm exact version on the downloads page
unzip Rack-SDK-2.6.6-mac-arm64.zip
export RACK_DIR=~/Downloads/Rack-SDK

# 3. Make sure the Link submodule is present (once, in the WAIL repo)
git submodule update --init --recursive vendor/link

# 4. Build + install into Rack's user plugins folder, then launch Rack
cd <WAIL repo>/rack
make            # builds plugin.dylib
make install    # copies to ~/Library/Application Support/Rack2/plugins-mac-arm64/
make test       # unit tests for the pure logic (FrameClock, StampAligner, RoomSlots)
```

Rack unpacks new plugin packages at startup, so restart Rack after `make install`.

Then launch Rack and add the **Most Distant** modules from the browser. To exercise Link,
run another Link peer on the LAN — Ableton Live, or WAIL itself (see the repo's
`scripts/pair-debug.sh` / test-tone tooling). With a peer present, **Link Clock**'s peer
light comes on.

To check timing without a DAW, send Link Clock's output through a **Link Audio Send** and
run `linkaudio-probe -onsets` (`wail-app/cmd/linkaudio-probe`): it reports each channel's
click-onset phase against the beat grid (0 ms = on the beat).

- `make dist` packages a distributable `.vcvplugin`.
- Intel Macs: use the `-mac-x64` SDK; the Makefile's `ARCH_OS`/`ARCH_CPU` handle the rest.

## How the Link SDK is wired in

`Makefile` adds `../plugins/wail_link.cpp` to `SOURCES` via `VPATH` (it `#include`s
`vendor/link`'s `abl_link.cpp`, i.e. the whole header-only Link 4.0 + Link Audio SDK — one
extra TU), sets the Link include paths and per-OS `LINK_PLATFORM_*` defines mirroring
`../plugins/CMakeLists.txt`, and forces `-std=c++17` (appended after `plugin.mk`, so it
wins over Rack's default `-std=c++11`). On Windows/MinGW the vendored `Channels.hpp`
`interface`-param rename is also required (untested here — see WAIL `DEVELOPMENT.md`).

## License

MIT — see [`LICENSE`](LICENSE).
