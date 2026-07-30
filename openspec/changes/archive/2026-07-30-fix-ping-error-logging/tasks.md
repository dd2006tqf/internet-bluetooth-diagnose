# Tasks: fix-ping-error-logging

- [x] 1 为 ping() 函数添加错误日志
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `ping 错误日志` | `添加 LOG(ERROR) 记录错误信息`
  - Verify: `build` `test`
  - 修改 `server/src/net_ping.cpp` 中的 `ping()` 函数，在每个错误返回点添加 `LOG(ERROR)` 记录具体错误信息
  - RED 阶段：编译验证语法正确
  - GREEN 阶段：添加所有错误日志
  - REGRESSION 阶段：运行 `test-all` 全量回归
