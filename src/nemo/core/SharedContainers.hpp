#pragma once

#include <algorithm>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// Structurally shared storage for the persistent document model (issue #72).
//
// A document version is a set of handles to these containers. Copying a
// document therefore copies handles, not payload: history, gesture previews
// and render snapshots retain the exact same storage until a controlled
// mutation replaces the container whose records actually changed.
//
// `CowVector` is a chunked sequence: chunks are shared independently, and the
// chunk index is shared until it must change. A mutation copies the index and
// only the chunks it touches, so the cost of one edit is bounded by the
// records it changes plus the O(size/chunk-capacity) index, never by a
// document-sized payload copy. Iteration order is the authored insertion
// order, matching the sequence semantics this storage replaces.
//
// `CowMap` is the same idea for the small ordered maps the model keeps
// (document sources, retired identity watermarks). It preserves the ordered
// map interface callers already use.

namespace nemo {

// A chunked, copy-on-write sequence. Copy is O(1). Non-const access clones
// the index (and the touched chunk) once per version, after which the version
// owns its own storage and further mutations of that version are in place.
template <class T, std::size_t kChunkCapacity = 16>
class CowVector {
    struct Index;

public:
    using Chunk = std::vector<T>;

    CowVector() = default;
    CowVector(std::initializer_list<T> values) { assign(std::vector<T>(values)); }
    explicit CowVector(std::vector<T> values) { assign(std::move(values)); }
    CowVector(const CowVector&) = default;
    CowVector(CowVector&&) noexcept = default;
    CowVector& operator=(const CowVector&) = default;
    CowVector& operator=(CowVector&&) noexcept = default;
    ~CowVector() = default;

    [[nodiscard]] std::size_t size() const noexcept { return index_ ? index_->size : 0; }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }

    [[nodiscard]] const T& operator[](std::size_t position) const {
        const Location where = locate(*index_, position);
        return (*index_->chunks[where.chunk])[where.offset];
    }
    [[nodiscard]] const T& at(std::size_t position) const {
        if (position >= size())
            throw std::out_of_range("CowVector position " + std::to_string(position) + " is out of range");
        return (*this)[position];
    }
    [[nodiscard]] const T& front() const { return (*this)[0]; }
    [[nodiscard]] const T& back() const { return (*this)[size() - 1]; }

    // Non-const element access copies the touched chunk once per version, then
    // hands out this version's own storage.
    [[nodiscard]] T& operator[](std::size_t position) { return mutableAt(position); }
    [[nodiscard]] T& front() { return mutableAt(0); }
    [[nodiscard]] T& back() { return mutableAt(size() - 1); }

    // Chunk structure, exposed so a caller can compare two versions of the
    // same sequence without touching records that still share a chunk.
    [[nodiscard]] bool sharesStorageWith(const CowVector& other) const noexcept {
        return index_.get() == other.index_.get();
    }
    [[nodiscard]] std::size_t chunkCount() const noexcept { return index_ ? index_->chunks.size() : 0; }
    [[nodiscard]] const void* chunkIdentity(std::size_t chunk) const noexcept {
        return index_ ? index_->chunks[chunk].get() : nullptr;
    }
    [[nodiscard]] std::size_t chunkStart(std::size_t chunk) const noexcept {
        return index_ ? index_->starts[chunk] : 0;
    }
    [[nodiscard]] std::size_t chunkSize(std::size_t chunk) const noexcept {
        if (!index_)
            return 0;
        const std::size_t end = chunk + 1 < index_->starts.size() ? index_->starts[chunk + 1] : index_->size;
        return end - index_->starts[chunk];
    }

    // Read-only random access iterator. It addresses the shared chunk index
    // directly, so iterators from two views of the same unchanged storage
    // compare equal, exactly like vector iterators.
    class const_iterator {
    public:
        using iterator_category = std::random_access_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        const_iterator() = default;
        const_iterator(const Index* index, std::size_t position) : index_(index), position_(position) {}

