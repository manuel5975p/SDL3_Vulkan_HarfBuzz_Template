# Presentation: the frame loop, present modes, and headless rendering

This is the part of the template most worth understanding before changing anything, because the
failure modes are silent: a frame pipeline that is *correct* but badly ordered does not crash, it
just halves your frame rate under vsync and looks like a slow GPU.

Everything here lives in `src/render/`: `vk_context` (devices), `swapchain`, `present` (the windowed
frame loop), `headless` (the windowless one).

## The one big design decision

**Nothing in this codebase ever renders into a swapchain image.** Every frame is drawn into an
offscreen `VK_FORMAT_B8G8R8A8_UNORM` image owned by the *render* device, and only at the end of the
frame is that image copied into the swapchain image the presentation engine handed over.

Two things pay for that extra full-screen copy:

- **The render GPU does not have to be able to present.** On a laptop where the discrete GPU has no
  display attached, `VulkanContext` renders on the GPU you asked for and pairs it with whichever one
  can actually reach the surface. Frames then cross between the two through host memory, because no
  portable Vulkan mechanism shares an image between devices (external memory handles exist but are
  driver- and platform-specific).
- **Alpha blends in gamma space on every device.** The target is UNORM, so writes are raw bytes and
  the blend hardware mixes gamma-encoded values, CSS-style — which is what the UI's colors assume.
  Getting that with a swapchain image would need `VK_KHR_swapchain_mutable_format` and an
  sRGB/UNORM view pair.

The consequence you have to remember: **build every pipeline that draws a frame with
`Presenter::kColorFormat`**. A 3D pass added on top of this template would therefore own its own
tonemap and sRGB encode on write, while the 2D UI stays in raw sRGB space.

## Frame structure

```
begin_frame(clear)     wait fence[i]  ->  reset  ->  begin cmd  ->  layout transition (+ optional clear)
   <app records: whatever it draws, all into the offscreen target>
end_frame()            barrier to TRANSFER_SRC  ->  ACQUIRE  ->  copy into swapchain image  ->
                       end cmd  ->  submit (wait image_available, signal render_finished[image])  ->  present
```

Two frames are in flight (`kFramesInFlight = 2`): the CPU records frame N while the GPU renders
N-1. Everything indexed by `frame_index` — command buffer, fence, offscreen target, and the callers'
own per-frame buffers — exists twice, and the fence waited at the top of `begin_frame()` is what
makes writing slot `i` safe without any further synchronisation. That is the contract
`UiRenderer::render(cmd, frame_index, ...)` relies on, and the one any renderer you add should keep.

### Acquire late. This is the important one.

`vkAcquireNextImageKHR` **blocks the calling thread** until the presentation engine gives an image
back. Under FIFO that means "until the next refresh". So where you put the acquire decides whether
your CPU work and your vsync wait *overlap* or *add up*:

```
acquire in begin_frame():   [ wait for vsync ][ record 2 ms ][ submit ] -> misses the refresh it was aiming at
acquire in end_frame():     [ record 2 ms ][ wait for vsync ][ submit ] -> same work, lands on time
```

With the first ordering, a frame whose CPU time plus GPU time exceeds what is left of the refresh
interval slips to the *next* one — which under FIFO means dropping straight from 60 fps to 30, with
no intermediate values. That is the classic "vsync halves my frame rate" symptom, and it is a
scheduling artefact, not a sign that the GPU is short of headroom.

So both paths acquire in `end_frame()`, after the app has recorded everything. The only thing the
acquire has to precede is the copy into the swapchain image, which is the last command recorded.

The submit waits on the acquire semaphore at `VK_PIPELINE_STAGE_TRANSFER_BIT`, not at
`COLOR_ATTACHMENT_OUTPUT`: the swapchain image is touched only by that final copy, so the app's own
passes are free to start before the presentation engine has released it. The barrier that
transitions the swapchain image must then be *sourced* at `TRANSFER` too — `TOP_OF_PIPE` names no
stage the semaphore wait covers, so the layout transition would be free to run before the engine had
finished reading the image. The synchronisation validation layer catches exactly this; see
"Checking your work" below.

### Out of date and suboptimal

`VK_ERROR_OUT_OF_DATE_KHR` and `VK_SUBOPTIMAL_KHR` can come back from either the acquire or the
present, on either path. They are not errors and never become an `unexpected`:

- **Out of date** — no image was acquired. The command buffer is still submitted, without the copy
  and without any semaphore, purely so this slot's fence signals and the frame protocol stays
  intact. Skipping the submit would leave the fence unsignalled and the next `begin_frame()` would
  block forever on it.
- **Suboptimal** — the image is usable and the frame is presented, but the swapchain no longer
  matches the surface.

