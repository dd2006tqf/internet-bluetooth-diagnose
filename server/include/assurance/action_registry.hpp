#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <regex>
#include <arpa/inet.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <chrono>
#include <cstring>
#include <cerrno>

extern char** environ;

namespace weaknet {

struct ActionParamSpec {
    std::string name;
    std::string type; // "string", "integer", "ipv4_address"
    bool required{true};
    std::vector<std::string> allowed_values;
};

struct ActionDef {
    std::string action_id;
    std::string description;
    std::vector<ActionParamSpec> param_specs;
    std::string executable;              // 严禁为空，严格走 posix_spawn / execve
    std::vector<std::string> args_template; // e.g. ["@{resolver}", "www.baidu.com"]
};

struct ExecSpec {
    std::string executable;
    std::vector<std::string> argv; // [executable, arg1, arg2, ...]
};

struct ExecutionResult {
    int exit_code{-1};
    std::string stdout_output;
    std::string stderr_output;
    bool timed_out{false};
    std::string error;
};

struct ActionValidationResult {
    bool ok{false};
    std::string error;
};

class ActionRegistry {
public:
    ActionRegistry() {
        registerDefaultActions();
    }

    void registerAction(ActionDef def) {
        actions_[def.action_id] = std::move(def);
    }

    bool hasAction(const std::string& action_id) const {
        return actions_.find(action_id) != actions_.end();
    }

    const ActionDef* getAction(const std::string& action_id) const {
        auto it = actions_.find(action_id);
        return (it != actions_.end()) ? &it->second : nullptr;
    }

    ActionValidationResult validate(const std::string& action_id,
                                    const std::map<std::string, std::string>& params) const {
        const auto* def = getAction(action_id);
        if (!def) {
            return {false, "Unknown action_id: " + action_id};
        }

        // 校验必填参数及参数类型
        for (const auto& spec : def->param_specs) {
            auto it = params.find(spec.name);
            if (it == params.end() || it->second.empty()) {
                if (spec.required) {
                    return {false, "Missing required parameter: " + spec.name + " for action " + action_id};
                }
                continue;
            }

            const std::string& val = it->second;

            // 白名单校验
            if (!spec.allowed_values.empty()) {
                bool found = false;
                for (const auto& allowed : spec.allowed_values) {
                    if (val == allowed) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    return {false, "Parameter '" + spec.name + "' value '" + val + "' is not in allowed list for " + action_id};
                }
            }

            // 类型校验（彻底杜绝 shell 注入与恶意输入）
            if (spec.type == "ipv4_address") {
                struct in_addr sa;
                if (inet_pton(AF_INET, val.c_str(), &sa) != 1) {
                    return {false, "Parameter '" + spec.name + "' is not a valid IPv4 address: " + val};
                }
            } else if (spec.type == "integer") {
                try {
                    size_t idx = 0;
                    std::stoi(val, &idx);
                    if (idx != val.size()) {
                        return {false, "Parameter '" + spec.name + "' is not a valid integer: " + val};
                    }
                } catch (...) {
                    return {false, "Parameter '" + spec.name + "' is not a valid integer: " + val};
                }
            }
        }

        return {true, ""};
    }

    std::optional<ExecSpec> buildExecSpec(const std::string& action_id,
                                          const std::map<std::string, std::string>& params) const {
        auto val_res = validate(action_id, params);
        if (!val_res.ok) {
            return std::nullopt;
        }

        const auto* def = getAction(action_id);
        ExecSpec spec;
        spec.executable = def->executable;
        spec.argv.push_back(def->executable); // argv[0]

        for (const auto& arg_tmpl : def->args_template) {
            std::string arg = arg_tmpl;
            for (const auto& [k, v] : params) {
                std::string placeholder = "@{" + k + "}";
                size_t pos = 0;
                while ((pos = arg.find(placeholder, pos)) != std::string::npos) {
                    arg.replace(pos, placeholder.length(), v);
                    pos += v.length();
                }
            }
            spec.argv.push_back(std::move(arg));
        }

        return spec;
    }

