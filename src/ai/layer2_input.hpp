#pragma once
// =============================================================================
// Layer2Input - Layer1 -> Layer2 interface
// =============================================================================
#include "ai/ui_element.hpp"
#include <string>
#include <vector>
#include <algorithm>

namespace mirage::ai {

// State expectation for False Negative detection
struct StateExpectation {
    std::string              state_name;
    std::vector<std::string> required_types;     // must have these element types
    std::vector<std::string> required_keywords;  // normalized text must contain these
};

// Action candidate (source_index avoids dangling pointer on vector realloc)
struct ActionCandidate {
    std::string action;
    int         source_index = -1;  // index into Layer2Input::elements; -1 = global action
    float       priority     = 0.f;
};

struct Layer2Input {
    std::vector<UiElementHit>    elements;    // dedup-ed UI elements from Layer1
    std::vector<ActionCandidate> candidates;  // Layer1-generated action candidates

    std::string previous_state;
    std::string task_goal;

    bool meetsExpectation(const StateExpectation& exp) const {
        for (const auto& kw : exp.required_keywords) {
            bool found = std::any_of(elements.begin(), elements.end(),
                [&](const UiElementHit& e){
                    return e.text_normalized.find(kw) != std::string::npos;
                });
            if (!found) return false;
        }
        for (const auto& t : exp.required_types) {
            bool found = std::any_of(elements.begin(), elements.end(),
                [&](const UiElementHit& e){ return e.type == t; });
            if (!found) return false;
        }
        return true;
    }

    // Returns true when Layer2 should fall back to full-frame rescan
    bool needsFullRescan(const StateExpectation* exp = nullptr) const {
        // No candidates at all
        if (candidates.empty()) return true;

        // All elements are low confidence
        if (!elements.empty()) {
            bool all_low = std::all_of(elements.begin(), elements.end(),
                [](const UiElementHit& e){ return e.confidence < 0.6f; });
            if (all_low) return true;
        }

        // Top-1 and Top-2 are too close (ambiguous)
        if (candidates.size() >= 2) {
            float diff = candidates[0].priority - candidates[1].priority;
            if (diff < 0.f) diff = -diff;
            if (diff < 0.1f) return true;
        }

        // Expected UI elements are missing (False Negative guard)
        if (exp && !meetsExpectation(*exp)) return true;

        return false;
    }
};

} // namespace mirage::ai
