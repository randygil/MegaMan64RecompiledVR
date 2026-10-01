# Mega Man 64: Recompiled for Android VR (Meta Quest)

A standalone Android build of the game with OpenXR support, made for Meta Quest 2, 3, 3S and Pro. Gameplay is played in
first person with stereo 3D and head tracking: the buster is on the left hand and the special weapon on the right.
Menus and cutscenes are shown on a floating screen.

## Installing on the headset

1. Turn on developer mode for the headset (Meta Horizon app on the phone: Devices > Headset settings > Developer mode)
   and connect it to the PC with a USB cable. Accept the USB debugging prompt inside the headset.
2. Install the APK:
   ```
   adb install -r app-release.apk
   ```
3. Start the game once from the headset's app library (Unknown Sources), then quit it. This creates its folder.
4. Copy the US ROM (`.z64`, `.n64` or `.v64`) into that folder. The game imports it on the next start and deletes the
   copy:
   ```
   adb push MegaMan64.us.rev1.z64 /sdcard/Android/data/com.megaman64recomp.vr/files/MegaMan64.us.rev1.z64
   ```
   Saves go to `/sdcard/Android/data/com.megaman64recomp.vr/files/saves/`. A save from the PC version
   (`%LOCALAPPDATA%\MegaMan64Recompiled\saves\megaman.n64.us.1.0.bin`) can be pushed there too.

Logs: `adb logcat -s MegaMan64`.

## Controls

| Controller           | Action                                             |
|----------------------|----------------------------------------------------|
| Left trigger         | Buster (fires from the left hand, where it points) |
| Right trigger        | Special weapon (fires from the right hand)         |
| Left stick           | Move (forward is where you look, sideways strafes) |
| Right stick          | Snap turn 30 degrees (menus: navigate)             |
| A                    | Jump / accept                                      |
| B or X               | Interact / talk                                    |
| Y                    | Map                                                |
| Grips                | Lock-on / strafe (Z and R)                         |
| Menu (left)          | Pause                                              |
| Left stick click     | L                                                  |
| Right stick click    | Settings menu (shown on the floating screen)       |

Leaning moves the camera a little. Walking around the room drags Mega Man's head along with you.

Mega Man's arms reach from his shoulders to the controllers, and his legs are drawn where the game animates them, so
kicks show when looking down. The settings menu has a **VR Field of View** option (General tab): how much of the
headset's view the game fills. The headset runs at 90 Hz (or 60 Hz), a multiple of the game's 30 fps, so every frame
is shown for the same time.

## Building

Needs, all portable (no Android Studio): JDK 17, the Android SDK with platform 34, build-tools 34, NDK 27 and CMake
3.30, Gradle 8.10, and the Khronos OpenXR loader AAR (`org.khronos.openxr:openxr_loader_for_android` from Maven
Central, extracted). The desktop build must be done first, since `file_to_c` runs on the host during the Android build.

```
export ANDROID_HOME=.../sdk ANDROID_NDK_HOME=.../sdk/ndk/27.2.12479018 JAVA_HOME=.../jdk17
export OPENXR_SDK=.../openxr_loader_for_android   # extracted AAR: jni/arm64-v8a and prefab/modules/headers
./android/build_native.sh                          # libmain.so and dependencies into app/src/main/jniLibs
cd android && gradle assembleRelease               # app/build/outputs/apk/release/app-release.apk
```

SDL2 and FreeType are downloaded by CMake (FetchContent). RT64 and its plume submodule point to forks with the changes
the port needs (Android support, the external swap chain and workload hooks, plume's Vulkan creation hooks):
[rt64-mm64vr](https://github.com/randygil/rt64-mm64vr) and [plume-mm64vr](https://github.com/randygil/plume-mm64vr),
branch `mm64vr`. Clone with `git clone --recursive`, or run `git submodule update --init --recursive` after cloning.

## How it works

- `src/vr/vr_openxr.cpp`: the OpenXR session. RT64 renders into an OpenXR swap chain instead of the window, through
  a swap chain object it creates with `RT64::Application::Core::createExternalSwapChain`. The Vulkan instance and
  device are created by plume with the extensions and GPU that OpenXR asks for.
- `patches/vr.c`: the game side. The head pose drives the gameplay camera, and the main draw renders the scene once per
  eye into the two halves of the 320x240 frame, with each eye's offset folded into its projection. The HUD is drawn at
  half size in each eye. Mega Man's arms are moved to the controllers and shots fly where the hand points.
- The controllers drive a virtual SDL game controller, so the game and the menus see a regular pad.

Development aids (desktop VR build, `-DRECOMP_VR=ON`): `MM64_VR_DEBUG=1` renders VR on the desktop window with a
simulated head and hands, `MM64_AUTOSTART=1` (or an `autostart.txt` file in the app folder) skips the launcher. The
Meta XR Simulator works as a desktop OpenXR runtime through `XR_RUNTIME_JSON`.