    /**
     * @brief 严格通过 posix_spawn / execve 数组直接调用外部命令
     * 绝不经过任何 shell，彻底消除 shell 元字符与命令注入隐患。
     * 使用 poll() 并行异步读取 stdout 与 stderr 管道，避免死锁并实施超时强杀机制。
     */
    static ExecutionResult safeExec(const ExecSpec& spec, int timeout_seconds = 5) {
        ExecutionResult result;
        if (spec.executable.empty() || spec.argv.empty()) {
            result.error = "Executable or argv empty";
            return result;
        }

        int out_pipe[2];
        int err_pipe[2];
        if (pipe(out_pipe) < 0) {
            result.error = "Failed to create stdout pipe";
            return result;
        }
        if (pipe(err_pipe) < 0) {
            close(out_pipe[0]);
            close(out_pipe[1]);
            result.error = "Failed to create stderr pipe";
            return result;
        }

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addclose(&actions, out_pipe[0]);
        posix_spawn_file_actions_adddup2(&actions, out_pipe[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, out_pipe[1]);

        posix_spawn_file_actions_addclose(&actions, err_pipe[0]);
        posix_spawn_file_actions_adddup2(&actions, err_pipe[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, err_pipe[1]);

        // 构建 argv 原始指针数组
        std::vector<char*> c_argv;
        for (const auto& arg : spec.argv) {
            c_argv.push_back(const_cast<char*>(arg.c_str()));
        }
        c_argv.push_back(nullptr);

        pid_t pid = 0;
        int ret = posix_spawn(&pid, spec.executable.c_str(), &actions, nullptr, c_argv.data(), ::environ);
        posix_spawn_file_actions_destroy(&actions);

        // 父进程关闭写端
        close(out_pipe[1]);
        close(err_pipe[1]);

        if (ret != 0) {
            close(out_pipe[0]);
            close(err_pipe[0]);
            result.error = "posix_spawn failed with error: " + std::to_string(ret);
            return result;
        }

        // 将读端设置为非阻塞，防止 read() 阻塞
        auto set_nonblocking = [](int fd) {
            int flags = fcntl(fd, F_GETFL, 0);
            if (flags >= 0) {
                fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            }
        };
        set_nonblocking(out_pipe[0]);
        set_nonblocking(err_pipe[0]);

        struct pollfd pfd[2];
        pfd[0].fd = out_pipe[0];
        pfd[0].events = POLLIN;
        pfd[1].fd = err_pipe[0];
        pfd[1].events = POLLIN;

        int open_fds = 2;
        auto start_time = std::chrono::steady_clock::now();
        const auto timeout_duration = std::chrono::seconds(std::max(1, timeout_seconds));

        while (open_fds > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time);
            auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(timeout_duration);
            int remaining_ms = static_cast<int>(total_ms.count() - elapsed.count());

            if (remaining_ms <= 0) {
                // 超时强制杀死子进程
                kill(pid, SIGKILL);
                result.timed_out = true;
                result.error = "Execution timed out after " + std::to_string(timeout_seconds) + " seconds";
                break;
            }

            int pr = poll(pfd, 2, std::min(remaining_ms, 200));
            if (pr < 0) {
                if (errno == EINTR) continue;
                result.error = "poll failed: " + std::string(strerror(errno));
                kill(pid, SIGKILL);
                break;
            }

            // 读取 stdout
            if (pfd[0].fd >= 0 && (pfd[0].revents & (POLLIN | POLLHUP | POLLERR))) {
                char buf[512];
                ssize_t n = 0;
                while ((n = read(pfd[0].fd, buf, sizeof(buf))) > 0) {
                    result.stdout_output.append(buf, n);
                }
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    close(pfd[0].fd);
                    pfd[0].fd = -1;
                    open_fds--;
                }
            }

            // 读取 stderr
            if (pfd[1].fd >= 0 && (pfd[1].revents & (POLLIN | POLLHUP | POLLERR))) {
                char buf[512];
                ssize_t n = 0;
                while ((n = read(pfd[1].fd, buf, sizeof(buf))) > 0) {
                    result.stderr_output.append(buf, n);
                }
                if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                    close(pfd[1].fd);
                    pfd[1].fd = -1;
                    open_fds--;
                }
            }
        }

        if (pfd[0].fd >= 0) close(pfd[0].fd);
        if (pfd[1].fd >= 0) close(pfd[1].fd);

        int status = 0;
        waitpid(pid, &status, 0);
        if (WIFEXITED(status)) {
            result.exit_code = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            result.exit_code = 128 + WTERMSIG(status);
        }

        return result;
    }

private:
    void registerDefaultActions() {
        // 1. 查看 DNS nameserver 配置
        registerAction({
            "CHECK_RESOLVER_CONFIG",
            "查看本地 /etc/resolv.conf 配置文件",
            {},
            "/bin/cat",
            {"/etc/resolv.conf"}
        });

        // 2. 直连公共解析器探针
        registerAction({
            "PROBE_PUBLIC_RESOLVER",
            "直连测试公共 DNS 解析器连通性",
            {
                {"resolver", "ipv4_address", true, {"223.5.5.5", "119.29.29.29", "8.8.8.8", "114.114.114.114"}}
            },
            "/usr/bin/dig",
            {"@{resolver}", "www.baidu.com", "+time=2", "+tries=1"}
        });

        // 3. 检查默认路由网关
        registerAction({
            "INSPECT_DEFAULT_GATEWAY",
            "检查系统当前默认路由及下一跳网关",
            {},
            "/sbin/ip",
            {"route", "show", "default"}
        });

        // 4. 重启特定网络接口
        registerAction({
            "RESTART_NETWORK_INTERFACE",
            "重新刷新本地网络接口状态",
            {
                {"interface", "string", true, {"wlan0", "eth0"}}
            },
            "/sbin/ip",
            {"link", "set", "@{interface}", "up"}
        });
    }

    std::map<std::string, ActionDef> actions_;
};

} // namespace weaknet
