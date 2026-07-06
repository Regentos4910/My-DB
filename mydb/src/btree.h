// btree.h — on-disk B+tree keyed by uint64_t, storing opaque byte-blob
// values (rows are serialized elsewhere, see row.h).
//
// Simplifications made deliberately for a one-week college project:
//  - Fixed-size leaf slots (value capped at MAX_ROW_BYTES). This trades
//    "arbitrarily long rows" for a much simpler page layout: no slotted
//    directory, no variable-offset bookkeeping. ponytail: raise
//    MAX_ROW_BYTES (and re-derive MAX_LEAF_SLOTS) if you need bigger rows;
//    a real slotted page is the upgrade path.
//  - MAX_INTERNAL_KEYS is capped low on purpose (not because the page is
//    full) so a modest demo dataset (tens of rows) still produces a
//    multi-level tree with visible splits, instead of needing thousands
//    of rows before the root ever splits.
//  - Delete has no underflow/merge step: a leaf can go sparse and stays
//    sparse. Real B-trees rebalance; skipping it is the standard
//    corner cut for a course project. Page reclaim would need it.
//  - Single writer, no locking, no transactions.
#pragma once
#include "pager.h"
#include <algorithm>
#include <functional>
#include <vector>

namespace mydb {

constexpr size_t MAX_ROW_BYTES = 500;
constexpr uint8_t PT_INTERNAL = 2;
constexpr uint8_t PT_LEAF = 3;

constexpr size_t LEAF_HEADER_SIZE = 7;                 // type(1) + num_slots(2) + next_leaf(4)
constexpr size_t LEAF_SLOT_SIZE = 8 + 2 + MAX_ROW_BYTES; // key + len + data
constexpr size_t MAX_LEAF_SLOTS = (PAGE_SIZE - LEAF_HEADER_SIZE) / LEAF_SLOT_SIZE;

constexpr size_t INTERNAL_HEADER_SIZE = 3;             // type(1) + num_keys(2)
constexpr size_t MAX_INTERNAL_KEYS = 4;                // deliberately small, see header note above
constexpr size_t INTERNAL_CHILDREN_BASE = INTERNAL_HEADER_SIZE + MAX_INTERNAL_KEYS * 8;

static_assert(MAX_LEAF_SLOTS >= 2, "page too small for even 2 leaf slots");
static_assert(INTERNAL_CHILDREN_BASE + (MAX_INTERNAL_KEYS + 1) * 4 <= PAGE_SIZE, "internal page layout overflows page");

struct BTreeEntry {
    uint64_t key;
    std::vector<uint8_t> value;
};

class BTree {
public:
    BTree(Pager& pager, uint32_t root_page_id) : pager_(pager), root_page_id_(root_page_id) {}

    static uint32_t create_empty(Pager& pager) {
        uint32_t id = pager.allocate_page();
        Page p{};
        p[0] = PT_LEAF;
        pager.write_page(id, p);
        return id;
    }

    uint32_t root() const { return root_page_id_; }

    void insert(uint64_t key, const std::vector<uint8_t>& value) {
        if (value.size() > MAX_ROW_BYTES)
            throw std::runtime_error("row exceeds fixed b-tree slot capacity (" + std::to_string(MAX_ROW_BYTES) + " bytes)");
        SplitResult r = insert_recursive(root_page_id_, key, value);
        if (r.split) {
            uint32_t new_root_id = pager_.allocate_page();
            Page rootp{};
            write_internal(rootp, {r.sep_key}, {root_page_id_, r.new_page_id});
            pager_.write_page(new_root_id, rootp);
            root_page_id_ = new_root_id;
        }
    }

    bool find(uint64_t key, std::vector<uint8_t>& out) const {
        uint32_t leaf_id = find_leaf_id(key);
        Page p; pager_.read_page(leaf_id, p);
        uint32_t next; std::vector<BTreeEntry> entries;
        read_leaf_entries(p, entries, next);
        for (auto& e : entries) if (e.key == key) { out = e.value; return true; }
        return false;
    }

