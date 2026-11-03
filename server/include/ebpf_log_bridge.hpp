/**
 * @file ebpf_log_bridge.hpp
 * @brief 把 libbpf 的加载/校验器输出接入项目日志系统
 *
 * 背景（为什么需要它）：
 *   libbpf 在 bpf_object__open* / bpf_object__load 失败时，真正的原因
 *   （"invalid mem access 'scalar'"、"map create: Operation not permitted" 等）
 *   是通过全局 print 回调打出来的，而且校验器日志是**成百上千行**的原文。
 *   历史实现只在失败处写一行 `failed to load BPF object`，把回调留给默认的
 *   stderr，于是：
 *     - 走 journald 时原始错误与监控器日志对不上，无法归因；
 *     - 校验器日志量太大，即使打出来也会把 journald 冲掉；
 *     - 现场只能看到"加载失败"，看不到"为什么失败"。
 *
 * 本桥接做三件事：
 *   1. 安装进程级 libbpf print 回调，逐行接管 libbpf 输出；
 *   2. 校验器原文按行降级到 DEBUG（开关见 `server.log_level debug`），
 *      真正的 libbpf 告警（以 "libbpf: " 开头）仍走 WARNING；
 *   3. 环形缓存最近若干行，失败处调用 takeLibbpfError() 把关键原因
 *      拼到自己的失败日志里 —— 生产环境不开 DEBUG 也能看到根因。
 *
 * 线程安全：libbpf_set_print 本身线程安全；缓存为 thread_local，
 * 因为失败原因总是与触发它的那个线程的 open/load 调用一一对应。
 */

#pragma once

#include <string>

namespace weaknet_dbus {

/// 安装全局 libbpf 打印回调（幂等；未链接 libbpf 时为空操作）。
/// 应在任何 BPF 对象加载之前、日志初始化之后调用。
void installLibbpfLogBridge();

/// 取走当前线程缓存的关键行并清空（"取走"语义，避免同一段原因被
/// 后续无关失败重复引用）。没有捕获到输出时返回空串；
/// 有内容时返回形如 `" — libbpf: a | b | c"` 的字符串，
/// 调用方直接拼在失败日志末尾即可：
/// @code
///   LOG_ERROR(LogModule::DNS, "failed to load BPF object"
///                            << takeLibbpfError());
/// @endcode
std::string takeLibbpfError();

/// 清空当前线程缓存（在开始一次新的加载尝试前调用，防止串味）
void clearLibbpfError();

}  // namespace weaknet_dbus
