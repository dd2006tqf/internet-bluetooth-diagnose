CC=g++
CXXFLAGS=-std=c++17 -O2 -Wall -Wextra -Wpedantic
INCLUDES=`pkg-config --cflags dbus-1`
LDFLAGS=`pkg-config --libs dbus-1` -lglog -lbpf -pthread

SRC=src/server.cpp src/dbus_service.cpp src/looper.cpp src/net_iface.cpp src/serializer.cpp src/net_info.cpp src/weak_netmgr.cpp src/iface_type.cpp src/using_iface.cpp src/net_ping.cpp src/rtt_monitor.cpp src/rssi_monitor.cpp src/net_wifiriss.cpp src/net_tcp.cpp src/tcp_loss_monitor.cpp src/event_manager.cpp src/logger.cpp src/traffic_analyzer.cpp src/net_traffic.cpp src/traffic_anomaly_detector.cpp src/network_quality_assessor.cpp src/bt_monitor.cpp src/jitter_monitor.cpp src/band_conflict_detector.cpp src/bt_audio_analyzer.cpp src/bt_audio_fusion.cpp src/tcp_retransmit_monitor.cpp src/dns_monitor.cpp src/main.cpp
INC=include/common.hpp include/net_iface.h include/serializer.hpp include/server.hpp include/net_tcp.h include/tcp_loss_monitor.hpp include/weak_netmgr.hpp include/net_info.hpp include/rtt_monitor.hpp include/rssi_monitor.hpp include/dbus_service.hpp include/looper.hpp include/event_manager.hpp include/logger.hpp include/traffic_analyzer.hpp include/network_quality_assessor.hpp include/bt_monitor.hpp include/jitter_monitor.hpp include/bt_audio_analyzer.hpp include/bt_audio_fusion.hpp include/iface_type.hpp include/tcp_retransmit_monitor.hpp include/dns_monitor.hpp

BIN_DIR=bin
BUILD_DIR=build
TARGET=$(BIN_DIR)/weaknet-dbus-server
BPF_TARGET=$(BUILD_DIR)/flow_rate.bpf.o
BPF_BT_TARGET=$(BUILD_DIR)/a2dp_media.bpf.o
BPF_TCP_RETRANS_TARGET=$(BUILD_DIR)/tcp_retransmit.bpf.o
BPF_DNS_TARGET=$(BUILD_DIR)/dns_monitor.bpf.o

all: dirs $(BPF_TARGET) $(BPF_BT_TARGET) $(BPF_TCP_RETRANS_TARGET) $(BPF_DNS_TARGET) $(TARGET)

dirs:
	mkdir -p $(BIN_DIR) $(BUILD_DIR)

# 自动生成 vmlinux.h（需要 bpftool 和 /sys/kernel/btf/vmlinux）
$(BUILD_DIR)/vmlinux.h:
	@if [ -f $@ ]; then \
		echo "vmlinux.h already exists, skipping generation"; \
	elif [ -f /sys/kernel/btf/vmlinux ]; then \
		bpftool btf dump file /sys/kernel/btf/vmlinux format c > $@; \
		echo "Generated vmlinux.h"; \
	else \
		echo "Warning: /sys/kernel/btf/vmlinux not found, vmlinux.h not generated"; \
		echo "You may need to: sudo apt install linux-tools-generic bpftool"; \
	fi

$(BPF_TARGET): src/flow_rate.bpf.c $(BUILD_DIR)/vmlinux.h
	clang -g -O2 -target bpf -I$(BUILD_DIR) -c $< -o $@

$(BPF_BT_TARGET): src/bpf/a2dp_media.bpf.c $(BUILD_DIR)/vmlinux.h
	clang -g -O2 -target bpf -I$(BUILD_DIR) -c $< -o $@

$(BPF_TCP_RETRANS_TARGET): src/tcp_retransmit.bpf.c $(BUILD_DIR)/vmlinux.h
	clang -g -O2 -target bpf -I$(BUILD_DIR) -c $< -o $@

$(BPF_DNS_TARGET): src/dns_monitor.bpf.c $(BUILD_DIR)/vmlinux.h
	clang -g -O2 -target bpf -I$(BUILD_DIR) -c $< -o $@

