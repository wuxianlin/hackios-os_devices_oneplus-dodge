# Dodge (OnePlus 13, SM8750) port notes

> **Names since the tree was restructured (October 2026):** the driver is
> `devices/oneplus-dodge/kernel/hakios.dodge_touch*` (was `dodge_synaptics*`),
> its two wiring lines are in `hakios.input.[ch]` (was `redwood_input.[ch]`),
> the kernel patch is `xnu/hakios.patch` (was `xnu/redwood.patch`), and the
> device profile is this directory (was `common/devices/dodge.conf`). The notes
> below tell the story with the names as they were at the time, where they
> differ.


状态（2026-10-24）：**iOS 16.5 用户态已在 OnePlus 13 上运行**（SpringBoard 渲染中）。
UEFI + XNUP payload + xnuloader(USB) + XNU（6 核，无 panic）+ launchd/backboardd/SpringBoard 全链路通。
**触摸驱动已写好并编译进内核**（`xnu/files/bsd/dev/hakios.dodge_touch.c`，1131 行，`synaptics=1`），
但被 **Gunyah 可信触摸架构阻断**（见下）。其余未完成：GPU 加速（Adreno 830）、Wi-Fi（ath12k）、声音。

参考 `docs/PORTING.md` 了解通用流程，这里只记录 dodge 特有的内容。

## 引导链

```
fastboot boot (boot image header v4!)          devices/oneplus-dodge/: BOOT_HEADER=4
  └► BootShim: FD(3MB)→0xA7100000, XNUP→0xF0000000
      └► Mu-Silicium UEFI (上游 dodgePkg + 我们的补丁, uefi-dodge/)
          ├► 内存映射加 XNU_Payload@0xF0000000 (0x4000000)      ← 上游没有，必须加
          ├► BeforeConsole: 中和虚拟看门狗 + 删存储引导项
          ├► AfterConsole: 画完 logo 直接 XnuLoaderMain()        ← 不等 BDS/UnableToBoot
          └► XnuLoader: 诊断日志→rawdump 分区, LoadImage loader.efi
              └► xnuloader: platform_dodge (按 fb_base=0xFC800000 识别)
                    └► XNU: GIC 步长二进制补丁（见下）→ launchd → SpringBoard
```

## 硬件事实（实测）

| 项 | 值 | 来源 |
|---|---|---|
| GICv3 | dist 0x16000000, redist 0x16080000 | 手机设备树 |
| CPU | 6×Phoenix-M MPIDR 0x000–0x500 (cap 0x700)，2×Phoenix-L 0x10000/0x10100 (cap 0x766) | 手机设备树 / sysfs cpu_capacity |
| 帧缓冲 | 1440×3168 @ 0xFC800000 (Display_Reserved) | loader 日志 |
| DDR | 0x80000000(2G) + 0x880000000(6G) + 0xA00000000(16G)，loader 在 0x8c0000000 找到 21G 空闲块 | ddr-regions / loader 日志 |
| 触摸 | Synaptics TCM（SPI）——redwood 的 Goodix 驱动不适用 | dumpsys input |
| Wi-Fi | WCN7850 → ath12k（ath11k 不适用） | SM8750 配套 |
| 分区 | 无 logdump（诊断日志写 rawdump，root 用 `su dd` 读回） | by-name 列表 |

## 已踩的坑

