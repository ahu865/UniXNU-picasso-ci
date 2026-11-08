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

## 2. 缺的是固件本身，不是代码

`adsp.mdt` 与 `adsp.b00..b2x` 不在任何我们已有的文件里：

- `NON-HLOS.bin`（FAT16，整份扫过）：只有 `image/wlanmdsp.mbn` 与 `image/regdb.bin`；
- 固件包 `vangogh_images_V13.0.7.0.../images/`：只有 `BTFM.bin` 与 `dspso.bin`（后者是 DSP 的共享库，不是镜像）；
- ADSP 镜像在**独立分区**（`adsp_a`），fastboot 刷机包不带它。

所以「声音」的第一步不是写驱动，是**把 adsp 分区读出来**。三条路，按可行性排序：

1. **在 XNU 里用已有的 UFS 驱动直接读分区**（推荐）。`drivers/NativeUfs` 已经能读 UFS，
   `native-ufs-root=1` 正在工作，GPT 分区表可读；把 `adsp_a` 的前 0x2800000 字节读进
   `pil_adsp_region`，再走 PAS，一步到位，且不占 payload（20 MB 塞不进 68 MB 的 payload）。
2. 进 Android 后 `dd if=/dev/block/bootdevice/by-name/adsp_a of=/sdcard/adsp.img`，再把镜像切碎嵌入。
3. 找整包 ROM（不是 fastboot 包）里的 `adsp.img`。

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
2. **读出 adsp 分区**：给 `NativeUfs` 加一个「按分区名读进指定物理区」的入口，先只读 512 字节验证
   GPT 名字匹配，再读全量 40 MB。日志报 `PICASSO_ADSP read=… crc=…`。
3. **PAS 载入 ADSP**：`init_image(1, adsp.mdt header)` → `mem_setup(1, 0x88d00000, 0x2800000)` →
   把各段拷进去 → `auth_and_reset(1)`。成功后 ADSP 自己的 SMP2P item 会变，这一步有硬证据可看。
4. **GLINK/QRTR**：建 `IPCRTR` 通道，向 ADSP 问它提供哪些服务（`QMI_SERVICE_AUDIO` 等）。这一步只读。
5. **出声**：AFE 端口 + WCD9385 上电序列（`wcd938x_reset` 引脚已经给出）→ 通过 ADSP 的 audio 服务
   建立 playback port → 把 PCM 喂进去。这一步才需要写「播放」代码。

第 2、3 步之间没有代码依赖之外的阻塞；第 5 步是整个项目里最长的部分。

## 5. 为什么先做 WLAN 的 PAS，而不是先做音频

两者共用同一套 SMC/PIL 代码，但**验证成本不同**：WLAN 的固件已经在 payload 里（`wlanmdsp.mbn`，
3,871,128 字节，已解析 7 段），PAS 探针可以在一次刷机里给出「哪个 id 接受这张镜像」的答案；
音频还卡在拿不到固件。所以先让 SMC 通道在真机上被测过，音频的 1、3 两步才有意义。
