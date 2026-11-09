# Black screen and spinner while the UI is used (boot 12)

The user reports the display going black and showing the spinner while operating the phone.
On this board that is not a display failure: it is SpringBoard being restarted. Boot 12's
capture (`boot12-ios7.log`, 20:06 to 20:23) shows 26 `PICASSO_INJECT SpringBoard started`
lines; 17 of the 25 restarts are preceded by an app launch inside the same second or two, and
the last four come after the Data volume has filled.

Two independent faults feed it: the **512 MiB volatile Data volume runs out of space** while
the phone is used, and **every app is handed an ad-hoc library that AMFI refuses**. The
storage fault comes first (it makes every write fail), the injection fault comes second (it
makes every launch fail), and either one is enough to send SpringBoard back to its boot
screen.

## The Data volume fills

`NATIVE_OWNER_DATA stage=1 selected=1000005 path=/dev/disk3s1 logical_bytes=536870912`
(line 500) and `handle_mount:817 ... block count: 131072` (line 511): `/private/var` is a
512 MiB RAM-backed APFS volume, re-seeded from the boot image at every boot. Writes allocate
pages lazily, so a bigger logical disk costs nothing until it is written.

In this session it filled in about twenty minutes of use:

```
line 15633  apfs_lowdisk_check_volume:285: disk3s1 sending lowdisk notification: 8192 ...
line 21131  fs_tx_enter_internal:27549: disk3s1 biomed: ENOSPC: ttype DELETE num_blocks 6010 free_blocks 3134 ...
```

`free_blocks` is down to 3134 of 131072 (about 12 MiB). Once that happens an app cannot
write its caches, cannot be snapshotted, and dies; SpringBoard restarts. The volume is
volatile, so a reboot restores the free space and the cycle starts again — which is why the
phone works for a while after each flash and then degrades.


## The sequence

`PICASSO_INJECT <name> started with DYLD_INSERT_LIBRARIES=/private/var/picasso/libpicasso-sb.dylib`
is followed by

```
AMFI: '/private/var/picasso/libpicasso-sb.dylib': unsuitable CT policy 0 for this platform/device, rejecting signature.
AMFI: code signature validation failed.
validation of code signature failed through MACF policy: 1
check_signature[pid: N]: error = 1
Failed to send exception EXC_CORPSE_NOTIFY. error code: 5 for pid: N
```

and then a new SpringBoard. The pairs in boot 12:

| app given the library | SpringBoard restarted at line | gap |
| --- | --- | --- |
| Spotlight 5062 | 5211 | 149 |
| Spotlight 5662, Preferences 5751 | 5769 | 18 |
| Spotlight 6130 | 6153 | 23 |
| ScreenshotServicesService 6849, Spotlight 6917 | 7199 | 150 |
| MobileSlideShow 9861, StoreKitUIService 9927 | 10078 | 150 |
| PosterBoard 10198 | 10342 | 144 |
| Spotlight 11434, Preferences 11459 | 11468 | 9 |
| Spotlight 11795 | 12096 | 301 |
| Spotlight 12815 | 12878 | 63 |
| Spotlight 13937 | 14310 | 373 |
| PosterBoard 14419, Preferences 14469 | 15474 | 1005 |

The quiet stretches (419 s to 647 s, 841 s to 1006 s) are the phone sitting idle: the
restarts cluster where the user opened something. `board.toml` carried
`picasso_inject_apps=1`, so the kernel added that library to **every** app main executable.
The library is ad-hoc signed; AMFI refuses the signature for an app process, dyld then fails
the image, and the launch dies. SpringBoard, which is in the middle of the scene transaction,
is the process that ends up restarting. The nine restarts without an app launch in front of
them are the ones the storage fault above explains.

## Not established

* The restarting SpringBoard faults at the same unslid address in every capture
  (`PC=0x1d3c3da4c`, `ESR=92000007`, a level-3 translation fault at a heap address:
  `FAR=0x15bf05940`, `0x143f06b50`, `0x116abd000` in different runs). That is inside the
  phone's own `dyld_shared_cache_arm64*`, which is not on the host, so the frame is not
  symbolised here. Fixing the injection removes the trigger; it does not claim to explain
  that frame.
* backboardd itself died once, at 647 s, with SIGBUS (`exit_signal=10 code=a`,
  `FAR=0x112c2c040`) and came back. That is a separate, unexplained event.
* Apps may still fail to run for other reasons once they no longer receive the library: this
  board has no working Metal device, and an app that insists on one will not draw.

## Changes made

1. `scripts/prepare-picasso-ram-data.py`: the volatile Data volume is 1024 MiB instead of
   512 MiB (`DATA_BYTES`), with the two artifact checks following the constant instead of
   repeating the number. The seed is packed by nonzero page, so this does not grow the boot
   image; it only lets `/private/var` hold more before the lowdisk path is reached.
2. `board.toml`: `picasso_inject_apps=1` is gone. `picasso_inject_sb` still applies the
   status bar metric to SpringBoard; app status bars fall back to UIKit's own Pad metrics
   until the library can be signed in a way AMFI accepts for an app.
