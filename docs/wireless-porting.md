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
