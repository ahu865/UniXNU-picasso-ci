# 声音（WCD9385 / ADSP）— 现状、依据与移植路线

这份文档只说证据和路线，不含猜测的地址。所有地址都来自本机自己的设备树
（`build/vangogh/vangogh-full.dtb`，即 boot 镜像里真正加载的那份）。

## 1. 这台机器的音频硬件在设备树里是完整的

| 节点 | 含义 | 关键属性 |
| --- | --- | --- |
| `soc/qcom,lpass@3000000` | 音频 DSP（LPASS 里的 Hexagon） | `compatible="qcom,pil-tz-generic"`，`qcom,pas-id = 1` |
| `reserved-memory/pil_adsp_region@88d00000` | ADSP 固件落地区 | `reg = <0 0x88d00000 0 0x2800000>`（40 MB） |
| `soc/qcom,msm-audio-apr/qcom,q6core-audio/sound` | 声卡 | `compatible="qcom,kona-asoc-snd"` |
| `.../vote_lpass_core_hw`、`.../vote_lpass_audio_hw` | 时钟/供电投票 | `compatible="qcom,audio-ref-clk"` |
| `soc/pinctrl@f100000/wcd938x_reset_{active,sleep}` | 编解码器复位脚 | mux + config |
| `soc/slim@3ac0000` | SLIMbus 控制器（蓝牙侧也在上面） | `compatible="qcom,slim-ngd"`，`reg = <0x3ac0000 0x2c000>, <0x3a84000 0x2a000>` |
| `soc/slim@3ac0000/wcn3990` | 蓝牙的 SLIMbus 从设备 | `compatible="qcom,btfmslim_slave"` |

**结论一：ADSP 走 PAS 是确定的，`pas-id = 1`，落地区 0x88d00000 / 40 MB。** 这和 WLAN 不同——WLAN 在这份设备树里
**没有** `qcom,pas-id`，音频有。

## 2. 固件在哪：不在 UFS 分区，在 NON-HLOS.bin 里（2026-11-09 真机证伪）

本节原来说「ADSP 镜像在独立分区 `adsp_a`，NON-HLOS.bin 里只有 WLAN 固件」——**两条都错**。

boot-vangogh-ios14 在真机上把 GPT 分区名修好之后，34 个分区全部列出：
`switch / ssd / dbg / bk01…bk09 / keystore / frp / misc / logfs / oops / devinfo /
oem_misc1 / metadata / splash / persist / persistbak / logdump / rawdump / minidump /
mtdblk / recovery / cache / exaid / cust / super / userdata`。**没有 `adsp_a` / `adsp_b`，
也没有 `modem`**。所以 `native_ufs_adsp_probe()` 永远返回 `rc=1`（跑通、没找到镜像），
按名读分区这条路对音频没有意义；它修好的价值是让其它分区（例如 `persist` 里的 WLAN
校准数据）以后可以按名读。

真正的 ADSP 固件就在刷机包自带的
`vangogh_images_V13.0.7.0.SJVCNXM_20220928.0000.00_12.0_cn/images/NON-HLOS.bin`
（178,958,336 字节，FAT16：4096 字节/扇区、4 扇区/簇、根目录 512 项）的 `/image` 目录里：

- `adsp.mdt` 9,340 字节 —— ELF32，`e_phoff=0x34`、`e_phentsize=32`、`e_phnum=34`，
  34 个程序头的 `p_vaddr` 落在 `0x8ba00000`、`0xf0100000`、`0xb2000000`、`0xe0a60000` 等
  Hexagon DSP 地址段；
- `adsp.b00` … `adsp.b32` 共 33 个分段，与 34 个程序头一一对应；
- 合计 **21,479,410 字节（约 20.5 MiB）**，与参考仓库 `hakios.adsp.c` 描述的
  `adsp.mdt + adsp.b02..b36` 分段 MDT 格式是同一类东西（分段编号不同代）。

提取脚本：`D:\Down\hakios\work\extract-adsp-fw.py`（解析 FAT16 根目录 + LFN + 簇链，
输出到 `work/adsp-fw/`）。固件已在本地，不再需要进 Android `dd` 分区，也不需要找整包 ROM。

