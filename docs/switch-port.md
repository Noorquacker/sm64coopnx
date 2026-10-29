# sm64coopdx — Nintendo Switch port (deko3d)

Status: **Phase 1 done (headless boot).** The NRO and NSP build and boot in yuzu. The game loads the ROM, built-in
mods and DynOS packs, then runs its loop with no renderer. Phase 2 (deko3d bring-up) is next.

## Building

Requires devkitPro (`switch-dev`) with `DEVKITPRO` exported, plus `switch-sdl2`, `switch-curl`, `switch-zlib`. NSP
builds also need `hacbrewpack` on `PATH`.

```sh
make TARGET_SWITCH=1 -j$(nproc)                            # build/us_nx/sm64coopdx.nro
make TARGET_SWITCH=1 nsp KEYS=/path/to/your/prod.keys      # build/us_nx/sm64coopdx.nsp
make TARGET_SWITCH=1 run-yuzu [YUZU=/path/to/yuzu]         # launch the NRO in yuzu
tools/switch/build_lua.sh                                  # rebuild lib/lua/switch/liblua53.a
```

- All Switch build logic is in `switch.mk`. The Makefile has only two hooks: the `TARGET_SWITCH` include near the
  top, and a block after `C_FILES` that adds `src/pc/switch/` and drops the SDL/GL backend files.
- The NSP is signed with **your own** console keys. Keys are never committed or used in CI. Title ID
  `0100C0DE5A640000` is arbitrary; override it with `SWITCH_TITLE_ID=...`.
- The build never needs the ROM. Neither the NRO nor the NSP contains game assets.

## SD card layout

```
sdmc:/switch/sm64coopdx/
  sm64coopdx.nro          (NRO builds; anywhere under /switch works)
  <your US ROM>.z64       any filename; the game finds it by MD5 and copies it to baserom.us.z64
  baserom.us.z64          created on first boot
  sm64config.txt, sav/    config and saves
  mods/                   user mods and mods downloaded from hosts
  log.txt                 stdout/stderr when not using nxlink
```

Built-in `lang/`, `palettes/`, `mods/`, `dynos/` and `licenses/` ship read-only inside the NRO/NSP romfs (`romfs:/`).

## Phase 1 results

| Check | Result |
|---|---|
| NRO builds (`build/us_nx/sm64coopdx.nro`, 87 MB incl. 30 MB built-in mods) | ✅ |
| NSP builds and boots in yuzu | ✅ |
| Boots in yuzu as an application, 3285 MiB available | ✅ |
| ROM found on SD, copied to `baserom.us.z64`, assets loaded | ✅ |
| Built-in mods and DynOS packs load from `romfs:` | ✅ |
| Game loop runs on the dummy renderer (~30 game fps, stable for 2 min) | ✅ |
| Logs to `log.txt` (or nxlink when launched with `nxlink -s`) | ✅ (nxlink untested, needs hardware) |
| Linux PC build compiles and runs headless | ✅ |
| Applet-mode warning screen | untested: yuzu always launches as an application |
| Real hardware | untested |

A stats thread (`src/pc/switch/platform_switch.c`) logs game frames and malloc heap use every 10 s. It's useful
until a renderer exists; gate or remove it later.

### Gotchas found in Phase 1

- **`u64` clash:** libnx defines `u64` as `unsigned long`, the game (`PR/ultratypes.h`) as `unsigned long long`.
  A file can't include both `<switch.h>` and game headers. Keep libnx-facing code in its own files. This matters
  for the deko3d backend: `deko3d.h` doesn't pull in `<switch.h>`, but anything using libnx services must stay
  separate from game headers.
- **`LLONG_MAX` in C++:** gcc's `<limits.h>` leaves it to `<climits>` in C++, which breaks `luaconf.h`. `switch.mk`
  force-includes `<climits>` in C++ files.
- **`std::filesystem::copy_file` fails:** libstdc++ calls `fchmod()`, which libnx doesn't implement. It throws and
  the game quits. The ROM copy uses `switch_copy_file()` on Switch. Other `std::filesystem` calls used so far
  (`exists`, `directory_iterator`, `rename`) work.
- **NPDM `debug_flags`:** current npdmtool's `debug_flags` capability makes yuzu refuse the NSP ("unable to parse
  kernel metadata"), so it's left out of `res/switch/sm64coopdx.json`.
