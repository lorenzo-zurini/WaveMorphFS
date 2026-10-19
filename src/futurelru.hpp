// SPDX-License-Identifier: GPL-3.0-or-later
// A bounded cache of values that are produced once and shared by the readers of
// the moment (decoded packets, encoded frames), with in-flight production.
#pragma once

#include <chrono>
#include <cstdint>
#include <future>
#include <list>
#include <map>
#include <memory>

namespace wm {

/// Entries are shared futures: in flight until the producer that claimed the entry
/// fulfils its promise. Only finished entries are evicted (an in-flight one has a
/// producer and perhaps waiters), least recently used first, while the total cost
/// of the entries exceeds the budget.
///
/// Not synchronised: the owner's mutex guards every call. Fulfilling a promise
/// needs no lock, so producers work outside it.
template <class K, class V>
class FutureLru {
public:
    using Future = std::shared_future<V>;
    using Promise = std::shared_ptr<std::promise<V>>;

    explicit FutureLru(uint64_t budget) : budget_(budget) {}

    /// The entry for `k`, marked as just used. If there is none, a new in-flight
    /// entry is made and its promise stored in `*claimed`: the caller must fulfil
    /// it (with a value or an exception). `*claimed` is left alone otherwise.
    Future get(const K& k, uint64_t cost, Promise* claimed) {
        if (auto it = map_.find(k); it != map_.end()) {
            lru_.splice(lru_.begin(), lru_, it->second.pos);
            return it->second.future;
        }
        auto p = std::make_shared<std::promise<V>>();
        Future f = p->get_future().share();
        lru_.push_front(k);
        map_.emplace(k, Entry{f, cost, p.get(), lru_.begin()});
        total_ += cost;
        *claimed = std::move(p);
        return f;
    }

    /// Evict finished entries, least recently used first, until the total cost is
    /// within the budget. Entries for which `keep(key)` is true stay (those a caller
    /// is about to use).
    template <class Keep>
    void trim(Keep keep) {
        for (auto it = lru_.end(); total_ > budget_ && it != lru_.begin();) {
            --it;
            auto e = map_.find(*it);
            if (keep(*it) || !ready(e->second.future)) continue;
            total_ -= e->second.cost;
            map_.erase(e);
            it = lru_.erase(it);
        }
    }

    /// Drop every finished entry.
    void drop_ready() {
        for (auto it = map_.begin(); it != map_.end();) {
            if (!ready(it->second.future)) {
                ++it;
                continue;
            }
            total_ -= it->second.cost;
            lru_.erase(it->second.pos);
            it = map_.erase(it);
        }
    }

    /// Forget the entry for `k` if it is still the one claimed with `owner` (after
    /// its production failed, so that a later read tries again; the entry may have
    /// been evicted and claimed anew meanwhile).
    void forget(const K& k, const std::promise<V>* owner) {
        auto it = map_.find(k);
        if (it == map_.end() || it->second.owner != owner) return;
        total_ -= it->second.cost;
        lru_.erase(it->second.pos);
        map_.erase(it);
    }

    size_t size() const { return map_.size(); }
    uint64_t total_cost() const { return total_; }

private:
    static bool ready(const Future& f) { return f.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }

    struct Entry {
        Future future;
        uint64_t cost;
        const std::promise<V>* owner;  // identifies this claim (see forget)
        typename std::list<K>::iterator pos;
    };
    std::map<K, Entry> map_;
    std::list<K> lru_;  // most recently used first
    uint64_t budget_;
    uint64_t total_ = 0;
};

}  // namespace wm
