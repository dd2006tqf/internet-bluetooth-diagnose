# 全量代码评审处置计划：免决策项执行

> 日期：2026-11-16
> 分类：工作流外实施
> 结果：已完成（O7 及处置计划第 5 行按规则留待 Planner）

## 构思

`/full-code-review` 产出报告 §7 的处置表，把发现分成两类：需要 Planner 决策的
（触及协议形状与公开 C ABI 的孤儿接口下线），和不需要决策的（实现缺陷与文档
失真）。用户指令是「按照处置计划，先把不用我决策的代码部分的任务都做了」。

不做进 OpenSpec change 的理由有两条，都不是为了省事：

1. 这批改动是**对既有行为的修正与补齐**，不是新能力。其中 manifest 哈希漂移
   本身会阻断一切 harness 包装器（`loadManifest()` 校验失败即抛），此时连
   `change_new.sh` 都跑不通——先修门禁才有可能谈工作流。
2. 代码已存在的情况下套用 OpenSpec 追认流程会踩项目明令禁止的坑：footprint 为
   0、`--path` 与 diff 不符、`planning changed during command` 死锁。

备选方案与取舍：唯一需要取舍的是导出符号收敛（I1）。可行方案有三个——
`-fvisibility=hidden`、逐个 `__attribute__((visibility))` 标注、链接器版本脚本。
选版本脚本，因为另外两条都有硬伤（见「实现方式」）。同一处取舍也适用于
`SOVERSION`：加 SONAME 会改变运行时 `DT_NEEDED`，必须同步改三条部署链，
否则板端加载失败——这不是可选项，是加 SONAME 的连带义务。

处置表第 5 行（O1–O5 孤儿接口下线：−7 个 D-Bus 信号常量、`ListInterfaces`、
蓝牙 4 方法 + C ABI、`Ping` 链、5 个仅测试使用的 C ABI）**一律未做**：它们改的是
协议形状与公开 C ABI，属于 Planner 决策范围。维护类 O6/O8 做了，O7 延后。

## 实现方式

### 1. manifest 门禁修复（C1）

`scripts/manifest_sync.sh` 在最后一步执行——它要对受管模板的**最终内容**取哈希，
必须等我改完 `CLAUDE.md` / `prompts/*` 之后才跑。结果：

```
FIX: CLAUDE.md
FIX: prompts/debt-scan.md
FIX: prompts/rca.md
Fixed 3 manifest hash(es)
```

之后 `openspec_preflight.sh` 与 `workflow_contract_check.sh` 均通过
（`manifest policy passed` / `OpenSpec preflight passed` / `[OK] workflow contract`）。

### 2. 三条打包链对齐（C2）

`tools/ci.sh`、`tools/release.sh`、`.github/workflows/release.yml` 的 dist 组装
必须逐项一致，否则同一版本号经不同路径产出不同固件。本次把后两条补齐到与
`ci.sh` 对齐：构建目标加入 `weaknet_health_guard` 与 `test_crash_handler`、
产物加入 `weaknet-health-guard`、Qt 看板与 `kmod/*.ko`。

CI 侧的 kmod 是**条件构建**：交叉编译要求目标板逐字一致的内核头文件
（`load_module()` 逐字比对 vermagic），GitHub 托管 runner 拿不到。因此有头文件
缓存才构建，无缓存时抛 `::warning::` 明确说明本次发布不含 kmod、以及补齐方式——
静默跳过会让「发布物里没有」与「模块加载失败」在板端表现相同，无从区分。

看板与 kmod 一样是**失败不中断**：看板是可选人机界面，不是所有部署都装显示器。

### 3. 文档失真同步（I2）

- 测试套件数 61 → 67（`README.md`、`CLAUDE.md`、`docs/skills-and-openspec-guide.md`、
  `docs/ci-测试使用指南.md` 三处、`docs/交叉编译与开发板部署.md`），
  `.ai-harness/project-profile.json` 的 reason 由 47 目标 / 46 套件 / 4.6s 更新为
  70 目标 / 67 套件 / 16.9s。
- `server/src/dbus_service.cpp` 头注释「28 个 DBus 方法」→ 35 条分发分支
  （34 个方法名，`GetInterfaces` 是 `ListInterfaces` 的别名）。实测核对：
  35 个 `dbus_message_is_method_call` 分支、35 个 `kMethod*` 常量、
  35 个 `bool DbusService::handle*` 定义。
- `docs/产品定位-工业无线诊断网关.md` 三处过时断言：视频 `NONE` 已接入评估循环
  （`NONE` 的语义是设备侧帧形态，不是「未接入」）；看板已进入三条链。

### 4. 对外 ABI 边界收敛（I1）

**版本脚本**（新文件 `client/weaknet.map`）：

```
{
  global:
    weaknet_*;
  local:
    *;
};
```

**为什么不用 `-fvisibility=hidden`**：隐藏符号无法被版本脚本重新提升为 global，
两者叠加的产物导出表是空的——库能编出来，但所有消费者链接失败。