1. **虚拟看门狗 (hh_virt_wdt)**：XBL 出来后没人喂，~30s 复位。`VIRT_WDT_CONTROL`(0x86000005) 禁用不被支持（返回 1），但 `SET_TIME`(0x86000008, 最大 0xfff0ms) + `PET`(0x86000007) 有效。UEFI BeforeConsole 里先中和，loader 的 wdt_arm（已对 dodge 启用）接管后 5s 一喂。内核 cmdline 自动带 `hv_wdt_pet=1`。
2. **BDS 走不到 UnableToBoot**：首次引导上游 Mu-dodge.img 注册的 Internal/USB Storage 引导项持久化在变量分区，BDS 去扫存储就死（看门狗或驱动崩溃）。解法：AfterConsole 画完 logo 直接启动 XNU payload，不进 BDS 引导流程。
3. **0xF0000000 不在固件静态内存表里就是未映射的**：读它直接异常。XNU_Payload 内存映射条目 + AT 指令探测（借自 UniXNU-pong）双保险。
4. **boot image 头版本是 v4**（Android 16），mkxnuimg.py 走 `BOOT_HEADER=4`。
5. **GIC 重分配器步长 0x40000（redwood 是 0x20000）**：内核 `gic_find_redistributor()` 硬编码 0x20000 步进，探测到未实现空间 → 同步外部中止（esr 0x96000010，DFSC 0x10 是 external abort 不是翻译故障！）→ cpu1 panic。无内核工具链，改由 **loader 在加载内核时二进制补丁**该指令（匹配任意寄存器的 `add xD,xN,#0x20000,lsl#12` + `cmp xD,x10` 特征对），步长取 `gic_redist_size/cores`。
6. **MPIDR 不连续**：6×Phoenix-M 在 0x000-0x500，2×Phoenix-L 大核在 0x10000/0x10100（隔两个 cluster）——loader 的 platform 结构加了 `mpidr_map` 显式映射表。
7. **大核 CPU_ON 待查**：`bootcpu=7`(0x10100) 早期 abort（Oryon 大核 cache 几何不同：768B vs 512B 行）。用 Phoenix-M 跑（cpus=8，6 核注册）。
8. **userland/build.sh 需要 umask 022**：否则 launchd 拒绝 plist（error 122 bad ownership）。
9. **OnePlus 无 logdump 分区**：XNU_LOG_PARTITION 用 rawdump；Android 侧 `adb shell su -c 'dd ...'` 读回（有 Magisk root）。
10. **RSC v3.0 命令槽**：TCS 基址 0xD00（DT `qcom,tcs-offset`），寄存器偏移与 v2 不同（见 rpmh-rsc.c ver_3_0 表）。TCS3 是 active（DT `qcom,tcs-config` 解码）。IRQ 位按 TCS 编号（bit3 = TCS3），在共享基址（rsc+0xD00）。
11. **GCC 部分寄存器对 VM 不可见**：读 0x10183DC（SE4 时钟配置）触发 stage-2 外部中止 → 内核 panic → 65s 看门狗复位手机（日志都丢）。原因是 Gunyah 对部分 MMIO 区域做了写保护/完全屏蔽。
12. **触摸被 Gunyah 可信触摸（Trusted Touch）阻断**：设备树 `qts,trusted-touch-mode = "vm_mode"`、`qts,trusted-touch-io-bases = 0xa90000`。GENI SE 由 Gunyah 动态分配给独立的 Touch VM；`fastboot boot` 的 guest VM 对 SE 只有读权限，**写任何寄存器立即杀死整个 VM**（stage-2 权限错误 → 硬件复位）。这是当前触摸无法工作的根本原因。要突破需要（a）通过 SMC 与 Gunyah 资源管理器协商将 SE 分配给我们的 VM，或（b）使用 Touch VM 的进程间通信。驱动代码已完整（协议、SPI 引擎、GPIO、RPMh），一旦 MMIO 访问解决即可工作。

## 日常工作流

```sh
source ../../../env.sh                      # clang/llvm(免root) + venv + 7zz
./xl enter                  # Android→fastboot→loader(USB)
./xl ramdisk userland/system.apfs   # 2.5G 系统镜像进 RAM（每次 loader 会话一次）
XL_DEVICE=dodge XL_SECS=900 XL_CMDLINE="-v cpus=8" ./xl run
./xl screenshot shot.png                    # 屏幕内容（也无需看手机）
./xl reboot                                 # 回 Android
# 诊断：adb shell su -c 'dd if=/dev/block/by-name/rawdump of=/sdcard/r.bin bs=4096 count=64'
```

## 相关目录

- `uefi-dodge/` — Mu-Silicium dodge 移植的补丁与文件（export.sh 从 ../../Mu-Silicium 导出）
- `devices/oneplus-dodge/` / `dodge.platform.c` — 设备档案与 loader 平台描述（.platform.c 为参考副本）
- `dada-v17-project-*/`（工作区根） — UniXNU 参考（pong/SM8475 已到主屏；initrd payload 通道、AT 映射技巧来自它）

