# NetworkDirect Remote Control

Remote-control software designed for low-latency, high-framerate streaming using RDMA (NetworkDirect).

## Build
Use CMake to configure and build the project.

## Installation
To keep the session active across Logon UI, UAC prompts, etc., run the process as SYSTEM.

Options:
- Register service.exe as a Windows service and place main_service.exe and the `shaders` directory in `C:\NDR`. (Use `sc.exe` to register the service. main_service defaults to adapter/display 0 -- see service.cpp for display/index settings.)
- Or use an external tool to launch as SYSTEM (example: `psexec -i -s`).

To uninstall: remove the registered service and delete the `C:\NDR` directory.

## Usage
Terminology:
- Remote = the machine being streamed (server).
- Local = the machine sending inputs (client).

Command-line:
- Run as Local: `-s {Local IP address}`
- Run as Remote: `-c {Remote IP address} {Local IP address} {R|C}`  
  - R = raw (uncompressed BGRA32 frames)  
  - C = compressed (YUV440 subsampled frames)

## Control modes
Two cursor control modes:

- Gaming Mode (default build): cursor is clipped to the window and uses raw input for minimal latency.
- Remote Desktop Mode: free cursor handling; higher latency (no raw input). Enable by add the `ABSCURSOR` macro in InputNDSession.cpp and D2DWindow.cpp.

## Audio
Client primary audio device should be 48 kHz, 16-bit stereo.

## Hotkeys / Escaping
When the streamed window is focused most hotkeys are consumed by the Remote. To release the cursor or switch focus:
- Use Alt+Tab, or
- Press Ctrl+Alt+Shift+X to free the cursor without changing focus.

## Compression notes
- `R` sends uncompressed BGRA32 frames.
- `C` sends YUV440 subsampled frames. Compression can reduce bandwidth (approximately 1/3 less) but may increase GPU usage. Use `C` when bandwidth is the bottleneck.
