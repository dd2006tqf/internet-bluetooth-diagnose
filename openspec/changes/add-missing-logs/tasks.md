# Tasks: add-missing-logs

- [x] 1 补全零日志文件的基础日志
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `错误路径日志` | `零日志文件添加基础日志`
  - Verify: `build`
  - 为 traffic_anomaly_detector.cpp、bt_audio_fusion.cpp、net_tcp.cpp、serializer.cpp、net_info.cpp 添加 logger.hpp 并补充日志

- [x] 2 补全高缺口文件的错误路径日志
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `错误路径日志` | `D-Bus 错误路径日志`
  - Verify: `build`
  - 为 dbus_service.cpp、looper.cpp、using_iface.cpp、net_wifiriss.cpp 的错误路径添加 LOG_ERROR

- [x] 3 补全中缺口文件的不一致日志
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `入口/出口日志` | `Safe 方法入口/出口日志`
  - Verify: `build`
  - 为 weak_netmgr.cpp、bt_monitor.cpp、band_conflict_detector.cpp、traffic_analyzer.cpp、network_quality_assessor.cpp 补全日志

- [x] 4 补全客户端 C API 日志
  - Covers: `specs/weaknet-server/spec.md` | `ADDED` | `错误路径日志` | `客户端 C API 日志`
  - Verify: `build`
  - 为 client.cpp 的核心 C API 函数添加 LOG_ERROR/LOG_INFO