Either way `needs_resize_` is armed, and the *next* `begin_frame()` returns `nullopt` before it
resets anything, so the caller can `resize()` and come straight back with the fence still signalled.

Suboptimal deserves a warning. Ignoring it leaves the window scaling forever, so it has to trigger a
rebuild — but some compositors report it on *every* present for reasons a rebuild cannot fix
(fractional scaling, an unusual surface size). Honouring those naively rebuilds the swapchain every
frame, which is far more expensive than running slightly mismatched. `suboptimal_settled_` is the
guard: once a rebuild comes back at the very same extent, suboptimal reports stop being acted on
until the extent genuinely changes.

## Present modes

`PresentPreference` is a *preference*: each value falls back down the list until it reaches
something the surface offers, and FIFO terminates the chain because the spec requires every surface
to support it. `Presenter::present_mode_name()` reports what is actually in use — always check it
rather than assuming you got what you asked for.

| Preference | Chain | Tearing | Paces the loop? |
| --- | --- | --- | --- |
| `Vsync` | FIFO | no | yes — the acquire blocks until the next refresh |
| `Mailbox` | MAILBOX → FIFO | no | no — renders freely, newest finished frame wins |
| `Immediate` | IMMEDIATE → MAILBOX → FIFO | **yes** | no |

CLI: `--present=fifo|mailbox|immediate`. `--no-vsync` is a deprecated alias for
`--present=immediate` — it has to mean "do not wait for the refresh" whatever the surface offers,
and mapping it to `Mailbox` would quietly leave it vsynced on a surface with no MAILBOX. In the
demo, `P` cycles all three and the top bar shows the resulting mode plus the frame rate it achieves.

Switching modes rebuilds the swapchain, because `presentMode` is fixed at creation. There is no
cheaper way.

### Swapchain image count is part of the present mode

`minImageCount` is the count at which the app is *allowed* to run, not the count at which it runs
well: with exactly that many, every acquire blocks until the presentation engine hands one back. One
spare decouples them, hence `minImageCount + 1`.

**MAILBOX needs three regardless of what the driver reports as its minimum.** The whole point of the
mode is that a finished frame can sit in the mailbox waiting for the next refresh while the app
renders its replacement — one image displayed, one waiting, one being drawn. With two, the app
blocks in acquire exactly as it would under FIFO and the mode buys nothing at all while looking like
it is enabled. `choose_image_count()` forces the floor; the startup line prints the count it got:

```
Present mode: IMMEDIATE (asked for immediate), 3 swapchain images
```

Not every surface offers MAILBOX. Where it does not, `--present=mailbox` reports `FIFO (vsync)` —
that is the fallback working as designed, not a bug.

## The cross-GPU path

When `ctx.cross_gpu()` is true, render and present are different `VkDevice`s and no semaphore can
span them, so the CPU is the synchronisation point: wait the render fence, `memcpy` from a
host-visible readback buffer on the render device into a host-visible staging buffer on the present
device, then upload and present there.

Doing that inline would serialise render + copy + present into one frame time. Measured upstream on
a 3070 → iGPU pair, inline is 70 fps against 92 for the deferred version, and it makes the frame long
enough that the present mode stops mattering at all. So **`end_frame()` submits frame N and then
hands over frame N-1**, whose fence went signalled a whole frame ago — the waits do not stall, and
the acquire (and with it the present mode) is what paces the loop again.

That is why the acquire on this path lives in `flush_pending()` rather than `end_frame()` proper,
and why a deferred frame is dropped rather than presented when `resize()` replaces the target it
points at. `has_deferred_frame()` exposes the state; `read_last_frame()` accounts for it when
picking which slot holds the newest complete frame.

Force the slow path for testing with `--present-gpu=N`.

## Headless

`HeadlessTarget` (`src/render/headless.hpp`) is the same begin/end/read frame shape with no window,
no surface and no swapchain — `VulkanContext::create(nullptr, gpu)` builds a surfaceless context (an
instance with no WSI extension and a device with no swapchain extension), and frames land in an offscreen
image that is read back to host memory. It exists so CI, screenshots and visual diffs need no
display, and because it hands out the same `FrameTarget` struct, every renderer works unchanged
against it.

```
vkhb_demo --headless --frames=30 --width=640 --height=400 --screenshot=out.ppm
```

`--width/--height` set the target size and the shot is taken from the last frame. Headless is what
makes the render path testable at all: given the same inputs the output is deterministic, so a
screenshot is a byte-exact regression test — same size, same frame count, `cmp` against a reference
taken before the change.

Note that `HeadlessTarget::begin_frame()` still takes a mandatory clear colour while
`Presenter::begin_frame()` takes an optional one. That asymmetry is deliberate: headless runs are
not frame-rate sensitive, so the clear is worth keeping as a guarantee that a screenshot never
contains uninitialised memory.

