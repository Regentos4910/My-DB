// Self-check for the pager + b-tree: assert-based, no framework.
#include "../src/btree.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace mydb;

std::vector<uint8_t> str_val(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }
std::string val_str(const std::vector<uint8_t>& v) { return std::string(v.begin(), v.end()); }

int main() {
    const char* path = "/tmp/mydb_test_btree.db";
    std::remove(path);

    Pager pager(path);
    uint32_t root = BTree::create_empty(pager);
    BTree tree(pager, root);

    // 1. insert + find roundtrip, enough rows to force leaf AND internal splits
    const int N = 200;
    for (int i = 0; i < N; i++) {
        tree.insert(static_cast<uint64_t>(i), str_val("row-" + std::to_string(i)));
    }
    assert(tree.root() != root && "root should have changed after enough splits");

    for (int i = 0; i < N; i++) {
        std::vector<uint8_t> out;
        bool found = tree.find(static_cast<uint64_t>(i), out);
        assert(found);
        assert(val_str(out) == "row-" + std::to_string(i));
    }

    // 2. scan returns everything, in ascending key order
    std::vector<uint64_t> seen;
    tree.scan([&](uint64_t k, const std::vector<uint8_t>&) { seen.push_back(k); return true; });
    assert(seen.size() == static_cast<size_t>(N));
    for (int i = 0; i < N; i++) assert(seen[i] == static_cast<uint64_t>(i));

    // 3. update in place
    assert(tree.update(50, str_val("row-50-updated")));
    std::vector<uint8_t> out;
    assert(tree.find(50, out) && val_str(out) == "row-50-updated");

    // 4. remove, then confirm gone but neighbors intact
    assert(tree.remove(50));
    assert(!tree.find(50, out));
    assert(tree.find(49, out) && val_str(out) == "row-49");
    assert(tree.find(51, out) && val_str(out) == "row-51");
    assert(!tree.remove(50)); // second remove of same key -> false

    // 5. scan count dropped by exactly one after the remove
    seen.clear();
    tree.scan([&](uint64_t k, const std::vector<uint8_t>&) { seen.push_back(k); return true; });
    assert(seen.size() == static_cast<size_t>(N - 1));

    // 6. duplicate insert must throw
    bool threw = false;
    try { tree.insert(1, str_val("dup")); } catch (const std::exception&) { threw = true; }
    assert(threw);

    // 7. oversized value must throw
    threw = false;
    try { tree.insert(999999, std::vector<uint8_t>(MAX_ROW_BYTES + 1, 'x')); }
    catch (const std::exception&) { threw = true; }
    assert(threw);

    // 8. persistence: reopen the file, data survives
    {
        Pager pager2(path);
        BTree tree2(pager2, tree.root());
        std::vector<uint8_t> out2;
        assert(tree2.find(0, out2) && val_str(out2) == "row-0");
        assert(tree2.find(199, out2) && val_str(out2) == "row-199");
        assert(!tree2.find(50, out2));
    }

    std::printf("test_btree: all checks passed (%d rows, tree grew to a multi-level b-tree)\n", N);
    return 0;
}