- **`TARGET_BITS`:** stays `0` (native). The handoff's `TARGET_BITS=64` would add `-m64`, which aarch64 gcc rejects.
  aarch64 is 64-bit anyway.
- **Binary size:** the static SDL2 links Mesa's EGL/nouveau even with SDL video unused, because `SDL_Init`
  references the video subsystem. Revisit in Phase 7 if size matters.

Base: upstream coopdx at `8cd6e5977` ("Update updater libs"), network version `v42.1`
(`src/pc/network/version.h`). The Switch build must keep this version so it can play with PC clients.

---

## 1. Toolchain found on the dev machine

| Component | State | Notes |
|---|---|---|
| devkitA64 | installed | `aarch64-none-elf-gcc` 16.1.0 |
| libnx | installed | `libnx.a`; headers include `ssl.h`, `csrng.h`, `set.h` |
| deko3d | installed | `libdeko3d.a`, `deko3d.h` |
| uam (host tool) | installed | `/opt/devkitpro/tools/bin/uam` |
| **uam as a library** | **missing** | Not shipped by devkitPro. It must be built from source (meson, C++) for aarch64-switch. This is a new dependency. See §6 |
| elf2nro / nxlink | installed | |
| switch-sdl2 | installed | 2.28.5 |
| switch-zlib, switch-libpng, switch-bzip2 | installed | |
| switch-mesa (EGL/GLESv2) | installed | Not used by the final build. It could serve as a temporary renderer only if we choose to (see §7, Q3) |
| **switch-curl** | **missing** | `dkp-pacman -S switch-curl` |
| **switch-mbedtls** | **missing** | `dkp-pacman -S switch-mbedtls` (needed by curl) |

## 2. How the build and runtime are structured (verified)

- **There is no compile-time `RENDER_API`/`WINDOW_API` variable.** The backend is picked at runtime in
  `select_graphics_backend()` (`src/pc/pc_main.c:219`) from `configGraphicsBackend` (`GAPI_GL`, `GAPI_D3D11`).
  The Switch port adds a `GAPI_DEKO3D` case behind `#ifdef TARGET_SWITCH`. This is a small change to a shared file.
- `gWindowApi`/`gRenderApi` default to `gfx_dummy_wm_api`/`gfx_dummy_renderer_api` (`pc_main.c:108`), and
  `--headless` skips backend selection entirely. **Phase 1 can reuse the existing `gfx_dummy.c`**, so no new null
  renderer is needed.
- **The ROM isn't needed at build time.** `extract_assets.py` is only used by `make distclean`. At runtime,
  `main_rom_handler()` (`src/pc/rom_checker.cpp`) scans the user dir, then the exe dir, for *any* `*.z64` whose
  MD5 matches US. It copies the match to `<user>/baserom.us.z64`. On Switch the user just drops their ROM into
  `sdmc:/switch/sm64coopdx/`.
- The ROM-missing screen says "drag & drop …". That makes no sense on Switch, so the string needs a Switch variant.
- Rendering happens on the main thread only. Game init runs on `gLoadingThread` while the main thread renders the
  loading screen (`src/pc/loading.c`, `pc_main.c:556`). deko3d command recording can therefore stay single-threaded.
- Runtime resource dirs come from `sys_resource_path()` (= exe dir on Linux): `lang/`, `mods/`, `palettes/`,
  `dynos/`. The Makefile copies them into `build/us_pc/`. On Switch these go in **romfs** (read-only bundled copies)
  or on the SD next to user data (see §7, Q2).
- `sys_exe_path_file()` reads `/proc/<pid>/exe`, which doesn't exist on Switch. `sys_user_path()` uses
  `SDL_GetPrefPath`. Both need a Switch branch in `src/pc/platform.c`, or better, a new `src/pc/switch/platform_switch.c`.

## 3. Rendering interface (`src/pc/gfx/gfx_rendering_api.h`)

The vtable is the **classic Fast3D one**. It has no framebuffer/render-target functions, no MSAA hooks and no
per-draw uniform API.

