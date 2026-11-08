# Wi-Fi and Bluetooth on Picasso, and what the HackiOS tree can give us

This note compares the wireless drivers of the upstream HackiOS kernel
(`hakios-xnu-kernel-main`, the POCO X5 Pro / redwood port) with what this phone actually
has, and says what can be carried over. It is a survey, not a port plan with dates.

## What the phone has

Read out of `uefi/mu-silicium/Resources/DTBs/picasso.dtb`, not from memory:

| node | says |
| --- | --- |
| `/soc/qcom,icnss@18800000` | `reg = <0x18800000 0x800000>`; `qcom,wlan-msa-fixed-region`; consumer of `vdd-cx-mx`, `vdd-1.8-xo`, `vdd-1.3-rfa`, `vdd-3.3-ch0`, `vdd-3.3-ch1` |
| `/reserved-memory/pil_wlan_fw_region@8ba00000`, `modem_wlan_region@8c000000` | the two carve-outs the radio firmware runs in |
| `/soc/qcom,smp2p-mpss/qcom,smp2p-wlan-1-in` | the WLAN side of the modem's SMP2P pair |
| `/soc/slim@3ac0000/wcn3990` | the Bluetooth half, a child of the **SLIMbus** controller |
| `/soc/bt_wcn3990` | `compatible = "qca,wcn3990"`, `qca,bt-sw-ctrl-gpio = <&tlmm 73 0>`, supplies io 1.8 V, core 1.304 V, pa 3.0–3.312 V, xtal 1.8 V |

So the radio is a **WCN3990**: Wi-Fi through `icnss` on SNOC, Bluetooth through SLIMbus with
one antenna-switch pin on TLMM 73. There is no WPSS node and no `qcom,wcn6750`.

## What the upstream tree has

| file | board | mechanism |
| --- | --- | --- |
| `drivers/wifi/hakios.wifi.c` (193 KB) | redwood, WCN6750 | boots the **WPSS** Hexagon DSP: loads `wpss.mdt`/`wpss.b02..b07` into its carve-out, asks TrustZone to authenticate it, then SMEM/SMP2P/IPCC/GLINK and QRTR; PM7325 S7/S1/S8 and PM8350C S2/S10 for the rails, AOP mailbox for the power vote |
| `drivers/wifi/hakios.wlfw.c` | same | the QMI service the Wi-Fi firmware answers |
| `drivers/wifi/hakios.lx.c`, `hakios.sta.c`, `hakios.apple80211.c`, `hakios.wlan.cpp` | same | Linux-driver layer, station state machine, the surface the Darwin Wi-Fi service expects, IOKit object |
| `drivers/bluetooth/hakios.bt.c` | same | step one only: reads reset pin 85, switch pin 86, Wi-Fi reset 84 and the supplies, writes nothing. Its own head says steps 2–5 (rails, HSUART at 115200, rampatch/NVM over the UART, H4) are unwritten |

## Why it does not drop in

* **Different bring-up path.** The WCN6750 is a DSP that has to be started by the AP; the
  WCN3990 has no WPSS. Its host side is `icnss`: a QMI WLFW conversation with the modem's
  WLAN PD (which is what `wlanmdsp.mbn` is for), and only then the BMI download of
  `athwlan.bin` over SNOC. `hakios.wifi.c`'s SMEM/GLINK/QRTR/WPSS half has nothing to talk to
  on this phone, and every address in it (PMIC bases, mailbox, carve-outs) belongs to redwood.
* **Different Bluetooth bus.** Redwood's BT is a UART (`hsuart0`) with rampatch/NVM files;
  Picasso's is a SLIMbus device under `/soc/slim@3ac0000`. We have no SLIMbus controller
  driver, no `btfm` codec driver and no HCI transport at all, so even upstream's read-only
  step would be reading different registers.
* **Different firmware.** Redwood needs `wpss.*`; this phone needs `athwlan.bin` (WCN3990 MAC
  firmware) plus `bdwlan.*`, and, for the modem side, `wlanmdsp.mbn`.

What is worth reusing is the **QMI WLFW layer** (`hakios.wlfw.c`): the service discovery,
TLV shape and indication handling are the same protocol family that `icnss` speaks. The
transport under it (ours: the modem's WLAN PD over SMD/GLINK; theirs: QRTR to the WPSS) has
to be written for this phone, not copied.

## Wi-Fi: still not usable, and why

Boot 12's five WLAN stages all complete (`SMR idr=4c017e51`, `msa=0x8b500000`,
`transport=snoc`), and stage 3 then reports, truthfully, that it has no `wlanmdsp`/`athwlan`
image to download. Nothing has changed since:

