# ember-latency

**Input latency in MangoHud on Linux.** Shows how long it takes from a key press or mouse click until
the frame it caused is on screen, as an `INPUT 18.5 ms` row under your FPS. It's like PresentMon's
"click to photon" or NVIDIA's PC Latency, but for any Vulkan game on any GPU, and with no cost to
performance.

It's two parts:

1. **`VK_LAYER_EMBER_latency`**, a small Vulkan layer (one C file, no dependencies) that measures and
   writes the result to `/dev/shm/ember-latency/<pid>`.
2. A **MangoHud patch** that adds a native `input_latency` row, drawn like the AVG/1% rows. If you
   don't want to rebuild MangoHud, there's also a no-patch option using `custom_text` (below).

## How it measures

```
key press / click        evdev kernel timestamp (CLOCK_MONOTONIC)
  -> next frame          the first frame the game starts after the press
  -> first pixel out     VK_EXT_present_timing: the compositor's real flip time
```

- **Presses only.** The layer reads `/dev/input` with `EVIOCSMASK`, so the kernel never wakes it for
  mouse motion, only for key and button presses.
- **The frame you saw.** If the compositor drops the frame after a press, it follows the press to the
  next frame that was actually shown.
- **Native Wayland and gamescope** report the real flip time (`FIRST_PIXEL_OUT`). gamescope reports
  its scheduled vblank, which can be a few ms in the future, so that's allowed for.
- **XWayland** only reports when the image was handed over. With vsync that's real. Without vsync it's
  a stale vblank, so after a press the layer adds one empty `vkQueueSubmit` that waits on the game's
  semaphores and reports when the GPU finished the frame (`xwayland-gpu` in the details line). With
  tearing allowed, that's about when it reaches the screen.
- **Without present timing** (other drivers), it falls back to when `vkWaitForPresentKHR` returns,
  and only in FIFO (vsync), where that means something.
- The readout is the average of your last 16 presses. It's an estimate: it assumes the game reads
  input at the start of a frame, and it can't see the panel's own pixel response. Expect it to be
  within about one frame.

**Cost.** It adds no GPU work, except that one empty submit on XWayland, and the game's threads never
wait on anything. While you aren't pressing anything, a present costs one atomic load. Only the frame
after a press asks the driver for timing, and the file is written by a worker thread, never on the
present path. Benchmarks at 30,000 fps (vkcube) and in CS2 (about 400 fps) were within noise.

## Requirements

- Linux with a Vulkan driver that has `VK_EXT_present_timing` for the real flip time (Mesa 26.2+ RADV
  tested). Anything else uses the vsync fallback.
- Your user in the **`input` group**, so the layer can read key-press timestamps from
  `/dev/input/event*`. Be aware that this group lets *any* program you run read your keyboard.
  The layer itself only looks at when keys and buttons go down, never at what you type.
- Build tools: a C compiler, make, Vulkan headers (Arch: `vulkan-headers`; Debian/Ubuntu:
  `libvulkan-dev`).

## Install

```sh
git clone https://github.com/Tjabooo/ember-latency.git
cd ember-latency
make install                          # ~/.local/lib/ember-latency + a user layer manifest; no root
sudo usermod -aG input "$USER"        # once, then log out and in
```

The layer turns itself on whenever MangoHud does (`MANGOHUD=1`, or `mangohud %command%` in Steam).
Turn it off for one game with `DISABLE_EMBER_LATENCY=1`. Remove it with `make uninstall`.

### Option A: native INPUT row (patched MangoHud)

```sh
sh mangohud/build-mangohud.sh         # builds the version of MangoHud you have installed
```

This downloads that MangoHud release, applies [`input-latency.patch`](mangohud/input-latency.patch),
builds it in `~/.cache`, and installs it as a **user** Vulkan layer that overrides the system
MangoHud: the layer name is the same, and the loader prefers `~/.local/share`. Your system package
isn't touched. Then add one line to `~/.config/MangoHud/MangoHud.conf`, for example right after
`fps_metrics`:

```ini
input_latency
```

Rerun the script after your distro updates MangoHud. The patch was written against **MangoHud 0.8.4**.
Undo: `rm ~/.local/share/vulkan/implicit_layer.d/MangoHud.x86_64.json`.
The patched build covers 64-bit Vulkan games. OpenGL and 32-bit games keep the stock MangoHud,
which ignores the `input_latency` line, so there the row is simply missing.

### Option B: stock MangoHud, no rebuild

MangoHud runs `exec` commands as children of the game, so `$PPID` is the game's PID:

```ini
custom_text=INPUT
exec=f=/dev/shm/ember-latency/$PPID; if [ -r "$f" ]; then read -r l < "$f"; echo "$l ms"; else echo -; fi
custom_text=
```

Stock MangoHud draws these in its smaller secondary font and in a single colour.

## Reading it

- `INPUT 18.5 ms`: the average of your last 16 presses.
- `-`: no data yet (nothing pressed, an OpenGL game, or the layer can't read `/dev/input`).
- `/dev/shm/ember-latency/<pid>`: line 1 is the average, line 2 has the details (method, samples,
  last value).

Debugging:

| Variable | Effect |
| --- | --- |
| `EMBER_LATENCY_DEBUG=1` | Log to stderr (`2` = every timing report) |
| `EMBER_LATENCY_NO_TIMING=1` | Force the vkWaitForPresent fallback |
| `EMBER_LATENCY_TEST=<n>` | Simulate *n* presses per second (no human needed) |
| `DISABLE_EMBER_LATENCY=1` | Turn the layer off |

Tested with vkcube (Wayland, XWayland and nested gamescope, all present modes), inside Steam's
Linux runtime (Proton games: the runtime shares `/dev/input` and `/dev/shm`), and with CS2. Sober
(a Flatpak) and OpenGL games show `-`.

## License

MIT