$(TARGET): $(SRC) $(INC) $(BPF_TARGET) $(BPF_BT_TARGET) $(BPF_TCP_RETRANS_TARGET) $(BPF_DNS_TARGET)
	$(CC) $(CXXFLAGS) $(INCLUDES) -Iinclude -o $@ $(SRC) $(LDFLAGS)

.PHONY: clean run test test-unit test-integration test-ebpf test-all coverage test-clean

clean:
	rm -rf $(BIN_DIR) $(BUILD_DIR)

run: $(TARGET)
	DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS $(TARGET)

# ============== 测试目标 ==============
TEST_DIR = test
TEST_BIN_DIR = $(TEST_DIR)/bin

# 测试编译参数（复用主项目参数，增加测试头文件目录）
TEST_CXXFLAGS = $(CXXFLAGS) -Iinclude -I$(TEST_DIR)/unit
TEST_LDFLAGS = -lglog $(INCLUDES) `pkg-config --libs dbus-1` -pthread
TEST_LDFLAGS_BPF = $(TEST_LDFLAGS) -lbpf

# 单元测试列表（含现有 test_net_info 与新增 6 个算法测试）
UNIT_TESTS = test_net_info test_quality_assessor test_anomaly_detector \
             test_audio_fusion test_band_conflict test_serializer \
             test_event_manager test_bt_full_link test_iface_type \
             test_logger test_traffic_analyzer
# 专项测试（蓝牙监控纯逻辑层，无需硬件）
SPECIAL_TESTS = test_bt_monitor
ALL_TESTS = $(UNIT_TESTS) $(SPECIAL_TESTS)

# ---- 仅编译所有单元测试（不运行，供自动化脚本使用） ----
test-build: $(addprefix $(TEST_BIN_DIR)/, $(ALL_TESTS))
	@echo "✅ 单元测试编译完成"

# ---- 编译并运行所有单元测试 ----
test-unit: test-build
	@for bin in $(addprefix $(TEST_BIN_DIR)/, $(ALL_TESTS)); do \
		echo "========== 运行 $$bin =========="; \
		./$$bin || exit 1; \
	done
	@echo "✅ 所有单元测试通过"

# ---- 集成测试（依赖服务端运行） ----
test-integration:
	bash $(TEST_DIR)/integration_test.sh

# ---- eBPF 专项测试（需 root） ----
test-ebpf: $(TEST_DIR)/test_ebpf
	sudo $(TEST_DIR)/test_ebpf

# eBPF 测试编译规则（复用 run_all_tests.sh 中的编译命令）
$(TEST_DIR)/test_ebpf: $(TEST_DIR)/test_ebpf.cpp \
    src/net_traffic.cpp src/traffic_analyzer.cpp src/traffic_anomaly_detector.cpp \
    src/serializer.cpp src/net_info.cpp src/logger.cpp
	$(CC) $(TEST_CXXFLAGS) -o $@ $(TEST_DIR)/test_ebpf.cpp \
	    src/net_traffic.cpp src/traffic_analyzer.cpp src/traffic_anomaly_detector.cpp \
	    src/serializer.cpp src/net_info.cpp src/logger.cpp \
	    $(TEST_LDFLAGS_BPF)

# ---- 全部测试（单元 + 自动化脚本） ----
test-all: test-unit
	bash $(TEST_DIR)/run_all_tests.sh

# ---- 覆盖率报告 ----
coverage: CXXFLAGS += --coverage -fprofile-arcs -ftest-coverage
coverage: test-unit
	lcov --capture --directory . --output-file coverage.info --no-external 2>/dev/null || true
	genhtml coverage.info -o $(TEST_DIR)/reports/coverage 2>/dev/null || \
		echo "[coverage] 需安装 lcov/genhtml: sudo yum install lcov"
	@echo "覆盖率报告: $(TEST_DIR)/reports/coverage/index.html"

# ---- 清理测试产物 ----
test-clean:
	rm -rf $(TEST_BIN_DIR) $(TEST_DIR)/reports/coverage coverage.info *.gcno *.gcda

# ============================================================================
# 单元测试编译规则（每个测试显式声明源码依赖）
# ============================================================================