* the V13 package's vendor partition holds `qca_cld3_wlan.ko`, whose firmware-name table
  names `athwlan.bin`, `athsetup.bin`, `otp.bin`, `firmware`, `epping.bin`, `utf.bin`,
  `fakeboar.bin` — a scan of the whole 7.8 GB `super.img` finds `athwlan.bin` **once**, at
  offset 7 016 827 768, inside that module's string table: the file itself is not in the image;
* `NON-HLOS.bin` (the modem partition) contains only the modem's own path strings
  (`/readonly/vendor/firmware_mnt/image/wlanmdsp.mbn`) at offset 104 153 062, not the image;
  `cust.img`, `cache.img` and the shipped `userdata.img` contain neither name.

So the first half of the Wi-Fi bring-up is blocked on a file this phone's factory images do
not ship, and the second half (BMI + WMI client, the `lx`/`ath` equivalent) does not exist in
our tree yet. Wi-Fi cannot be made to associate by porting from the upstream tree alone.

## Bluetooth: what a first step would look like

Upstream's own step one is "read three pins and print them". The same step is possible here
and costs nothing: read TLMM pin 73's configuration and level, and read the four
`bt_wcn3990` supplies through the PMIC arbiter the buttons and haptic drivers already use.
Nothing on SLIMbus should be touched until its clock is known to be on: the WLAN probe showed
what an MMIO read to an unclocked block does to this bus (it hangs the thread, not the read).

After that, in order: SLIMbus controller (`/soc/slim@3ac0000`) → `btfm` codec/children →
HCI over SLIMbus → the rampatch/NVM download that `hci_qca` does. None of it exists today.

## Suggested order for the other subsystems

Low risk, high value, and the pieces already exist on this phone: battery/fuel gauge,
thermal, haptics, torch, and audio. The upstream `power/`, `haptic/`, `camera/` and `audio/`
drivers are written for redwood's PMICs and codecs, so they are templates for structure, not
drop-ins; each needs this phone's register map, the way `NativeBoardButtons` needed the PON
and GPIO blocks read out of `picasso.dtb`.

## Boot 13 update (2026-11-06): firmware located, first staged step done

The firmware is not missing after all. The vangogh modem partition image in the
local ROM directory (`NON-HLOS.bin`, FAT16) carries it:

* `image/wlanmdsp.mbn` — 3 871 128 bytes, sha256 38e67bd7…, the WCN3990 WLAN
  firmware image the ICNSS/WLFW flow downloads into the MSA region;
* `image/bdwlan.bXX` — board data per board id, plus `image/regdb.bin`.

No public blob repo mirrors `firmware_mnt/image`, so the two files are pinned
as release assets of the CI repository (tag `picasso-wlan-fw-vangogh-1`) and
the workflow stages + verifies them like the Adreno set. Stage 3 of
`NativeWlan` now parses the real image against the ELF/MBN parser.

What stage 3 is and is not: it validates the container and publishes the
WLFW/QMI handshake plan; the load step still reports ENOTSUP (no SCM call, no
QMI/GLINK client) and stage 5 stops at the honest blocker — there is no
IO80211 stack. The port order from here, following the upstream redwood
method (drivers/wifi/hakios.wifi.c, WCN6750, steps 1–16: survey SMEM/SMP2P/
IPCC/RPMh rails, load the signed image, bring up GLINK/QRTR, talk WLFW):

