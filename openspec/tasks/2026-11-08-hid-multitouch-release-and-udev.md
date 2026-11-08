# hid-multitouch 进发布包 + udev 按需加载

> 日期：2026-11-08
> 分类：工作流外实施
> 结果：已完成

## 构思

### 问题

`kmod/hid_multitouch/` 在 2026-11-07 就验证通过（板端编译、vermagic 匹配、38 符号
全导出、虚拟 MT 设备绑定、MT 能力位齐全、单帧事件贯通、`insmod`/`rmmod` 可逆），
但当时的结论是「**不进发布包、默认不加载**」，写在三处：

| 文件 | 原表述 |
| --- | --- |
| `kmod/hid_multitouch/README.md:53` | 「板端编译是唯一受支持方式」 |
| `kmod/hid_multitouch/README.md:195` | 「不进 `tools/ci.sh`、不进 OTA manifest、不随发布包分发」 |
| `kmod/hid_multitouch/Makefile:6` | 「不进 tools/ci.sh、不进 OTA manifest、不随发布包分发」 |

这两个结论有**不同的性质**，必须分开处理：

1. **「板端编译是唯一受支持方式」是一个未实测的保守推断。** 原文的推理是
   「内核若开启了 `CONFIG_MODVERSIONS` 或 BTF 差异会导致 `insmod` 拒绝」——
   但**从来没查过本板到底开没开**。这个推断直接阻断了「随发布包分发」的可行性：
   现场没有内核头文件，就不可能在板端编译。
2. **「不进发布包」是一个决策**，理由是「会与 `hid-generic` 抢设备、改变全局输入
   行为」。这个理由**方向对但结论过头**：正确的做法是**控制加载时机**，而不是
   不交付模块。不进包意味着交付一个现场装不上的能力。

### 关键实测（推翻推断）

| 检查 | 实测值 | 含义 |
| --- | --- | --- |
| `CONFIG_MODVERSIONS` | **not set** | 无符号 CRC 校验，不看编译器版本 |
| `CONFIG_MODULE_SIG` | **not set** | 不强制签名 |
| `CONFIG_MODULE_SIG_FORCE` | config 里没有该键 | 同上 |
| `CONFIG_DEBUG_INFO_BTF` | `=y` | 但 headers 无 `vmlinux`，模块 BTF 生成被跳过；内核加载不要求模块带 BTF |

决定性验证：在 x86 宿主用 **gcc 13.3.0** 交叉编译（板端内核是 **gcc 10.2.1** 编的），
产物 564088 字节、`vermagic=5.15.147-99btf1-a733 SMP preempt mod_unload aarch64`
逐字匹配、38 个未定义符号全在 `Module.symvers` 里，然后：

```text
板端 sudo insmod /tmp/hid-multitouch-xbuild.ko   → rc=0
lsmod                                            → hid_multitouch 24576 0
/sys/bus/hid/drivers/hid-multitouch              → 目录建立
sudo rmmod hid_multitouch                        → rc=0，目录干净消失
```

**编译器版本差三个大版本照样能加载**，因为 `MODVERSIONS` 关着时内核只认 vermagic。

### 加载时机怎么控制

「进包」与「自动加载」是两件事。原决策把两者捆在一起否掉了，这是过度反应。
真实的影响面比原表述窄：

- 通用兜底项 `hid-multitouch.c:2168` 是
  `HID_DEVICE(HID_BUS_ANY, HID_GROUP_MULTITOUCH, HID_ANY_ID, HID_ANY_ID)`，
  只匹配被 HID 核心判为 `HID_GROUP_MULTITOUCH` 的设备。
- 普通键鼠是 `HID_GROUP_GENERIC`（`g0001`），**不会**被抢。
  所以原表述「所有 HID 设备（键鼠、条码枪、工业手柄）的绑定关系都会变」
  **不成立**，属于过度担忧。

但「所有 MT 设备的绑定会变」是真的，对触摸屏是改善，仍是**全局输入行为变更**。
一台不带触摸屏的网关没有理由承担它。所以开关应由「设备是否存在」这个物理事实
驱动，而不是由开机顺序驱动 —— 这就是 udev 规则。

### 备选方案与取舍