        [[nodiscard]] reference operator*() const {
            const Location where = locate(*index_, position_);
            return (*index_).chunks[where.chunk]->operator[](where.offset);
        }
        [[nodiscard]] pointer operator->() const { return &**this; }
        [[nodiscard]] reference operator[](difference_type offset) const { return *(*this + offset); }

        const_iterator& operator++() {
            ++position_;
            return *this;
        }
        const_iterator operator++(int) {
            const_iterator copy = *this;
            ++position_;
            return copy;
        }
        const_iterator& operator--() {
            --position_;
            return *this;
        }
        const_iterator operator--(int) {
            const_iterator copy = *this;
            --position_;
            return copy;
        }
        const_iterator& operator+=(difference_type offset) {
            position_ = static_cast<std::size_t>(static_cast<difference_type>(position_) + offset);
            return *this;
        }
        const_iterator& operator-=(difference_type offset) {
            position_ = static_cast<std::size_t>(static_cast<difference_type>(position_) - offset);
            return *this;
        }
        [[nodiscard]] friend const_iterator operator+(const_iterator value, difference_type offset) {
            return value += offset;
        }
        [[nodiscard]] friend const_iterator operator+(difference_type offset, const_iterator value) {
            return value += offset;
        }
        [[nodiscard]] friend const_iterator operator-(const_iterator value, difference_type offset) {
            return value -= offset;
        }
        [[nodiscard]] friend difference_type operator-(const const_iterator& left, const const_iterator& right) {
            return static_cast<difference_type>(left.position_) - static_cast<difference_type>(right.position_);
        }
        [[nodiscard]] friend bool operator==(const const_iterator&, const const_iterator&) = default;

        [[nodiscard]] std::size_t position() const noexcept { return position_; }

    private:
        const Index* index_{};
        std::size_t position_{};
    };

    [[nodiscard]] const_iterator begin() const { return const_iterator(index_.get(), 0); }
    [[nodiscard]] const_iterator end() const { return const_iterator(index_.get(), size()); }
    [[nodiscard]] const_iterator cbegin() const { return begin(); }
    [[nodiscard]] const_iterator cend() const { return end(); }

    // Index of the first record matching `predicate`, or size() when absent.
    template <class Predicate>
    [[nodiscard]] std::size_t indexOf(Predicate&& predicate) const {
        const std::size_t count = size();
        for (std::size_t i = 0; i < count; ++i)
            if (predicate((*this)[i]))
                return i;
        return count;
    }
    // Pointer to the first matching record: stable while this version's
    // storage is alive, which is exactly the lifetime of a document version.
    template <class Predicate>
    [[nodiscard]] const T* find(Predicate&& predicate) const {
        const std::size_t found = indexOf(std::forward<Predicate>(predicate));
        return found == size() ? nullptr : &(*this)[found];
    }

    // A private, mutable reference to one record. The whole stored value is
    // copied when the chunk is still shared with another version, so no other
    // version ever observes this mutation.
    [[nodiscard]] T& mutableAt(std::size_t position) {
        Index& index = uniqueIndex();
        const Location where = locate(index, position);
        return uniqueChunk(index, where.chunk)[where.offset];
    }

    void push_back(T value) {
        Index& index = uniqueIndex();
        if (index.chunks.empty() || index.chunks.back()->size() == kChunkCapacity) {
            std::shared_ptr<Chunk> chunk = std::make_shared<Chunk>();
            chunk->reserve(kChunkCapacity);
            chunk->push_back(std::move(value));
            index.starts.push_back(index.size);
            index.chunks.push_back(std::move(chunk));
        } else {
            Chunk& chunk = uniqueChunk(index, index.chunks.size() - 1);
            chunk.push_back(std::move(value));
        }
        ++index.size;
    }

    template <class... Args>
    T& emplace_back(Args&&... args) {
        push_back(T(std::forward<Args>(args)...));
        return mutableAt(size() - 1);
    }

    void pop_back() { erase(size() - 1); }

