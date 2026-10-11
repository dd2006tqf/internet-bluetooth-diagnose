#include "assurance/edge_ai_scheduler.hpp"
#include "assurance/legacy_adapter.hpp"
#include "logger.hpp"

#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace weaknet {

namespace {

// 轻量提取 JSON 顶层或嵌套字段（专为解析 edge_network_rag.py 标准输出设计）
std::string extractJsonStringField(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return "";
    size_t colon = json.find(':', pos + pattern.size());
    if (colon == std::string::npos) return "";
    size_t q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos) return "";
    std::string out;
    for (size_t i = q1 + 1; i < json.size(); ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) {
            char next = json[i + 1];
            if (next == 'n') out.push_back('\n');
            else if (next == 't') out.push_back('\t');
            else out.push_back(next);
            ++i;
        } else if (json[i] == '"') {
            return out;
        } else {
            out.push_back(json[i]);
        }
    }
    return "";
}

std::vector<std::string> extractJsonStringArray(const std::string& json, const std::string& key) {
    std::vector<std::string> res;
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) return res;
    size_t open_b = json.find('[', pos + pattern.size());
    if (open_b == std::string::npos) return res;
    size_t close_b = json.find(']', open_b + 1);
    if (close_b == std::string::npos) return res;

    std::string slice = json.substr(open_b + 1, close_b - open_b - 1);
    size_t idx = 0;
    while (idx < slice.size()) {
        size_t q1 = slice.find('"', idx);
        if (q1 == std::string::npos) break;
        size_t q2 = slice.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        res.push_back(slice.substr(q1 + 1, q2 - q1 - 1));
        idx = q2 + 1;
    }
    return res;
}

}  // namespace

EdgeAiScheduler::EdgeAiScheduler(
    std::string device_id,
    HypothesisSink sink,
    std::chrono::seconds cooldown,
    std::chrono::milliseconds hard_timeout,
    std::string script_path)
    : device_id_(std::move(device_id)),
      sink_(std::move(sink)),
      cooldown_(cooldown),
      hard_timeout_(hard_timeout),
      script_path_(std::move(script_path)) {}

EdgeAiScheduler::~EdgeAiScheduler() {
    stop();
}

void EdgeAiScheduler::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
    stop_requested_.store(false);
    worker_ = std::thread(&EdgeAiScheduler::workerLoop, this);
}

void EdgeAiScheduler::stop() {
    stop_requested_.store(true);
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    running_.store(false);
}

bool EdgeAiScheduler::submitIfEligible(std::shared_ptr<const AssessmentSnapshot> snapshot) {
    if (!snapshot) return false;
    const auto& exp = snapshot->experience;

    std::lock_guard<std::mutex> lock(mutex_);
    stats_.submitted_total++;

    // 仅在网络劣化 (DEGRADED 或 BAD) 且存在明确 primary_issue 时才触发端侧推理
    if ((exp.overall != HealthState::DEGRADED && exp.overall != HealthState::BAD) ||
        !exp.primary_issue.has_value() || exp.primary_issue->empty()) {
        stats_.skipped_healthy++;
        return false;
    }

    // 冷却键：gateway_id + ":" + interface + ":" + anomaly_type
    std::string cooldown_key = device_id_ + ":" + exp.iface + ":" + exp.primary_issue.value();
    auto now = std::chrono::steady_clock::now();

    auto it = cooldown_map_.find(cooldown_key);
    if (it != cooldown_map_.end()) {
        if ((now - it->second) < cooldown_) {
            stats_.skipped_cooldown++;
            return false;
        }
    }

    // 更新冷却时间戳
    cooldown_map_[cooldown_key] = now;

    // 放入容量为 1 的有界等待槽（若已有等待任务则覆盖，保留最新观测）
    if (pending_slot_ != nullptr) {
        stats_.overwritten_pending++;
    }
    pending_slot_ = std::move(snapshot);
    cv_.notify_one();
    return true;
}

EdgeAiSchedulerStats EdgeAiScheduler::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void EdgeAiScheduler::workerLoop() {
    while (!stop_requested_.load()) {
        std::shared_ptr<const AssessmentSnapshot> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() {
                return stop_requested_.load() || pending_slot_ != nullptr;
            });
            if (stop_requested_.load()) break;
            task = std::move(pending_slot_);
            pending_slot_ = nullptr;
        }

        if (!task) continue;

        EdgeAiHypothesisRecord record;
        if (executeWithHardTimeout(*task, &record)) {
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stats_.completed_ok++;
            }
            if (sink_) {
                sink_(std::move(record));
            }
        } else {
            std::lock_guard<std::mutex> lock(mutex_);
            stats_.failed_or_killed++;
        }
    }
}

