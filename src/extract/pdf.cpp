#include "extract/pdf.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <vector>

#include "fpdf_text.h"
#include "fpdfview.h"

namespace filo {
namespace {

constexpr size_t kMaxOutputBytes = 8u << 20;
constexpr int kMaxPages = 3000;

// Below this character count a page counts as having no text: a scanned PDF
// often yields a few stray characters from the margins or the headers, and a
// hard zero is not a reliable test.
constexpr int kMinCharsPerPage = 12;

// PDFium reads the document through this struct instead of from disk. That is
// deliberate: the worker has no filesystem access at all, and the bytes are
// handed to it by the coordinator through shared memory.
struct BufferReader {
    const unsigned char* data;
    size_t size;
};

int readBlock(void* param, unsigned long position, unsigned char* buffer,
              unsigned long length) {
    auto* reader = static_cast<BufferReader*>(param);
    if (position > reader->size || length > reader->size - position) return 0;
    std::memcpy(buffer, reader->data + position, length);
    return 1;
}

// PDFium returns UTF-16; the index wants UTF-8.
void appendUtf16(const unsigned short* text, int count, std::string* out) {
    if (count <= 0) return;
    const int bytes = WideCharToMultiByte(CP_UTF8, 0,
                                          reinterpret_cast<const wchar_t*>(text), count,
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return;
    const size_t at = out->size();
    out->resize(at + static_cast<size_t>(bytes));
    WideCharToMultiByte(CP_UTF8, 0, reinterpret_cast<const wchar_t*>(text), count,
                        out->data() + at, bytes, nullptr, nullptr);
}

bool engineReady = false;

}  // namespace

void initPdfEngine() {
    if (engineReady) return;
    FPDF_LIBRARY_CONFIG config = {};
    config.version = 2;
    FPDF_InitLibraryWithConfig(&config);
    engineReady = true;
}

void shutdownPdfEngine() {
    if (!engineReady) return;
    FPDF_DestroyLibrary();
    engineReady = false;
}

PdfResult extractPdf(const char* data, size_t size, std::string* out, int* pageCount,
                     int* pagesWithText) {
    out->clear();
    *pageCount = 0;
    *pagesWithText = 0;

    if (size < 5 || std::memcmp(data, "%PDF-", 5) != 0) return PdfResult::NotAPdf;
    initPdfEngine();

    BufferReader reader{reinterpret_cast<const unsigned char*>(data), size};
    FPDF_FILEACCESS access = {};
    access.m_FileLen = static_cast<unsigned long>(size);
    access.m_GetBlock = readBlock;
    access.m_Param = &reader;

    FPDF_DOCUMENT document = FPDF_LoadCustomDocument(&access, nullptr);
    if (!document) {
        return FPDF_GetLastError() == FPDF_ERR_PASSWORD ? PdfResult::Encrypted
                                                        : PdfResult::Failed;
    }

    const int pages = std::min(FPDF_GetPageCount(document), kMaxPages);
    *pageCount = pages;

    std::vector<unsigned short> buffer;
    for (int index = 0; index < pages && out->size() < kMaxOutputBytes; ++index) {
        FPDF_PAGE page = FPDF_LoadPage(document, index);
        if (!page) continue;

        FPDF_TEXTPAGE textPage = FPDFText_LoadPage(page);
        if (textPage) {
            const int count = FPDFText_CountChars(textPage);
            if (count > 0) {
                // +1 for the terminator FPDFText_GetText always writes.
                buffer.assign(static_cast<size_t>(count) + 1, 0);
                const int written =
                    FPDFText_GetText(textPage, 0, count, buffer.data());
                if (written > 1) {
                    appendUtf16(buffer.data(), written - 1, out);
                    out->push_back('\n');
                }
                if (count >= kMinCharsPerPage) ++*pagesWithText;
            }
            FPDFText_ClosePage(textPage);
        }
        FPDF_ClosePage(page);
    }

    FPDF_CloseDocument(document);

    if (pages == 0) return PdfResult::Failed;
    // No page with text: it is a scan. That must be SAID, not swallowed.
    if (*pagesWithText == 0) return PdfResult::NeedsOcr;
    return out->empty() ? PdfResult::NeedsOcr : PdfResult::Ok;
}

}  // namespace filo