    void insert(std::size_t position, T value) {
        if (position >= size()) {
            push_back(std::move(value));
            return;
        }
        Index& index = uniqueIndex();
        const Location where = locate(index, position);
        const std::size_t chunkIndexValue = where.chunk;
        Chunk& chunk = uniqueChunk(index, chunkIndexValue);
        chunk.insert(chunk.begin() + static_cast<std::ptrdiff_t>(where.offset), std::move(value));
        ++index.size;
        for (std::size_t i = chunkIndexValue + 1; i < index.starts.size(); ++i)
            ++index.starts[i];
        if (chunk.size() > kChunkCapacity)
            splitChunk(index, chunkIndexValue);
    }

    void erase(std::size_t position) {
        if (position >= size())
            throw std::out_of_range("CowVector erase position " + std::to_string(position) + " is outside size " +
                                    std::to_string(size()));
        Index& index = uniqueIndex();
        const Location where = locate(index, position);
        const std::size_t chunkIndexValue = where.chunk;
        Chunk& chunk = uniqueChunk(index, chunkIndexValue);
        chunk.erase(chunk.begin() + static_cast<std::ptrdiff_t>(where.offset));
        --index.size;
        for (std::size_t i = chunkIndexValue + 1; i < index.starts.size(); ++i)
            --index.starts[i];
        rebalance(index, chunkIndexValue);
    }

    // Removes every record the predicate accepts, copying only the chunks that
    // lose a record. Untouched chunks stay shared with every other version.
    template <class Predicate>
    void eraseIf(Predicate&& predicate) {
        if (!index_ || index_->size == 0)
            return;
        Index& index = uniqueIndex();
        std::vector<std::shared_ptr<Chunk>> keptChunks;
        std::vector<std::size_t> keptStarts;
        std::size_t total = 0;
        for (std::size_t i = 0; i < index.chunks.size(); ++i) {
            const Chunk& source = *index.chunks[i];
            const bool removesAny =
                std::any_of(source.begin(), source.end(), [&](const T& value) { return predicate(value); });
            if (!removesAny) {
                if (!source.empty()) {
                    keptStarts.push_back(total);
                    keptChunks.push_back(index.chunks[i]);
                    total += source.size();
                }
                continue;
            }
            Chunk kept;
            kept.reserve(source.size());
            for (const T& value : source)
                if (!predicate(value))
                    kept.push_back(value);
            if (!kept.empty()) {
                keptStarts.push_back(total);
                total += kept.size();
                keptChunks.push_back(std::make_shared<Chunk>(std::move(kept)));
            }
        }
        index.chunks = std::move(keptChunks);
        index.starts = std::move(keptStarts);
        index.size = total;
    }

    void clear() { index_.reset(); }

    // Element-wise equality; chunks that still share storage are equal without
    // comparing their records.
    [[nodiscard]] bool operator==(const CowVector& other) const {
        if (sharesStorageWith(other))
            return true;
        if (size() != other.size())
            return false;
        for (std::size_t position = 0; position < size(); ++position)
            if (!((*this)[position] == other[position]))
                return false;
        return true;
    }
    [[nodiscard]] bool operator!=(const CowVector& other) const { return !(*this == other); }

    // Bulk replacement used by deserialization and history restoration: the
    // records are moved into freshly chunked storage.
    void assign(std::vector<T> values) {
        index_.reset();
        for (T& value : values)
            push_back(std::move(value));
    }

private:
    struct Index {
        std::vector<std::shared_ptr<Chunk>> chunks;
        // starts[i] is the first position of chunk i, so a record's chunk is
        // the last chunk whose start is not above its position.
        std::vector<std::size_t> starts;
        std::size_t size{0};
    };

    struct Location {
        std::size_t chunk{};
        std::size_t offset{};
    };
    [[nodiscard]] static Location locate(const Index& index, std::size_t position) {
        const std::size_t chunk = chunkIndex(index, position);
        return Location{chunk, position - index.starts[chunk]};
    }
    [[nodiscard]] static std::size_t chunkIndex(const Index& index, std::size_t position) {
        return static_cast<std::size_t>(std::upper_bound(index.starts.begin(), index.starts.end(), position) -
                                        index.starts.begin()) -
               1;
    }

