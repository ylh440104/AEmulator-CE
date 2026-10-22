# AEmulator Plus

A modified build of [uxazu/AEmulator](https://github.com/uxazu/AEmulator) 0.0.0.3.

AEmulator runs classic Android firmware (2.3–17) on a modern arm64 phone: a recovery ZIP, an Odin archive, a Google factory image or a GSI goes in, and the firmware's own system comes up in a normal app. No root, no PC.

This repository tracks that release and carries fixes for what was found in the code. It is not the upstream project; bugs of my own making belong here, not there.

## What is different

### Data safety

- A machine that is running can no longer be deleted or factory-reset. The lock is per image: a pid marker in `run/alive` is checked against `/proc`, so a machine left over by a crashed process is told apart from a live one.
- Changes made while a machine boots are kept. The boot used to write a snapshot taken minutes earlier, which rolled back a rename or a settings change made in the meantime.
- Renaming a machine no longer orphans its shared memory-card folder. The folder recorded at the last start is reused; a folder carrying the image id is adopted; only then is a new one made.
- A cloned container now carries the recovery ramdisk, and the links inside it point at the new container instead of the original.

### Resources

- The guest log files (`dev/log/*`) and the per-service logs (`run/*.log`) are capped at 16 MB, keeping the tail. Before, they grew without limit across every boot.
- An import checks free space before unpacking. A firmware that does not fit used to stop halfway, minutes in.

### Screen recording

- The guest's sound is recorded along with its screen (AAC, both tracks in one muxer). The PCM is taken from the playback path, so no recording permission is involved.
- If the phone has no AAC encoder, or the encoder never reports its format, the recording falls back to video only instead of losing the whole file.

### Log panel

- A filter, and the scroll position is kept while the guest log refreshes.

### Importer

- Firmware that cannot boot is refused at import time rather than after minutes of a failing start:
  - a tree that is not ARM (x86, MIPS, RISC-V — there is no translator for them here),
  - a tree without `/system/bin/app_process`,
  - a tree whose `BOOTCLASSPATH` comes out empty.
- A tree that has no `build.prop` gets its ABI from its layout (`system/lib64`, `app_process64`, `framework/arm64`), not always `armeabi-v7a`. A 64-bit dump used to be sent through the 32-bit translator, where its `app_process64` cannot load.

### Updater

- A release may carry more than one APK (the `v0003` copy installs next to this app). The update now picks this package's own file instead of whichever came first.

### Smaller

- The running-machine notification opens the machine it belongs to; it used to open an empty screen and finish.
- The accelerometer listener is dropped when a machine stops.
- `adb shell` notices a closed client, and a command that ran out of time says so in its output.
- The firmware folder uses the same casing as the shared card, and lists only the formats the importer actually reads.

## Building

```sh
./gradlew :app:assembleStandardDebug
```

Needs JDK 17 or newer, Android SDK 36 and NDK r28. The native parts under `native/*/` are built with their own scripts.

APKs built by CI are in the run's artifacts. They are debug-signed unless a release key is present; see `.github/workflows/build-apk.yml` for the four secrets it reads.

## License

GPL-3.0, as the original. Android, the vendor names and the firmware belong to their owners.

Upstream: [uxazu/AEmulator](https://github.com/uxazu/AEmulator) · [forum](https://aeforum.uxazuu.space/) · [site](https://aemulator.gt.tc)
