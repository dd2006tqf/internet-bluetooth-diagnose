/**
 * @file json_reader.hpp
 * @brief 轻量 JSON 读取器 —— 仅够看板使用，不追求通用性
 *
 * ## 为什么不引第三方库
 *
 * 项目全仓不使用 nlohmann/json。`server/src/net_info.cpp:15` 明确记录了
 * 这个决定：「使用自定义轻量 JSON 解析器（不依赖第三方库如 nlohmann/json）」。
 * 看板沿用同样策略，避免在嵌入式目标上引入额外依赖。
 *
 * ## 与 net_info.cpp 的关系
 *
 * `net_info.cpp` 里的 JSON 工具函数是文件内 static 的，不对外暴露。
 * 本文件独立实现一份，只提供看板需要的能力：
 *   - 按 key 取字符串 / 数字 / 布尔
 *   - 按 key 取数组（字符串数组、对象数组）
 *   - 嵌套对象下钻
 *
 * 不做的事：序列化、写 JSON、未知类型推断、错误恢复。
 * 解析失败一律返回默认值，由调用方通过 `ok()` 判断整体有效性。
 *
 * ## 设计取舍
 *
 * 使用「路径式查找」而非完整 DOM 树：
 *
 * ```cpp
 * JsonReader r(json);
 * auto state = r.getString("overall.state");
 * auto score = r.getDouble("overall.display_score", -1.0);
 * ```
 *
 * 理由：看板只需要读少数已知字段，构建完整 DOM 是浪费。
 * 代价：同一个路径重复查找会重复扫描。看板场景下 JSON 只有几 KB，
 * 这点开销可忽略。
 */

#ifndef WEAKNET_QT_DASHBOARD_JSON_READER_HPP
#define WEAKNET_QT_DASHBOARD_JSON_READER_HPP

#include <string>
#include <vector>

namespace weaknet {
namespace dashboard {

/**
 * @brief 轻量 JSON 读取器
 *
 * 构造时不做完整解析（懒解析），首次调用 getXxx 时才扫描。
 * 这样如果调用方只关心一个字段，就不必为整棵 JSON 付出代价。
 */
class JsonReader {
public:
    explicit JsonReader(const std::string& json);

    /**
     * @brief 整个 JSON 是否看起来有效（非空、首尾括号匹配）
     *
     * 这不是严格的语法校验，只是快速排除「空串 / 错误信息 / 截断」。
     */
    bool valid() const;

    /**
     * @brief 按路径取字符串
     *
     * @param path     点分路径，如 "overall.state"；顶层字段直接写 "schema_version"
     * @param fallback 找不到或类型不符时的返回值
     */
    std::string getString(const std::string& path, const std::string& fallback = "") const;

    /**
     * @brief 按路径取数字
     */
    double getDouble(const std::string& path, double fallback = 0.0) const;

    /**
     * @brief 按路径取整数
     */
    int getInt(const std::string& path, int fallback = 0) const;

    /**
     * @brief 按路径取布尔
     */
    bool getBool(const std::string& path, bool fallback = false) const;

    /**
     * @brief 按路径取字符串数组
     *
     * 用于 `warnings: ["a","b"]` 这类结构。
     */
    std::vector<std::string> getStringArray(const std::string& path) const;

    /**
     * @brief 按路径取对象数组的原始 JSON 片段
     *
     * 用于 `evidence: [{"metric":...,"value":...}]` 这类结构。
     * 返回每个元素的原始 JSON 字符串，调用方再用 JsonReader 逐个解析。
     *
     * 之所以不直接返回结构体：数组元素形状由调用方决定，
     * 在这里做通用映射反而增加耦合。
     */
    std::vector<std::string> getObjectArray(const std::string& path) const;

    /**
     * @brief 判断某个路径是否存在且非 null
     */
    bool has(const std::string& path) const;

    /**
     * @brief 判断根节点是不是数组
     *
     * `GetHistory` 返回的是顶层数组，`locate()` 只处理对象根节点，
     * 因此需要这个额外入口。
     */
    bool rootIsArray() const;

    /**
     * @brief 取根数组的每个元素原始 JSON
     *
     * 仅在 `rootIsArray()` 为真时有意义。
     */
    std::vector<std::string> getRootObjectArray() const;

    /**
     * @brief 取根数组的长度
     */
    size_t rootArraySize() const;

    /**
     * @brief 取原始 JSON 字符串（用于调试）
     */
    const std::string& raw() const { return json_; }

private:
    /**
     * @brief 定位路径对应的值起始位置
     *
     * @return 值的起始下标；找不到返回 std::string::npos
     */
    size_t locate(const std::string& path) const;

    /**
     * @brief 从 pos 开始跳过一个值，返回其结束位置（不含）
     */
    size_t skipValue(size_t pos) const;

    /**
     * @brief 从 pos 开始跳过一个字符串，返回结束引号之后的位置
     */
    size_t skipString(size_t pos) const;

    /**
     * @brief 从 pos 开始跳过一个对象或数组
     */
    size_t skipContainer(size_t pos) const;

    /**
     * @brief 跳过空白
     */
    size_t skipWhitespace(size_t pos) const;

    /**
     * @brief 解析 pos 处的字符串值（含转义）
     */
    std::string parseStringAt(size_t pos, size_t* end_out) const;

    /**
     * @brief 在 [begin, end) 范围内按顶层逗号切分
     */
    std::vector<std::pair<size_t, size_t>> splitTopLevel(size_t begin, size_t end) const;

    std::string json_;
};

} // namespace dashboard
} // namespace weaknet

#endif // WEAKNET_QT_DASHBOARD_JSON_READER_HPP