1. MSA/WLFW download of wlanmdsp.mbn (our probe already computed the MSA plan
   and the transport is SNOC — closer to ath11k's WLFW than to ath10k).
2. GLINK/QRTR + WLFW service conversation (port from hakios.wifi.c/wlfw).
3. BMI/HTT/WMI against the running firmware.
4. An IO80211 replacement — the part upstream has not finished either; even
   redwood's driver reaches "firmware in mission mode, radio registers
   readable", not an associated connection.

Bluetooth: `BTFM.bin` carries `crbtfw32.tlv` + `crnv32.bin` (the WCN3990 UART
rampatch and NVM pair), extracted alongside the WLAN image when needed.

Audio: no codec driver exists in this tree at all — the WCD9385 chain
(SoundWire/SLIMbus + va/rx/tx macros) has not been started; volume key events
reach the HID layer, but sound output has nothing to reach yet.

## The secure-monitor call, and what it answered about the id

Stage 3's ENOTSUP was a gap in this tree, not in the firmware: PIL's four calls
(`svc 0x02`, `cmd 1 init_image / 2 mem_setup / 5 auth_and_reset / 6 shutdown`)
are now in `drivers/NativeWlan/WlanScm.cpp`, in the interruptible convention
`qcom_scm-smc.c` uses, with the image's header in one physically contiguous
page (`IOMallocContiguous`), because TrustZone reads it by physical address.

The open question was which peripheral id owns the WLAN. This board's device
tree answers it by omission: `qcom,pas-id` exists for `mss=4`, `lpass=1`,
`turing=18`, `venus=9`, `ipa_fw=15`, `npu=23` and `kgsl-hyp=13`, and the
`qcom,icnss@18800000` node carries `qcom,wlan-msa-fixed-region` (phandle to
`wlan_fw_region@8b500000`, 2 MB) instead — there is no WLAN PAS node at all.
On this SoC the WLAN is a child of the modem subsystem, which is also why the
`modem_wlan_region@8c000000` (248 MB) and `qmi-tmd-devices/modem/modem_wlan`
nodes exist.

`init_image` is the one call that can be made while that is unknown: it uploads
the image's header and starts nothing. So `sheng-wlan-pas=1` walks the ids
2..23 (skipping 1, 4 and 13, which the tree names as other subsystems) and
reports, per id, what TrustZone answered. A wrong id returns an error and the
phone is untouched; the boot report gains `native_wlan_pas_answers[24]`.
`sheng-wlan-pas=2` would additionally hand the accepted id
`WLAN_MSA_BASE/WLAN_MSA_SIZE` and let it authenticate — deliberately not on in
the test build, because the MSA region is 2 MB and the image's payload is
3.25 MB, which is the first sign that this firmware may not be loaded through
PAS at all but through the CE/BMI path ath10k uses for WCN3990.

What each of the two answers means for the next build:

- an id accepts the image: continue with `mem_setup` + `auth_and_reset`, then
  GLINK/QRTR against whatever came up;
- no id accepts it, or the id that does is the modem's: the WLAN firmware is
  uploaded by `ath10k`'s BMI over a copy engine, and the next work is the CE
  ring setup (the transport stage already says the block is integrated SNOC/CE)
  rather than the PIL calls.

## What the probe answered on the phone (boot 16)

The calls reach TrustZone, and TrustZone answers with its own codes — that part
is settled: `smc 0x42000201`, argument descriptor `0x82` (one value, one buffer),
`x2` the id, `x3` the header's physical address. Per id, from the boot log:

```
id 2..17   init_image -> -3145797  (0x300045)
id 18..22  init_image -> -3145738  (0x30000A)
id 1, 4, 13 skipped (lpass, mss, kgsl-hyp: named by the tree as other subsystems)
probe done, tried=20 accepted=0 id=0
```

Nothing accepted the image. Two readings, and the second is the one the rest of
the tree supports:

1. the metadata was too small — `init_image` was given the first 4 KiB only;
2. this firmware is not loaded through PAS at all.

The image itself says (2): parsing `wlanmdsp.mbn` as the ELF32 it actually is
(`\x7fELF`, `e_phnum = 7`, `e_phoff = 0x34`) shows the hash segment as
`ph0 PT_NULL off=0 filesz=276`, then three `PT_LOAD`s at `0xb0000000`,
`0xb0300000`, `0xb0781000`, a `PT_DYNAMIC` nested in the second load and a
46-byte `PT_NOTE` nested in the third — exactly the nesting that made the
stage-3 overlap check fail before `00efc51`. So the image *is* an authenticatable
PIL image and it *does* carry a hash segment, yet every id refuses it, while the
ids that do not exist answer with a different code. A valid image offered to the
wrong subsystem fails; a valid image offered to *no* subsystem fails the same way
for all of them. Combined with the device tree having no WLAN PAS node, the
`qcom,icnss` node carrying a fixed MSA region instead, and the 248 MB
`modem_wlan_region` next to `qmi-tmd-devices/modem/modem_wlan`, the conclusion is
that on this SoC the WLAN image is uploaded and authenticated by the modem side,
not by a PAS id of its own.

What that leaves as the real WLAN path (ath10k's, for exactly this part):

1. `ath10k_bmi_start()` — the WCSS's block-memory interface, reached through a
   copy engine, not through PAS;
2. `ath10k_bmi_fast_download()` of the 3.87 MB image into `wlan_fw_region`;
3. QMI WLFW (the `requires QMI` layer in the reference port) to bring the
   radio up, then HTT/WMI;
4. an IO80211 replacement for Darwin's `wifid` (the reference port's
   `hakios.wlan.cpp` + `apple80211.c` show the shape; that part is chip-neutral).

The CE/BMI step needs the copy-engine register layout (12 engines, per
`CE_COUNT_MAX`, with the pipe map in the WCSS window), which this driver does not
have yet — that is the next piece of work, and it is independent of the PAS calls
that now exist.
