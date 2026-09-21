#pragma once

#include <string>
#include <vector>
#include <variant>
#include <optional>
#include <cmath>
#include <algorithm>

namespace weaknet {

enum class PredicateOp {
    Eq,
    Ne,
    Gt,
    Gte,
    Lt,
    Lte,
    In,
    NotIn,
    IsNull,
    NotNull
};

enum class EvalResult {
    True,
    False,
    Unknown // 字段缺失或类型不匹配，守住 Missing != false 原则
};

using PredicateValue = std::variant<std::string, double, bool, std::vector<std::string>>;

struct Predicate {
    std::string field;
    PredicateOp op;
    PredicateValue value;
};

struct PredicateGroup {
    enum class Logic { All, Any };
    Logic logic{Logic::All};
    std::vector<Predicate> predicates;
    std::vector<PredicateGroup> groups; // 嵌套支持
};

class PredicateEngine {
public:
    using FieldResolver = std::function<std::optional<PredicateValue>(const std::string& field)>;

    static EvalResult evaluate(const Predicate& pred, const FieldResolver& resolver) {
        auto actual_opt = resolver(pred.field);

        if (pred.op == PredicateOp::IsNull) {
            return (!actual_opt.has_value()) ? EvalResult::True : EvalResult::False;
        }
        if (pred.op == PredicateOp::NotNull) {
            return (actual_opt.has_value()) ? EvalResult::True : EvalResult::False;
        }

        // 其它操作符下，字段不存在直接返回 Unknown
        if (!actual_opt.has_value()) {
            return EvalResult::Unknown;
        }

        const auto& actual = actual_opt.value();
        return compare(actual, pred.op, pred.value);
    }

    static EvalResult evaluateGroup(const PredicateGroup& group, const FieldResolver& resolver) {
        if (group.predicates.empty() && group.groups.empty()) {
            return EvalResult::True;
        }

        if (group.logic == PredicateGroup::Logic::All) {
            bool has_unknown = false;
            for (const auto& p : group.predicates) {
                auto res = evaluate(p, resolver);
                if (res == EvalResult::False) return EvalResult::False;
                if (res == EvalResult::Unknown) has_unknown = true;
            }
            for (const auto& g : group.groups) {
                auto res = evaluateGroup(g, resolver);
                if (res == EvalResult::False) return EvalResult::False;
                if (res == EvalResult::Unknown) has_unknown = true;
            }
            return has_unknown ? EvalResult::Unknown : EvalResult::True;
        } else { // Logic::Any
            bool has_unknown = false;
            for (const auto& p : group.predicates) {
                auto res = evaluate(p, resolver);
                if (res == EvalResult::True) return EvalResult::True;
                if (res == EvalResult::Unknown) has_unknown = true;
            }
            for (const auto& g : group.groups) {
                auto res = evaluateGroup(g, resolver);
                if (res == EvalResult::True) return EvalResult::True;
                if (res == EvalResult::Unknown) has_unknown = true;
            }
            return has_unknown ? EvalResult::Unknown : EvalResult::False;
        }
    }

    static std::string opToString(PredicateOp op) {
        switch (op) {
            case PredicateOp::Eq: return "eq";
            case PredicateOp::Ne: return "ne";
            case PredicateOp::Gt: return "gt";
            case PredicateOp::Gte: return "gte";
            case PredicateOp::Lt: return "lt";
            case PredicateOp::Lte: return "lte";
            case PredicateOp::In: return "in";
            case PredicateOp::NotIn: return "not_in";
            case PredicateOp::IsNull: return "is_null";
            case PredicateOp::NotNull: return "not_null";
        }
        return "unknown";
    }

    static std::optional<PredicateOp> stringToOp(const std::string& s) {
        if (s == "eq" || s == "==") return PredicateOp::Eq;
        if (s == "ne" || s == "!=") return PredicateOp::Ne;
        if (s == "gt" || s == ">") return PredicateOp::Gt;
        if (s == "gte" || s == ">=") return PredicateOp::Gte;
        if (s == "lt" || s == "<") return PredicateOp::Lt;
        if (s == "lte" || s == "<=") return PredicateOp::Lte;
        if (s == "in") return PredicateOp::In;
        if (s == "not_in") return PredicateOp::NotIn;
        if (s == "is_null") return PredicateOp::IsNull;
        if (s == "not_null") return PredicateOp::NotNull;
        return std::nullopt;
    }

private:
    static EvalResult compare(const PredicateValue& actual, PredicateOp op, const PredicateValue& expected) {
        // 数值比较
        if (std::holds_alternative<double>(actual) && std::holds_alternative<double>(expected)) {
            double a = std::get<double>(actual);
            double e = std::get<double>(expected);
            switch (op) {
                case PredicateOp::Eq: return (std::abs(a - e) < 1e-6) ? EvalResult::True : EvalResult::False;
                case PredicateOp::Ne: return (std::abs(a - e) >= 1e-6) ? EvalResult::True : EvalResult::False;
                case PredicateOp::Gt: return (a > e) ? EvalResult::True : EvalResult::False;
                case PredicateOp::Gte: return (a >= e) ? EvalResult::True : EvalResult::False;
                case PredicateOp::Lt: return (a < e) ? EvalResult::True : EvalResult::False;
                case PredicateOp::Lte: return (a <= e) ? EvalResult::True : EvalResult::False;
                default: return EvalResult::Unknown;
            }
        }
        // 字符串比较
        if (std::holds_alternative<std::string>(actual) && std::holds_alternative<std::string>(expected)) {
            const auto& a = std::get<std::string>(actual);
            const auto& e = std::get<std::string>(expected);
            switch (op) {
                case PredicateOp::Eq: return (a == e) ? EvalResult::True : EvalResult::False;
                case PredicateOp::Ne: return (a != e) ? EvalResult::True : EvalResult::False;
                default: return EvalResult::Unknown;
            }
        }
        // 布尔值比较
        if (std::holds_alternative<bool>(actual) && std::holds_alternative<bool>(expected)) {
            bool a = std::get<bool>(actual);
            bool e = std::get<bool>(expected);
            switch (op) {
                case PredicateOp::Eq: return (a == e) ? EvalResult::True : EvalResult::False;
                case PredicateOp::Ne: return (a != e) ? EvalResult::True : EvalResult::False;
                default: return EvalResult::Unknown;
            }
        }
        // In / NotIn (字符串在列表中)
        if (std::holds_alternative<std::string>(actual) && std::holds_alternative<std::vector<std::string>>(expected)) {
            const auto& a = std::get<std::string>(actual);
            const auto& list = std::get<std::vector<std::string>>(expected);
            bool found = (std::find(list.begin(), list.end(), a) != list.end());
            if (op == PredicateOp::In) return found ? EvalResult::True : EvalResult::False;
            if (op == PredicateOp::NotIn) return !found ? EvalResult::True : EvalResult::False;
        }

        return EvalResult::Unknown;
    }
};

} // namespace weaknet
