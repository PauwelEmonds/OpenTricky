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
| Launcher | OpenTricky.exe writes `settings.ini` | `settings.ini` by hand; Android asks for the disc image |

Not ported yet (all off by default on Windows too): the post chain and SMAA,
soft shadows, the GPU profiler. Windows-only from 0.1.1: the launcher
(OpenTricky.exe) and the crash reports with minidumps (a crash is written to
the log instead); online play (`XBOX_NET`) builds on BSD sockets but is
untested. The HD textures and the button icons work (the .rc's files are
compiled in by `port/cmake/EmbedResources.cmake`, PNGs decoded with zlib). Vertex programs run on the GPU as on Windows,
as GLSL (`gles/gles_vsh.c`, the HLSL generator's rules). Point sprites too:
ES 3.0 has no geometry shader, so each point is an instance of six vertices
whose shader builds the square (`XBOX_FIX_POINTS_GPU=0` expands them on the
CPU instead).

Shaders: every GL program is kept in `ShaderCache/` (in the data folder) as
the driver's binary, so a second run builds nothing. New vertex-program
shaders are built on a worker thread with its own EGL context; until one is
ready its draws take the CPU path, which looks the same. New combiner
(pixel) shaders build there too; meanwhile their draws use a combiner
"ubershader" (`gles/gles_psh_uber.c`) that reads the combiner registers at
run time -- the same pixels (`OT_PSH_UBER_CHECK=1` compares the two, draw by
draw; `OT_PSH_UBER=1` draws everything with it). A driver that refuses its own binaries (the emulator's)
gets a `.refused` marker and no disk cache. `OT_SHADER_CACHE=0` and
`OT_SHADER_ASYNC=0` turn either off; the log's `[SHADERS]` lines count both.

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

Without `--direct` the game reads `settings.ini` beside the executable (or in
`OT_DATA_DIR`); the first start without a usable disc image writes it with
the defaults: set `DiscImage =` in `[Game]`. `OT_TOUCH=1` shows
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

Or put the image into the APK, so the phone needs no picking: on Windows,
drag the `.iso` onto `pack_iso.bat` (or run
`pack_iso.bat "SSX Tricky (USA).iso" [in.apk] [out.apk]`). It writes
`OpenTricky-android-SSX-Tricky.apk` beside `OpenTricky-android-debug.apk`,
the image stored uncompressed as `assets/game.iso` (read straight out of the
installed APK), aligned and signed with the same debug key, so it installs
over the plain one. It needs the Android SDK build-tools and a JDK 11+, as
the build does. The APK must stay under 4 GB, so a full 7-8 GB dump needs
trimming first (`extract-xiso -r`); installing needs about twice the APK's
size free for a moment. The result has your game in it: keep it to yourself.

Phones get the game's own 16:9 menus (the `Menus` setting, forced by
`XBOX_WIDE_MENUS=16:9`); races fill the whole screen.

Files (settings, saves, log, screenshots) are in the app's folder,
`Android/data/io.github.opentricky.ssxtricky/files/`, which a USB cable
reaches. `settings.ini` there takes the same settings as on Windows (the file
OpenTricky.exe writes; copy one over, or edit it).

Testing from a PC:

```bash
adb shell am start -n io.github.opentricky.ssxtricky/io.github.opentricky.OpenTrickyActivity \
    -e env "XBOX_FPS_LOG=1"
adb pull "/sdcard/Android/data/io.github.opentricky.ssxtricky/files/SSX Tricky.log"
```
