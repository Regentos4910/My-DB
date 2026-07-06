// pager.h — fixed-size page file I/O + free-list page allocator.
//
// File layout:
//   page 0            = header page (magic, page size, catalog root, next
//                        table id, free list head, page count)
//   page 1            = catalog b-tree root (created on fresh file)
//   pages 2..N        = table b-tree pages, allocated on demand
//
// ponytail: single-writer, no WAL/journal. A crash mid-write can leave a
// torn page. Fine for a college demo; add a journal file if durability
// under crashes ever matters.
#pragma once
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace mydb {

constexpr uint32_t PAGE_SIZE = 4096;
constexpr uint32_t HEADER_PAGE_ID = 0;
constexpr uint32_t CATALOG_ROOT_PAGE_ID = 1;
constexpr char MAGIC[4] = {'M', 'Y', 'D', 'B'};

using Page = std::array<uint8_t, PAGE_SIZE>;

// Layout of the header page (all fields little-endian, plain memcpy since
// we always read/write on the same machine/architecture).
struct HeaderData {
    char magic[4];
    uint32_t page_size;
    uint32_t catalog_root_page;
    uint32_t next_table_id;
    uint32_t free_list_head;   // 0 = empty
    uint32_t num_pages;        // total pages currently in the file
};

class Pager {
public:
    explicit Pager(const std::string& path) : path_(path) {
        bool exists = std::ifstream(path).good();
        file_.open(path, std::ios::in | std::ios::out | std::ios::binary);
        if (!exists || !file_.good()) {
            // create fresh file
            file_.clear();
            file_.open(path, std::ios::out | std::ios::binary);
            file_.close();
            file_.open(path, std::ios::in | std::ios::out | std::ios::binary);
            init_fresh();
        } else {
            load_header();
        }
    }

    void read_page(uint32_t page_id, Page& out) {
        file_.seekg(static_cast<std::streamoff>(page_id) * PAGE_SIZE);
        file_.read(reinterpret_cast<char*>(out.data()), PAGE_SIZE);
        if (!file_) throw std::runtime_error("pager: read_page failed for " + std::to_string(page_id));
    }

    void write_page(uint32_t page_id, const Page& in) {
        file_.seekp(static_cast<std::streamoff>(page_id) * PAGE_SIZE);
        file_.write(reinterpret_cast<const char*>(in.data()), PAGE_SIZE);
        if (!file_) throw std::runtime_error("pager: write_page failed for " + std::to_string(page_id));
        file_.flush();
    }

    // Allocate a page: reuse from the free list if possible, else grow the file.
    uint32_t allocate_page() {
        uint32_t id;
        if (hdr_.free_list_head != 0) {
            id = hdr_.free_list_head;
            Page p;
            read_page(id, p);
            uint32_t next;
            std::memcpy(&next, p.data(), sizeof(next));
            hdr_.free_list_head = next;
        } else {
            id = hdr_.num_pages;
            hdr_.num_pages++;
        }
        Page zero{};
        write_page(id, zero);
        flush_header();
        return id;
    }

    void free_page(uint32_t page_id) {
        Page p{};
        uint32_t next = hdr_.free_list_head;
        std::memcpy(p.data(), &next, sizeof(next));
        write_page(page_id, p);
        hdr_.free_list_head = page_id;
        flush_header();
    }

    uint32_t catalog_root() const { return hdr_.catalog_root_page; }
    void set_catalog_root(uint32_t page_id) { hdr_.catalog_root_page = page_id; flush_header(); }

    uint32_t next_table_id() { uint32_t id = hdr_.next_table_id++; flush_header(); return id; }

private:
    void init_fresh() {
        std::memcpy(hdr_.magic, MAGIC, 4);
        hdr_.page_size = PAGE_SIZE;
        hdr_.num_pages = 2; // header page + catalog root
        hdr_.free_list_head = 0;
        hdr_.next_table_id = 1;
        hdr_.catalog_root_page = CATALOG_ROOT_PAGE_ID;
        flush_header();
        // zero the catalog root leaf page (see btree.h for leaf layout)
        Page catalog_page{};
        catalog_page[0] = 3; // PageType::LEAF
        write_page(CATALOG_ROOT_PAGE_ID, catalog_page);
    }

    void load_header() {
        Page p;
        read_page(HEADER_PAGE_ID, p);
        std::memcpy(&hdr_, p.data(), sizeof(HeaderData));
        if (std::memcmp(hdr_.magic, MAGIC, 4) != 0)
            throw std::runtime_error("pager: not a mydb file: " + path_);
    }

    void flush_header() {
        Page p{};
        std::memcpy(p.data(), &hdr_, sizeof(HeaderData));
        write_page(HEADER_PAGE_ID, p);
    }

    std::string path_;
    std::fstream file_;
    HeaderData hdr_{};
};

} // namespace mydb