| Function | GL implementation | deko3d plan |
|---|---|---|
| `z_is_from_0_to_1` | returns `false` | Return `true` and use `DkDepthMode_ZeroOne`. Check `gfx_pc.c:1211` for the z remap first. Otherwise keep `false` and use `DkDepthMode_MinusOneOne` |
| `unload_shader` / `load_shader` | `glUseProgram`, attrib enable | bind DKSH pair + vertex attrib/buffer state; mark uniforms dirty |
| `create_and_load_new_shader(cc)` | builds GLSL 1.20 from `ColorCombiner` | same generator emitting GLSL 4.60 → uam at runtime → DKSH cache |
| `lookup_shader(cc)` | linear search, key `cc->hash` (u64) | same; disk cache key = `cc->hash` + backend version |
| `shader_get_info` | returns inputs/textures | same |
| `new_texture` / `select_texture` / `upload_texture` | `glTexImage2D` RGBA8 | staging buffer + `dkCmdBufCopyBufferToImage`; image pool |
| `set_sampler_parameters(tile, linear, cms, cmt)` | per-texture GL params | per-tile `DkSampler` (wrap: clamp / mirror / repeat per axis; nearest/linear) |
| `set_depth_test` / `set_depth_mask` | `glEnable/glDepthMask`, func `LEQUAL` | `DkDepthStencilState` (prebuilt variants) |
| `set_zmode_decal` | `glPolygonOffset(-2,-2)` | `DkRasterizerState` depth bias (constant/slope) |
| `set_viewport` / `set_scissor` | GL, origin bottom-left | `dkCmdBufSetViewports/Scissors`. **Y flip needed**: GL origin is bottom-left, deko3d top-left |
| `set_use_alpha` | `glBlendFunc(SRC_ALPHA, 1-SRC_ALPHA)` on/off | `DkBlendState` + `DkColorState` blend enable |
| `draw_triangles(buf, len, ntris)` | `glBufferData` + `glDrawArrays` | copy into per-frame ring buffer + `dkCmdBufBindVtxBuffer` + `dkCmdBufDraw` |
| `init` / `on_resize` / `start_frame` / `end_frame` / `finish_render` / `get_name` / `shutdown` | clear color+depth in `start_frame` | device/queue/memblocks; acquire image + clear in `start_frame`; submit in `end_frame`; present via WM |

### Shader features the generator must reproduce (all in `gfx_opengl.c:245–684`)

- Vertex: one interleaved float stream. Order: `aVtxPos` (vec4) → `aTexCoord0/1` (vec2, if used) → `aFog` (vec4) →
  `aLightMap` (vec2) → `aInput1..N` (vec3, or vec4 if alpha). Passthrough VS, `gl_Position = aVtxPos`.
- Color combiner: 1- or 2-cycle formulas (`single`, `multiply`, `mix`, `(a-b)*c+d`), with separate color/alpha.
- Texture sampling: optional **3-point filtering** in-shader (`uFilter == 2`, per-texture `uTexNFilter`, `uTexNSize`).
- Texture edge (`alpha > 0.3` else `discard`), fog mix, noise and alpha dither (`uFrameCount`).
- Light map (coopdx extension): second UV set and `uLightmapColor`.
- **World-geometry shader flags** (coopdx Lua-exposed post FX): `uShaderFlags[8]` / `uShaderFlagValues[8]`. These
  cover hue, saturation, brightness, contrast, exposure, 4×4 dither, posterize and scanlines (`gfx_pc.h` `enum ShaderFlag`).

Every uniform maps cleanly to one deko3d uniform buffer per draw (push via `dkCmdBufPushConstants`). Samplers map to
texture/sampler handles bound per tile.

### Features that need nothing from the backend

These run on the **CPU** in `gfx_pc.c` and arrive at the backend as plain vertex colors:

- Lighting engine (`engine/lighting_engine.h`, `le_calculate_*`, `G_LIGHTING_ENGINE_EXT`)
- F3DEX3-style Fresnel (`G_FRESNEL_COLOR_EXT` / `G_FRESNEL_ALPHA_EXT`)
- Packed normals (`G_PACKED_NORMALS_EXT`)
- Vertex color override (`gVertexColor`)

The only exception is the lightmap uniform above.

### Shader count

`CC_MAX_SHADERS = 64`, and the shader pool is a **ring** (`gfx_opengl.c:597`): the 65th shader overwrites the oldest.
The deko3d backend must free DKSH code memory for an evicted slot, or keep a larger pool. `gfx_cc_precomp()`
(`gfx_cc.c:80`) already lists the common vanilla combiners. We can reuse that list to pre-warm or ship a DKSH cache.

