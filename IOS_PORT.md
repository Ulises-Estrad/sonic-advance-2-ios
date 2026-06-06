# Sonic Advance 2 iOS Port Notes

This fork is a private iPhone port path for the existing SDL version of Sonic Advance 2.

The first milestone is a Sideloadly-ready physical-device IPA:

- workflow: `iOS Device IPA`
- artifact: `sonic-advance-2-ios-device-arm64`
- IPA: `build-products/SonicAdvance2-ios-device-adhoc.ipa`
- bundle id: `dev.local.sonicadvance2.ios`
- app display name: `Sonic Advance 2`

## Current iOS Target

The `sdl_ios` Makefile target reuses the upstream SDL platform layer and builds it with Xcode's `iphoneos` SDK:

```bash
make sdl_ios SDL2_IOS_ROOT=/path/to/sdl2-ios-prefix IOS_SDK=iphoneos IOS_ARCH=arm64
```

GitHub Actions builds SDL2 from source, builds `sa2.sdl_ios`, packages it as:

```text
Payload/
  SonicAdvance2.app/
    Info.plist
    PkgInfo
    SonicAdvance2
```

The app is ad-hoc signed so Sideloadly can re-sign/install it on a physical iPhone.

## Current Baseline

- first passing iOS artifact run: `27059448898`
- artifact: `sonic-advance-2-ios-device-arm64`
- IPA: `build-products/SonicAdvance2-ios-device-adhoc.ipa`
- producing commit: `cf439d68`

The inherited upstream `CI` workflow is manual-only in this fork because it expects upstream ROM/assets/gh-pages infrastructure that is not required for the iPhone IPA path.

## Controls

The first iOS build includes a simple fixed touch overlay:

- left joystick: D-pad
- `A`: GBA A
- `B`: GBA B
- `L`: GBA L
- `R`: GBA R
- `ST`: Start
- `SE`: Select

This is intentionally a first-pass control surface. After first render/install proof, refine the layout based on physical iPhone playtesting.

## Diagnostics

The iOS build writes Files-visible diagnostics under:

```text
On My iPhone/
  SonicAdvance2/
    SA2_DIAGNOSTICS/
      CURRENT_SESSION_RUNTIME_LOG.txt
      PREVIOUS_SESSION_RUNTIME_LOG.txt
      LATEST_CRASH_OR_ABRUPT_EXIT_REPORT.txt
```

If the app crashes, reopen it once and send `LATEST_CRASH_OR_ABRUPT_EXIT_REPORT.txt`. If that is not enough, also send `CURRENT_SESSION_RUNTIME_LOG.txt` and `PREVIOUS_SESSION_RUNTIME_LOG.txt`.

Saves are stored in iOS Application Support through SDL's preference path as `sa2.sav`.

## First Test Checklist

1. Download `sonic-advance-2-ios-device-arm64` from the latest successful GitHub Actions run.
2. Extract the artifact.
3. Install `build-products/SonicAdvance2-ios-device-adhoc.ipa` through Sideloadly.
4. Launch on iPhone.
5. Check:
   - app opens,
   - title/menu renders,
   - BGM/SFX play,
   - touch overlay is visible,
   - joystick/buttons navigate/start gameplay,
   - diagnostics folder appears in Files.

Do not send changes upstream to `SAT-R/sa2`; this iOS port is maintained separately.
