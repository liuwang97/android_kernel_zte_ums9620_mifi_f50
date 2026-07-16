# Custom Kernel for ZTE U30 Air (Unisoc T760 / UMS9620)

> A daily-driver kernel for the **ZTE U30 Air** 5G MiFi hotspot (`mifi_f50`, Unisoc T760 / UMS9620),
> built from ZTE's GPL source — with modern memory management, in-kernel root, a faster network
> stack, and two device-specific bug fixes the stock kernel never got.

<p>
<img alt="device" src="https://img.shields.io/badge/device-ZTE%20U30%20Air-0a7bbb">
<img alt="soc" src="https://img.shields.io/badge/SoC-Unisoc%20T760%20/%20UMS9620-5b2c86">
<img alt="kernel" src="https://img.shields.io/badge/kernel-5.4.254--android12--9-orange">
<img alt="root" src="https://img.shields.io/badge/root-KernelSU%20(built--in)-2ea44f">
<img alt="license" src="https://img.shields.io/badge/license-GPL--2.0-blue">
</p>

**English** · [简体中文](#简体中文)

---

## What is this?

The ZTE U30 Air is a pocket 5G router. Its stock kernel is a locked-down Android 12 GKI build:
`MODULE_SIG_FORCE` on, 102 signed vendor modules (charger, MIPI display, USB-RNDIS, SAR, IMS,
ethernet…), and no root. This project rebuilds that kernel **from ZTE's own GPL source** and turns
it into something you can actually tinker with — **without replacing a single vendor driver** and
without touching `vendor_boot`.

The headline is not any one feature. It's that all of this ships in **one `boot.img`** that boots
cleanly on **100% unmodified stock vendor modules**.

## ⭐ Highlights

- **Boots on stock vendor drivers, untouched.** The kernel's ABI fingerprint
  (`module_layout = 0x2f279e7b`) and all 1447 exported symbol CRCs are held **bit-identical** to the
  factory kernel. Every one of the 102 signed vendor `.ko` loads with `MODVERSIONS` intact —
  charging, display, 5G modem, SAR, IMS all work on day one. Verified against the real device:
  **1447/1447 symbols match, 0 mismatch.**
- **`module_overlay` — swap vendor modules at load time, from inside the kernel.** A small in-kernel
  hook intercepts module loading and transparently substitutes our own rebuilt copies of specific
  drivers (the USB stack) for the vendor ones — *by internal module name, at `finit_module` time* —
  so we can fix vendor-driver bugs **while leaving the stock, signed `vendor_boot` partition
  completely untouched**. Clean rollback, nothing to un-patch.
- **Two deep, device-specific bug fixes** the stock kernel never received — a USB-tether hang and a
  5 GHz Wi-Fi regression — both root-caused from scratch on-device. See [Signature fixes](#signature-fixes).
- **In-kernel root via KernelSU** (`CONFIG_KSU=y`, manual-hook) — pair with the KernelSU-Next
  manager app; no ramdisk Magisk patching needed.
- **A memory & network stack tuned for a always-on router**, not a 2020 phone: MGLRU, DAMON,
  upgraded zstd + zram recompression, BBR + CAKE.

## Feature matrix

| Area | What's enabled | Why it matters on a MiFi router |
|---|---|---|
| **Memory** | **MGLRU** (`LRU_GEN`) multi-gen LRU | Smoother reclaim under the constant browser/tether load; far less jank than the stock 2-gen LRU |
| **Memory** | **DAMON** + `DAMON_RECLAIM` (proactive reclaim) | Reclaims cold pages *before* pressure hits — keeps a 4–6 GB device responsive for days of uptime |
| **Swap** | **zram** with `ZRAM_MULTI_COMP` + **modern zstd** | Higher compression ratio and background recompression → more effective RAM without a bigger chip |
| **Network** | **BBR** congestion control + **CAKE** / `fq_codel` qdisc | The whole point of the device is sharing a link — BBR+CAKE cut bufferbloat and keep latency low under load |
| **IRQ** | **sbalance** IRQ balancer | Spreads the PCIe-Wi-Fi / SIPA IRQ storm across cores instead of pinning one |
| **Root** | **KernelSU** built-in (`KSU_MANUAL_HOOK`) | Root without a modified ramdisk; survives the stock `init_boot` |
| **Build** | **Full LTO + CFI**, exact factory vermagic | Required to stay ABI-compatible with the signed vendor modules (see below) |

## Signature fixes

These two are the reason this kernel exists as a *daily driver* and not just a config tweak. Both
were diagnosed live on the device, from symptom to root cause to patch.

### 1. USB-tether periodic disconnect (RNDIS control-channel wedge)

**Symptom:** tether over USB works from a cold boot, but the moment you unplug and replug once, the
link starts dropping on a ~100 s cycle (Windows event 10400), and only a full reboot clears it.

**Root cause:** the SPRD USB glue tore the gadget down fire-and-forget (a blind `msleep(20)` that
raced composite teardown and discarded pending disconnect events), stranding the PAM/PAMU3 hardware
offload half-released. Its refcount lives in an always-on power domain, so the skew survived every
reconnect — every replug took a "dirty restart" path instead of the clean bring-up the cold boot
gets. The RNDIS **control** channel (OID/keepalive) wedged while the data plane, running on
autonomous PAM event buffers, stayed alive — hence "connected but no heartbeat."

**Fix:** drive the PAM block to a fully-closed state at **both** ends of every USB session (via the
standard `usb_phy_shutdown` hook), and make glue teardown *wait* for the disconnect IRQ to finish
instead of napping. A replug now starts from the exact same virgin state as a cold boot.
→ `drivers/usb/dwc3/dwc3-sprd.c`, `drivers/usb/misc/sprd-pamu3.c` (shipped via `module_overlay`).

### 2. 5 GHz Wi-Fi hotspot stuck on 802.11n (stale regulatory certificate)

**Symptom:** enable 5 GHz / 802.11ac in settings, but the hotspot always comes up on 2.4 GHz
802.11n. The stock kernel is fine.

**Root cause:** the factory `vendor` partition ships a **2024** `wireless-regdb` (`regulatory.db`),
signed with the rotated 2024 *sforshee* key. The public GPL source snapshot still embedded the
**2017** key, so signature verification failed, the regdb was permanently poisoned early in boot,
every country hint was silently dropped, and the regulatory domain stayed pinned to world `00` — under
which all 5 GHz channels are `NO-IR`, so the AP's channel selection could only ever pick 2.4 GHz.
The chain was proven end-to-end on-device with a custom `nl80211` probe.

**Fix:** embed the 2024 signing certificate (extracted from the device's own `regulatory.db.p7s`,
byte-identical to the one inside the stock kernel) alongside the old one — dual-key trust, pure
built-in data, zero ABI impact. → `net/wireless/certs/sforshee-2024.hex`.

## Building

> **⚠️ The build rules below are not optional.** Break any of them and the kernel either won't boot
> or will reject all 102 vendor modules. This is the price of running stock signed drivers.

**Toolchain:** AOSP Clang **r416183b** (clang 12.0.5) — the exact compiler the factory defconfig
pins. Do **not** substitute a distro clang. `LLVM=1 LLVM_IAS=1`, `aarch64-linux-gnu-` cross GCC.

**The three iron rules:**

1. **Exact vermagic.** `SUBLEVEL = 254` in the Makefile, `LOCALVERSION="-android12-9-g7463324543ed"`,
   `LOCALVERSION_AUTO=n`. The final string must read *exactly*
   `5.4.254-android12-9-g7463324543ed`.
2. **Let the vendor modules in.** `MODULE_SIG_FORCE=n`, but keep `MODVERSIONS=y` — the CRCs are
   ABI-compatible and must stay enforced.
3. **Full LTO + CFI, locked.** `LTO_CLANG=y` (ThinLTO **off**), `CFI_CLANG=y`. These affect how
   `module_layout` and every symbol CRC are computed; changing them flips `0x2f279e7b` and rejects
   every vendor module. Do not toggle `LTO_CLANG` (it silently disables CFI).

Use the **device's real running config** (`/proc/config.gz`) as the base — the in-repo defconfig is
for a *different* hardware variant. Then, roughly:

```sh
export PATH=/path/to/clang-r416183b/bin:$PATH
export CROSS_COMPILE=aarch64-linux-gnu-
make O=out LLVM=1 LLVM_IAS=1 ARCH=arm64 <your.config>
# build the overlay's vendor-replacement modules first, then:
make O=out LLVM=1 LLVM_IAS=1 ARCH=arm64 -j"$(nproc)" Image
# verify: strings out/vmlinux | grep -m1 "Linux version 5.4"
#         nm out/vmlinux | grep '__crc_module_layout'   # must be 0x2f279e7b
```

Repack the resulting `out/arch/arm64/boot/Image` into the **stock** `boot.img` (keeping the stock
ramdisk) with `magiskboot unpack` / `repack`.

## Flashing & safety

- Flash the repacked `boot.img` to `boot_a`/`boot_b` (SoC download mode; the device uses Unisoc
  `spd_dump`-class tooling). Because a self-built kernel is unsigned, keep whatever secure-boot
  bypass your flashing flow already uses.
- **Always keep a backup of your stock `boot` partition.** It is your only clean rollback.
- Root: after flashing, install the **KernelSU-Next** manager APK to actually gain root.
- **Do not let the device auto-update its system.** These images ship the stock ramdisk with no
  OTA-block; a firmware bump can break `adb`/tooling access.

*This is an enthusiast project. There is no warranty; flashing carries a real risk of a brick. You
are responsible for your own device.*

## Credits & license

- **ZTE** — original GPL kernel source release for the UMS9620 platform.
- Base snapshot: [`Enceka/android_kernel_zte_ums9620_mifi_f50`](https://github.com/Enceka/android_kernel_zte_ums9620_mifi_f50).
- [KernelSU-Next](https://github.com/KernelSU-Next/KernelSU-Next) — in-kernel root (GPL-3.0).
- Upstream authors of MGLRU, DAMON, zstd, CAKE, and `wireless-regdb`.

Licensed under **GPL-2.0**, same as the Linux kernel. All in-tree third-party components keep their
original licenses.

---
---

# 简体中文

> 面向 **中兴 U30 Air** 5G 随身路由(`mifi_f50`,紫光展锐 T760 / UMS9620)的日用内核。
> 从中兴 GPL 源码自编译,带来现代内存管理、内核内置 root、更快的网络栈,以及两个原厂内核从未修过的
> 设备专属 bug 修复。

[English](#custom-kernel-for-zte-u30-air-unisoc-t760--ums9620) · **简体中文**

## 这是什么?

U30 Air 是一台口袋 5G 路由。它的原厂内核是锁死的 Android 12 GKI 构建:强制模块签名
(`MODULE_SIG_FORCE`)、从 vendor 分区加载 102 个签名内核模块(充电、MIPI 显示、USB-RNDIS、SAR、
IMS、以太网……)、无 root。本项目**从中兴自己的 GPL 源码**重编这个内核,把它变成可以折腾的东西——
而且**不替换任何一个 vendor 驱动**、**不动 `vendor_boot` 分区**。

真正的亮点不是某一个功能,而是这一切装进**同一个 `boot.img`**,却能在 **100% 未改动的原厂 vendor
模块**上干净启动。

## ⭐ 亮点

- **在原厂驱动上原样启动。** 内核的 ABI 指纹(`module_layout = 0x2f279e7b`)与全部 1447 个导出符号
  CRC 与原厂内核**逐位一致**。102 个签名 vendor `.ko` 在 `MODVERSIONS` 开启下全部正常加载——充电、
  显示、5G 基带、SAR、IMS 开箱即用。已用真机校验:**1447/1447 符号匹配,0 不符**。
- **`module_overlay`——在内核内部、于加载时替换 vendor 模块。** 一个内核内置钩子拦截模块加载,在
  `finit_module` 时**按模块内部名**把指定驱动(USB 栈)透明替换成我们重编的版本——于是可以修 vendor
  驱动的 bug,却让**原厂签名的 `vendor_boot` 分区完全保持原样**。回滚干净,无需反向打补丁。
- **两个深度的设备专属 bug 修复**,原厂内核从未收到——USB 网络共享挂死、5 GHz Wi-Fi 回退,均在真机上
  从零定位根因。见 [招牌修复](#招牌修复)。
- **内核内置 KernelSU root**(`CONFIG_KSU=y`,manual-hook)——配 KernelSU-Next 管理器 APK,无需
  改 ramdisk 打 Magisk。
- **面向常开路由调优的内存与网络栈**,而非 2020 年的手机:MGLRU、DAMON、升级版 zstd + zram 重压缩、
  BBR + CAKE。

## 功能一览

| 领域 | 启用了什么 | 对随身路由的意义 |
|---|---|---|
| **内存** | **MGLRU**(`LRU_GEN`)多代 LRU | 在持续的浏览/共享负载下回收更平滑,比原厂两代 LRU 卡顿少得多 |
| **内存** | **DAMON** + `DAMON_RECLAIM`(主动回收) | 在压力到来*之前*就回收冷页——让 4–6 GB 的设备连续跑数天仍跟手 |
| **交换** | **zram** 开 `ZRAM_MULTI_COMP` + **新版 zstd** | 更高压缩比 + 后台重压缩 → 不换内存芯片也能有更大有效内存 |
| **网络** | **BBR** 拥塞控制 + **CAKE** / `fq_codel` 队列 | 设备本职就是共享网络——BBR+CAKE 压制缓冲膨胀,高负载下延迟依然低 |
| **中断** | **sbalance** 中断均衡 | 把 PCIe-Wi-Fi / SIPA 的中断风暴摊到多核,而非钉死一核 |
| **Root** | **KernelSU** 内建(`KSU_MANUAL_HOOK`) | 无需改 ramdisk 的 root;兼容原厂 `init_boot` |
| **构建** | **Full LTO + CFI**,精确原厂 vermagic | 与签名 vendor 模块保持 ABI 兼容的前提(见下) |

## 招牌修复

这两个才是本内核能作为*日用*而不只是改配置的原因。均在真机上从症状到根因到补丁全程定位。

### 1. USB 网络共享周期性掉线(RNDIS 控制通道卡死)

**症状:** 冷启动后 USB 共享正常,但只要拔插一次,链路就开始约 100 秒一周期地掉(Windows 事件
10400),且只有彻底重启才能清除。

**根因:** 展锐 USB glue 的拆解是"发射后不管"(一个盲目的 `msleep(20)`,与 composite 拆解竞态并丢弃
未处理的断开事件),使 PAM/PAMU3 硬件卸载停在"半释放"状态。它的引用计数活在常开电源域里,所以这个错位
能扛过每一次重连——每次拔插都走"脏重启"路径,而非冷启动才有的干净初始化。RNDIS 的**控制**通道
(OID/心跳)卡死,而跑在 PAM 自主事件缓冲上的数据面还活着——于是表现为"连上了但没有心跳"。

**修复:** 在每个 USB 会话的**两端**都用标准 `usb_phy_shutdown` 钩子把 PAM 模块推到全关状态,并让 glue
拆解*等待*断开中断处理完毕而非盲睡。拔插从此与冷启动进入完全相同的初始状态。
→ `drivers/usb/dwc3/dwc3-sprd.c`、`drivers/usb/misc/sprd-pamu3.c`(经 `module_overlay` 下发)。

### 2. 5 GHz Wi-Fi 热点卡在 802.11n(管制证书过期)

**症状:** 设置里开 5 GHz / 802.11ac,热点却总是落在 2.4 GHz 802.11n。原厂内核无此问题。

**根因:** 原厂 `vendor` 分区带的是 **2024** 版 `wireless-regdb`(`regulatory.db`),用 2024 轮换后的
*sforshee* 新密钥签名。而公开 GPL 源码快照里内嵌的还是 **2017** 旧密钥,于是验签失败、regdb 在开机早期
被永久毒化、之后所有国家码 hint 被静默丢弃、管制域一直钉在世界域 `00`——该域下 5 GHz 全部信道为
`NO-IR`,AP 选信道只能落 2.4 GHz。整条链路用自写的 `nl80211` 探针在真机上完整验证。

**修复:** 把 2024 版签名证书(从设备自己的 `regulatory.db.p7s` 提取,与原厂内核内嵌的逐字节一致)与
旧证书并存——双钥信任,纯内置数据,零 ABI 影响。→ `net/wireless/certs/sforshee-2024.hex`。

## 编译

> **⚠️ 下面的构建规则不是可选项。** 破坏任何一条,内核要么起不来,要么拒绝全部 102 个 vendor 模块。
> 这是运行原厂签名驱动的代价。

**工具链:** AOSP Clang **r416183b**(clang 12.0.5)——原厂 defconfig 写死的那个编译器。**不要**换发行版
自带 clang。`LLVM=1 LLVM_IAS=1`,交叉 GCC `aarch64-linux-gnu-`。

**三条铁律:**

1. **精确 vermagic。** Makefile 里 `SUBLEVEL = 254`、`LOCALVERSION="-android12-9-g7463324543ed"`、
   `LOCALVERSION_AUTO=n`。最终版本串必须*精确*等于 `5.4.254-android12-9-g7463324543ed`。
2. **放行 vendor 模块。** `MODULE_SIG_FORCE=n`,但保留 `MODVERSIONS=y`——CRC 是 ABI 兼容的,必须继续
   强制。
3. **Full LTO + CFI,锁死。** `LTO_CLANG=y`(ThinLTO **关**)、`CFI_CLANG=y`。它们影响
   `module_layout` 与每个符号 CRC 的计算方式;一改就会把 `0x2f279e7b` 翻掉、拒绝所有 vendor 模块。
   不要 toggle `LTO_CLANG`(会连带静默关掉 CFI)。

以**设备真实运行配置**(`/proc/config.gz`)为基础——仓库里的 defconfig 是*另一个*硬件变体的。然后大致:

```sh
export PATH=/path/to/clang-r416183b/bin:$PATH
export CROSS_COMPILE=aarch64-linux-gnu-
make O=out LLVM=1 LLVM_IAS=1 ARCH=arm64 <你的.config>
# 先编 overlay 的 vendor 替换模块,然后:
make O=out LLVM=1 LLVM_IAS=1 ARCH=arm64 -j"$(nproc)" Image
# 校验:strings out/vmlinux | grep -m1 "Linux version 5.4"
#       nm out/vmlinux | grep '__crc_module_layout'   # 必须是 0x2f279e7b
```

把产出的 `out/arch/arm64/boot/Image` 用 `magiskboot unpack` / `repack` 塞回**原厂** `boot.img`
(保留原厂 ramdisk)。

## 刷机与安全

- 把重打包的 `boot.img` 刷到 `boot_a`/`boot_b`(SoC 下载模式,设备用 Unisoc `spd_dump` 一类工具)。
  自编内核未签名,请保留你刷机流程里已有的 secure-boot 绕过。
- **务必备份原厂 `boot` 分区。** 那是你唯一干净的回滚。
- Root:刷完后安装 **KernelSU-Next** 管理器 APK 才能真正拿到 root。
- **切勿让设备自动升级系统。** 这些镜像用的是原厂 ramdisk、无禁 OTA;固件升级可能破坏 `adb`/工具通道。

*这是发烧友项目,无任何担保;刷机有真实变砖风险,请自负其责。*

## 致谢与许可

- **中兴(ZTE)**——UMS9620 平台原始 GPL 内核源码发布。
- 基础快照:[`Enceka/android_kernel_zte_ums9620_mifi_f50`](https://github.com/Enceka/android_kernel_zte_ums9620_mifi_f50)。
- [KernelSU-Next](https://github.com/KernelSU-Next/KernelSU-Next)——内核内置 root(GPL-3.0)。
- MGLRU、DAMON、zstd、CAKE、`wireless-regdb` 的上游作者。

以 **GPL-2.0** 许可,与 Linux 内核一致。所有树内第三方组件保留其原始许可。