| 决策点 | 备选 | 选择 | 理由 |
| --- | --- | --- | --- |
| `.ko` 进不进包 | A 进包 / B 不进包 | **A** | 不进包 = 现场装不上 = 交付了不可用的能力。交叉编译已证可行，没有理由不带 |
| 加载时机 | A 开机 systemd / B 手动 insmod / C udev 按需 | **C** | A 让无触摸屏的网关白白承担输入行为变更；B 现场每次插屏要敲命令 |
| udev 匹配条件 | A 匹配所有 HID / B 匹配 MT group | **B** | 匹配所有会让插键盘也加载模块，完全违背「按需」 |
| 编译失败处理 | A 整体失败 / B 只告警 | **B** | hid-multitouch 是可选输入能力，缺它不影响业务；而观测模块缺失会让 `observer_attribution` 报 unavailable，不能让前者拖累后者 |
| 判断模块已加载 | A `lsmod` 查名 / B `test -d /sys/module` | **B** | udev 的 `RUN+=` 不去重，同设备多接口会触发多次；`lsmod` 有解析成本且受输出格式影响，目录存在是权威判据 |

## 实现方式

### 1. `tools/kmod_cross_build.sh`：改为两次独立构建

原脚本只 `cp "$ROOT"/kmod/*.c *.h Makefile` 平铺进一个临时目录，假设所有模块
共享一个 Makefile。hid-multitouch 在子目录且有自己的 `obj-m`，无法被这条路径覆盖。

改为两个构建单元：

```bash
kmod/                 → weaknet_cpu_stat.ko + weaknet_softirq.ko（原有）
kmod/hid_multitouch/  → hid-multitouch.ko（新增第二遍 make）
```

**失败隔离**是关键设计：hid-multitouch 编译失败时只 `echo` 告警并 `tail` 日志，
**不 `fail`**。因为 `tools/ci.sh:181` 的 `else` 分支会 `rm -rf dist-arm64/kmod`，
如果这里 `fail`，会把已经编好的两个观测模块一起删掉——用一个可选能力拖死两个
必需能力。已实测：故意塞入坏源码后，脚本退出码仍 0，产出目录只有 2 个观测模块。

### 2. `tools/weaknet-hid-multitouch.rules`：新建 udev 规则

匹配条件用 HID group，而不是「所有 HID」：

```udev
ACTION!="remove", SUBSYSTEM=="hid", \
  ATTRS{modalias}=="hid:*g0002v*", \
  RUN+="/bin/sh -c 'test -d /sys/module/hid_multitouch || /sbin/insmod /home/radxa/weaknet/current/kmod/hid-multitouch.ko'"
```

`g0002` 是 `HID_GROUP_MULTITOUCH`（`include/linux/hid.h:371`），`g0004` 是
`HID_GROUP_MULTITOUCH_WIN_8`（`:373`）。写成 `hid:*g0002v*` 而不是 `hid:*g0002*`
是为了用紧跟的 `v`（vendor 前缀）做边界，不可能误匹配到其它字段里的 `g0002` 子串。

两个为什么：

- **为什么 `test -d /sys/module/hid_multitouch ||`**：udev 的 `RUN+=` 不去重。
  一块触摸屏常同时暴露 Digitizer 与 Mouse 两个 collection，规则会被触发两次；
  第二次 `insmod` 报 `File exists`，虽无害但会在 journal 里留下看不懂的错误行。
- **为什么 `insmod` 而不是 `modprobe`**：模块住在 OTA 的 `releases/<ver>/kmod/`，
  不在 `/lib/modules` 下。经 `current` 软链寻址，OTA 翻链后自动指向新版本。

### 3. `tools/ci.sh`：分发 `.ko` + 安装规则

- 第 181 行 `kmod_cross_build.sh --out dist-arm64/kmod` 本身不用改——脚本多产出一个
  `.ko`，后续 rsync 自动带进 `releases/<ver>/kmod/`。
- 新增约 20 行：在 `weaknet-kmod.service` 落盘之后，`scp` 规则到
  `/etc/udev/rules.d/99-weaknet-hid-multitouch.rules` 并 `udevadm control --reload-rules`。
  失败只 `warn`，不阻塞部署——没有触摸屏时这条规则本就不会触发。

### 4. 文档订正

