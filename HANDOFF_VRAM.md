# Cartograph — VRAM Efficiency Work — Handoff

**Date:** 2026-06-27
**Branch:** `Cartograph`
**Where this work lives:** `Mods/GameFeatures/Cartograph/` (the rest of the repo is the SML dev project)
**Toolchain:** Satisfactory **1.2**, **SML 3.12** (`>=491125`), Unreal Engine **5.6.1-CSS**

> This file was written in WSL. Continue from a **native Windows** clone — the Unreal editor,
> C++ compile, and Alpakit packaging cannot run from WSL.

---

## What the mod is
Cartograph overlays your built structures onto the in-game Satisfactory map. It draws every
buildable into a large off-screen `UCanvasRenderTarget2D`, indexed by a quad tree, updated live via
SML engine hooks, paced with UE5Coro coroutines, and replicated for multiplayer. The overlay is shown
in a widget on the vanilla map screen, with a layered category toggle menu.

## The problem we set out to fix
Reported symptom: the mod eats a lot of **VRAM** and causes **other textures to go blurry**.

**Root cause (confirmed):** the render target asset
`Content/UI/CanvasRenderTarget_BuildingVisualization.uasset` is **8192×8192, RTF_RGBA8, with auto mips** =
**~256 MB (≈340 MB with mips)** of VRAM, held for the **entire session** even when the map is closed.
Render targets live *outside* the texture streaming pool, so this silently starves the pool → the game
drops mip levels on other (vanilla) textures → blurry textures. The mod's own icon/foundation textures
are tiny (24×56, 32×32) and are NOT the problem.

---

## What we implemented (C++ — DONE, but NOT yet compiled)

All edits are in `Mods/GameFeatures/Cartograph/Source/Cartograph/`. Three behaviors, all now
**config-driven**:

1. **Configurable resolution** via a **dropdown** (2K / 4K / 6K / 8K).
2. **Mips on/off** (mips ≈ +33% VRAM).
3. **Free-on-close**: release the render target's VRAM whenever the map is closed; reallocate + repaint
   on open. While the map is closed, building add/remove still updates CPU data but does **no GPU work**.

Default behavior (4K, mips off, free-on-close) ≈ **64 MB only while the map is open, ~0 during play**,
vs the old permanent ~340 MB.

### Files changed
- **`Public/CartographGameInstanceModule.h`**
  - Replaced `constexpr int RENDER_TEXTURE_SIZE = 8192` with runtime `GRenderTextureSize` (default 4096).
  - Added `UENUM ECartographRenderResolution { Res2K=0, Res4K=1, Res6K=2, Res8K=3 }` + `RenderResolutionToPixels()`
    (2048/4096/6144/8192). **This enum is what backs the config dropdown.**
  - `PIXEL_PER_CENTIMETER[]` (constexpr) → `pixel_per_centimeter_x()/y()` functions.
  - Coordinate templates now use `GRenderTextureSize`.
  - Declared `EnsureRenderTargetReady()`, `ReleaseRenderTargetResource()`.
  - Made `OnVanillaMapMenuShown` non-const; added `OnVanillaMapMenuHidden()`.
  - Added members `bMapVisible`, `bGenerateMips`, `bFreeRenderTargetWhenClosed`.
- **`Public/Cartograph_ConfigStruct.h`** — added config fields: `int32 RenderResolution`, `bool GenerateMapMips`,
  `bool FreeRenderTargetWhenMapClosed`. (Field names MUST match the config-asset property names exactly.)
- **`Private/CartographGameInstanceModule.cpp`**
  - `OnWorldLoaded`: latches the 3 knobs from config; reconfigures the RT down from 8192 to the chosen
    size/mips; frees its VRAM immediately if free-on-close is on.
  - `RedrawMapCoroutine`: after updating CPU data, **skips GPU work + RT allocation when `!bMapVisible`**.
  - `OnVanillaMapMenuShown`: allocate + full repaint on open.
  - `OnVanillaMapMenuHidden`: free on close (deferred to `OnCoroutineFinishedOrCancelled` if a draw is in flight).
  - Replaced the old constexpr knobs with the runtime members; updated all `RENDER_TEXTURE_SIZE` /
    `PIXEL_PER_CENTIMETER` usages.

