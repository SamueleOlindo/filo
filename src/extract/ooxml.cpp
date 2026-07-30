#include "extract/ooxml.h"

#include <algorithm>
#include <cstring>
#include <vector>

#include "extract/xmlscan.h"
#include "extract/zip.h"

namespace filo {
namespace {

constexpr size_t kMaxOutputBytes = 8u << 20;

bool startsWith(const std::string& text, const char* prefix) {
    const size_t length = std::strlen(prefix);
    return text.size() >= length && std::memcmp(text.data(), prefix, length) == 0;
}

bool endsWith(const std::string& text, const char* suffix) {
    const size_t length = std::strlen(suffix);
    return text.size() >= length &&
           std::memcmp(text.data() + text.size() - length, suffix, length) == 0;
}

// "Natural" ordering: slide2 before slide10.
// With alphabetical ordering the slides would come out 1, 10, 11, 2 — and the
// extracted text would tell the presentation in the wrong order.
bool naturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const bool digitA = a[i] >= '0' && a[i] <= '9';
        const bool digitB = b[j] >= '0' && b[j] <= '9';
        if (digitA && digitB) {
            size_t endA = i, endB = j;
            while (endA < a.size() && a[endA] >= '0' && a[endA] <= '9') ++endA;
            while (endB < b.size() && b[endB] >= '0' && b[endB] <= '9') ++endB;
            const unsigned long long numberA = std::strtoull(a.substr(i, endA - i).c_str(), nullptr, 10);
            const unsigned long long numberB = std::strtoull(b.substr(j, endB - j).c_str(), nullptr, 10);
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

// Excel cannot write certain characters straight into the XML and encodes them
// as _xHHHH_ : a line break inside a cell becomes _x000D_. Left untranslated
// they would end up in the index as if they were words, and would show up in
// the previews right in front of the user.
//
// The sequence _x005F_ is the escape of the escape: it stands for a real
// underscore that comes before something that would look like an encoding.
void decodeOoxmlEscapes(std::string* text) {
    if (text->find("_x") == std::string::npos) return;

    std::string out;
    out.reserve(text->size());

    size_t at = 0;
    while (at < text->size()) {
        if (text->compare(at, 2, "_x") == 0 && at + 7 <= text->size() &&
            (*text)[at + 6] == '_') {
            unsigned int cp = 0;
            bool valid = true;
            for (size_t k = 2; k < 6; ++k) {
                const char c = (*text)[at + k];
                if (c >= '0' && c <= '9') cp = cp * 16 + (c - '0');
                else if (c >= 'a' && c <= 'f') cp = cp * 16 + (c - 'a' + 10);
                else if (c >= 'A' && c <= 'F') cp = cp * 16 + (c - 'A' + 10);
                else { valid = false; break; }
            }
            if (valid) {
                if (cp == 0x005F) out.push_back('_');
                else if (cp == 0x000D || cp == 0x000A) out.push_back('\n');
                else if (cp < 0x20) out.push_back(' ');
                else if (cp < 0x80) out.push_back(static_cast<char>(cp));
                else if (cp < 0x800) {
                    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
                at += 7;
                continue;
            }
        }
        out.push_back((*text)[at]);
        ++at;
    }
    text->swap(out);
}

void appendPart(const ZipReader& zip, const ZipEntry& entry, const XmlTextRules& rules,
                std::string* out) {
    if (out->size() >= kMaxOutputBytes) return;
    std::string xml;
    if (!zip.extract(entry, &xml) || xml.empty()) return;
    scanXmlText(xml.data(), xml.size(), rules, out);
    if (!out->empty() && out->back() != '\n') out->push_back('\n');
}

// --- Word -------------------------------------------------------------------
const char* const kWordText[]  = {"w:t", nullptr};
// w:instrText holds the field CODES (MERGEFIELD, HYPERLINK, PAGEREF): that is
// syntax, not prose, and in the index it would poison the scoring.
const char* const kWordSkip[]  = {"w:instrText", "w:delText", nullptr};
const char* const kWordBreak[] = {"w:p", "w:br", "w:tab", "w:tc", nullptr};

// --- Excel ------------------------------------------------------------------
// In an .xlsx the cell text is not in the sheets: it is in a shared table, and
// the sheets point into it by index. But files written in streaming mode
// (Apache POI SXSSF and the like) use "inline" strings: read both.
const char* const kExcelText[]  = {"t", nullptr};
const char* const kExcelBreak[] = {"si", "c", "row", nullptr};

// --- PowerPoint -------------------------------------------------------------
const char* const kSlideText[]  = {"a:t", nullptr};
const char* const kSlideBreak[] = {"a:p", "a:br", nullptr};

// --- OpenDocument -----------------------------------------------------------
// The text sits directly inside the paragraphs; nested elements (text:span,
// text:a) fall under the depth of the paragraph that contains them.
const char* const kOdfText[]  = {"text:p", "text:h", nullptr};
const char* const kOdfBreak[] = {"text:p", "text:h", "text:line-break", "table:table-cell", nullptr};

}  // namespace

OfficeResult extractOfficeXml(const char* data, size_t size, std::string* out) {
    out->clear();
    if (size < 4) return OfficeResult::NotAnArchive;

    // A password-protected OOXML is NOT a ZIP: it is a CFB container with the
    // encrypted package inside. Spotting it here saves wasting a parser on it
    // and lets us tell the user why that file is not searchable.
    static const unsigned char kCfb[] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
    if (size >= 8 && std::memcmp(data, kCfb, 8) == 0) return OfficeResult::Encrypted;

    ZipReader zip;
    if (!zip.open(data, size)) return OfficeResult::NotAnArchive;

    // The type is told from the parts present, not from the extension.
    bool isWord = false, isExcel = false, isSlides = false, isOdf = false;
    for (const ZipEntry& entry : zip.entries()) {
        if (entry.name == "word/document.xml") isWord = true;
        else if (startsWith(entry.name, "xl/")) isExcel = true;
        else if (startsWith(entry.name, "ppt/slides/")) isSlides = true;
        else if (entry.name == "content.xml") isOdf = true;
    }

    std::vector<const ZipEntry*> parts;
    XmlTextRules rules;

    if (isWord) {
        rules = {kWordText, kWordSkip, kWordBreak};
        for (const ZipEntry& entry : zip.entries()) {
            // Besides the body: headers, footers, notes and comments.
            // A contract often has its important clauses right there.
            if (entry.name == "word/document.xml" ||
                startsWith(entry.name, "word/header") ||
                startsWith(entry.name, "word/footer") ||
                entry.name == "word/footnotes.xml" ||
                entry.name == "word/endnotes.xml" ||
                entry.name == "word/comments.xml") {
                parts.push_back(&entry);
            }
        }
    } else if (isSlides) {
        rules = {kSlideText, nullptr, kSlideBreak};
        for (const ZipEntry& entry : zip.entries()) {
            if (startsWith(entry.name, "ppt/slides/slide") && endsWith(entry.name, ".xml")) {
                parts.push_back(&entry);
            } else if (startsWith(entry.name, "ppt/notesSlides/notesSlide") &&
                       endsWith(entry.name, ".xml")) {
                parts.push_back(&entry);
            }
        }
    } else if (isExcel) {
        rules = {kExcelText, nullptr, kExcelBreak};
        for (const ZipEntry& entry : zip.entries()) {
            if (entry.name == "xl/sharedStrings.xml" ||
                (startsWith(entry.name, "xl/worksheets/sheet") && endsWith(entry.name, ".xml"))) {
                parts.push_back(&entry);
            }
        }
    } else if (isOdf) {
        rules = {kOdfText, nullptr, kOdfBreak};
        for (const ZipEntry& entry : zip.entries()) {
            if (entry.name == "content.xml") parts.push_back(&entry);
        }
    } else {
        return OfficeResult::Unsupported;
    }

    if (parts.empty()) return OfficeResult::Unsupported;

    std::sort(parts.begin(), parts.end(),
              [](const ZipEntry* a, const ZipEntry* b) { return naturalLess(a->name, b->name); });

    for (const ZipEntry* entry : parts) appendPart(zip, *entry, rules, out);
    decodeOoxmlEscapes(out);

    return out->empty() ? OfficeResult::Empty : OfficeResult::Ok;
}

}  // namespace filo