**剩下的工程问题**是把它送进内核：payload 目前 68 MiB，加 20.5 MiB 需要把
`board.toml` 的 `size_mib` 从 68 扩到约 92，并同步改所有依据 `spec['size_mib']` 校验的
脚本（这类扩容 60→68 已经做过一次）。128 MiB 的 boot 分区放得下（kernel 2 MB +
ramdisk 约 91 MB + dtb 1.6 MB）。之后才能走第 3 步的 PAS 载入。

## 3. 参考实现：`oosexgang/hakios-xnu-kernel` 的 `drivers/audio/`

那个仓库跑在 POCO X5 Pro（redwood / SM7325）上，音频已经是能出声的：

- `hakios.adsp.c`（124 KB）：PIL 载入 → 请 TrustZone 认证启动（它的 image 1） → SMP2P 说「起来了」→
  GLINK/QRTR 建链。**这套流程和 PAS 的 `init_image / mem_setup / auth_and_reset` 是同一件事**，
  我们在 `drivers/NativeWlan/WlanScm.cpp` 里已经把这四个调用写出来了（见
  `docs/wireless-porting.md`），音频可以直接复用同一份代码，只把 id 换成 1、区域换成 0x88d00000。
- `hakios.sound.c`（60 KB）、`hakios.speaker.c`（43 KB，I2C 上的两个功放）、`hakios.mic.c`（74 KB）：
  ADSP 的 AFE 端口、播放馈送、麦克风。**这部分是 redwood 的端口号与时钟**，只能当结构参考。

**不能照搬的部分**：LPASS 的时钟树、AFE 端口号、编解码器宏（`wcd938x` 的寄存器序列）。SM7250 与 SM7325
的 LPASS 不同代，需要按本机 `slim@3ac0000` + `lpass@3000000` 的实际寄存器重写。

## 4. 分步路线（每步都可单独验证，不依赖前一步的「成功」）

1. **PAS 通不通**（和 WLAN 共用）：`wlan_pas_probe` 已经在做这件事。音频的 id 是已知的 1，所以只要
   `sheng-wlan-pas=2` 那一步在 id 1 上返回 0，就说明 SMC 通道本身是通的。
2. **把固件送进内核**：`adsp.mdt` + `adsp.bNN` 已从 NON-HLOS.bin 提取到 `work/adsp-fw/`；
   扩 payload 到约 92 MiB 并像 `wlanmdsp.mbn` 一样嵌入（或直接作为 ramdisk 文件），
   先只校验 ELF32 程序头与分段长度和，不要一开始就投电源轨。
3. **PAS 载入 ADSP**：`init_image(1, adsp.mdt header)` → `mem_setup(1, 0x88d00000, 0x2800000)` →
   把各段拷进去 → `auth_and_reset(1)`。成功后 ADSP 自己的 SMP2P item 会变，这一步有硬证据可看。
4. **GLINK/QRTR**：建 `IPCRTR` 通道，向 ADSP 问它提供哪些服务（`QMI_SERVICE_AUDIO` 等）。这一步只读。
5. **出声**：AFE 端口 + WCD9385 上电序列（`wcd938x_reset` 引脚已经给出）→ 通过 ADSP 的 audio 服务
   建立 playback port → 把 PCM 喂进去。这一步才需要写「播放」代码。

第 2、3 步之间没有代码依赖之外的阻塞；第 5 步是整个项目里最长的部分。

## 5. 为什么先做 WLAN 的 PAS，而不是先做音频

两者共用同一套 SMC/PIL 代码，但**验证成本不同**：WLAN 的固件已经在 payload 里（`wlanmdsp.mbn`，
3,871,128 字节，已解析 7 段），PAS 探针可以在一次刷机里给出「哪个 id 接受这张镜像」的答案；
音频当时卡在拿不到固件；现在固件已从 NON-HLOS.bin 提取（见第 2 节）。但要注意：
真机上 WLAN 的 `init_image` 对 id 2..23 全拒（`tried=20 accepted=0`），说明这台机器的
TZ 并不把 WLAN 当 PAS 外设；音频的 `pas-id = 1` 是设备树明写的、另一条独立路径，
不能因为 WLAN 被拒就推断音频也通。第 1 步应当在 **id 1** 上单独测。
