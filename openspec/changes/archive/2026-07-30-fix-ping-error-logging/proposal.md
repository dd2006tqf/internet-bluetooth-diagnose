# Proposal: fix-ping-error-logging

## Why
`net_ping.cpp` 中的 `ping()` 函数在遇到错误时只返回负数错误码（-1 到 -9），但没有记录任何错误信息。这使得调试网络问题时非常困难，无法知道具体是什么原因导致的失败。

## Problem
代码中缺少错误日志记录，所有错误路径都只是返回错误码，没有使用 `LOG(ERROR)` 记录具体的错误信息。

## Proposed Solution
在每个错误返回点添加 `LOG(ERROR)` 记录：
- socket 创建失败时记录 `strerror(errno)`
- 绑定设备失败时记录接口名和错误信息
- 解析主机失败时记录主机名
- 发送失败时记录错误信息
- 接收超时或错误时记录相应信息
- 响应验证失败时记录原因

## Impact
- 修改文件：`server/src/net_ping.cpp`
- 影响范围：仅添加日志，不改变函数返回值和行为
- 风险：低，仅添加日志输出
