# The native part of the Android build: SDL2 (fetched; its Java glue is in
# src/main/java/org/libsdl) and the game of ../../port as libmain.so, which
# SDLActivity loads and starts through SDL_main.
#
# The translated game code (../../port/src/recomp/gen) is made from your own
# copy of the game, as for the Windows build (docs/building.md); generate it
# first.
cmake_minimum_required(VERSION 3.18)
project(OpenTrickyAndroid C)

include(FetchContent)
FetchContent_Declare(SDL2
    URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz
    URL_HASH SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL2)

# x86 semantics the translated code and the runtime were written for:
# signed char, and no fused multiply-add (ARM's default would round the
# game's float maths differently from the Xbox and the PC builds).
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsigned-char -ffp-contract=off -Wno-int-conversion -Wno-incompatible-pointer-types")
set(CMAKE_C_FLAGS_RELEASE "-O2 -DNDEBUG")

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../port ${CMAKE_BINARY_DIR}/port)
target_link_libraries(ssx_tricky PRIVATE log android)
