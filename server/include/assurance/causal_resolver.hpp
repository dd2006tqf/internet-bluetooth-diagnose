#pragma once

#include "assurance/diagnosis_rules.hpp"
#include <vector>
#include <set>
#include <string>
#include <algorithm>

namespace weaknet {

struct MatchedRule {
    const DiagnosisRule* rule{nullptr};
    DiagnosisConfidence confidence{DiagnosisConfidence::MEDIUM};
    std::vector<std::string> evidence_refs;
};

struct ResolvedDiagnosis {
    const MatchedRule* primary{nullptr};
    std::vector<const MatchedRule*> secondary;
    std::vector<const MatchedRule*> suppressed;
};

class CausalResolver {
public:
    /**
     * @brief 严格按照因果判定序列确定 Primary、Secondary 和被抑制规则：
     *
     * 1. 显式因果抑制图剪枝 (Explicit Suppression Check)
     * 2. 特异性与置信度比较 (Rule Specificity / Confidence: HIGH > MEDIUM > LOW)
     * 3. 优先级降序 (Priority)
     * 4. 稳定 Tie-break (Rule ID 字典序)
     *
     * 核心原则：Protocol-layer precedence != causal precedence
     */
    static ResolvedDiagnosis resolve(const std::vector<MatchedRule>& matches) {
        ResolvedDiagnosis res;
        if (matches.empty()) {
            return res;
        }

        // 1. 构建所有命中的规则抑制黑名单
        std::set<std::string> suppressed_ids;
        for (const auto& m : matches) {
            for (const auto& sup_id : m.rule->suppresses) {
                suppressed_ids.insert(sup_id);
            }
        }

        std::vector<const MatchedRule*> active_candidates;
        for (const auto& m : matches) {
            if (suppressed_ids.find(m.rule->id) != suppressed_ids.end()) {
                res.suppressed.push_back(&m);
            } else {
                active_candidates.push_back(&m);
            }
        }

        if (active_candidates.empty()) {
            // 所有规则互抑时的保底：退回到抑制列表中 priority 最高者
            active_candidates.push_back(res.suppressed.front());
            res.suppressed.erase(res.suppressed.begin());
        }

        // 2. 严格层级确定性排序
        std::sort(active_candidates.begin(), active_candidates.end(),
                  [](const MatchedRule* a, const MatchedRule* b) {
                      // a. 置信度降序
                      if (a->confidence != b->confidence) {
                          return static_cast<int>(a->confidence) > static_cast<int>(b->confidence);
                      }
                      // b. Priority 降序
                      if (a->rule->priority != b->rule->priority) {
                          return a->rule->priority > b->rule->priority;
                      }
                      // c. 稳定字典序平局消除
                      return a->rule->id < b->rule->id;
                  });

        res.primary = active_candidates.front();
        for (size_t i = 1; i < active_candidates.size(); ++i) {
            res.secondary.push_back(active_candidates[i]);
        }

        return res;
    }
};

} // namespace weaknet
