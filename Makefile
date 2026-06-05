# 包含配置文件
include config.mk

CC=g++
CXXFLAGS=-std=c++17 -O2 -Wall -Wextra -Wpedantic
LDFLAGS=`pkg-config --libs dbus-1` -lglog
INCLUDES=`pkg-config --cflags dbus-1`

SRC_SERVER=server.cpp serializer.cpp
SRC_CLIENT_LIB=client/client.cpp server/src/serializer.cpp
SRC_CLIENT_TEST=client/test_client.cpp

all: dirs server-client-lib

server-client-lib: client-lib

# 单独编译服务端
server-bin:
	@$(MAKE) -C server

# 单独编译客户端动态库和测试程序（不依赖服务端编译）
client-lib: dirs
	@echo "编译WeakNet客户端动态库..."
	@mkdir -p $(CLIENT_LIB_DIR) $(CLIENT_BIN_DIR)
	$(CC) $(CXXFLAGS) $(INCLUDES) -I$(SERVER_DIR)/include -I$(CLIENT_DIR) -fPIC -shared -o $(CLIENT_LIB_DIR)/libweaknet.so $(SRC_CLIENT_LIB) $(LDFLAGS)
	@echo "编译WeakNet客户端测试程序..."
	$(CC) $(CXXFLAGS) $(INCLUDES) -I$(SERVER_DIR)/include -I$(CLIENT_DIR) -o $(CLIENT_BIN_DIR)/test-client $(SRC_CLIENT_TEST) -L$(CLIENT_LIB_DIR) -lweaknet $(LDFLAGS)

# 支持原来的命名，保持兼容性
server-client: server-client-lib

dirs:
	mkdir -p $(BUILD_DIR) $(BIN_DIR) $(CLIENT_BIN_DIR) $(CLIENT_LIB_DIR)

.PHONY: clean run-server run-client

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR) $(CLIENT_BIN_DIR) $(CLIENT_LIB_DIR)
	rm -f *.bin

run-server: $(BIN_DIR)/weaknet-dbus-server
	DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS $(BIN_DIR)/weaknet-dbus-server

test-client: client-lib
	@if [ "$(COMMAND)" = "" ]; then \
		echo "用法: make test-client COMMAND=[all|get|health|file|ping|check|events|bt-devices|bt-adapter|bt-events|event-types|test-*]"; \
		echo "示例: make test-client COMMAND=all"; \
		echo "      make test-client COMMAND=get"; \
		echo "      make test-client COMMAND=ping google.com"; \
		echo "      make test-client COMMAND=bt-devices"; \
		echo "      make test-client COMMAND=bt-adapter"; \
		echo "      make test-client COMMAND=bt-events"; \
		echo "      make test-client COMMAND=test-bt"; \
		echo "      make test-client COMMAND=test-basic"; \
	else \
		echo "运行客户端测试程序: $(COMMAND)"; \
		LD_LIBRARY_PATH=$(CLIENT_LIB_DIR):$$LD_LIBRARY_PATH DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS $(CLIENT_BIN_DIR)/test-client $(COMMAND); \
	fi

test-lib: client-lib
	@echo "运行动态库基本功能测试..."
	LD_LIBRARY_PATH=$(CLIENT_LIB_DIR):$$LD_LIBRARY_PATH DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS $(CLIENT_BIN_DIR)/test-client lib-test

test-events: client-lib
	@echo "运行事件监听功能测试..."
	LD_LIBRARY_PATH=$(CLIENT_LIB_DIR):$$LD_LIBRARY_PATH DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS $(CLIENT_BIN_DIR)/test-client test-events

test-all: client-lib
	@echo "运行完整接口验证测试..."
	LD_LIBRARY_PATH=$(CLIENT_LIB_DIR):$$LD_LIBRARY_PATH DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS $(CLIENT_BIN_DIR)/test-client all

test-ping: client-lib
	@echo "运行Ping功能测试..."
	LD_LIBRARY_PATH=$(CLIENT_LIB_DIR):$$LD_LIBRARY_PATH DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS $(CLIENT_BIN_DIR)/test-client test-ping

test-performance: client-lib
	@echo "运行性能测试..."
	LD_LIBRARY_PATH=$(CLIENT_LIB_DIR):$$LD_LIBRARY_PATH DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS $(CLIENT_BIN_DIR)/test-client test-performance

run-client: client-lib
	@echo "运行客户端订阅模式..."
	LD_LIBRARY_PATH=$(CLIENT_LIB_DIR):$$LD_LIBRARY_PATH DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS $(CLIENT_BIN_DIR)/test-client subscribe

test-bt: client-lib
	@echo "运行蓝牙功能测试..."
	LD_LIBRARY_PATH=$(CLIENT_LIB_DIR):$$LD_LIBRARY_PATH DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS $(CLIENT_BIN_DIR)/test-client test-bt

test-bt-callback: client-lib
	@echo "运行蓝牙事件回调测试..."
	LD_LIBRARY_PATH=$(CLIENT_LIB_DIR):$$LD_LIBRARY_PATH DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS $(CLIENT_BIN_DIR)/test-client test-bt-callback

test-bt-devices: client-lib
	@echo "获取蓝牙设备列表..."
	LD_LIBRARY_PATH=$(CLIENT_LIB_DIR):$$LD_LIBRARY_PATH DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS $(CLIENT_BIN_DIR)/test-client bt-devices

test-bt-adapter: client-lib
	@echo "获取蓝牙适配器信息..."
	LD_LIBRARY_PATH=$(CLIENT_LIB_DIR):$$LD_LIBRARY_PATH DBUS_SESSION_BUS_ADDRESS=$$DBUS_SESSION_BUS_ADDRESS $(CLIENT_BIN_DIR)/test-client bt-adapter



