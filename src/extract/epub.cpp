#include "extract/epub.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "extract/html.h"
#include "extract/zip.h"

namespace filo {
namespace {

constexpr size_t kMaxOutputBytes = 8u << 20;

bool endsWithIgnoreCase(const std::string& text, const char* suffix) {
    const size_t length = std::strlen(suffix);
    if (text.size() < length) return false;
    for (size_t i = 0; i < length; ++i) {
        char c = text[text.size() - length + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
        if (c != suffix[i]) return false;
    }
    return true;
}

bool naturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool digitA = a[i] >= '0' && a[i] <= '9';
        const bool digitB = b[j] >= '0' && b[j] <= '9';
        if (digitA && digitB) {
            size_t endA = i, endB = j;
            while (endA < a.size() && a[endA] >= '0' && a[endA] <= '9') ++endA;
            while (endB < b.size() && b[endB] >= '0' && b[endB] <= '9') ++endB;
            const unsigned long long numberA =
                std::strtoull(a.substr(i, endA - i).c_str(), nullptr, 10);
            const unsigned long long numberB =
                std::strtoull(b.substr(j, endB - j).c_str(), nullptr, 10);
            if (numberA != numberB) return numberA < numberB;
            i = endA;
            j = endB;
            continue;
        }
        if (a[i] != b[j]) return a[i] < b[j];
        ++i;
        ++j;
    }
    return a.size() < b.size();
}

}  // namespace

bool extractEpub(const char* data, size_t size, std::string* out) {
    out->clear();

    ZipReader zip;
    if (!zip.open(data, size)) return false;

    std::vector<const ZipEntry*> chapters;
    for (const ZipEntry& entry : zip.entries()) {
        if (!endsWithIgnoreCase(entry.name, ".xhtml") &&
            !endsWithIgnoreCase(entry.name, ".html") &&
            !endsWithIgnoreCase(entry.name, ".htm")) {
            continue;
        }
        // Contents page and cover are not text of the work: they are navigation.
        if (entry.name.find("nav") != std::string::npos ||
            entry.name.find("toc") != std::string::npos ||
            entry.name.find("cover") != std::string::npos) {
            continue;
        }
        chapters.push_back(&entry);
    }
    if (chapters.empty()) return false;

    std::sort(chapters.begin(), chapters.end(),
              [](const ZipEntry* a, const ZipEntry* b) { return naturalLess(a->name, b->name); });

    std::string chapter;
    for (const ZipEntry* entry : chapters) {
        if (out->size() >= kMaxOutputBytes) break;
        if (!zip.extract(*entry, &chapter) || chapter.empty()) continue;
        stripHtmlToText(chapter.data(), chapter.size(), out);
        if (!out->empty() && out->back() != '\n') out->push_back('\n');
    }
    return !out->empty();
}

}  // namespace filo
