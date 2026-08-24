# MGS3 Crouch Walk compatibility patch

This tree is a defensive compatibility rebuild based on the supplied 2026 fork and the supplied `METAL GEAR SOLID3.exe`.

## What changed

- Pattern scanning now searches executable PE sections only.
- All signature targets are resolved and validated before any hooks are installed.
- Invalid `-0x10` / `-0x46` adjusted addresses are rejected instead of underflowing.
- MinHook creation/enabling is checked; partially-installed hooks are rolled back.
- `VirtualProtect` and `FlushInstructionCache` are checked/used around the crouch/prone byte patch.
- The game module is acquired inside the worker thread instead of during DLL global initialization.
- Null checks were added around the motion/camo/status hooks and animation-control pointers.
- Malformed or missing INI values fall back to defaults instead of terminating the initialization thread.
- A log is written to `%TEMP%\\MGS3CrouchWalk.log` and to the debugger output.

## Important finding for the supplied 3.0.0 executable

The signatures present in the supplied source all have matches in the supplied executable. Therefore this patch does **not** pretend that the 3.0.0 problem is caused by missing signatures. The additional validation/logging is intentional: if the game still refuses to load the plugin, the log should identify whether MinHook, a derived target address, `InitializeCamoIndex`, or the executable's startup/loading environment is the failing layer.

## Build

Open `MGS3CrouchWalk.sln` in Visual Studio 2022 and build **Release | x64**. The project already targets `.asi` for x64 Release builds and links the included MinHook library.

The supplied game executable was not modified.
