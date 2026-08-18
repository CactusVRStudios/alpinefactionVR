<p align="center">
  <img src="docs/red-faction-vr-logo.png" alt="Red Faction VR" width="640">
</p>

# Alpine Faction VR

Alpine Faction VR is a highly experimental virtual-reality implementation for the 2001 FPS **Red Faction**, built on top of [Alpine Faction](https://github.com/GooberRF/alpinefaction).

> [!CAUTION]
> This is an early alpha. It is incomplete, probably does not work correctly on many systems, and may crash or cause severe visual, input, or gameplay issues. Back up your files and saves before trying it.

## Current scope

The project adds an optional OpenXR path to Alpine Faction's Direct3D 11 renderer, including stereoscopic rendering, headset tracking, VR-oriented camera and weapon handling, and configurable snap or smooth turning. It currently targets the **singleplayer campaign only**. Multiplayer and dedicated-server use are unsupported.

An active OpenXR runtime and a working PC VR setup are required. VR mode forces the Direct3D 11 renderer.

## Installation

1. Install a legitimate copy of **Red Faction**.
2. Install the regular release of [Alpine Faction](https://alpinefaction.com). This prepares the game installation and patches it to the required `RF_120na` version.
3. Download the latest ZIP from this repository's [Releases](https://github.com/CactusVRStudios/alpinefactionVR/releases) page.
4. Open the ZIP and extract the contents of the `VR Mod` folder over your installed **Alpine Faction** directory, replacing files when prompted.
5. Start `AlpineFactionLauncher.exe`, open **Options**, enable **VR / OpenXR**, and select your preferred turn mode.
6. Make sure your headset's OpenXR runtime is active, then launch the singleplayer campaign.

You can also enable VR with the `-vr` command-line option. Settings are stored in `alpine_settings.ini`.

## Removing the VR build

Reinstall the regular Alpine Faction release to restore its original files.

## Building from source

This project builds for Win32 with Visual Studio and CMake. Configure with OpenXR enabled, then build the Release configuration:

```powershell
cmake -S . -B build-afvr -G "Visual Studio 17 2022" -A Win32 -DAF_ENABLE_OPENXR=ON
cmake --build build-afvr --config Release
```

The pinned Khronos OpenXR SDK is fetched during configuration, and the 32-bit loader is linked statically. See [docs/BUILDING.md](docs/BUILDING.md) for more information.

## Credits and license

All credit for Alpine Faction goes to the **Alpine Faction development team**. This experimental VR work would not exist without their extensive Red Faction patch and the projects on which it is based.

Alpine Faction is a fork of [Dash Faction](https://github.com/rafalh/dashfaction). Source code is licensed under the Mozilla Public License 2.0; see [LICENSE.txt](LICENSE.txt) and [resources/licensing-info.txt](resources/licensing-info.txt) for full licensing and contributor information.
