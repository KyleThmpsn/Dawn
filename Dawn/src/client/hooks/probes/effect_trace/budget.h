#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace dawn::client::hooks::probes::effect_trace {

/**
 * Rolling per-key write budget: up to `PerInterval` lines per key every `IntervalMs`, with the
 * rest counted and reported once the interval ends, so a repeating perk never floods the log.
 * It holds no lock; its owner serializes every call.
 */
template <std::size_t Capacity, std::uint32_t PerInterval, std::uint64_t IntervalMs>
class RollingBudget final {
public:
    /** One key's interval that ended with lines held back. */
    struct Ended final {
        std::uint64_t key{};
        std::uint32_t suppressed{};
        std::uint64_t spanMs{};
    };

    /**
     * Decides whether one line is written.
     * @param ended Receives the key's previous interval when this call closed it with lines held
     * back; its `suppressed` is zero otherwise.
     * @return False when the key is over budget and the line is only counted.
     */
    [[nodiscard]] bool admit(std::uint64_t key, std::uint64_t now, Ended& ended) noexcept {
        ended = {};
        // The key's own slot wins over an earlier free one, or the key would hold two budgets.
        Slot* slot = nullptr;
        for (auto& candidate : slots_) {
            if (candidate.used && candidate.key == key) {
                slot = &candidate;
                break;
            }
            if (!candidate.used && slot == nullptr) {
                slot = &candidate;
            }
        }
        if (slot == nullptr) {
            // Every slot holds another key; write this line rather than lose it.
            return true;
        }
        if (!slot->used) {
            *slot = {key, now, 0, 0, true};
        } else if (now - slot->since >= IntervalMs) {
            if (slot->suppressed != 0) {
                ended = {key, slot->suppressed, IntervalMs};
            }
            *slot = {key, now, 0, 0, true};
        }
        if (slot->written >= PerInterval) {
            ++slot->suppressed;
            return false;
        }
        ++slot->written;
        return true;
    }

    /**
     * Frees every slot whose interval has ended, or every slot when `closing`, and copies those
     * that held lines back into `out`.
     * @return How many were copied.
     */
    std::size_t collect(std::uint64_t now, bool closing, std::span<Ended> out) noexcept {
        std::size_t count = 0;
        for (auto& slot : slots_) {
            if (!slot.used || (!closing && now - slot.since < IntervalMs)) {
                continue;
            }
            if (slot.suppressed != 0 && count < out.size()) {
                out[count++] = {slot.key, slot.suppressed,
                                (std::min)(now - slot.since, IntervalMs)};
            }
            slot = {};
        }
        return count;
    }

    /** Forgets every key. */
    void clear() noexcept { slots_ = {}; }

private:
    struct Slot final {
        std::uint64_t key{};
        std::uint64_t since{};
        std::uint32_t written{};
        std::uint32_t suppressed{};
        bool used{};
    };
    std::array<Slot, Capacity> slots_{};
};

} // namespace dawn::client::hooks::probes::effect_trace