# test_net_info（现有测试，无 glog 依赖，使用 assert 风格）
$(TEST_BIN_DIR)/test_net_info: $(TEST_DIR)/test_net_info.cpp \
    src/net_info.cpp src/serializer.cpp \
    include/net_info.hpp include/serializer.hpp
	@mkdir -p $(TEST_BIN_DIR)
	$(CC) $(TEST_CXXFLAGS) -o $@ $(TEST_DIR)/test_net_info.cpp src/net_info.cpp src/serializer.cpp

# test_quality_assessor（网络质量加权评分算法）
$(TEST_BIN_DIR)/test_quality_assessor: $(TEST_DIR)/unit/test_quality_assessor.cpp \
    src/network_quality_assessor.cpp src/net_info.cpp src/serializer.cpp src/logger.cpp \
    $(TEST_DIR)/unit/test_common.hpp
	@mkdir -p $(TEST_BIN_DIR)
	$(CC) $(TEST_CXXFLAGS) -o $@ $(TEST_DIR)/unit/test_quality_assessor.cpp \
	    src/network_quality_assessor.cpp src/net_info.cpp src/serializer.cpp src/logger.cpp $(TEST_LDFLAGS)

# test_anomaly_detector（流量异常检测：数据泄露/可疑连接/时序异常）
$(TEST_BIN_DIR)/test_anomaly_detector: $(TEST_DIR)/unit/test_anomaly_detector.cpp \
    src/traffic_anomaly_detector.cpp src/logger.cpp \
    $(TEST_DIR)/unit/test_common.hpp
	@mkdir -p $(TEST_BIN_DIR)
	$(CC) $(TEST_CXXFLAGS) -o $@ $(TEST_DIR)/unit/test_anomaly_detector.cpp \
	    src/traffic_anomaly_detector.cpp src/logger.cpp $(TEST_LDFLAGS)

# test_audio_fusion（蓝牙音频 D-Bus + eBPF 融合评分）
$(TEST_BIN_DIR)/test_audio_fusion: $(TEST_DIR)/unit/test_audio_fusion.cpp \
    src/bt_audio_fusion.cpp src/logger.cpp \
    $(TEST_DIR)/unit/test_common.hpp
	@mkdir -p $(TEST_BIN_DIR)
	$(CC) $(TEST_CXXFLAGS) -o $@ $(TEST_DIR)/unit/test_audio_fusion.cpp \
	    src/bt_audio_fusion.cpp src/logger.cpp $(TEST_LDFLAGS)

# test_band_conflict（2.4GHz 频段冲突检测：Wi-Fi/蓝牙 RSSI 相关性）
$(TEST_BIN_DIR)/test_band_conflict: $(TEST_DIR)/unit/test_band_conflict.cpp \
    src/band_conflict_detector.cpp src/logger.cpp \
    $(TEST_DIR)/unit/test_common.hpp
	@mkdir -p $(TEST_BIN_DIR)
	$(CC) $(TEST_CXXFLAGS) -o $@ $(TEST_DIR)/unit/test_band_conflict.cpp \
	    src/band_conflict_detector.cpp src/logger.cpp $(TEST_LDFLAGS)

# test_serializer（序列化工具：字符串/int32/文件）
$(TEST_BIN_DIR)/test_serializer: $(TEST_DIR)/unit/test_serializer.cpp \
    src/serializer.cpp src/logger.cpp \
    $(TEST_DIR)/unit/test_common.hpp
	@mkdir -p $(TEST_BIN_DIR)
	$(CC) $(TEST_CXXFLAGS) -o $@ $(TEST_DIR)/unit/test_serializer.cpp \
	    src/serializer.cpp src/logger.cpp $(TEST_LDFLAGS)

# test_event_manager（事件注册/分发/注销）
# 注意：event_manager.cpp 引用 DbusService::emitSpecificSignal，使用 mock 桩避免链接整个服务端
$(TEST_BIN_DIR)/test_event_manager: $(TEST_DIR)/unit/test_event_manager.cpp \
    src/event_manager.cpp $(TEST_DIR)/unit/mock_dbus_service.cpp src/logger.cpp \
    $(TEST_DIR)/unit/test_common.hpp
	@mkdir -p $(TEST_BIN_DIR)
	$(CC) $(TEST_CXXFLAGS) -o $@ $(TEST_DIR)/unit/test_event_manager.cpp \
	    src/event_manager.cpp $(TEST_DIR)/unit/mock_dbus_service.cpp src/logger.cpp $(TEST_LDFLAGS)

