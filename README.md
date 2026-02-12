# Toy Story 2 Fix
Toy Story 2 Fix is a program that fixes and enhances Toy Story 2 for the PC. Features include:
* Fixes the "Unable to Enumerate Device" error.
* Enables the selection of 32-bit resolutions.
* Fixes the framerate issues that can occur on modern PCs.
* Allows the player to immediately skip the ESRB and Copyright screens.
* Allows the game to be played in widescreen with no 3D stretching.
* Increases the render distance of levels.
* Works on every regional release of the game.

# Download
Get the latest version [from the releases page](https://github.com/RibShark/ToyStory2Fix/releases/latest). Extract the ZIP file into the folder you installed Toy Story 2 into.

# Configuration
You can enable or disable any part of the patch by opening the `scripts\ToyStory2Fix.ini` file and setting the options to `true` or `false`.

`FixFramerate` limits gameplay to 60 FPS by default. If `NativeRefreshRate` is enabled, the cap follows the detected game-monitor refresh rate (with a primary-monitor fallback during startup). On supported executables, gameplay simulation remains 60 Hz while rendering can run above 60 Hz. Demo mode remains capped at 30 FPS.

If auto detection still reports 60 Hz on your setup, set `TargetRefreshRate` in `ToyStory2Fix.ini` to a value like `120`, `144`, or `165` to force the cap.

Advanced framerate safety options are also available in `ToyStory2Fix.ini`:
* `AutoFallbackTo60` automatically downgrades unstable high-refresh states.
* `StartupGuardMs` keeps risky timing paths disabled for a short startup window.
* `AllowFrontendCustomTiming` and `AllowFrontendZeroStep` can be used for frontend/menu timing experiments.
* `FramerateDiagnostics` enables debug logging via `OutputDebugString`.