**为什么不用逐个 `__attribute__((visibility))` 标注**：需要改动每个声明点，
新增函数忘标注就从公开 ABI 静默消失，而版本脚本是白名单，默认就是「不导出」。

`client/CMakeLists.txt` 同时加 `VERSION`/`SOVERSION` 与 `LINK_FLAGS=-Wl,--version-script=...`。
顺带修了 `client.cpp` 里 `weaknet_get_version` 硬编码的 `v1.0.0`（`VERSION` 已是
1.1.0），改为 `WEAKNET_VERSION` 宏；该宏经 `server/CMakeLists.txt` 以 `PUBLIC`
挂在 `weaknet_common` 上，`compile_commands.json` 实测确实带
`-DWEAKNET_VERSION=\"1.1.0\"`。

**安全前提先证后改**：枚举全部仓库内消费者的未定义符号（`test_client_bin`、
`weaknet_cli`、看板 `data_source.cpp`），与 libweaknet 的定义符号求交集——
所需的全部落在 `weaknet_*` 前缀内，无任何 C++ 符号被外部消费，收敛不会破坏消费者。

加 SONAME 的连带义务已全部落实：`tools/ci.sh`（打包用 `cp -a ... libweaknet.so*`
保留软链，上传按真实文件名 `libweaknet.so.<VERSION>` 单文件 scp 而非 `scp -r`，
板端建 `libweaknet.so.1` 与 `libweaknet.so` 两条链再 `ldconfig`）、
`tools/release.sh`、`.github/workflows/release.yml`、
`tools/weaknet-local-update.sh`（`COMPAT_ENTRIES` 加 `lib`）。

### 5. 维护类（O6 / O8）

- O6：`prompts/rca.md` 补「用 `scripts/rca_new.sh` 而不是手改
  `defect-rca.md`」（脚本会取 harness 锁、按哈希检测并发修改，手工编辑会丢条目）；
  `prompts/debt-scan.md` 补 `scripts/ai_debt_scan.sh` 是启发式扫描、不是工作队列。
- O8：新增 `tools/README.md`，索引三个 `board_*.cpp` 一次性探针（各自回答什么问题、
  为什么不进 CMake、结论记在哪），并声明三条打包路径的 dist 组装必须一致。

### 验证手段

全部为本地可复现命令，无板端声明（当时 `ssh board` 反向隧道不可用，
故本记录**不包含任何板端状态断言**）：

```
cmake --build build-x86 / build-arm64-native          # 两条链全量编译通过
ctest --test-dir build-x86/server --output-on-failure # 67/67 通过，16.82s
nm -D --defined-only libweaknet.so.1.1.0 | wc -l      # 39（原 421）
readelf -d libweaknet.so.1.1.0 | grep SONAME          # libweaknet.so.1
readelf -d weaknet_cli | grep NEEDED.*weaknet         # libweaknet.so.1
bash -n tools/ci.sh tools/release.sh tools/weaknet-local-update.sh
python3 -c 'yaml.safe_load(...)'                      # release.yml 7 个步骤、顺序正确
```

`tools/release.sh` 的新 dist 组装另在 `/tmp/rel-sim` 空目录忠实模拟过一遍：
26 个文件、软链链完整、`weaknet-cli` 的 `DT_NEEDED` 为 `libweaknet.so.1`。

## 结果

已实现并本地验证：manifest 门禁转绿；三条打包链对齐；文档失真全部同步；
`libweaknet.so` 导出从 421 收敛到 39 并带上 `SONAME libweaknet.so.1`；
`ctest` 67/67。

**遗留问题**：

- **O7 延后**：给 `scripts/audit_summary.sh`、`scripts/change_abort.sh` 登记
  需要同时改 `scripts/manifest_policy.js` 的 `templatePaths` 和
  `.ai-harness/workflow-contract.json` 的 `managed_scripts`（反向对齐校验
  `sameSet` 要求两边同步），这会扩大治理范围，属于 Planner 决策。
- **处置表第 5 行整体未动**：O1–O5 触及 D-Bus 协议形状与公开 C ABI，须回 Planner。
- `tools/ci.sh:178` 仍 `mkdir dist-arm64/tools`，但已无任何产物落到那里
  （watchdog 接管脚本按 Plan B 整体删除后遗留）。无害，未清理——清理属于
  「无关顺手改动」，超出本次范围。
- `tools/weaknet-local-update.sh` 的 legacy-flat 迁移循环（约 742 行）仍是
  硬编码字面量列表，没有复用 `COMPAT_ENTRIES` 数组。两处条目当前内容一致，
  但存在漂移风险；改动会触及升级路径分支，未在本次擅自合并。

**是否升格为正式 OpenSpec change**：不需要。这批是缺陷修复与文档同步，
没有引入新能力。反过来看，O1–O5 的孤儿接口下线**适合**走一次正式 change
（Planner 定范围 → Generator 实现 → Evaluator 用真实消费者验证），
尤其蓝牙 4 方法与 `Ping` 链涉及 C ABI 与协议形状，需要迁移与回滚方案。
