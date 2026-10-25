#pragma once

// Plays each shot's effects once (docs/SteamMultiplayerImplementation.md section 10.1). A
// client shows its own shot at once (flash, sound, recoil) and the host's confirmation
// arrives a round trip later; a remote player's shot is shown only when the host's event
// arrives. Both name the shot by the same key: shooter, life, the input tick it was fired
// with and its ordinal within that tick. The confirmation of a shot already shown then
// only corrects it (where the tracer ends, a hit marker) instead of playing it again.
//
// A fixed ring of the last kCapacity keys; nothing allocates.

#include <array>
#include <cstdint>

namespace shockwave::net {

struct ShotKey {
    uint64_t shooter = 0;   // network ID
    uint32_t incarnation = 0;
    uint32_t inputTick = 0;
    uint16_t ordinal = 0;
    friend bool operator==(const ShotKey&, const ShotKey&) = default;
};

class PresentationDedup {
public:
    static constexpr uint32_t kCapacity = 256;

    enum class Seen : uint8_t {
        New,   // not shown yet: show it
        Predicted,   // already shown by this machine's prediction: only correct it
        Duplicate,   // already confirmed: ignore it
    };

    // This machine showed the shot ahead of the host. False if it already had.
    bool predicted(const ShotKey& key) {
        if (find(key)) return false;
        add(key, false);
        return true;
    }
    // The host's word on a shot: what to do with it. A prediction can be a tick or two off
    // the host's when the shooter's weapon timer and the host's drift apart; with a window,
    // an unconfirmed prediction of the same shooter, life and ordinal within that many
    // ticks counts as this shot (the nearest one).
    Seen confirmed(const ShotKey& key, uint32_t windowTicks = 0) {
        Entry* e = find(key);
        if (e && e->confirmed) return Seen::Duplicate;
        if (!e && windowTicks > 0) e = nearestPrediction(key, windowTicks);
        if (e) {
            e->confirmed = true;
            e->key = key;   // later copies of this confirmation match exactly
            return Seen::Predicted;
        }
        add(key, true);
        return Seen::New;
    }
    void clear() {
        m_entries = {};
        m_next = 0;
    }

private:
    struct Entry {
        ShotKey key;
        bool used = false, confirmed = false;
    };
    Entry* find(const ShotKey& key) {
        for (Entry& e : m_entries)
            if (e.used && e.key == key) return &e;
        return nullptr;
    }
    Entry* nearestPrediction(const ShotKey& key, uint32_t window) {
        Entry* best = nullptr;
        uint32_t bestGap = window + 1;
        for (Entry& e : m_entries) {
            if (!e.used || e.confirmed || e.key.shooter != key.shooter || e.key.incarnation != key.incarnation ||
                e.key.ordinal != key.ordinal)
                continue;
            const uint32_t gap =
                e.key.inputTick > key.inputTick ? e.key.inputTick - key.inputTick : key.inputTick - e.key.inputTick;
            if (gap < bestGap) {
                best = &e;
                bestGap = gap;
            }
        }
        return best;
    }
    void add(const ShotKey& key, bool confirmed) {
        m_entries[m_next] = Entry{key, true, confirmed};
        m_next = (m_next + 1) % kCapacity;
    }
    std::array<Entry, kCapacity> m_entries{};
    uint32_t m_next = 0;
};

}   // namespace shockwave::net