bool EdgeAiScheduler::executeWithHardTimeout(const AssessmentSnapshot& snap,
                                             EdgeAiHypothesisRecord* out_record) {
    // 构造传入脚本的 JSON（内嵌真实 snapshot_id）
    std::string exp_json = LegacyAdapter::toExperienceJsonV2(snap.experience);
    std::string snap_id = "snap-" + device_id_ + "-" +
                          std::to_string(snap.network_epoch) + "-" +
                          std::to_string(snap.sequence_id);

    // 将 snapshot_id 注入 JSON 头部
    if (!exp_json.empty() && exp_json.front() == '{') {
        exp_json = "{\"snapshot_id\":\"" + snap_id + "\"," + exp_json.substr(1);
    }

    std::string cmd;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!custom_cmd_for_test_.empty()) {
            cmd = custom_cmd_for_test_;
        } else {
            cmd = "python3 " + script_path_;
        }
    }

    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) != 0) return false;
    if (pipe(out_pipe) != 0) {
        close(in_pipe[0]);
        close(in_pipe[1]);
        return false;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        return false;
    }

    if (pid == 0) {
        // 子进程：创建独立进程组，便于超时连带孙进程一并 SIGKILL 强杀
        setpgid(0, 0);
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);

        execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }

    // 父进程
    close(in_pipe[0]);
    close(out_pipe[1]);

    // 写入 JSON 到子进程标准输入并关闭写端
    ssize_t written = write(in_pipe[1], exp_json.data(), exp_json.size());
    (void)written;
    close(in_pipe[1]);

    // 将读管道设为非阻塞
    int flags = fcntl(out_pipe[0], F_GETFL, 0);
    fcntl(out_pipe[0], F_SETFL, flags | O_NONBLOCK);

    std::string output;
    char buf[1024];
    auto start_time = std::chrono::steady_clock::now();
    bool child_exited = false;
    int status = 0;

    while (!stop_requested_.load()) {
        // 读取可用输出
        while (true) {
            ssize_t n = read(out_pipe[0], buf, sizeof(buf));
            if (n > 0) {
                output.append(buf, static_cast<size_t>(n));
            } else {
                break;
            }
        }

        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            child_exited = true;
            // 读尽剩余管道数据
            while (true) {
                ssize_t n = read(out_pipe[0], buf, sizeof(buf));
                if (n > 0) output.append(buf, static_cast<size_t>(n));
                else break;
            }
            break;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time);
        if (elapsed >= hard_timeout_) {
            // 触发硬超时：SIGKILL 强杀整个子进程组
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            close(out_pipe[0]);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                stats_.timed_out_killed++;
            }
            LOG_WARNING(weaknet_dbus::LogModule::SYSTEM,
                        "Edge AI 推理任务触发硬超时 (" << hard_timeout_.count()
                        << "ms)，已 SIGKILL 强杀清理 pid=" << pid);
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (!child_exited) {
        // stop_requested 触发退出：立即强杀并回收，绝不阻塞服务关闭
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        close(out_pipe[0]);
        return false;
    }

    close(out_pipe[0]);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return false;
    }

    // 解析 edge_ai_hypothesis 结构
    std::string hyp_id = extractJsonStringField(output, "hypothesis_id");
    std::string summary = extractJsonStringField(output, "summary");
    std::string created_at = extractJsonStringField(output, "created_at");
    if (hyp_id.empty() || summary.empty() || created_at.empty()) {
        return false;
    }

    out_record->hypothesis_id = hyp_id;
    out_record->device_id = device_id_;
    out_record->network_epoch = snap.network_epoch;
    out_record->sequence_id = snap.sequence_id;
    out_record->interface_name = snap.experience.iface;
    out_record->source = "EDGE_LOCAL_LLM";
    out_record->model_id = "qwen2.5-0.5b-instruct-q4_k_m";
    out_record->summary = summary;
    out_record->evidence_refs = {snap_id};
    out_record->recommended_actions = extractJsonStringArray(output, "recommended_actions");
    out_record->status = "UNVERIFIED";
    out_record->created_at = created_at;
    out_record->ttl_seconds = 300;
    out_record->cloud_eligible = true;
    return true;
}

}  // namespace weaknet
