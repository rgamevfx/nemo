#pragma once

#include <cassert>
#include <utility>

namespace nemo::ui {

// One presentation interaction per project, explicitly shared by all windows
// and parameter adapters. GUI-thread-only; participants must release before
// destruction and must not acquire from their cancellation callback. This owns
// neither a document transaction nor history: cancellation goes to the existing
// participant, which discards its preview and invalidates its input/request token.
class ParameterInteraction final {
public:
    ParameterInteraction() = default;
    ParameterInteraction(const ParameterInteraction&) = delete;
    ParameterInteraction& operator=(const ParameterInteraction&) = delete;
    ~ParameterInteraction() { assert(owner_ == nullptr); }

    void acquire(void* owner, void (*cancelOwner)(void*)) {
        assert(owner && cancelOwner);
        cancel();
        owner_ = owner;
        cancelOwner_ = cancelOwner;
    }

    void release(void* owner) {
        if (owner_ != owner)
            return;
        owner_ = nullptr;
        cancelOwner_ = nullptr;
    }

    void cancel() {
        auto* owner = std::exchange(owner_, nullptr);
        const auto callback = std::exchange(cancelOwner_, nullptr);
        if (callback)
            callback(owner);
        assert(owner_ == nullptr);
    }

private:
    void* owner_{};
    void (*cancelOwner_)(void*){};
};
}  // namespace nemo::ui