## 2026-10-25 下午会话记录

### 修复：内核无法启动（重大）

**症状**：01:54 之后所有内核秒死（无输出、无异常、60s 看门狗复位）。原厂 boot.img 能 fastboot boot。

**真凶**：`xnuloader/loader.c` 的 handoff 路径里，凌晨实验时加的块（在 `flush_dcache(phys_base, top)` 之后）：
```c
BS->exit_boot_services(...)                      // con_out 从此失效
chnl[n] 循环读 0x0c500000                        // 外部中止！
```
读中止区 → 异常钩子 → 钩子里 print() 用已死的 con_out → 二次异常 → CPU 锁死。
删掉整个块（EBS+SPMI）后一切恢复。**教训：任何"加在跳转前"的实验代码必须先审查是否触碰中止区。**

### SPMI v7 仲裁器（已完全掌握，读取路径可用）

- core=0x0c400000: ver=0x70020000, FEATURES(core+4) bits[10:0]=APID 数(896)
- **APID 表**：core+0x2000+4*i，ppid=(v>>8)&0xfff，count=FEATURES
- **GPIO3（触摸 AVDD，slave 9 ppid 0x98b）= APID 663**（owner=EE0，与 Android debugfs 反推一致）
- observer（读）：0x0c440000+0x20*apid，CMD+0x00 STATUS+0x08 WDATA+0x10 RDATA+0x18
- RW 通道（写）：0x0c500000+0x1000*apid（APID 0 槽位=中止区，勿碰）
- 命令：`(opc<<27)|((addr&0xff)<<4)|bc`；EXT_WRITE=2，EXT_READ=13
- **写被 Gunyah 无声丢弃**（UEFI 和内核上下文都一样）：WDATA 可写回读，CMD/CONFIG 写被吃，命令"完成"st=1 但 PMIC 寄存器不变（已用 gpio4 对照组证明）
- Android 正常状态（触摸工作时）：gpio3 = **out high** vin-1 pull-down-10uA（MODE_CTL=1, VIN=1, PULL=4, OUT=0x80, EN=0x80）
- XBL 每次启动把 gpio3 停靠为 mode=input（vin/pull/en 保留）——所以无法借 Android 的状态过重启

### RSC/RPMh（部分可用）

- apps_rsc=0x16500000（drv@2 TCS@+0xD00=0x16520D00）：**内核写 → 外部中止**（panic 日志确认）
- disp_rsc=0x0AF20000（drv@0 TCS@+0xD00）：**可写不崩，但 RPM 无响应**（RESP_REQ 无 ACK）
- cmd_db 关键地址：ldo.b4(触摸1.8V)=0x40800，gpio.sp=0x45900，vreg.sp=0x45800
- v3.0 布局：TCS stride 0x2A0，cmd stride 0x18，CONTROL+0x24(AMC enable bit16/trigger bit24)，CMD_ENABLE+0x2C，MSGID+0x34(写=0x10008)，ADDR+0x38，DATA+0x3C

### 结论：触摸被 Gunyah VM 沙箱完全封锁

fastboot-boot 的 guest VM 被授予的外设集不含：GENI SE4 写、SPMI PMIC 写、apps_rsc 投票。
**唯一出路 = 与 Gunyah 资源管理器（RM）SMC 协商**，把 GENI SE4 + PMIC 访问（或 gpio3 资源）分配给我们的 VM。
参考：PORTING-DODGE.md #12、Gunyah hypervisor 接口（GH_* SMC、消息队列）。

### 今日基础设施（保留在系统中）

- 内核驱动含：RPMh 投票尝试（disp_rsc）、内核侧 SPMI 写尝试、完整日志
- 触摸驱动本体（GENI FIFO + TouchComm v2 + 报文解码）就绪，一旦芯片有电即可工作
- 备份在 /tmp/opencode/backup-20261025/（含验证过的 UEFI SPMI 代码）

## 触摸进度总结（2026-10-25 晚）