## 4. Window manager interface (`gfx_window_manager_api.h`)

The 21 entries include keyboard/text/scroll callbacks, `main_loop`, `get_dimensions`, `start_frame`,
`swap_buffers_begin/end`, clipboard, cursor, `get_max_msaa`, window title and `has_focus`.

**Plan:** a new `src/pc/switch/gfx_switch_wm.c` that owns the libnx `NWindow`/deko3d swapchain, applet message loop
(focus, `OperationModeChanged` → 720p/1080p) and frame pacing. Input stays on SDL2 game controllers (no SDL video
window needed). Clipboard, cursor and window title are no-ops. `start_text_input` → `swkbd`.

## 5. Platform-sensitive code (verified)

| Area | Location | Switch issue | Plan |
|---|---|---|---|
| User/exe paths | `src/pc/platform.c:299–389` | `/proc/<pid>/exe`, `SDL_GetPrefPath` | Switch branch: user = `sdmc:/switch/sm64coopdx`, resource = `romfs:` (or SD) |
| `std::filesystem` | `src/pc/rom_checker.cpp` | libstdc++ in devkitA64 supports it on newlib; **verify** `directory_iterator` on `sdmc:` | test in Phase 1 |
| `fork`/`execlp` | `src/pc/utils/misc.c:613` (`open_url`) | no processes | `#ifdef` out; use `webWifi`/no-op |
| Threads | `src/pc/thread.c` (pthreads) | libnx provides pthreads via newlib | should work; verify stack size |
| BSD sockets | `src/pc/network/socket/socket_linux.*` | libnx BSD is POSIX-like; needs `socketInitializeDefault()` | likely reusable as-is |
| curl | `src/pc/update_checker.c` | needs switch-curl + TLS init | **disable the update checker on Switch** (no way to apply updates) |
| Updater exec | Makefile `UPDATER` | n/a | force `UPDATER=0` |
| Discord SDK | `lib/discordsdk`, `DISCORD_SDK` | binary-only, no Switch build | force `DISCORD_SDK=0` |
| Mumble positional audio | `src/pc/mumble/mumble.c` (`shm_open`) | no shm | disable on Switch |
| Crash handler | `src/pc/crash_handler.c` (`_WIN32 \|\| __linux__`) | compiled out since `__linux__` isn't defined | fine for now; libnx exception handler later |
| Game audio | `src/pc/audio/audio_sdl.c` (`SDL_QueueAudio`) | switch-sdl2 has audio | reuse |
| **Mod audio** | `src/pc/lua/utils/smlua_audio_utils.c:195` `ma_engine_init(NULL, …)` | miniaudio opens **its own device** and has no Switch backend | init engine with `noDevice` and mix into the SDL stream, or a custom miniaudio backend (Phase 4) |
| Text input | `gfx_sdl.c` `SDL_StartTextInput` | needs swkbd | Phase 4 |
| ROM prompt text | `src/pc/loading.c:190` | "drag & drop" | Switch-specific string |

`__linux__` is **not** defined by devkitA64. Code under `#if defined(__linux__)` will silently drop out, and code under
`#else` (the "Unix" branch in `platform.c`) will be used. Every such site needs checking when the build first compiles.

## 6. Prebuilt binary dependencies (`lib/`)

| Library | Version | Shipped as | Source available? | aarch64-switch plan |
|---|---|---|---|---|
| Lua | 5.3.5 (`lib/lua/include/lua.h`) | `.a` per platform (win32/64, linux x64/arm/arm64, mac) | Yes, upstream lua.org. Not vendored | Build **Lua 5.3.5** from source with devkitA64 into `lib/lua/switch/liblua53.a` (static, `LUA_USE_C89`-ish, no `popen`/`dlopen`). Must stay **5.3** for mod compatibility. Commit the `.a` plus `tools/switch/build_lua.sh` (§7.4) |
| CoopNet | — | `libcoopnet.a` per platform | Yes: github.com/coop-deluxe/coopnet | build from source for switch (Phase 5) |
| libjuice | 1.6.2 (mac dylib name) | `libjuice.a` per platform | Yes: github.com/paullouisageneau/libjuice | build from source for switch (Phase 5). Needs BSD sockets and `getaddrinfo`, both in libnx |
| Discord Game SDK | — | dll/so/dylib only | **No** (closed) | **disabled** on Switch (`DISCORD_SDK=0`) |
| discord-rpc | — | dll/so/dylib | open, but not useful | not linked (`RPC_LIBS` empty) |
| coopdx_updater | — | prebuilt exe | — | **disabled** (`UPDATER=0`) |
| libultra reimpl (`lib/src`) | — | C source | yes | builds as-is |
| uam (new) | latest | — | Yes: github.com/devkitPro/uam (zlib + Mesa MIT; see §7.1) | build as static lib for aarch64-switch (Phase 3) |
| curl + mbedtls (new for Switch) | portlibs | dkp-pacman | yes | only needed if CoopNet/update checker need HTTPS (Phase 5) |

