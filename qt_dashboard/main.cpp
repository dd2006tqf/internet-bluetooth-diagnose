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
 */

#include <QApplication>
#include <QCommandLineParser>

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

    parser.process(app);

    weaknet::dashboard::MainWindow window;
    window.show();
    window.start();

    return app.exec();
}