    Index& uniqueIndex() {
        if (!index_)
            index_ = std::make_shared<Index>();
        else if (index_.use_count() != 1)
            index_ = std::make_shared<Index>(*index_);
        return *index_;
    }

    static Chunk& uniqueChunk(Index& index, std::size_t chunkIndexValue) {
        std::shared_ptr<Chunk>& slot = index.chunks[chunkIndexValue];
        if (slot.use_count() != 1)
            slot = std::make_shared<Chunk>(*slot);
        return *slot;
    }

    static void splitChunk(Index& index, std::size_t chunkIndexValue) {
        Chunk& chunk = *index.chunks[chunkIndexValue];
        const std::size_t half = chunk.size() / 2;
        std::shared_ptr<Chunk> right = std::make_shared<Chunk>();
        right->reserve(kChunkCapacity);
        right->assign(std::make_move_iterator(chunk.begin() + static_cast<std::ptrdiff_t>(half)),
                      std::make_move_iterator(chunk.end()));
        chunk.erase(chunk.begin() + static_cast<std::ptrdiff_t>(half), chunk.end());
        // The chunk that followed `chunkIndexValue` keeps its position; it now
        // follows the new chunk, whose size is exactly the amount removed.
        index.starts.insert(index.starts.begin() + static_cast<std::ptrdiff_t>(chunkIndexValue + 1),
                            index.starts[chunkIndexValue] + chunk.size());
        index.chunks.insert(index.chunks.begin() + static_cast<std::ptrdiff_t>(chunkIndexValue + 1), std::move(right));
    }

    // Keeps chunks non-empty and balanced enough that the chunk count stays
    // proportional to size()/kChunkCapacity. Only the affected chunks are
    // copied; the untouched tail keeps sharing storage with other versions.
    static void rebalance(Index& index, std::size_t chunkIndexValue) {
        constexpr std::size_t kMinimum = kChunkCapacity / 2;
        for (;;) {
            if (index.chunks.size() <= 1 || chunkIndexValue >= index.chunks.size())
                return;
            Chunk& chunk = uniqueChunk(index, chunkIndexValue);
            if (chunk.empty()) {
                index.chunks.erase(index.chunks.begin() + static_cast<std::ptrdiff_t>(chunkIndexValue));
                index.starts.erase(index.starts.begin() + static_cast<std::ptrdiff_t>(chunkIndexValue));
                continue;
            }
            if (chunk.size() >= kMinimum || chunkIndexValue + 1 >= index.chunks.size())
                return;
            Chunk& next = uniqueChunk(index, chunkIndexValue + 1);
            const std::size_t moved = std::min(kChunkCapacity - chunk.size(), next.size());
            chunk.insert(chunk.end(), std::make_move_iterator(next.begin()),
                         std::make_move_iterator(next.begin() + static_cast<std::ptrdiff_t>(moved)));
            next.erase(next.begin(), next.begin() + static_cast<std::ptrdiff_t>(moved));
            index.starts[chunkIndexValue + 1] = index.starts[chunkIndexValue] + chunk.size();
            if (!next.empty())
                return;
            index.chunks.erase(index.chunks.begin() + static_cast<std::ptrdiff_t>(chunkIndexValue + 1));
            index.starts.erase(index.starts.begin() + static_cast<std::ptrdiff_t>(chunkIndexValue + 1));
        }
    }

    std::shared_ptr<Index> index_;
};

// Ordered map with the same copy-on-write sharing contract as CowVector. It
// preserves the ordered-map interface (`find`, `upper_bound`, ordered
// iteration, `operator[]`) the model's small keyed stores already use.
template <class Key, class Value, class Compare = std::less<Key>>
class CowMap {
public:
    using Map = std::map<Key, Value, Compare>;
    using const_iterator = typename Map::const_iterator;