Headless also creates its instance without validation layers, unlike `VulkanContext` — so the sync
validation run below is a windowed-only exercise.

## The clear, and why it is optional

`Presenter::begin_frame()` takes `std::optional<glm::vec3>`. With a colour, the target is cleared
through `vkCmdClearColorImage` (a transfer write, which is why the target carries `TRANSFER_DST`
usage); with `nullopt` it goes straight from `UNDEFINED` to `COLOR_ATTACHMENT_OPTIMAL` and the app
promises its first pass writes every pixel.

The demo passes a colour, because a 2D UI does not cover the frame. **Pass `nullopt` only if your
first pass genuinely does** — a fullscreen sky or background quad, say — in which case the clear is
a full-screen write thrown away every frame, and the pass itself should use `LOAD_OP_DONT_CARE`
rather than `LOAD_OP_LOAD` for the same reason.

**Be honest about the size of that win**: measured upstream on a 3070 at 1280×800 it is roughly 8 MB
of write plus 8 MB of attachment load per frame, and it did not move a differenced 1000-frame
benchmark at all (3.350 vs 3.340 ms/frame — the same number inside the noise). It matters at 4K, on
tiled and integrated GPUs, and on anything bandwidth-bound; mostly it is a correctness tidy-up, since
`LOAD_OP_LOAD` on an attachment whose barrier only declared `COLOR_ATTACHMENT_WRITE` was also a
synchronisation hazard.

The clear components are raw sRGB-space values, because the target is UNORM and nothing is encoded
on write.

## Checking your work

The frame loop is exactly the kind of code where a bug shows up as "it works on my machine". Two
tools, both cheap:

**Synchronisation validation.** Validation is compiled in for `Debug` builds only
(`kWantValidation` in `vk_context.cpp`). Core validation is not enough — the interesting hazards are
sync ones, which are off by default:

```sh
cmake -B build-dbg -G Ninja -DCMAKE_BUILD_TYPE=Debug && cmake --build build-dbg
cat > /tmp/vk_layer_settings.txt <<'EOF'
khronos_validation.validate_sync = true
khronos_validation.validate_core = true
khronos_validation.thread_safety = true
EOF
VK_LAYER_SETTINGS_PATH=/tmp/vk_layer_settings.txt ./build-dbg/src/vkhb_demo --frames=200 --present=immediate
```

Run it in every present mode. It should print nothing. It is what found both of the barrier bugs
described above, and neither of them produced a visible artefact.

**Differenced benchmarking.** Frame-rate numbers off a single run are dominated by startup. Run two
frame counts and difference them:

```sh
t1=$(/usr/bin/time -f %e ./build/src/vkhb_demo --frames=100  --present=immediate 2>&1 >/dev/null | tail -1)
t2=$(/usr/bin/time -f %e ./build/src/vkhb_demo --frames=1100 --present=immediate 2>&1 >/dev/null | tail -1)
python3 -c "print(f'{($t2-$t1):.3f} ms/frame')"
```

Use `--present=immediate` for this. FIFO measures your monitor, not your renderer.

### One environment trap

A display-less X server will make FIFO look catastrophically broken. With an Xwayland server
reporting `current 0 x 0` — no outputs — nothing consumes frames and the driver falls back to a
roughly 1 Hz heartbeat:

```
FIFO      ~1 fps      <- not a renderer problem
IMMEDIATE ~300 fps
headless  ~200 fps
```

Check `xrandr` before believing a FIFO number. **The ordering described in this document has been
verified for correctness but not benchmarked on the machine this file was written on** — SDL there
finds no displays at all, so the windowed paths could not be run. If you have a real display, a
differenced FIFO benchmark before and after is the measurement that is still missing.

## Things deliberately not done

- **`kFramesInFlight` is 2, not 3.** Three absorbs more CPU jitter under FIFO, at the cost of one
  more of everything per-frame — offscreen target, UBOs, and whatever your app duplicates — and one
  more frame of input latency. Worth revisiting with a real display and a FIFO benchmark; not worth
  changing blind.
- **The offscreen copy is a copy, not a blit.** `vkCmdCopyImage` between UNORM and SRGB of equal
  texel size is a size-compatible raw byte move with no colour conversion, which is exactly what
  keeps the gamma-space blend intact. A `vkCmdBlitImage` would resample and defeat that.
- **SDL's dynamic-API shim is left alone.** `SDL_dynapi.h` deliberately `#error`s if you define
  `SDL_DYNAMIC_API=0` on the command line. It is not a problem under LTO either — see
  `docs/packaging.md`.
