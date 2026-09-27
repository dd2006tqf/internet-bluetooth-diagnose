# WeakNet Server Spec — Delta for ebpf-map-sizing-wiring

## ADDED Requirements

### Requirement: eBPF Map 容量定标贯通到全部加载路径

服务端 MUST 把已解算的 Map 容量计划传递到每一个实际的 `bpf_object` 加载点，使 `applyMapSizingPlan` 在 `bpf_object__open` 与 `bpf_object__load` 之间作用于目标 map；MUST NOT 让已解算的计划在共享 fd、单例复用或提前返回的分支上被丢弃。

#### Scenario: flow_rate 两张 map 经流量分析通路获得定标

- **WHEN** 流量分析线程以 `auto` 或 `fixed` 模式启动，且 `process_profiler` 配置块给出定标参数
- **THEN** MUST 按该配置块解算容量计划并沿流量分析器传递到 `flow_rate.bpf.o` 的加载点，使 `current_sec` 与 `process_stats` 的 `max_entries` 等于解算值而不是内核编译期默认值

#### Scenario: 共享 map fd 路径记录 resolved 容量

- **WHEN** 进程画像器复用流量分析器已加载的 `process_stats` map fd
- **THEN** MUST 记录该 map 的 resolved 容量，使逐 key 驱逐判定以真实容量作水位比较，而不是因容量未知而跳过判定

#### Scenario: 蓝牙两张 map 获得定标

- **WHEN** 蓝牙音频分析器以 `auto` 或 `fixed` 模式初始化，且 `bluetooth` 配置块给出定标参数
- **THEN** MUST 在 `bpf_object__open` 与 `bpf_object__load` 之间对 `active_sessions` 与 `bt_traffic` 应用解算容量，使既有的 `bluetooth.map_sizing` 配置键产生实际效果

### Requirement: Map 定标规格表与内核编译期常量一致

规格表中每个 map 的 `default_entries` MUST 等于该 map 在对应 `.bpf.c` 源码中的编译期 `max_entries` 常量，以保证「未配置任何定标参数时行为与 v1 零变化」；该一致性 MUST 由可执行的交叉校验测试守住，而不是仅靠注释约定。

#### Scenario: 规格表默认值与 BPF 源码声明逐一相符

- **WHEN** 交叉校验测试遍历全部 `MapSizingScope` 的规格表条目，并与对应 `.bpf.c` 中该 map 声明的 `max_entries`（含 `#define` 常量）比对
- **THEN** MUST 逐项相等且不遗漏任何受管 map
