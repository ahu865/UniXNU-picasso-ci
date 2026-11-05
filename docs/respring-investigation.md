# Black screen and spinner while the UI is used (boot 12)

The user reports the display going black and showing the spinner while operating the phone.
On this board that is not a display failure: it is SpringBoard being restarted. Boot 12's
capture (`boot12-ios7.log`, 20:06 onwards) shows 21 `PICASSO_INJECT SpringBoard started`
lines, and every restart after the first is preceded by an app launch inside the same
second or two.

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
restarts cluster exactly where the user opened something. `board.toml` carried
`picasso_inject_apps=1`, so the kernel added that library to **every** app main executable.
The library is ad-hoc signed; AMFI refuses the signature for an app process, dyld then fails
the image, and the launch dies. SpringBoard, which is in the middle of the scene transaction,
is the process that ends up restarting.

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

1. `board.toml`: `picasso_inject_apps=1` is gone. `picasso_inject_sb` still applies the
   status bar metric to SpringBoard; app status bars fall back to UIKit's own Pad metrics
   until the library can be signed in a way AMFI accepts for an app.
2. `drivers/NativeBoardButtons/ButtonsProbe.cpp`: volume-up is debounced on its own level bit
   and only counted after the line has rested released for about 300 ms. Boot 12 counted
   10 204 presses against 52 power presses and 14 volume-down presses in 22 minutes, because
   one GPIO block sat low for seconds at a time (`0x81 -> 0x80` while the key was untouched).

## What the next boot should show

* no `PICASSO_INJECT` line for Spotlight, Preferences, PosterBoard, MobileSlideShow,
  ScreenshotServicesService or StoreKitUIService;
* `PICASSO_BTN alive ... presses=<power>/<volume-down>/<volume-up>` with the third number of
  the same order as the other two, not thousands;
* fewer, or no, SpringBoard restarts while apps are opened. If restarts continue without an
  app launch in front of them, the injection was not the only trigger and the faulting frame
  has to be resolved against the on-device cache.
