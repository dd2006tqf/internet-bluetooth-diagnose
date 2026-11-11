# 诊断规则的字段解析器只认 SLE evidence，没有字段等于永远无法归因

> 发现于：2026-11-10
> 位置：`server/include/assurance/diagnosis_engine.hpp:205`（`buildResolver` 白名单）
> 影响：`excluded_causes` 永远不含视频与 thermal 类排除项，归因链是断的

## 现象

`NetworkExperience` 里明明有 `video_qoe_attribution`、`video_qoe_issues` 这些字段，
`legacy_adapter` 也把它们序列化了，`weaknet-cli experience` 能读出来。

但诊断规则里写的 `excluded_candidates` 条件，只要涉及视频或 thermal，**永远不成立**。
规则文本写着「排除视频侧劣化」，实际运行时这个排除项从来不出现。

问题在于它**不报错**。没有异常、没有日志、没有测试失败——`excluded_causes`
就是安静地少了一条。

## 成因

`buildResolver` 返回的 lambda 是诊断规则的**唯一字段来源**。它只做两件事：

1. 匹配几个硬编码的 SLE 字段（`overall_state`、`link_type`、`dns_service.state` 等）；
2. 遍历一个**写死的 SLE 列表**去找 `item.metric`：

```cpp
for (const auto* sle : {&exp.ip_reachability, &exp.responsiveness, &exp.reliability,
                        &exp.rf_health, &exp.dns_service, &exp.tcp_connect,
                        &exp.tcp_rtt, &exp.http_access, &exp.captive_portal}) {
    for (const auto& item : sle->evidence) {
        if (field == item.metric) return item.value;
    }
}
return std::nullopt;   // 字段缺失返回 nullopt，保证 Missing != false
```

这个列表里**没有 thermal，也没有 video**。

`video_qoe_attribution` 不是某个 SLE 的 `evidence` 条目，它是
`NetworkExperience` 上的**顶层字段**，压根不在遍历范围内。

所以无论视频归因算出什么，resolver 拿到的都是 `std::nullopt`。而
`evaluateGroup` 对 `nullopt` 的语义是「缺失」，既不等于 true 也不等于 false——
所以对应的 `excluded_candidates` 永远不会被写进 `excluded_causes`。

**这是两种数据形态的错位**：视频归因是「顶层结论字段」，而 resolver 只认
「SLE 下的 evidence 条目」。字段存在，但不在解析器够得着的地方。

## 修法

**2026-11-10 决定暂不修**，理由有三条：

1. 要让 resolver 看到，必须把视频证据挂进某个 SLE 的 `evidence` 数组。这会
   污染那个 SLE 的语义——`dns_service.evidence` 里出现 `frame_loss_ratio`
   是说不通的。
2. 现有的正确范本是 observer attribution：它挂进 `exp.dns_service.evidence`
   （见 `server/src/server.cpp:630` 注释）。这个选择有实质依据，不是凑合——
   代码注释写着「DNS 是当前唯一带 observer 质量门禁的 SLE」，
   即 observer 的证据确实是在说「DNS 这一项为什么可能判错」，归属关系成立。
   而视频帧统计与任何单个 SLE 都没有这种归属关系。
3. 更根本的问题：`excluded_causes` 的语义是「机器判定这些原因可以排除」。
   要排除视频侧，前提是**视频那一路确实采到了数据**。当前生产态
   `video_qoe_attribution` 恒为 `NONE`（帧源未接进评估循环），
   严格来说「无数据」根本不能作为「已排除」的依据。

所以正确顺序是：先让帧源接进评估循环、确认能产出非 `NONE` 结论，再讨论
要不要给 resolver 开一个「顶层归因字段」的读取通道。

## 边界

这个决定意味着**归因链目前是断的**，而且断在一个不会报错的地方。

如果后续要给 resolver 扩展，要注意两条：

- 不要把顶层字段塞进不相干的 SLE evidence，那会让 `evidence` 失去
  「属于这个 SLE 的支撑材料」这个语义；
- 扩展 resolver 时，缺失语义必须保持 `nullopt` 而不是 `false`。
  把「不知道」写成「不成立」，会让排除项在不该出现的时候出现——
  比不出现更危险。

## 教训

**「字段存在」和「字段可达」是两件事。**

序列化链路通了、命令行能读出来、测试直接构造也能过——这三件事都不代表
消费方真的能拿到它。要验证一个字段是否可消费，必须走完**消费方的实际解析路径**，
而不是只看它有没有被写进数据结构。

这类问题的共同特征是：**没有任何一处会报错**。数据生产正常，序列化正常，
只是中途某一个解析器不认识它。所以只能靠「从消费端反查」来发现——
先问「谁会读这个字段」，再问「它读的时候用的是哪个入口」。
