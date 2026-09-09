"""
weaknet_bridge.py
通过 Python 标准库 ctypes 直接加载 libweaknet.so，对外提供线程安全的 WeakNet C API 封装。
"""

import ctypes
import json
import logging
import os
import threading
from typing import Any, Dict, List, Optional

logger = logging.getLogger("weaknet.bridge")


class WeakNetBridge:
    _instance: Optional["WeakNetBridge"] = None
    _lock = threading.Lock()

    def __init__(self, lib_path: Optional[str] = None):
        self._mutex = threading.Lock()
        self._lib = None
        self._connected = False

        resolved_path = self._resolve_library_path(lib_path)
        if not resolved_path:
            logger.warning("libweaknet.so not found, bridge running in mock/offline mode")
            return

        try:
            self._lib = ctypes.CDLL(resolved_path)
            self._bind_c_functions()
            self._init_client()
            logger.info("Successfully bound libweaknet.so from: %s", resolved_path)
        except Exception as e:
            logger.error("Failed to load or bind libweaknet.so: %s", e)
            self._lib = None

    @classmethod
    def get_instance(cls, lib_path: Optional[str] = None) -> "WeakNetBridge":
        with cls._lock:
            if cls._instance is None:
                cls._instance = cls(lib_path)
            return cls._instance

    def _resolve_library_path(self, custom_path: Optional[str]) -> Optional[str]:
        candidates = []
        if custom_path:
            candidates.append(custom_path)

        env_lib = os.getenv("LIBWEAKNET_PATH")
        if env_lib:
            candidates.append(env_lib)

        # 优先查找当前工作目录与开发板标准部署路径
        import platform
        arch = platform.machine()
        if arch in ["aarch64", "arm64"]:
            candidates.extend([
                "/home/radxa/weaknet/client/lib/libweaknet.so",
                os.path.abspath("./dist-arm64/client/lib/libweaknet.so"),
                os.path.abspath("./build-arm64/client/lib/libweaknet.so"),
            ])
        else:
            candidates.extend([
                os.path.abspath("./build-x86/client/lib/libweaknet.so"),
                os.path.abspath("../build-x86/client/lib/libweaknet.so"),
            ])

        candidates.extend([
            "/home/radxa/weaknet/client/lib/libweaknet.so",
            os.path.abspath("./build-x86/client/lib/libweaknet.so"),
            os.path.abspath("./build-arm64/client/lib/libweaknet.so"),
            "/usr/local/lib/libweaknet.so",
            "/usr/lib/libweaknet.so",
        ])

        for path in candidates:
            if os.path.exists(path):
                return path
        return None

    def _bind_c_functions(self):
        lib = self._lib
        # bool weaknet_init()
        lib.weaknet_init.restype = ctypes.c_bool
        # void weaknet_cleanup()
        lib.weaknet_cleanup.restype = None
        # bool weaknet_is_connected()
        lib.weaknet_is_connected.restype = ctypes.c_bool

        # bool weaknet_get_interfaces(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size)
        lib.weaknet_get_interfaces.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.weaknet_get_interfaces.restype = ctypes.c_bool

        # bool weaknet_health_check(char* result_buffer, size_t result_size, char* error_buffer, size_t error_size)
        lib.weaknet_health_check.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.weaknet_health_check.restype = ctypes.c_bool

        # bool weaknet_get_ebpf_monitor_health(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size)
        lib.weaknet_get_ebpf_monitor_health.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.weaknet_get_ebpf_monitor_health.restype = ctypes.c_bool

        # bool weaknet_get_skb_drop_stats(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size)
        lib.weaknet_get_skb_drop_stats.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.weaknet_get_skb_drop_stats.restype = ctypes.c_bool

        # bool weaknet_get_bluetooth_devices(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size)
        lib.weaknet_get_bluetooth_devices.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.weaknet_get_bluetooth_devices.restype = ctypes.c_bool

        # bool weaknet_get_bluetooth_adapter(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size)
        lib.weaknet_get_bluetooth_adapter.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.weaknet_get_bluetooth_adapter.restype = ctypes.c_bool

        # bool weaknet_get_coexistence_conflict(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size)
        lib.weaknet_get_coexistence_conflict.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.weaknet_get_coexistence_conflict.restype = ctypes.c_bool

        # bool weaknet_list_monitors(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size)
        lib.weaknet_list_monitors.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.weaknet_list_monitors.restype = ctypes.c_bool

        # bool weaknet_restart_monitor(const char* name, char* buffer, size_t buffer_size, char* error_buffer, size_t error_size)
        lib.weaknet_restart_monitor.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.weaknet_restart_monitor.restype = ctypes.c_bool

        # bool weaknet_enable_monitor(const char* name, char* buffer, size_t buffer_size, char* error_buffer, size_t error_size)
        lib.weaknet_enable_monitor.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.weaknet_enable_monitor.restype = ctypes.c_bool

        # bool weaknet_disable_monitor(const char* name, char* buffer, size_t buffer_size, char* error_buffer, size_t error_size)
        lib.weaknet_disable_monitor.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.weaknet_disable_monitor.restype = ctypes.c_bool

        # bool weaknet_get_dns_stats(...)
        lib.weaknet_get_dns_stats.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.weaknet_get_dns_stats.restype = ctypes.c_bool

        # bool weaknet_get_wifi_loss_stats(...)
        lib.weaknet_get_wifi_loss_stats.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.weaknet_get_wifi_loss_stats.restype = ctypes.c_bool

        # bool weaknet_get_http_latency_stats(...)
        lib.weaknet_get_http_latency_stats.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.weaknet_get_http_latency_stats.restype = ctypes.c_bool

        # bool weaknet_get_process_profiling(...)
        lib.weaknet_get_process_profiling.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
        lib.weaknet_get_process_profiling.restype = ctypes.c_bool

        # bool weaknet_get_monitor_param(const char* monitor, char* buffer, size_t buffer_size, char* error_buffer, size_t error_size)
        if hasattr(lib, "weaknet_get_monitor_param"):
            lib.weaknet_get_monitor_param.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
            lib.weaknet_get_monitor_param.restype = ctypes.c_bool

        # bool weaknet_set_monitor_param(const char* key, const char* value, char* error_buffer, size_t error_size)
        if hasattr(lib, "weaknet_set_monitor_param"):
            lib.weaknet_set_monitor_param.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
            lib.weaknet_set_monitor_param.restype = ctypes.c_bool

        # bool weaknet_save_monitor_overrides(char* buffer, size_t buffer_size, char* error_buffer, size_t error_size)
        if hasattr(lib, "weaknet_save_monitor_overrides"):
            lib.weaknet_save_monitor_overrides.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_size_t]
            lib.weaknet_save_monitor_overrides.restype = ctypes.c_bool

    def _init_client(self):
        if not self._lib:
            return
        try:
            self._connected = bool(self._lib.weaknet_init())
        except Exception as e:
            logger.error("weaknet_init failed: %s", e)
            self._connected = False

    def is_connected(self) -> bool:
        if not self._lib:
            return False
        with self._mutex:
            try:
                return bool(self._lib.weaknet_is_connected())
            except Exception:
                return False

    def _call_string_api(self, func_name: str, buf_size: int = 16384, *extra_args) -> Dict[str, Any]:
        if not self._lib:
            return {"success": False, "error": "libweaknet library not loaded"}

        func = getattr(self._lib, func_name, None)
        if not func:
            return {"success": False, "error": f"Function {func_name} not found in libweaknet.so"}

        buf = ctypes.create_string_buffer(buf_size)
        err = ctypes.create_string_buffer(1024)

        with self._mutex:
            try:
                args = list(extra_args) + [buf, buf_size, err, 1024]
                ok = bool(func(*args))
                if ok:
                    raw_val = buf.value.decode("utf-8", errors="replace")
                    return {"success": True, "data": raw_val}
                else:
                    err_val = err.value.decode("utf-8", errors="replace")
                    return {"success": False, "error": err_val or "Unknown D-Bus error"}
            except Exception as e:
                return {"success": False, "error": str(e)}

    def get_health(self) -> Dict[str, Any]:
        res = self._call_string_api("weaknet_health_check", 4096)
        if res["success"]:
            try:
                raw = json.loads(res["data"])
                # 旧 D-Bus HealthCheck 的 payload 自带 {"success":true,"data":{...}}
                # 这里把内层 data 提升到顶层，避免出现 data.data 嵌套（保持旧契约）
                inner = raw.get("data", raw)
                return {"success": True, "data": inner}
            except Exception:
                return res
        return res

    def get_interfaces(self) -> Dict[str, Any]:
        res = self._call_string_api("weaknet_get_interfaces", 2048)
        if res["success"]:
            # 通常以换行或逗号分隔
            names = [s.strip() for s in res["data"].replace("\n", ",").split(",") if s.strip()]
            return {"success": True, "interfaces": names, "raw": res["data"]}
        return res

    def get_ebpf_health(self) -> Dict[str, Any]:
        res = self._call_string_api("weaknet_get_ebpf_monitor_health", 8192)
        if res["success"]:
            try:
                return {"success": True, "data": json.loads(res["data"])}
            except Exception:
                return res
        return res

    def get_skb_drop_stats(self) -> Dict[str, Any]:
        res = self._call_string_api("weaknet_get_skb_drop_stats", 8192)
        if res["success"]:
            try:
                return {"success": True, "data": json.loads(res["data"])}
            except Exception:
                return res
        return res

    def get_bluetooth_adapter(self) -> Dict[str, Any]:
        return self._call_string_api("weaknet_get_bluetooth_adapter", 2048)

    def get_bluetooth_devices(self) -> Dict[str, Any]:
        res = self._call_string_api("weaknet_get_bluetooth_devices", 8192)
        if res["success"]:
            devices = []
            for line in res["data"].strip().split("\n"):
                if not line.strip():
                    continue
                parts = line.split("|")
                if len(parts) >= 6:
                    devices.append({
                        "mac": parts[0],
                        "name": parts[1],
                        "rssi": int(parts[2]) if parts[2].lstrip("-").isdigit() else -100,
                        "connected": parts[3] == "1",
                        "type": parts[4],
                        "level": parts[5]
                    })
                else:
                    devices.append({"raw": line})
            return {"success": True, "devices": devices}
        return res

    def get_coexistence_conflict(self) -> Dict[str, Any]:
        res = self._call_string_api("weaknet_get_coexistence_conflict", 4096)
        if res["success"]:
            try:
                return {"success": True, "data": json.loads(res["data"])}
            except Exception:
                return res
        return res

    def list_monitors(self) -> Dict[str, Any]:
        res = self._call_string_api("weaknet_list_monitors", 16384)
        if res["success"]:
            try:
                return {"success": True, "monitors": json.loads(res["data"])}
            except Exception:
                return res
        return res

    def _resolve_cli_path(self):
        """定位 weaknet-cli 可执行文件路径，找不到返回 None"""
        cli_candidates = [
            "/home/radxa/weaknet/client/bin/weaknet-cli",
            os.path.abspath("./dist-arm64/client/bin/weaknet-cli"),
            os.path.abspath("./build-x86/client/bin/weaknet_cli"),
            os.path.abspath("../build-x86/client/bin/weaknet_cli"),
            "weaknet-cli"
        ]
        for c in cli_candidates:
            if os.path.exists(c):
                return c
        return None

    def _control_via_cli(self, action: str, name: str) -> Dict[str, Any]:
        """通过 weaknet-cli 子进程安全隔离执行监控器生命周期切换，规避主进程长期连接死锁"""
        import subprocess
        cli_path = self._resolve_cli_path()
        if not cli_path:
            return self._call_string_api(f"weaknet_{action}_monitor", 4096, name.encode("utf-8"))

        try:
            res = subprocess.run([cli_path, "monitor", action, name], capture_output=True, text=True, timeout=8)
            output = res.stdout.strip()
            if res.returncode == 0:
                return {"success": True, "data": output}
            else:
                err = res.stderr.strip() or output
                return {"success": False, "error": err}
        except Exception as e:
            return {"success": False, "error": str(e)}

    def get_monitor_config(self, name: str) -> Dict[str, Any]:
        """查询指定监控器当前运行参数，优先走 weaknet-cli get，回退 ctypes C API。
        返回 {"success": bool, "config": {...}} 或 {"success": False, "error": ...}
        """
        import subprocess
        cli_path = self._resolve_cli_path()
        raw = None
        if cli_path:
            try:
                res = subprocess.run([cli_path, "get", name], capture_output=True, text=True, timeout=8)
                if res.returncode == 0:
                    raw = res.stdout.strip()
                else:
                    return {"success": False, "error": res.stderr.strip() or res.stdout.strip()}
            except Exception as e:
                return {"success": False, "error": str(e)}
        else:
            res = self._call_string_api("weaknet_get_monitor_param", 8192, name.encode("utf-8"))
            if not res.get("success"):
                return {"success": False, "error": res.get("error", "查询配置失败")}
            raw = res.get("data")

        try:
            parsed = json.loads(raw)
            # CLI 输出形如 {"rtt": {...}}，取出该监控器的参数字典
            if isinstance(parsed, dict) and name in parsed and isinstance(parsed[name], dict):
                cfg = parsed[name]
            elif isinstance(parsed, dict):
                cfg = parsed
            else:
                return {"success": False, "error": f"无法解析监控器 '{name}' 的配置输出"}
            return {"success": True, "config": cfg, "monitor": name}
        except Exception as e:
            return {"success": False, "error": f"配置解析失败: {e}", "raw": raw}

    def set_monitor_config(self, key: str, value: str) -> Dict[str, Any]:
        """热设置单个参数键值，优先走 weaknet-cli set，回退 ctypes C API"""
        import subprocess
        cli_path = self._resolve_cli_path()
        if cli_path:
            try:
                res = subprocess.run([cli_path, "set", key, str(value)], capture_output=True, text=True, timeout=8)
                if res.returncode == 0:
                    return {"success": True, "data": res.stdout.strip() or "ok"}
                return {"success": False, "error": res.stderr.strip() or res.stdout.strip() or "Set failed"}
            except Exception as e:
                return {"success": False, "error": str(e)}

        # ctypes 回退：weaknet_set_monitor_param(key, value, err, err_size)
        if not self._lib or not hasattr(self._lib, "weaknet_set_monitor_param"):
            return {"success": False, "error": "libweaknet 未加载或不支持 set_monitor_param"}
        err = ctypes.create_string_buffer(1024)
        with self._mutex:
            try:
                ok = bool(self._lib.weaknet_set_monitor_param(
                    key.encode("utf-8"), str(value).encode("utf-8"), err, 1024))
                if ok:
                    return {"success": True, "data": "ok"}
                return {"success": False, "error": err.value.decode("utf-8", errors="replace") or "Set failed"}
            except Exception as e:
                return {"success": False, "error": str(e)}

    def save_monitor_overrides(self) -> Dict[str, Any]:
        """固化当前内存态参数覆盖到配置文件，优先走 weaknet-cli monitor save"""
        import subprocess
        cli_path = self._resolve_cli_path()
        if cli_path:
            try:
                res = subprocess.run([cli_path, "monitor", "save"], capture_output=True, text=True, timeout=8)
                if res.returncode == 0:
                    return {"success": True, "data": res.stdout.strip() or "saved"}
                return {"success": False, "error": res.stderr.strip() or res.stdout.strip() or "Save failed"}
            except Exception as e:
                return {"success": False, "error": str(e)}

        if not self._lib or not hasattr(self._lib, "weaknet_save_monitor_overrides"):
            return {"success": False, "error": "libweaknet 未加载或不支持 save_monitor_overrides"}
        buf = ctypes.create_string_buffer(4096)
        err = ctypes.create_string_buffer(1024)
        with self._mutex:
            try:
                ok = bool(self._lib.weaknet_save_monitor_overrides(buf, 4096, err, 1024))
                if ok:
                    return {"success": True, "data": buf.value.decode("utf-8", errors="replace")}
                return {"success": False, "error": err.value.decode("utf-8", errors="replace") or "Save failed"}
            except Exception as e:
                return {"success": False, "error": str(e)}

    def restart_monitor(self, name: str) -> Dict[str, Any]:
        return self._control_via_cli("restart", name)

    def enable_monitor(self, name: str) -> Dict[str, Any]:
        return self._control_via_cli("enable", name)

    def disable_monitor(self, name: str) -> Dict[str, Any]:
        return self._control_via_cli("disable", name)

    def get_dns_stats(self) -> Dict[str, Any]:
        return self._call_string_api("weaknet_get_dns_stats", 4096)

    def get_wifi_loss_stats(self) -> Dict[str, Any]:
        return self._call_string_api("weaknet_get_wifi_loss_stats", 4096)

    def get_http_latency_stats(self) -> Dict[str, Any]:
        return self._call_string_api("weaknet_get_http_latency_stats", 4096)

    def get_process_profiling(self) -> Dict[str, Any]:
        return self._call_string_api("weaknet_get_process_profiling", 8192)