    bool update(uint64_t key, const std::vector<uint8_t>& new_value) {
        if (new_value.size() > MAX_ROW_BYTES)
            throw std::runtime_error("row exceeds fixed b-tree slot capacity (" + std::to_string(MAX_ROW_BYTES) + " bytes)");
        uint32_t leaf_id = find_leaf_id(key);
        Page p; pager_.read_page(leaf_id, p);
        uint32_t next; std::vector<BTreeEntry> entries;
        read_leaf_entries(p, entries, next);
        bool found = false;
        for (auto& e : entries) if (e.key == key) { e.value = new_value; found = true; break; }
        if (!found) return false;
        write_leaf_entries(p, entries, next);
        pager_.write_page(leaf_id, p);
        return true;
    }

    bool remove(uint64_t key) {
        uint32_t leaf_id = find_leaf_id(key);
        Page p; pager_.read_page(leaf_id, p);
        uint32_t next; std::vector<BTreeEntry> entries;
        read_leaf_entries(p, entries, next);
        size_t before = entries.size();
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                      [&](const BTreeEntry& e) { return e.key == key; }),
                      entries.end());
        if (entries.size() == before) return false;
        write_leaf_entries(p, entries, next); // rebuilding from the filtered list compacts the page
        pager_.write_page(leaf_id, p);
        return true;
    }

    // visitor(key, value) -> return false to stop early
    void scan(const std::function<bool(uint64_t, const std::vector<uint8_t>&)>& visitor) const {
        uint32_t pid = leftmost_leaf();
        while (pid != 0) {
            Page p; pager_.read_page(pid, p);
            uint32_t next; std::vector<BTreeEntry> entries;
            read_leaf_entries(p, entries, next);
            for (auto& e : entries) {
                if (!visitor(e.key, e.value)) return;
            }
            pid = next;
        }
    }

