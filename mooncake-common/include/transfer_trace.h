#pragma once

#include <glog/logging.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>

namespace mooncake {

class TransferTrace {
   public:
    using Clock = std::chrono::steady_clock;

    static bool Enabled() {
        static const bool enabled = [] {
            const auto* value = std::getenv("MC_MF_TRACE");
            return value && std::strcmp(value, "1") == 0;
        }();
        return enabled;
    }

    static Clock::time_point Now() {
        return Enabled() ? Clock::now() : Clock::time_point{};
    }

    static int64_t ElapsedUs(Clock::time_point start) {
        return Enabled()
                   ? std::chrono::duration_cast<std::chrono::microseconds>(
                         Clock::now() - start)
                         .count()
                   : 0;
    }

    explicit TransferTrace(const char* stage, bool new_get = false) {
        if (!Enabled()) return;
        state_ = std::make_unique<State>();
        state_->stage = stage;
        state_->previous_get_id = current_get_id_;
        state_->new_get = new_get;
        if (new_get)
            current_get_id_ =
                next_get_id_.fetch_add(1, std::memory_order_relaxed);
        state_->get_id = current_get_id_;
    }

    ~TransferTrace() {
        if (!state_) return;
        const auto total_us = ElapsedUs(state_->start);
        if (state_->new_get) current_get_id_ = state_->previous_get_id;
        LOG(INFO) << "[MF_TRACE] pid=" << getpid() << " stage=" << state_->stage
                  << " get_id=" << state_->get_id << " total_us=" << total_us
                  << state_->fields.str();
    }

    bool enabled() const { return state_ != nullptr; }

    template <typename T>
    void Add(const char* name, const T& value) {
        if (state_) state_->fields << ' ' << name << '=' << value;
    }

    void Mark(const char* name) {
        if (!state_) return;
        const auto now = Clock::now();
        state_->fields << ' ' << name << "_us="
                       << std::chrono::duration_cast<std::chrono::microseconds>(
                              now - state_->last)
                              .count();
        state_->last = now;
    }

   private:
    struct State {
        const char* stage = nullptr;
        uint64_t get_id = 0;
        uint64_t previous_get_id = 0;
        bool new_get = false;
        Clock::time_point start = Clock::now();
        Clock::time_point last = start;
        std::ostringstream fields;
    };

    inline static std::atomic<uint64_t> next_get_id_{1};
    inline static thread_local uint64_t current_get_id_ = 0;
    std::unique_ptr<State> state_;
};

}  // namespace mooncake