Verified by inspection only — **no leftover references to the old identifiers**. NOT compiled (no UE
toolchain in WSL).

---

## What's LEFT TO DO (all on Windows)

### A. Install the toolchain (see below)
### B. Editor-only steps (cannot be done in source files)
1. **`Content/Cartograph_Config`** (Mod Configuration asset) — add 3 properties, names matching the struct:
   - `RenderResolution` → **Integer**, widget class **CP_Integer**, `WidgetType = Enum`,
     `EnumClass = ECartographRenderResolution` → renders the **2K/4K/6K/8K dropdown**. Default `1` (4K).
   - `GenerateMapMips` → **Boolean** checkbox. Default `false`.
   - `FreeRenderTargetWhenMapClosed` → **Boolean** checkbox. Default `true`.
   - (SML has no native enum type — the dropdown comes specifically from CP_Integer's `CPI_Enum` widget.)
2. **Bind the close event:** in the map menu widget Blueprint (where `OnVanillaMapMenuShown` is already
   called), call **`OnVanillaMapMenuHidden`** on the widget's `Destruct`/hide. If skipped, everything
   still works but the RT just isn't freed between viewings.
3. *(Optional)* Set `Content/UI/CanvasRenderTarget_BuildingVisualization` default Size X/Y to 4096 so it
   never even briefly allocates at 8192 on load.

### C. Build + deploy
- First open of `FactoryGame.uproject` compiles the C++ (confirms the changes build). Fix any errors here.
- Alpakit → set game path → **Alpakit Dev** on Cartograph with *Copy to game* → launch.

### D. Verify the fix
- Mod Config shows the resolution dropdown + the 2 checkboxes.
- `stat streaming` / `Stat GPU` in console: with map closed, more pool headroom; blurry textures sharpen.
- Open map → overlay paints in (progressive, coroutine-paced); close → VRAM reclaimed.
- Toggle resolution 8K↔2K and **reload the save** to feel the trade-off.

### Caveats to remember
- **Resolution is latched at world load** (screen positions are cached against it) — changes need a save reload.
- **Free-on-close needs the BP binding** (step B2) to actually free anything.
- Out-of-the-box with NO asset edits, the config reads defaults (RenderResolution=0 → **2K**, mips off,
  free-off) — still a huge VRAM win, good enough to confirm the mechanism before doing the editor work.

---

## Toolchain to install (Windows)
Authoritative source: **docs.ficsit.app → Development Setup** (engine download is access-gated).

1. **Satisfactory** (Steam/Epic) on the **1.2** build + **Satisfactory Mod Manager** (installs SML 3.12 for runtime testing).
2. **Visual Studio 2022** (Community ok) — workloads: *Desktop development with C++*, *Game development
   with C++*, *.NET desktop development*; components: latest Windows SDK, MSVC v143, .NET Framework 4.8
   SDK + targeting pack. (Rider works too, still needs MSVC underneath.)
3. **Custom Unreal Engine `5.6.1-CSS`** — link Epic↔GitHub, gain access, download the prebuilt fork
   (100 GB+ extracted). Stock Epic UE 5.6 will NOT work.
4. **Wwise** integration (version per the docs) — the project depends on it; editor won't fully open without it.
5. **This repo on Windows** (now at `D:\Development\Cartograph`). No Git LFS. Right-click the `.uproject`
   → Generate VS project files → open.
6. **Alpakit** — already in the project; just set the game path in its settings.

First engine download + first compile is a multi-hour / 100 GB+ one-time cost; C++ iteration is fast
afterward (Live Coding).

---

## Conversation context (so a fresh Claude can pick up)
- We scanned the mod, summarized what/how/does, confirmed it targets Satisfactory 1.2 (SML 3.12 / CL 491125).
- Diagnosed the VRAM/blurry-texture issue to the 8192² render target starving the streaming pool.
- Implemented the config-driven resolution dropdown + mips toggle + free-on-close lazy allocation (above).
- Next action when resuming on Windows: install toolchain → do editor steps B → build/deploy via Alpakit → verify.