### 已打通
| 步骤 | 状态 | 方法 |
|---|---|---|
| AVDD 2.8V | ✅ | SPMI opcode 0 (EXT_WRITEL)，不是 opcode 2 |
| VDD_IO 1.8V | ✅ | XBL 默认开启（init-voltage=1.8V） |
| 芯片 IRQ | ✅ | 断言 = 芯片已启动、有数据 |
| TLMM 引脚 mux | ✅ | 48-51 → qup1_se4 (func 1) |
| GCC 时钟"启用" | ✅* | ClockDxe 协议返回 Success（但见下） |

### 被阻断的
| 步骤 | 症状 | 原因 |
|---|---|---|
| GENI SE 固件启动 | status=0x41 但无 IRQ/无数据 | **串行时钟未真正到位** |
| GCC 直写 | 回读正确但无效果 | Gunyah 缓存写入（CPU cache），总线事务被过滤 |
| ClockDxe/SCMI | 返回 Success 但无效 | SCMI 也被 Gunyah 限制 |

### 根因
**Gunyah 对 fastboot-boot VM 的资源分配不包含 GENI SE4 的串行时钟。**
固件已加载（版本 0x102），GENI_SE_CTRL/OUTPUT_CTRL/SEQ_START 都能写入，
但序列器无法启动——时钟不到。

### 解决路径（下一步）
1. **Gunyah RM（Resource Manager）SMC 协商**——通过 SMC 与资源管理器通信，
   请求将 SE4 及其时钟分配给我们的 VM。需要实现 Gunyah 消息队列协议。
2. **或**让 XBL/ABL 在引导链早期启用 SE4（通过修改引导配置）
3. **或**在 UEFI 的 QupDxe/SPIDxe 中找到方法让 GENI 初始化完成

## MISO 被 Gunyah 屏蔽的发现（2026-10-25 深夜）

### 证据
- Pin 162 (IRQ) 读取正常（能检测到芯片的 IRQ 信号）
- Pin 48 (MISO) 读值永远为 0——即使：
  - 加了内部上拉 (pull=3)
  - GENI SE 输出已完全断开 (OUTPUT_CTRL=0)
  - 逐位时钟翻转并逐位读取
  - 芯片确认活着（IRQ 断言）
- Ace 5 上完全相同的代码（bit-bang SPI + 同样引脚）工作正常

### 结论
**Gunyah 把 touch SPI 引脚（48-51）的输入路径分配给了独立的 Touch VM。**
我们的 fastboot-boot VM 可以写 TLMM 配置寄存器和输出寄存器（mux/drive/output 值），
但读不到输入寄存器的真实值。这不是软件问题——是硬件级虚拟化隔离。

### 影响
- GPIO bit-bang SPI：❌ 永远不可行（MISO 读不到）
- GENI 硬件 SPI：❌ 串行时钟被封锁
- SPMI PMIC 写：✅ 可以（opcode 0 EXT_WRITEL 绕过）
- IRQ 读取：✅ 可以（pin 162 不在 Touch VM 的独占集合里）

### 唯一出路
1. **Gunyah Resource Manager SMC 协商**：通过 SMC 向 RM 请求将 SE4+pins+clock 分配给我们的 VM
2. **通过 Touch VM 的 IPC**：与已经拥有这些资源的 Touch VM 通信
3. **禁用 Gunyah**：在 XBL/ABL 级别禁用虚拟化（需要修改引导链）

### 技术细节供后续参考
- Gunyah RM 的 DT 节点在 /hypervisor/qcom,resource-manager-rpc@73025cf0a9902f87
- 消息队列用 qcom,is-full-duplex; qcom,rx-message-size; qcom,tx-message-size
- Linux 的 Gunyah 驱动在 drivers/virt/gunyah/ 和 include/linux/gunyah/
- 关键 API: gh_rm_mem_accept, gh_irq_lend, gh_msgq

## 触摸进度（2026-10-26 通宵会话）

### 已解决
1. **AVDD GPIO 页错误（历史性）**：DT 的 gpio 3 = 0 基引脚 2 = 寄存器页 0x8a00（ppid 0x98a），
   此前所有写入都打到了 0x8bxx（GPIO4！）。正确值（Android regmap debugfs 实测）：
   40=02(数字输出) 41=01(vin-1) 42=04 44=80(输出高) 45=01 46=80(使能)。
   修正后模拟前端活了：delta 帧出现真实电容数据（nz≈540/701）。