## 7. Decisions

1. **uam licensing: OK to link.** Checked against uam's `LICENSE` and the headers of every source file:
   - fincs' own code (`source/`): **zlib**.
   - Imported Mesa code (`mesa-imported/`): **MIT** (Mesa default). Khronos headers (`GL/glext.h`,
     `KHR/khrplatform.h`) are MIT-style. No GPL, LGPL or Apache files.

   coopdx has **no license file** (none in git history either), the same as the sm64 decomp and sm64ex. Permissive
   licenses impose only attribution, so linking uam adds no obligation that conflicts with the project.
   **Obligation:** ship the uam zlib notice and the Mesa MIT notice with the binary, as a `licenses/` folder in romfs
   and in this doc. The other Switch-only libraries carry similar notice duties: libnx (ISC), deko3d (zlib), SDL2 (zlib),
   curl (curl/MIT-style), mbedtls (Apache-2.0). libjuice (MPL-2.0, file-level copyleft) is already used upstream on PC;
   we only build unmodified sources.
2. **Resources go in the NRO's romfs**: `lang/`, `palettes/`, built-in `mods/`, `dynos/`, `licenses/`. User mods
   and host-downloaded mods go on the SD at `sdmc:/switch/sm64coopdx/mods/`.
3. **No SDL video.** Own the `NWindow` directly from deko3d, and use SDL only for game controllers and audio.
4. **Prebuilt libs: follow upstream.** Upstream has no submodules (no `.gitmodules`). It commits prebuilt `.a` files
   straight into `lib/` (e.g. `acf3f7775 Added missing lua libs`). The port does the same: `lib/lua/switch/liblua53.a`
   (and later CoopNet/libjuice), each with a `tools/switch/build_*.sh` that rebuilds it from a pinned upstream version.
5. **Packages:** `switch-curl` and `switch-mbedtls` are now installed (verified `libcurl.a`, `libmbedtls.a`).

## 7a. NSP requirement (new)

Besides the `.nro`, the build must produce an installable **`.nsp`** (`build/us_nx/sm64coopdx.nsp`).

- **Why it helps:** an installed title always runs as a full application. That removes the applet-mode memory
  problem; the `.nro` still needs title takeover.
- **Pipeline:** `elf2nso` → `main`, `npdmtool` (from a checked-in `res/switch/sm64coopdx.json`) → `main.npdm`, plus romfs
  and control (NACP + icon) → **hacBrewPack** → `.nsp`. This needs a fixed homebrew **title ID** (pick an unused
  `01xxxxxxxxxxx000` value and record it here).
- **Keys:** hacBrewPack needs the user's own `prod.keys` dumped from their console. **Never commit keys or put them in
  CI secrets.** The `make nsp` target takes `KEYS=path/to/prod.keys` and is skipped when it isn't given. CI builds only
  the `.nro`.
- **Installing:** an NSP needs CFW with signature patches. Document this; it is the user's own setup.
- **Hard rule still applies:** the NSP holds no ROM data or extracted assets. Assets come from the user's ROM at runtime
  on the SD.
- The NSP must find user data on the SD the same way the NRO does (no `argv[0]`). Detect it with `envIsNso()` /
  `envHasArgv()` and don't rely on the `.nro` path.
- Lands in Phase 1 as a packaging target. A title-mode launch is also the best yuzu test, since yuzu's NRO loader
  gives a different memory layout.

## 7b. Expected client-side scaling with many players (estimate, to be measured in Phase 7)

