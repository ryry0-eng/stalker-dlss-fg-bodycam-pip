# Slop-Ray v0.7.2-dev + Gamepad At Your Service (v0.2.2-preview)

Merged C++ source tree combining:
- **Slop-Ray Engine `v0.7.2-dev`** (`smoke-0n-the-water/stalker-dlss-fg-bodycam-pip` commit `431a1adc5`): Bodycam, true PiP scopes, DX12/DLSS/FSR 3/Frame Gen, and v0.7.2 crash fixes.
- **Gamepad At Your Service `v0.2.2-preview`** (`oesoy1/Gamepad-At-Your-Service`): native SDL3 `3.4.18` controller input, 360° analog movement, response curves, RayQuery aim assist, C++ UI focus/cursor/OSK, and all Lua exports.

## Bodycam + PiP Integration Fixes Included
- **`Actor.cpp`**: `UpdateGamepadLookInput()` runs right before `BodycamReference::EngineFrameAdapter::Update()`, so both Bodycam Reference mode and Classic camera receive the updated right-stick view on the same frame.
- **`ActorInput.cpp`**: `IR_OnGamepadLook()` scales sensitivity using Slop-Ray's PiP SVP FOV curve (`g_svp_sens` / `g_svp_sens_curve`) when aiming through true PiP scopes, and feeds `m_bodycam.AddMouseLookDelta()` so Bodycam sway/inertia reacts smoothly to right-stick turning.
- **`Actor_Movement.cpp`**: Preserves `BodycamReferenceGait()`, `BodycamReferenceSprintDirection()`, and `BodycamReferenceMoveScale()` alongside GAYS's 360° analog velocity and sprint cone.

## How to Build

### Option A — GitHub Actions (No Visual Studio Required)
1. Fork `smoke-0n-the-water/stalker-dlss-fg-bodycam-pip` on GitHub (or create a new private repo with this folder).
2. Push this folder (note: `src/3rd party/SDL3/` is optional in Git — the workflow auto-downloads SDL3 `3.4.18` if missing).
3. In GitHub → **Actions** → **Build Slop-Ray 0.7.2 + Gamepad** → **Run workflow**.
4. Download the `SlopRay-0.7.2-Gamepad-DX12` (and/or `DX11-AVX`) artifact containing `AnomalyDX12.exe` + `SDL3.dll`.

### Option B — Local Visual Studio 2022 Build Tools
`SDL3 3.4.18` is already staged in `src\3rd party\SDL3\`. Open **x64 Native Tools Command Prompt for VS 2022** here and run:
```bat
msbuild /m /p:Configuration=DX12 /p:Platform=x64 src\engine-vs2022.sln
msbuild /m /p:Configuration=DX11-AVX /p:Platform=x64 src\engine-vs2022.sln
```
Output lands in `_build\_game\bin_dbg\AnomalyDX12.exe` (and `SDL3.dll`).

## How to Install Together
1. Run the Slop-Ray v0.7.2-dev installer from `B:\SlopRay_0.7.2\SlopRay v0.7.2-dev\` (installs all Bodycam/PiP/DLSS/FG mods, DLLs, and `00_modded_exes_gamedata.db0`).
2. Copy the compiled `AnomalyDX12.exe` (and/or `AnomalyDX11AVX.exe`) + `SDL3.dll` into `B:\Anomaly\bin\`, replacing only the stock Slop-Ray `.exe`.
3. Install the **Gamepad At Your Service** `gamedata\` mod into MO2 at the bottom of your load order — **do NOT run `Install_Engine.cmd`** (that would overwrite your merged Slop-Ray+Gamepad `.exe`).