2. **SPMI 写路径真伪（本 VM 内核态）**：
   - chnls(0x0c500000)+opcode 0 = 真实写入 ✓
   - chnls+opcode 2、observer 通道写 = 静默丢弃 ✗
   - observer 读（0x0c440000+0x20*apid, opcode 13）= 真实硬件状态 ✓（唯一可信读回）
3. **start.s 的 FBMARK 彩条**：修启动回归时加的调试残留，已删除（用户报告的红绿蓝白屏）。
4. ldob4(VDD_IO 1.8V) 通过 RSC TCS 投票 {1800,1,4} 真实生效。
5. 完整复刻 touchDaemon 初始化序列（从真机 debug_level 抓取）：
   0x04 软复位→0x02 identify→0x20 app_info→0x35→0x36→0x23{ff}→0x25 保配置
   →DC{01,0}→使能0x23→DC{11,1}HBP→DC{f2,150}→DC{f5,150}→0x23{d5}→DC{d5,0}

### 待解决（最后一步）
- **mutual 扫描不启动**：delta 中 mutual 节点（0-645）始终噪声级，仅 self-cap 段（646-700，
  约 55 通道）有触摸相关信号（±25..150）。Android 中 mutual 正常。
- **0x23 HBP 帧只来 8 帧即停**（~3 秒），连续背靠背读也无法维持（10 秒紧读=8 帧+15 万空读）。
- 0x11 LBP 触摸报告从不产生。
- 头号嫌疑：**面板-触摸同步**。显示由 ABL 拉起后我们链里无人重新初始化面板；
  Android 内核会用 AA569 完整 DCS 序列重新初始化（live DT 中
  qcom,mdss_dsi_panel_AA569_P_3_A0019_dsc_cmd_evb 的 on-command 约 13 字节起
  `39000040000004ff5aa5001500004...`）。OLED 互容扫描可能与面板扫描时隙同步有关。
- 工具：Android 端 /proc/touchpanel/debug_level=1 + dmesg 抓 send_message 序列；
  /sys/kernel/debug/regmap/0-09/{count,address,data} 读任意 PMIC 寄存器真值。

### 帧格式笔记
- 0x23 HBP 帧：1518 字节；[0:2] 帧号、[4:6] 时戳、含固定结构（e8 03=1000, 90 01=400...）
- 0x12 delta 帧：1402 字节；[0]=序号；mutual 段 + self-cap 段（尾部 ~55 值）

## ✅ 触摸完成（2026-10-26）

**最终可用配方**（boot-arg `synaptics=1`）：
1. **供电**：ldob4(VDD_IO 1.8V) 走 RSC TCS 投票 {1800,1,4}；AVDD 走真正的 GPIO3
   页 0x98a（40=02 数字输出, 44=80 输出高, 46=80 使能），经 chnls+opcode0 写入
2. **传输**：GPIO 位拆 SPI（引脚 48-51），~1MHz，无需 GENI/时钟
3. **初始化**：pin 复位 → 0x07 检测魔数 → 退出深度休眠(0x2d) → rezero(0x27)
   → app info(0x20) → 保留默认报告配置(0x25) → DC{01,1} 禁 doze
4. **上报**：使能 0x11 + **帧率 DC f2=150 f5=150（关键！没有它们只有 ~7Hz，
   有了 125Hz）**
5. **解码**：通用 TouchComm 配置解释器；接受 finger/gloved/stylus/small/
   **EDGE(8)** 分类（边缘=主屏手势）；坐标钳制到面板边界
6. **注入**：hakios_touch_post → /dev/redwood-touch → backboardd → SpringBoard

实测：点按、拖动、边缘上滑手势全部可用，125Hz 报告率。

## 音频（ADSP）进度（2026-10-29）

详细记录：research/audio-adsp-progress.md（工作区）。要点：

