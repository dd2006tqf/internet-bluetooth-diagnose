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
 * 因此默认走 `eglfs`，但允许用 `-platform` 参数覆盖（调试时用 `offscreen`）。
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
#include <QCommandLineParser>
#include <QPixmap>
#include <QScreen>
#include <QTimer>

#include "ui/main_window.hpp"

int main(int argc, char* argv[]) {
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
        QStringLiteral("指定网卡（默认自动检测）"),
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
    if (parser.isSet(iface_option)) {
        window.setInterface(parser.value(iface_option));
    }
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