private:
    struct SplitResult {
        bool split = false;
        uint64_t sep_key = 0;
        uint32_t new_page_id = 0;
    };

    static void read_leaf_entries(const Page& p, std::vector<BTreeEntry>& out, uint32_t& next_leaf) {
        out.clear();
        uint16_t num_slots; std::memcpy(&num_slots, &p[1], 2);
        std::memcpy(&next_leaf, &p[3], 4);
        for (uint16_t i = 0; i < num_slots; i++) {
            size_t off = LEAF_HEADER_SIZE + i * LEAF_SLOT_SIZE;
            uint64_t key; std::memcpy(&key, &p[off], 8);
            uint16_t len; std::memcpy(&len, &p[off + 8], 2);
            BTreeEntry e;
            e.key = key;
            e.value.assign(p.begin() + off + 10, p.begin() + off + 10 + len);
            out.push_back(std::move(e));
        }
    }

    static void write_leaf_entries(Page& p, const std::vector<BTreeEntry>& entries, uint32_t next_leaf) {
        if (entries.size() > MAX_LEAF_SLOTS) throw std::runtime_error("btree: leaf overflow (internal bug)");
        p.fill(0);
        p[0] = PT_LEAF;
        uint16_t num_slots = static_cast<uint16_t>(entries.size());
        std::memcpy(&p[1], &num_slots, 2);
        std::memcpy(&p[3], &next_leaf, 4);
        for (size_t i = 0; i < entries.size(); i++) {
            size_t off = LEAF_HEADER_SIZE + i * LEAF_SLOT_SIZE;
            std::memcpy(&p[off], &entries[i].key, 8);
            uint16_t len = static_cast<uint16_t>(entries[i].value.size());
            std::memcpy(&p[off + 8], &len, 2);
            if (len > 0) std::memcpy(&p[off + 10], entries[i].value.data(), len);
        }
    }

    static void read_internal(const Page& p, std::vector<uint64_t>& keys, std::vector<uint32_t>& children) {
        uint16_t num_keys; std::memcpy(&num_keys, &p[1], 2);
        keys.resize(num_keys);
        for (uint16_t i = 0; i < num_keys; i++) std::memcpy(&keys[i], &p[INTERNAL_HEADER_SIZE + i * 8], 8);
        children.resize(num_keys + 1);
        for (uint16_t i = 0; i < num_keys + 1; i++)
            std::memcpy(&children[i], &p[INTERNAL_CHILDREN_BASE + i * 4], 4);
    }

    static void write_internal(Page& p, const std::vector<uint64_t>& keys, const std::vector<uint32_t>& children) {
        if (keys.size() > MAX_INTERNAL_KEYS) throw std::runtime_error("btree: internal overflow (internal bug)");
        p.fill(0);
        p[0] = PT_INTERNAL;
        uint16_t num_keys = static_cast<uint16_t>(keys.size());
        std::memcpy(&p[1], &num_keys, 2);
        for (size_t i = 0; i < keys.size(); i++) std::memcpy(&p[INTERNAL_HEADER_SIZE + i * 8], &keys[i], 8);
        for (size_t i = 0; i < children.size(); i++) std::memcpy(&p[INTERNAL_CHILDREN_BASE + i * 4], &children[i], 4);
    }

    // children[i] covers the key range [keys[i-1], keys[i]); children[0] covers
    // everything below keys[0]; children.back() covers everything >= keys.back().
    static size_t child_index_for_key(const std::vector<uint64_t>& keys, uint64_t key) {
        size_t i = 0;
        while (i < keys.size() && key >= keys[i]) i++;
        return i;
    }

    uint32_t find_leaf_id(uint64_t key) const {
        uint32_t pid = root_page_id_;
        while (true) {
            Page p; pager_.read_page(pid, p);
            if (p[0] == PT_LEAF) return pid;
            std::vector<uint64_t> keys; std::vector<uint32_t> children;
            read_internal(p, keys, children);
            pid = children[child_index_for_key(keys, key)];
        }
    }

    uint32_t leftmost_leaf() const {
        uint32_t pid = root_page_id_;
        while (true) {
            Page p; pager_.read_page(pid, p);
            if (p[0] == PT_LEAF) return pid;
            std::vector<uint64_t> keys; std::vector<uint32_t> children;
            read_internal(p, keys, children);
            pid = children[0];
        }
    }

    SplitResult insert_recursive(uint32_t page_id, uint64_t key, const std::vector<uint8_t>& value) {
        Page p; pager_.read_page(page_id, p);
        if (p[0] == PT_LEAF) {
            uint32_t next_leaf; std::vector<BTreeEntry> entries;
            read_leaf_entries(p, entries, next_leaf);
            for (auto& e : entries)
                if (e.key == key) throw std::runtime_error("btree: duplicate key " + std::to_string(key));
            auto it = std::lower_bound(entries.begin(), entries.end(), key,
                                        [](const BTreeEntry& e, uint64_t k) { return e.key < k; });
            entries.insert(it, BTreeEntry{key, value});
            if (entries.size() <= MAX_LEAF_SLOTS) {
                write_leaf_entries(p, entries, next_leaf);
                pager_.write_page(page_id, p);
                return {};
            }
            size_t mid = entries.size() / 2;
            std::vector<BTreeEntry> left(entries.begin(), entries.begin() + mid);
            std::vector<BTreeEntry> right(entries.begin() + mid, entries.end());
            uint32_t right_id = pager_.allocate_page();
            Page leftp{}; write_leaf_entries(leftp, left, right_id); pager_.write_page(page_id, leftp);
            Page rightp{}; write_leaf_entries(rightp, right, next_leaf); pager_.write_page(right_id, rightp);
            SplitResult r; r.split = true; r.sep_key = right.front().key; r.new_page_id = right_id;
            return r;
        } else {
            std::vector<uint64_t> keys; std::vector<uint32_t> children;
            read_internal(p, keys, children);
            size_t idx = child_index_for_key(keys, key);
            SplitResult child_split = insert_recursive(children[idx], key, value);
            if (!child_split.split) return {};
            keys.insert(keys.begin() + idx, child_split.sep_key);
            children.insert(children.begin() + idx + 1, child_split.new_page_id);
            if (keys.size() <= MAX_INTERNAL_KEYS) {
                write_internal(p, keys, children);
                pager_.write_page(page_id, p);
                return {};
            }
            size_t mid = keys.size() / 2;
            uint64_t promoted = keys[mid];
            std::vector<uint64_t> left_keys(keys.begin(), keys.begin() + mid);
            std::vector<uint64_t> right_keys(keys.begin() + mid + 1, keys.end());
            std::vector<uint32_t> left_children(children.begin(), children.begin() + mid + 1);
            std::vector<uint32_t> right_children(children.begin() + mid + 1, children.end());
            uint32_t right_id = pager_.allocate_page();
            Page leftp{}; write_internal(leftp, left_keys, left_children); pager_.write_page(page_id, leftp);
            Page rightp{}; write_internal(rightp, right_keys, right_children); pager_.write_page(right_id, rightp);
            SplitResult r; r.split = true; r.sep_key = promoted; r.new_page_id = right_id;
            return r;
        }
    }

    Pager& pager_;
    uint32_t root_page_id_;
};

} // namespace mydb