**已全部打通（每步单独验证）**：SMEM 逐页读写、固件解析（Linux 式可重定位布局）、
AOP mailbox、DTB 子 PAS（0x24/0x25 完整周期含 auth_and_reset 成功）、主镜像
INIT_IMAGE/MEM_SETUP/53 段加载读回零差异、SMP2P 项、lcx/lmx=384 供电、
LP0/SH0/MC0 带宽 BCM 投票、xo.lvl=3 时钟、GIC 门铃线屏蔽、load_state on、
再 INIT——即 Linux adsp_start 的每一步都已复刻。

**唯一剩余阻塞**：PAS_AUTH_AND_RESET（PAS 1）整机冻结。决定性对照实验：**CDSP
（PAS 18，另一颗 Q6、另一子系统）同样冻结**，而两个 DTB PAS（不释放核心）都成功
→ 问题在 Q6 核心释放路径本身，非 ADSP 特有配置。

**机理（有 Android 侧证据）**：SM8750 的 DSP 是 Gunyah 管理的受保护 VM。Android
内核跑全套 gh_rm_drv/mem_buf/qrtr_gunyah 栈；我们的 fastboot 上下文没有向 Gunyah
资源管理器注册，核心释放 SMC 进入的 TZ→Gunyah 路径因缺少 VM 状态在 EL2 死锁。

**回归路径（有信心再动手）**：① Android 侧 kprove PAS SMC 流；② 主线
drivers/virt/gunyah 的 RM 协议最小握手；③ 先搞自恢复（gh_virt_wdt 或 OPLUS
pmicwd）；④ 才再碰 auth。

关键启动参数（hakios.adsp.c，全部在 init 解析）：adsp_step=N 安全阶梯、
adsp_noauth=1（只跳过 ask）、adsp_bw=1（BCM 投票）、adsp_fw=cdsp+adsp_pas=18+
adsp_dtb_pas=0x25+区域参数（CDSP 对照）、adsp_cmddump=1（cmd_db 转储）。
RPMh 细节：v7 寄存器布局（msgid 0x34/步进 24/control 0x24）；ARC 电压写 selector、
ARC 时钟写原始值（xo.lvl=3，写 selector 会冻结）；BCM 命令
VALID|COMMIT|(x<<14)|y（cmd_db slave[2] 大写名）。


## 按键（2026-10-28，三键全工作）

电源键与音量上键走 PON（KPDPWR + RESIN；本机 resin 是音量上，buttons_swap=1），
天生配置好、只读即可。音量下键是 PM8550 sid1 GPIO6（buttons_pin=0x18d，寄存器
0x8d00；0x18e 是 GPIO7，属主 EE4——写它内核直接中止）。固件把引脚留成禁用，
驱动经仲裁器 chnls 通道（APID 610，cnfg 归属表属主 EE0，与触摸的通道同类）把
DIG_EN_CTL(0x46)=0x80 写上即启用。修了三个叠着的 bug 才通：

- 引脚号差一（0x8d00 才是 GPIO6：pinctrl 基址 0x8800 = GPIO1，+0x100/脚）
- 写循环的 start 未初始化，状态查询瞬间退出误报"无应答"
- v8 observer 每次只回 4 字节，8 字节读的后 4 字节是假 0——enable 位一直在
  读假数据（写成功了也显示"still not right"）

## 按键（2026-10-28，三键全工作）

电源键与音量上键走 PON（KPDPWR + RESIN；本机 resin 是音量上，buttons_swap=1），
天生配置好、只读即可。音量下键是 PM8550 sid1 GPIO6（buttons_pin=0x18d，寄存器
0x8d00；0x18e 是 GPIO7，属主 EE4——写它内核直接中止）。固件把引脚留成禁用，
驱动经仲裁器 chnls 通道（APID 610，cnfg 归属表属主 EE0，与触摸的通道同类）把
DIG_EN_CTL(0x46)=0x80 写上即启用。修了三个叠着的 bug 才通：

- 引脚号差一（0x8d00 才是 GPIO6：pinctrl 基址 0x8800 = GPIO1，+0x100/脚）
- 写循环的 start 未初始化，状态查询瞬间退出误报"无应答"
- v8 observer 每次只回 4 字节，8 字节读的后 4 字节是假 0——enable 位一直在
  读假数据（写成功了也显示"still not right"）
