#pragma once

// Subsystems: services found by type rather than wired through constructors, as in
// Unreal. A collection owns them, hands them out with find<T>() and destroys them in
// reverse order of creation, so a subsystem can rely on the ones added before it.
//
// Collections are filled and cleared on the main thread. Looking a subsystem up is a
// read and safe from any thread while nothing is being added or removed.

#include <memory>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <utility>
#include <vector>

namespace shockwave {

class Subsystem {
public:
    virtual ~Subsystem() = default;
};

class SubsystemCollection {
public:
    SubsystemCollection() = default;
    SubsystemCollection(const SubsystemCollection&) = delete;
    SubsystemCollection& operator=(const SubsystemCollection&) = delete;
    ~SubsystemCollection() { clear(); }

    // Creates T in place. One subsystem per type.
    template <class T, class... Args>
    T& add(Args&&... args) {
        static_assert(std::is_base_of_v<Subsystem, T>, "subsystems derive from shockwave::Subsystem");
        if (find<T>()) throw std::logic_error(std::string("subsystem added twice: ") + typeid(T).name());
        auto owned = std::make_unique<T>(std::forward<Args>(args)...);
        T& result = *owned;
        m_entries.push_back({std::type_index(typeid(T)), std::move(owned)});
        return result;
    }

    template <class T>
    T* find() const {
        for (const Entry& entry : m_entries)
            if (entry.type == std::type_index(typeid(T))) return static_cast<T*>(entry.subsystem.get());
        return nullptr;
    }

    template <class T>
    T& get() const {
        if (T* found = find<T>()) return *found;
        throw std::logic_error(std::string("no subsystem ") + typeid(T).name());
    }

    // Destroys the T there is and creates a new one in its place, keeping its spot in
    // the destruction order. Adds T when there's none yet.
    template <class T, class... Args>
    T& replace(Args&&... args) {
        for (Entry& entry : m_entries) {
            if (entry.type != std::type_index(typeid(T))) continue;
            entry.subsystem.reset();   // the old one goes before the new one is built
            auto owned = std::make_unique<T>(std::forward<Args>(args)...);
            T& result = *owned;
            entry.subsystem = std::move(owned);
            return result;
        }
        return add<T>(std::forward<Args>(args)...);
    }

    void clear() {
        while (!m_entries.empty()) m_entries.pop_back();   // newest first
    }
    bool empty() const { return m_entries.empty(); }

private:
    struct Entry {
        std::type_index type;
        std::unique_ptr<Subsystem> subsystem;
    };
    std::vector<Entry> m_entries;
};

}   // namespace shockwave