    CowMap() = default;
    CowMap(const CowMap&) = default;
    CowMap(CowMap&&) noexcept = default;
    CowMap& operator=(const CowMap&) = default;
    CowMap& operator=(CowMap&&) noexcept = default;
    ~CowMap() = default;

    [[nodiscard]] std::size_t size() const noexcept { return map_ ? map_->size() : 0; }
    [[nodiscard]] bool empty() const noexcept { return size() == 0; }
    [[nodiscard]] const_iterator begin() const { return map_ ? map_->begin() : emptyMap().begin(); }
    [[nodiscard]] const_iterator end() const { return map_ ? map_->end() : emptyMap().end(); }
    [[nodiscard]] const_iterator find(const Key& key) const { return map_ ? map_->find(key) : emptyMap().end(); }
    [[nodiscard]] bool contains(const Key& key) const { return find(key) != end(); }
    [[nodiscard]] std::size_t count(const Key& key) const { return contains(key) ? 1 : 0; }
    [[nodiscard]] const_iterator lower_bound(const Key& key) const {
        return map_ ? map_->lower_bound(key) : emptyMap().end();
    }
    [[nodiscard]] const_iterator upper_bound(const Key& key) const {
        return map_ ? map_->upper_bound(key) : emptyMap().end();
    }
    [[nodiscard]] const_iterator begin(const Key& key) const { return lower_bound(key); }
    [[nodiscard]] const_iterator end(const Key& key) const { return end(); }
    // Throwing lookup for authored keys that must exist.
    [[nodiscard]] const Value& at(const Key& key) const {
        const const_iterator found = find(key);
        if (found == end())
            throw std::out_of_range("CowMap has no entry for key " + describeKey(key));
        return found->second;
    }

    [[nodiscard]] bool operator==(const CowMap& other) const {
        if (size() != other.size())
            return false;
        return std::equal(begin(), end(), other.begin(), [](const auto& left, const auto& right) {
            return left.first == right.first && left.second == right.second;
        });
    }
    [[nodiscard]] bool operator!=(const CowMap& other) const { return !(*this == other); }

    // A private, mutable reference to one value, inserting a default value when
    // absent. The map is copied once per version while it is still shared.
    [[nodiscard]] Value& operator[](const Key& key) { return unique()[key]; }
    [[nodiscard]] Value& at(const Key& key) {
        Map& map = unique();
        const typename Map::iterator found = map.find(key);
        if (found == map.end())
            throw std::out_of_range("CowMap has no entry for key " + describeKey(key));
        return found->second;
    }
    template <class... Args>
    auto emplace(Args&&... args) {
        return unique().emplace(std::forward<Args>(args)...);
    }
    template <class... Args>
    auto try_emplace(const Key& key, Args&&... args) {
        return unique().try_emplace(key, std::forward<Args>(args)...);
    }
    void insert_or_assign(const Key& key, Value value) { unique().insert_or_assign(key, std::move(value)); }
    std::size_t erase(const Key& key) { return unique().erase(key); }
    typename Map::iterator erase(typename Map::const_iterator position) { return unique().erase(position); }
    void clear() { map_.reset(); }

    [[nodiscard]] bool sharesStorageWith(const CowMap& other) const noexcept { return map_.get() == other.map_.get(); }

    [[nodiscard]] Map& unique() {
        if (!map_)
            map_ = std::make_shared<Map>();
        else if (map_.use_count() != 1)
            map_ = std::make_shared<Map>(*map_);
        return *map_;
    }

private:
    // Keys are authored strings or document identities; both print in the
    // diagnostic that names the missing key.
    [[nodiscard]] static std::string describeKey(const Key& key) {
        std::ostringstream stream;
        stream << key;
        return stream.str();
    }

    static const Map& emptyMap() {
        static const Map empty;
        return empty;
    }

    std::shared_ptr<Map> map_;
};

}  // namespace nemo
