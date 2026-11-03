# MOI3 NAR CarPlay Maps in Virtual Cockpit

Experimental CarPlay AltScreen support for the LG MOI3/MIB3 head unit in
North American MQB vehicles. It asks the iPhone for the 1280×480 CarPlay map
stream and forwards the H.264 video to the digital cluster over the factory
MOI3 cluster-video network.

> **Beta status:** the R8410L firmware has been fully checked statically and
> the payload builds cleanly, but this release has not yet completed an
> end-to-end test in a vehicle. The installer therefore runs for one boot and
> automatically falls back to stock after any reboot.

## Supported firmware

This first beta supports **only**:

- LG MOI3 / MIB3 for MQB 
- `MOI3_EU_SE_R8410L`, MU `0841`
- part number `575919360B`
- CarPlay plug-in `320.17.4`

The `EU` text in this train name is misleading: the supplied build contains
North American English, Canadian French, and Mexican Spanish content. Every
critical stock binary is hash-checked before installation. See
[TARGETS.md](TARGETS.md) for the exact fingerprints.

## What the beta changes

The patch installs one library and a small launcher wrapper in the writable
CarPlay application directory. It does not modify the read-only root file
system, bootloader, HMI, navigation database, coding, or component protection.

On the first CarPlay start after installation, the library:

1. advertises CarPlay AltScreen stream type 111 at 1280×480;
2. receives and decrypts that H.264 stream;
3. wraps it in the MOI3 cluster's MPEG-TS/RTP format; and
4. sends it to `[ff14::2:4]:42800` through `eth0.5`.

The normal center-screen CarPlay display remains stock. Android Auto is not
part of this beta.

## Before installing

You need:

- the exact supported firmware and an LG MOI3 unit with a digital cluster;
- a stable 12 V supply or a running vehicle;
- a FAT32 USB drive;
- a compatible MOI3 LG engineering/post-script container that executes
  `fix_hw_partnumber.sh` as root.

The engineering container is **not included** because it is proprietary.
The normal signed R8410L update alone does not provide the external root-script
trigger. Do not edit or resign the full R8410L firmware package for this beta.

## Install the one-boot beta

1. Download `moi3-nar-carplay-maps-r8410l-beta.zip` from Releases.
2. Format a USB drive as FAT32.
3. Put your compatible MOI3 LG engineering/post-script container on the USB
   drive, preserving its `Data` and `Meta` folders.
4. Extract the beta ZIP over the root of that same USB drive. The root should
   contain:

   ```text
   Data/
   Meta/
   fix_hw_partnumber.sh
   ENABLE_ALTSCREEN.txt
   moi3-nar/
     payload/altscreen_handler.so
     payload/altscreen_handler.so.sha256
     scripts/install.sh
     scripts/uninstall.sh
     scripts/collect.sh
   ```

5. Turn the ignition on and keep power stable. Disconnect the phone.
6. Insert the USB drive.
7. Open the Red Engineering Menu by holding **MENU** for about 5 seconds.
8. Open **Update**, select the USB drive, and start the update.
9. Confirm the summary shows the post-script completed successfully. Check
   `moi3nar-install.log` on the USB: it must end with `INSTALLED`.
10. Use **Resume** and allow the unit to reboot normally.
11. Connect the iPhone, start Apple Maps, Google Maps, or Waze, enter a route,
    and select the navigation/map view in the Virtual Cockpit.

Do not retry after an `ABORTED` result. Share the log in a GitHub issue; the
hash gate stopped the patch before changing CarPlay.

## Safety and recovery

- The original CarPlay binary is hash-verified and backed up before changes.
- The patch is armed for one boot only.
- Any later reboot automatically starts the preserved stock binary.
- To disable it immediately, create
  `/data/lge/smartlink/carplay/MOI3NAR_DISABLE` and restart `carplay.service`.
- A full uninstall restores the original binary and verifies its hash first.

### Full uninstall from USB

1. Delete `ENABLE_ALTSCREEN.txt` from the prepared USB.
2. Create an empty file named `REVERT.txt` in the USB root.
3. Run the same engineering/post-script update once more.
4. Confirm `moi3nar-revert.log` says `UNINSTALLED` and reboot.

## Collect logs for a test report

After the test, use a separate USB or change the prepared drive as follows:

1. Remove `ENABLE_ALTSCREEN.txt` and `REVERT.txt`.
2. Create `COLLECT_LOGS.txt` in the USB root.
3. Run the post-script container again.
4. Upload the new folder under `MOI3_NAR_LOGS/` to the GitHub issue.

Please include the vehicle, cluster part number, whether wired or wireless
CarPlay was used, the map app, and exactly what appeared in the cluster.

## Build from source

The handler is AArch64 and must remain compatible with glibc 2.17. With Zig
installed:

```sh
make verify
make package
```

The verifier requires AArch64, `libdl.so.2`, GLIBC 2.17 only, and all seven
exported hooks. The release builder includes no extracted firmware files.

## Known limits

- End-to-end vehicle behavior is not yet confirmed.
- The first release is deliberately locked to R8410L/MU0841.
- A compatible root-capable post-script transport is required.
- `viewAreas` is disabled by default because incomplete view-area advertising
  can make the iPhone tear down the session.
- CarPlay crashes can trigger the stock service's immediate-reboot policy; the
  one-boot arm prevents a persistent reboot loop.

## Credits and license

The AltScreen handler is derived from
[lipedesign/mib3-moi3-altscreen](https://github.com/lipedesign/mib3-moi3-altscreen).
Pipeline details were cross-checked against
[lipedesign/mib3-moi3-lg-research](https://github.com/lipedesign/mib3-moi3-lg-research),
[lipedesign/moi3-cluster-pipeline-re](https://github.com/lipedesign/moi3-cluster-pipeline-re),
and [githubAll3/moi3_LG_vc](https://github.com/githubAll3/moi3_LG_vc).

MIT licensed. This independent project is not affiliated with Volkswagen AG,
SEAT, LG Electronics, Apple, or Qualcomm. Vehicle software modification can
affect reliability, warranty, and regulatory compliance; use at your own risk.
