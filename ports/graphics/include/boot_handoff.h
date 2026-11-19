/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <cstdint>

// One boot-only presentation handoff. Keep the last visible boot image while
// RS switches its exclusive boot display to the normal display tree. Never
// defer boot completion, release fences, or application rendering. A first
// compositor call after completion, power-off, or deadline ends the policy.
class AuroraBootHandoff {
    bool first_ = true, done_ = false, saw_boot_content_ = false, held_ = false;
    uint64_t start_ = 0, completed_ = 0;
public:
    bool inspect(uint64_t now, bool enabled, bool ready, bool completed)
    {
        if (done_) return false;
        if (first_) {
            first_ = false;
            start_ = now;
            if (!enabled || completed) { done_ = true; return false; }
        }
        if (now - start_ >= 120000) { done_ = true; return false; }
        if (completed && !completed_) completed_ = now;
        if (completed_ && now - completed_ >= 2000) { done_ = true; return false; }
        return ready;
    }
    bool hold(bool has_rgb)
    {
        if (done_) return false;
        if (!completed_) {
            saw_boot_content_ = saw_boot_content_ || has_rgb;
            return false;
        }
        if (!has_rgb && saw_boot_content_) { held_ = true; return true; }
        if (held_ && has_rgb) done_ = true;
        return false;
    }
    void stop() { done_ = true; }
    bool done() const { return done_; }
    bool playing(bool started, bool finished) const
    {
        return !done_ && saw_boot_content_ && !completed_ && started && !finished;
    }
};
