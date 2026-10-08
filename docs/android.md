# OpenTricky on Linux and Android

The same game as the Windows build, on SDL2 and OpenGL ES 3.0. One code path
serves both: the Android app is the Linux build packaged as `libmain.so`.

## What is different from Windows

| Part | Windows | Linux / Android |
|---|---|---|
| Renderer | Direct3D 11 (`xboxrecomp/src/d3d/d3d8_*.c`) | OpenGL ES 3.0 (`xboxrecomp/src/d3d/gles/`) |
| Combiner shaders | HLSL (`nv2a_psh.c`) | GLSL ES 3.00 (`nv2a_psh.c`, `OT_GLES`) |
| Window, keys, pads | Win32 window, XInput | SDL2 (`port/src/host_sdl.c`) |
| Touch controls | - | `port/src/touch_sdl.c` |
| Sound | XAudio2 | SDL audio (`apu_xaudio2.c`) |
| APU / AC'97 / DAC registers | page faults + x86-64 decoder (VEH) | page faults + x86-64 / AArch64 decoder (`platform/posix_fault.c`) |
| Win32 calls of the runtime | Windows | `platform/win32_compat.c` and `platform/posix/windows.h` |
| Launcher | the launcher window | settings in `SSX Tricky.ini`; Android asks for the disc image |

Not ported yet (all off by default on Windows too): the post chain and SMAA,
soft shadows, the GPU profiler. Vertex programs run on the GPU as on Windows,
as GLSL (`gles/gles_vsh.c`, the HLSL generator's rules); point sprites are
expanded on the CPU (ES 3.0 has no geometry shader).

Switches for testing: `OT_GL_VSH=0` runs vertex programs on the CPU;
`OT_GL_UPLOAD=map|sub` picks how vertices go up (mapped by default,
`glBufferSubData` on emulators); `OT_UPLOAD_STATS=1` logs the bytes uploaded
per frame; `OT_SHOT_DIR=dir OT_SHOT_EVERY=n` saves every n-th frame.

## Building for Linux

```bash
sudo apt install build-essential cmake ninja-build clang libsdl2-dev libgles-dev
# the translated game, from your disc image (as for Windows: docs/building.md)
python3 port/tools/extract_iso.py "/path/to/SSX Tricky (USA).iso" game_files
bash port/tools/fork_regen.sh
CC=clang cmake -S port -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-linux
"build-linux/SSX Tricky" --direct "/path/to/SSX Tricky (USA).iso"
```

Without `--direct` the game uses `SSX Tricky.ini` beside the executable
(written the first time; set `DiscImage=` in `[Game]`). `OT_TOUCH=1` shows
the touch controls, driven with the mouse.

## Building for Android

Needs the Android SDK with NDK 27.2 and CMake 3.22.1 (Android Studio installs
them), and the translated game in `port/src/recomp/gen/` (generate it as
above, on any machine). Then:

```bash
cd android
./gradlew assembleRelease                 # arm64-v8a and x86_64
./gradlew assembleRelease -PotAbis=arm64-v8a
```

The APK is in `android/app/build/outputs/apk/`. It contains code translated
from your copy of the game: keep it to yourself, as with the Windows build.

## Playing on Android

The first start opens the system's file picker: choose your **SSX Tricky
(USA)** Xbox disc image (`.iso`). It is read in place (nothing is copied) and
remembered. A controller works as on PC; without one, on-screen controls
appear at the first touch.

Files (settings, saves, log, screenshots) are in the app's folder,
`Android/data/io.github.opentricky.ssxtricky/files/`, which a USB cable
reaches. `SSX Tricky.ini` there takes the same settings as on Windows
(`Resolution`, `AntiAliasing`, ...).

Testing from a PC:

```bash
adb shell am start -n io.github.opentricky.ssxtricky/io.github.opentricky.OpenTrickyActivity \
    -e env "XBOX_FPS_LOG=1"
adb pull "/sdcard/Android/data/io.github.opentricky.ssxtricky/files/SSX Tricky.log"
```