3. `drivers/NativeBoardButtons/ButtonsProbe.cpp`: volume-up is debounced on its own level bit
   and only counted after the line has rested released for about 300 ms. Boot 12 counted
   10 204 presses against 52 power presses and 14 volume-down presses in 22 minutes, because
   one GPIO block sat low for seconds at a time (`0x81 -> 0x80` while the key was untouched).
   The PON path gets the same treatment: a block whose status carries bits outside the two key
   pairs is no longer believed (boot 12: `status=0f released=04 diff=0b`, every poll, which kept
   the power key latched down and printed `pon bit outside the key map` thousands of times).
   That line is now printed four times per boot, not per poll.

## What the next boot should show

* `NATIVE_OWNER_DATA stage=1 ... logical_bytes=1073741824`, and no
  `apfs_lowdisk_check_volume` or `fs_tx_enter_internal ... ENOSPC` line for the first hour
  of use;
* no `PICASSO_INJECT` line for Spotlight, Preferences, PosterBoard, MobileSlideShow,
  ScreenshotServicesService or StoreKitUIService;
* `PICASSO_BTN alive ... presses=<power>/<volume-down>/<volume-up>` with the third number of
  the same order as the other two, not thousands;
* fewer, or no, SpringBoard restarts while apps are opened. If restarts continue without an
  app launch in front of them, neither the injection nor the storage fault was the only
  trigger and the faulting frame has to be resolved against the on-device cache.

## Later evidence: the crash is a bad surface descriptor (boot 14 / boot-vangogh-ios14)

The boot-12 storage/AMFI explanation above is not the cause of the swipes-to-spinner
behaviour the user still sees. Every later boot has a **repeating SpringBoard abort** with a
fixed faulting PC and `ESR=92000007` (EL0 data-abort, translation fault).  Resolving
`PC - cache_slide = 0x1d3c3da4c` against the on-device dyld shared cache gives
`/usr/lib/libate.dylib +0xfa4c` (Apple's texture/image library, image at 0x1d3c2e000).
The symbolization script now scans page-aligned `MH_MAGIC_64` headers instead of the dead
`imagesCountOld` field and can export the image (`--dump-image`) for offline disassembly.

Disassembly of that image (byte 0 = image base; the Mach-O header's own segment offsets
point into the cache and must not be used on the exported file):

```
0xf9e8  mov  x19, x0           ; x0 = surface descriptor
0xfa00  ldr  x8, [x20, #0x58]! ; x20 = x0 + 0x58
0xfa04  ldr  x9, [x20, #0x10]  ; = [x0 + 0x68]  stride
0xfa08  ldp  x11, x10, [x20,#-0x40]   ; x10 = [x0 + 0x20]  base
0xfa0c  madd x28, x9, x1, x10  ; x28 = stride * row + base
0xfa4c  ldr  q0, [x28]         <<< CRASH: read 16 bytes of source pixels
```

So SpringBoard dies reading the first row of a source surface whose base/stride came from
a descriptor that was never valid. Captured registers show `x1=0` on the first abort, i.e.
`x28` is just `[descriptor+0x20]`, an unmapped user address. This is consistent with the
single `IOMFB: default_fb_surface: No Default FB Surface found` line per boot.

FBUC telemetry from the newest boot (first 160 `IOMobileFramebuffer` calls):

| call | count | result |
| --- | --- | --- |
| selector 6, `sin=3 sout=0` | 65 | 39 success, 26 `0xe00002d5` = `kIOReturnBusy` |
| selector 5, `in=0x46c` | 30 | success; `swap 0x46c -> 0x508` re-laid 4 times |
| selector 4 | 30 | success |
| selector 8 (`CollectionsPoster`) | 13 | success |
| selector 79 / 50 | 3 / 2 | `0xe00002c7` = `kIOReturnUnsupported` |
| selector 12 (power change) | 3 | rejected only for power-off, `picasso-fb-reject-off=1` |

**Ruled out:** `scripts/tests/picasso-ios16-surface-abi-host.cpp` explicitly tests the
`structureOutputDescriptor` path and asserts the hook leaves it untouched, and it verifies
`picasso_ios16_limits` / `picasso_ios16_lock` / `picasso_ios16_swap` byte-for-byte. The
missing-conversion-on-descriptor theory is therefore wrong: that path is intentional.

**Next hypothesis:** an iOS 16 backboardd is talking to a retained iPadOS 17
IOMobileFramebuffer whose selector numbering differs; selectors 50 and 79 returning
`kIOReturnUnsupported` are the visible symptom. The fix is a selector translation table
(like the structure-size translation already in the hook), not another structure re-layout.

boot-vangogh-ios14 (GPT name fix + PAS panic fix, `sheng-wlan-pas=0`) measured on the phone:
0 `panic(cpu`, 0 PAS probe, 34 correctly named GPT partitions, 0 `GPT geometry rejected`,
but **3 SpringBoard aborts by t=261 s** and 5 automatic swipe gestures — down from 8 in the
previous boot, not fixed. The crash PC is unchanged (`PC - slide = 0x1d3c3da4c`).