| 文件 | 改动 |
| --- | --- |
| `kmod/hid_multitouch/README.md` | 开头「不随发布包分发」→「随包分发但默认不加载」；「构建」节整节重写（含实测表）；「定位：不进发布包」整节替换为「发布与加载模型」；「边界声明」补 udev 双向验证；「消费者状态」重写（UI 消费者已落地、发布链已接入） |
| `kmod/hid_multitouch/Makefile` | 头注整段重写：说明「两种加载模型」（systemd 无条件 vs udev 按需） |
| `kmod/Makefile` | 头注去掉「不进 tools/ci.sh、不进 OTA manifest」的过时表述，补「hid_multitouch 是另一个独立构建单元」 |
| `驱动面能力引入与业务融合方案.md` | §7.5 验证表补 4 行（交叉编译/udev 触发/udev 不误触发/加载顺序无关）；§7.6.2 状态表「不进发布包」→「进包 + udev 按需」；§14 选购建议订正（原写「USB 电容触控板」是错的） |
| `docs/产品定位-工业无线诊断网关.md` | 第八节「现场交互终端」行：从「触摸交互点尚未实现」订正为实际状态 |

### 5. 板端验证

#### 双向 udev 触发

| 场景 | 结果 |
| --- | --- |
| 模块未加载 + 插入虚拟 MT 设备（`g0002`） | ✅ udev 自动 `insmod`，随后 MT 能力位齐全、7 个 input 事件贯通 |
| 模块未加载 + 插入虚拟标准鼠标（`g0001`） | ✅ 模块**未**被加载，鼠标仍由 `hid-generic` 绑定 |

#### 加载顺序无关性

原 README 假设「驱动必须先于设备加载」，因此建议用 `new_id` / `unbind`+`bind` 兜底。
实测证明不必要：

```text
步骤 1: hid_multitouch 未加载 → 创建设备 → 绑定驱动 = hid-generic
步骤 2: insmod hid-multitouch（不做任何 bind 操作）
        加载后绑定驱动 = hid-multitouch     ← 内核自动改绑
```

**结论：udev 规则即使在 `hid-generic` 已接管之后才生效，也能把设备抢回来。**
README 里那两条手工兜底路径仍然保留（特殊固件可能需要），但不再是主路径。

#### 失败隔离

塞入无效 C 源码后跑完整脚本：

```text
[kmod] 警告: hid-multitouch 编译失败，本次发布不含触摸支持
[kmod]   weaknet_cpu_stat.ko  vermagic 校验通过
[kmod]   weaknet_softirq.ko  vermagic 校验通过
[kmod] 完成：2 个模块
--- 脚本退出码: 0 ---
```

观测模块正常产出，`ci.sh` 不会误删。

#### 语法检查

```bash
bash -n tools/kmod_cross_build.sh   # OK
bash -n tools/ci.sh                 # OK
udevadm verify tools/weaknet-hid-multitouch.rules
# → 1 udev rules files have been checked.  Success: 1  Fail: 0
```

## 结果

**已完成**。三个模块全部随发布包分发，hid-multitouch 走 udev 按需加载。

| 项 | 状态 |
| --- | --- |
| 宿主机交叉编译 hid-multitouch | ✅ 564360 B，vermagic 逐字匹配 |
| 板端 `insmod` 交叉编译产物 | ✅ rc=0（推翻「板端编译是唯一方式」） |
| `.ko` 进发布包 | ✅ `tools/kmod_cross_build.sh` + `tools/ci.sh` |
| udev 规则 | ✅ `tools/weaknet-hid-multitouch.rules`，双向实测 |
| 加载顺序无关性 | ✅ 实测自动改绑 |
| 失败隔离 | ✅ 实测观测模块不受牵连 |
| 文档同步 | ✅ 5 个文件 |

### 遗留

1. **真实触摸屏未到位**。驱动链路、udev 规则、UI 交互点全部闭环，但物理设备没有，
   因此「真机点按」这一步仍未验证。这一条只能等硬件。
2. **`hid-ids.h` 里固定 VID/PID 表是否覆盖具体型号未知**。通用兜底条目
   （`HID_GROUP_MULTITOUCH`）覆盖大多数情况，但廉价设备描述符不规范时可能落到
   `hid-generic`。届时按 README 的手工路径处理（`new_id` 或 `unbind`/`bind`）。
3. **多点手势未接线**。白名单三项（指标切换 / 时间窗切换 / 截图导出）全是单击，
   单点就够。捏合缩放、双指滑动这类手势不在白名单内，也不该做。
4. **udev 规则的 `current` 路径依赖**。规则写死
   `/home/radxa/weaknet/current/kmod/`，与 `weaknet-kmod.service` 的寻址方式一致。
   如果将来 OTA 布局改名，两处必须同步改。
