/**
 * @file main.cpp
 * @brief Qt 看板入口
 *
 * ## 平台插件的选择
 *
 * 板端没有 Xorg，也没有 Wayland，因此 `xcb` 插件不可用。实测确认的可用路径：
 *
 * | 插件 | 依赖 | 板端可用 |
 * |------|------|---------|
 * | `eglfs` | DRM/KMS + EGL | ✅ **推荐**（直连 HDMI） |
 * | `linuxfb` | `/dev/fb*` | ❌ 板端无 fbdev |
 * | `xcb` | Xorg | ❌ 无图形栈运行 |
 * | `offscreen` | 无 | ✅ 仅用于测试，不显示 |
 *
 * 因此默认走 `eglfs`，但允许用 `-platform` 参数或 `QT_QPA_PLATFORM` 环境变量
 * 覆盖（调试时用 `offscreen`）。两个例外由 `apply_default_platform()` 处理：
 * 显式指定时完全不干预；带 `--screenshot` 时自动改用 `offscreen`，因为该选项
 * 的用途就是「无显示器验证」，此时 eglfs 会在启动阶段因拿不到 DRM 而失败。
 *
 * ## 权限
 *
 * `eglfs` 需要打开 `/dev/dri/card0`，通常要求 root 或 `video` 组成员。
 * 若启动报 `Permission denied`，检查：
 *
 * ```bash
 * ls -l /dev/dri/card0
 * groups
 * sudo usermod -aG video $USER
 * ```
 *
 * ## 无显示器验证：--screenshot
 *
 * 没有 HDMI 显示器时，用 `offscreen` 平台插件跑起来，等首次取数完成后
 * 把窗口 `grab()` 成 PNG。这样能在接显示器之前就验证「数据真的画上去了」：
 *
 * ```bash
 * QT_QPA_PLATFORM=offscreen ./weaknet_dashboard --screenshot /tmp/dash.png
 * ```
 *
 * 截图后进程自动退出，适合脚本与 CI。
 */

#include <QApplication>
#include <QByteArray>
#include <QCommandLineParser>
#include <QPixmap>
#include <QTimer>

#include <cstdlib>
#include <cstring>

#include "ui/main_window.hpp"

/**
 * @brief argv 里是否出现了 `--screenshot`（或 `--screenshot=...`）
 *
 * 为什么手工扫 argv 而不是用 QCommandLineParser：
 * 平台插件在 `QApplication` **构造时**就要确定，而 parser 是构造之后
 * 才能处理的。要在那之前决定默认平台插件，只能自己看 argv。
 *
 * 只认长选项：`--screenshot` 没有短形式，这里也不该为它发明一个。
 */
static bool has_screenshot_option(int argc, char* argv[]) {
    static const char kOpt[] = "--screenshot";
    constexpr size_t kOptLen = sizeof(kOpt) - 1;
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (!arg) {
            continue;
        }
        if (std::strncmp(arg, kOpt, kOptLen) == 0 &&
            (arg[kOptLen] == '\0' || arg[kOptLen] == '=')) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 未显式指定平台插件时，决定默认值
 *
 * 头注释推荐了 `eglfs`，但在此之前代码里没有设置任何默认值：完全依赖
 * `QT_QPA_PLATFORM` 环境变量，未设置时由 Qt 在可用插件里自行挑选，板端
 * 挑到哪个不确定。这里把推荐落到实处。
 *
 * 优先级（已实测确认，见下）：
 *
 * 1. `QT_QPA_PLATFORM` 环境变量已设置 → 不动
 * 2. 显式 `-platform <x>` → 由 Qt 自己处理，会压过这里 qputenv 的值
 * 3. `--screenshot` → 用 `offscreen`。这个选项的用途就是「无显示器验证」，
 *    此时默认 eglfs 会在 QApplication 构造时因拿不到 DRM/EGL 直接失败，
 *    根本走不到截图逻辑。
 * 4. 其余 → `eglfs`
 *
 * 实测依据（x86 无 DRM 环境）：
 *   - 不设环境变量：报 "Could not initialize egl display"（默认 eglfs 生效）
 *   - `-platform offscreen`：platformName() == "offscreen"（命令行压过默认）
 *   - 环境变量 offscreen：platformName() == "offscreen"（不被覆盖）
 */
static void apply_default_platform(int argc, char* argv[]) {
    if (!qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
        return;  // 调用方已显式指定（命令行 -platform 或环境变量）
    }
    qputenv("QT_QPA_PLATFORM",
            has_screenshot_option(argc, argv) ? QByteArrayLiteral("offscreen")
                                             : QByteArrayLiteral("eglfs"));
}

int main(int argc, char* argv[]) {
    apply_default_platform(argc, argv);
    QApplication app(argc, argv);

    QApplication::setApplicationName(QStringLiteral("weaknet-dashboard"));
    QApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QApplication::setOrganizationName(QStringLiteral("WeakNet"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("WeakNet 本地诊断看板（实验性）"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption iface_option(
        QStringList() << QStringLiteral("i") << QStringLiteral("interface"),
        QStringLiteral("指定网卡；不传则查询全部网卡"),
        QStringLiteral("name"));
    parser.addOption(iface_option);

    QCommandLineOption shot_option(
        QStringLiteral("screenshot"),
        QStringLiteral("取数完成后把窗口存为 PNG 并退出（无显示器验证用）"),
        QStringLiteral("path"));
    parser.addOption(shot_option);

    QCommandLineOption shot_delay_option(
        QStringLiteral("screenshot-delay"),
        QStringLiteral("截图前等待的毫秒数，留给首轮取数（默认 3000）"),
        QStringLiteral("ms"),
        QStringLiteral("3000"));
    parser.addOption(shot_delay_option);

    parser.process(app);

    weaknet::dashboard::MainWindow window;
    // 无论是否传 -i 都调用：setInterface("") 会把标题写成「全部网卡」，
    // 同时把 history_limit_ 设成与 -i 时同一个值。之前只在 isSet 时调用，
    // 导致不传 -i 时标题停在 build_ui 的默认文案、limit 也停在类默认值，
    // 与「查询全部网卡」的实际行为对不上。
    window.setInterface(parser.isSet(iface_option)
                            ? parser.value(iface_option)
                            : QString());
    window.show();
    window.start();

    if (parser.isSet(shot_option)) {
        const QString path = parser.value(shot_option);
        const int delay_ms = parser.value(shot_delay_option).toInt();
        // 等待后台线程完成首轮取数，再抓窗口。
        // 用 singleShot 而不是 sleep：sleep 会阻塞事件循环，信号送不回主线程，
        // 界面就永远是未刷新状态。
        QTimer::singleShot(delay_ms > 0 ? delay_ms : 0, &window, [&window, path]() {
            const QPixmap shot = window.grab();
            if (shot.isNull()) {
                qWarning("screenshot: grab() returned null");
                QCoreApplication::exit(2);
                return;
            }
            if (!shot.save(path, "PNG")) {
                qWarning("screenshot: cannot save to %s", qPrintable(path));
                QCoreApplication::exit(3);
                return;
            }
            qInfo("screenshot: %dx%d saved to %s",
                  shot.width(), shot.height(), qPrintable(path));
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
