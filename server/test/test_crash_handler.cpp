/**
 * @file test_crash_handler.cpp
 * @brief 崩溃处理器验证探针：主动触发 SIGSEGV，检查调用栈能否符号化
 *
 * 存在的理由：
 *   Logger::installCrashHandler() 调的是 glog 的 InstallFailureSignalHandler()。
 *   "编译通过"不等于"崩溃时能打出有用的栈"——产出质量取决于二进制的
 *   .symtab 是否还在盘上（glog 0.4.0 走 /proc/self/maps + ELF 解析，
 *   不走 dladdr，所以与 -rdynamic 无关；被 strip 过就只能看到裸地址）。
 *   这个探针让"崩溃栈可符号化"成为一条可在板端重复执行的断言，
 *   而不是一次性的人工观察。
 *
 * 用法：
 *   test_crash_handler            正常运行 -> 故意崩溃，栈打到 stderr，退出码 139
 *   test_crash_handler --check    同上，但自行判定符号化是否成功：
 *                                 成功退出 0，失败退出 1（供测试脚本调用）
 *
 * 注意：本程序**必然**以非零退出码结束（除非崩溃没发生），
 * 不要把它放进 ctest 的常规回归集里。
 */

#include <glog/logging.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

namespace {

// 三层调用，便于验证栈回溯能穿过多个帧、且每层都能解析出名字
__attribute__((noinline)) void crashLevel3() {
    volatile int* p = reinterpret_cast<volatile int*>(0x10);
    *p = 42;  // 故意解引用非法地址 -> SIGSEGV
}

__attribute__((noinline)) void crashLevel2() { crashLevel3(); }
__attribute__((noinline)) void crashLevel1() { crashLevel2(); }

}  // namespace

int main(int argc, char** argv) {
    const bool check_mode = (argc > 1 && std::strcmp(argv[1], "--check") == 0);

    google::InitGoogleLogging(argv[0]);
    FLAGS_logtostderr = 1;
    FLAGS_colorlogtostderr = 0;

    // 必须显式安装：Logger::init() 不会替任何进程做这件事
    google::InstallFailureSignalHandler();

    if (!check_mode) {
        LOG(INFO) << "[crash-probe] about to crash deliberately";
        crashLevel1();
        // 理论上到不了这里
        fprintf(stderr, "[crash-probe] ERROR: crash did not happen\n");
        return 2;
    }

    // --check 模式：把 stderr 重定向到临时文件，崩溃后由父进程读回分析。
    // 这里用 fork 而不是 popen，是因为子进程会死于信号，
    // popen 的退出状态在部分实现上会混淆。
    fflush(stderr);

    std::string tmp_path = "/tmp/crash-probe-XXXXXX";
    int fd = mkstemp(&tmp_path[0]);
    if (fd < 0) {
        fprintf(stderr, "[crash-probe] mkstemp failed\n");
        return 3;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "[crash-probe] fork failed\n");
        close(fd);
        unlink(tmp_path.c_str());
        return 3;
    }

    if (pid == 0) {
        // 子进程：把 stderr 指向临时文件，然后崩溃
        dup2(fd, STDERR_FILENO);
        close(fd);
        crashLevel1();
        _exit(2);
    }

    // 父进程：等子进程死掉，读回 stderr 内容判定
    close(fd);
    int status = 0;
    waitpid(pid, &status, 0);

    std::string output;
    {
        FILE* f = fopen(tmp_path.c_str(), "r");
        if (f) {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
                output.append(buf, n);
            }
            fclose(f);
        }
    }
    unlink(tmp_path.c_str());

    // 判定：glog 的输出里，符号化的帧形如 "    @  0x... crashLevel3()"
    const bool caught = output.find("SIGSEGV") != std::string::npos;
    const bool symbolized = output.find("crashLevel3()") != std::string::npos &&
                            output.find("crashLevel1()") != std::string::npos;

    if (!caught) {
        fprintf(stderr, "[crash-probe] FAIL: glog 未捕获 SIGSEGV\n");
        fprintf(stderr, "%s\n", output.c_str());
        return 1;
    }
    if (!symbolized) {
        fprintf(stderr, "[crash-probe] FAIL: 栈未符号化（二进制可能被 strip）\n");
        fprintf(stderr, "%s\n", output.c_str());
        return 1;
    }

    printf("[crash-probe] OK: SIGSEGV 已捕获且调用栈已符号化\n");
    return 0;
}