# test_bt_full_link（蓝牙全链路集成测试：频段冲突→EventManager→D-Bus 信号载荷）
# 链接 BandConflictDetector + NetworkEventManager + DbusService 桩，验证端到端路由
$(TEST_BIN_DIR)/test_bt_full_link: $(TEST_DIR)/unit/test_bt_full_link.cpp \
    src/band_conflict_detector.cpp src/event_manager.cpp \
    $(TEST_DIR)/unit/mock_dbus_service.cpp src/logger.cpp \
    $(TEST_DIR)/unit/test_common.hpp
	@mkdir -p $(TEST_BIN_DIR)
	$(CC) $(TEST_CXXFLAGS) -o $@ $(TEST_DIR)/unit/test_bt_full_link.cpp \
	    src/band_conflict_detector.cpp src/event_manager.cpp \
	    $(TEST_DIR)/unit/mock_dbus_service.cpp src/logger.cpp $(TEST_LDFLAGS)

# test_bt_monitor（蓝牙监控纯逻辑层 + 降级场景，专项测试）
# 依赖 event_manager.cpp（bt_monitor.cpp 引用 getEventManager）与 mock 桩
$(TEST_BIN_DIR)/test_bt_monitor: $(TEST_DIR)/special/test_bt_monitor.cpp \
    src/bt_monitor.cpp src/bt_audio_analyzer.cpp src/bt_audio_fusion.cpp \
    src/event_manager.cpp $(TEST_DIR)/unit/mock_dbus_service.cpp src/logger.cpp \
    $(TEST_DIR)/unit/test_common.hpp
	@mkdir -p $(TEST_BIN_DIR)
	$(CC) $(TEST_CXXFLAGS) -o $@ $(TEST_DIR)/special/test_bt_monitor.cpp \
	    src/bt_monitor.cpp src/bt_audio_analyzer.cpp src/bt_audio_fusion.cpp \
	    src/event_manager.cpp $(TEST_DIR)/unit/mock_dbus_service.cpp src/logger.cpp $(TEST_LDFLAGS_BPF)

# test_iface_type（Wi-Fi 接口类型识别：wireless 目录 / phy80211 符号链接 / 接口名前缀后备）
$(TEST_BIN_DIR)/test_iface_type: $(TEST_DIR)/unit/test_iface_type.cpp \
    src/iface_type.cpp include/iface_type.hpp \
    $(TEST_DIR)/unit/test_common.hpp
	@mkdir -p $(TEST_BIN_DIR)
	$(CC) $(TEST_CXXFLAGS) -o $@ $(TEST_DIR)/unit/test_iface_type.cpp \
	    src/iface_type.cpp $(TEST_LDFLAGS)

# test_logger（日志系统：格式化宏、磁盘满保护、日志清理）
$(TEST_BIN_DIR)/test_logger: $(TEST_DIR)/unit/test_logger.cpp \
    src/logger.cpp include/logger.hpp \
    $(TEST_DIR)/unit/test_common.hpp
	@mkdir -p $(TEST_BIN_DIR)
	$(CC) $(TEST_CXXFLAGS) -o $@ $(TEST_DIR)/unit/test_logger.cpp \
	    src/logger.cpp $(TEST_LDFLAGS)

# test_traffic_analyzer（流量分析器降级模式）
$(TEST_BIN_DIR)/test_traffic_analyzer: $(TEST_DIR)/unit/test_traffic_analyzer.cpp \
    src/traffic_analyzer.cpp src/net_traffic.cpp src/logger.cpp \
    include/traffic_analyzer.hpp include/net_traffic.h include/logger.hpp \
    $(TEST_DIR)/unit/test_common.hpp
	@mkdir -p $(TEST_BIN_DIR)
	$(CC) $(TEST_CXXFLAGS) -o $@ $(TEST_DIR)/unit/test_traffic_analyzer.cpp \
	    src/traffic_analyzer.cpp src/net_traffic.cpp src/logger.cpp $(TEST_LDFLAGS_BPF)