As a **client** the Switch is not spared the per-player work. Every remote player is a full Mario object locally:
- `bhv_mario_update` runs Mario physics for each one (`src/game/object_list_processor.c:245`).
- Lua `HOOK_BEFORE_MARIO_UPDATE` / `HOOK_MARIO_UPDATE` fire once per Mario, so per player, per frame.
- Its model is skinned and vertex-transformed and lit **on the CPU** in `gfx_sp_vertex` (`gfx_pc.c:767`), like all
  Fast3D geometry.

Cost therefore grows **linearly with player count** on a ~1 GHz Cortex-A57, plus nametags and DJUI. Network traffic
per player is small and isn't the bottleneck. Rough expectations, before any measurement:

| Scenario | Expectation |
|---|---|
| 2–4 players, vanilla, 30 fps | very likely fine |
| 5–8 players, vanilla or light mods | probably 30 fps; 60 fps unlikely |
| 9–16 players, or Lua-heavy gamemodes | likely below 30 fps in busy scenes |
| High-poly DynOS character packs | multiply the per-player render cost |

Main mitigations (Phase 7): `-O2`/LTO with NEON, skip vertex work for off-screen or distant players, an option to
draw distant players at low detail or not at all, audio mixing on a second core, and keeping Lua hook cost in check.
These are guesses until measured on hardware.

## 8. Phase 1 plan (done, see "Phase 1 results" above)

1. Makefile: `TARGET_SWITCH=1` block (devkitA64 `CROSS`, libnx specs, `-D__SWITCH__ -DTARGET_SWITCH`, forced
   `DISCORD_SDK=0 COOPNET=0 UPDATER=0 TARGET_BITS=64`, `BUILD_DIR=build/us_nx`, `elf2nro` with icon and NACP, romfs
   with `lang/ palettes/ mods/ dynos/ licenses/`), plus a `make nsp KEYS=…` target (§7a).
   All flag changes go inside one `ifeq ($(TARGET_SWITCH),1)` block to keep merge conflicts small.
2. `lib/lua/switch/liblua53.a` built from Lua 5.3.5 source.
3. `src/pc/switch/platform_switch.c`: `romfsInit`, `socketInitializeDefault`, nxlink stdio, SD log file,
   applet-type check with a clear "launch via title takeover" message, and the user/resource paths.
4. `platform.c` / `misc.c` / `mumble.c`: minimal `#ifdef TARGET_SWITCH` guards.
5. Boot in `--headless`-equivalent mode with `gfx_dummy` (forced on Switch for now), extract assets, run the loop,
   log to `sdmc:/switch/sm64coopdx/log.txt`.
6. Confirm the Linux PC build still compiles.

## 9. Disabled / not-yet-done features on Switch

| Feature | Reason | Where |
|---|---|---|
| Discord SDK / rich presence | closed-source, no Switch binary | `switch.mk` (`DISCORD_SDK=0`) |
| Auto-updater | can't replace a running `.nro`; no updater binary | `switch.mk` (`UPDATER=0`), `misc.c` `update_game` |
| CoopNet | no aarch64-switch build yet (Phase 5); `COOPNET=1` errors out | `switch.mk` |
| Mumble positional audio | needs POSIX shared memory | `mumble.c` |
| `open_url` / `open_folder` | no process spawning (may use the `webWifi` applet later) | compile to no-ops |
| Rendering, window, keyboard/text input | SDL/GL backend excluded; deko3d in Phases 2–3 | `switch.mk`, `pc_main.c` |
| HOME button / applet messages | nothing calls `appletMainLoop()` yet; the Switch window manager (Phase 2) will | — |

The **update checker stays on**: it only shows an "update available" popup and fails quietly when offline.

## 10. Reference projects and licenses

No code borrowed from the reference projects yet. Each entry should name the project, the file(s) and the license.

| Item | Source | License |
|---|---|---|
| `res/switch/sm64coopdx.json` | derived from devkitPro `switch-examples` `templates/sysmodule/sysmodule.json` | public domain (switch-examples) |
| `lib/lua/switch/liblua53.a` | Lua 5.3.5, lua.org (sha256-verified in `tools/switch/build_lua.sh`) | MIT |
| `res/switch/icon.jpg` | converted from the project's `res/icon.ico` | project asset |

License notices shipped in romfs (`res/switch/licenses/`): libnx (ISC), SDL2 (zlib), curl, zlib, Lua (MIT),
uam + Mesa (zlib/MIT, for Phase 3).
