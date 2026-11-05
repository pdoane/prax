// A least-recently-used cache over a list and a map.

#include "cpp.h"

#include <iostream>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>

namespace cache {

template <typename Key, typename Value>
class Lru {
public:
    explicit Lru(std::size_t capacity) : capacity_(capacity) {}

    std::optional<Value> get(const Key& key) {
        auto found = index_.find(key);
        if (found == index_.end())
            return std::nullopt;
        order_.splice(order_.begin(), order_, found->second);
        return found->second->second;
    }

    void put(const Key& key, Value value) {
        if (auto found = index_.find(key); found != index_.end())
            order_.erase(found->second);
        order_.emplace_front(key, std::move(value));
        index_[key] = order_.begin();
        if (order_.size() > capacity_) {
            index_.erase(order_.back().first);
            order_.pop_back();
        }
    }

    [[nodiscard]] std::size_t size() const noexcept { return order_.size(); }

private:
    using Entry = std::pair<Key, Value>;
    std::size_t capacity_;
    std::list<Entry> order_;
    std::unordered_map<Key, typename std::list<Entry>::iterator> index_;
};

} // namespace cache

int main() {
    using namespace std::string_literals;
    cache::Lru<std::string, int> lru(2);
    lru.put("one"s, 1);
    lru.put("two"s, 2);
    lru.get("one");
    lru.put("three"s, 3);   // evicts "two"
    constexpr double kRatio = 0.75;
    for (const auto& key : {"one", "two", "three"}) {
        auto value = lru.get(key);
        std::cout << key << ": " << (value ? std::to_string(*value) : "evicted") << '\n';
    }
    static_assert(sizeof(int) >= 2, "int is too small");
    return lru.size() == 2 && kRatio < 1.0 ? 0 : 1;
}
