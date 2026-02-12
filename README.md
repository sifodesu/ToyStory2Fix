# Toy Story 2 Fix
Toy Story 2 Fix is a program that fixes and enhances Toy Story 2 for the PC. Features include:
* Fixes the "Unable to Enumerate Device" error.
* Enables the selection of 32-bit resolutions.
* Fixes the framerate issues that can occur on modern PCs.
* Allows the player to immediately skip the ESRB and Copyright screens.
* Allows the game to be played in widescreen with no 3D stretching.
* Reduces z-buffer fighting by enforcing stable depth-state setup.
* Increases the render distance of levels.
* Works on every regional release of the game.

# Download
Get the latest version [from the releases page](https://github.com/RibShark/ToyStory2Fix/releases/latest). Extract the ZIP file into the folder you installed Toy Story 2 into.

# Configuration
Configure options in `scripts\ToyStory2Fix.ini`.

The INI now uses grouped sections:
* `[Framerate]` for timing/refresh behavior (`enabled`, `native_refresh`, `target_refresh_rate`, `auto_fallback_60`, `startup_guard_ms`, diagnostics/frontend options).
* `[Rendering]` for widescreen/depth/render-distance behavior (`widescreen`, `zbuffer_fix`, `increase_render_distance`, `render_distance_scale`, `render_distance_max`).
* `[Compatibility]` for device/splash compatibility patches (`allow_32bit`, `ignore_vram`, `skip_splash`).

Framerate defaults keep gameplay simulation at 60 Hz while allowing higher render cadence on supported executables. Demo mode remains capped at 30 FPS.

If auto-detection reports 60 Hz on your setup, set `[Framerate] target_refresh_rate` to your panel rate (`120`, `144`, `165`, etc.).

Legacy flat keys under `[ToyStory2Fix]` are still accepted as fallback aliases for compatibility.
