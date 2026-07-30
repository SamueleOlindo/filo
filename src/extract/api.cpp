// The single entry point of the extractors, called ONLY inside the worker.
//
// Everything in here that does offset arithmetic on bytes of arbitrary origin
// (ZIP, XML, CFB, RTF, HTML, PDF) lives behind this function, which in turn
// lives behind the SEH guard and inside a process we can afford to lose.

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "content/gate.h"
#include "content/normalize.h"
#include "content/selector.h"
#include "extract/epub.h"
#include "extract/html.h"
#include "extract/ooxml.h"
#include "extract/rtf.h"
#include "worker/protocol.h"

#if FILO_HAVE_PDFIUM
#include "extract/pdf.h"
#endif

namespace {

// Recursion the compiler cannot eliminate: it is there to really blow the
// stack in the resilience test.
__declspec(noinline) int burnStack(int depth) {
    volatile char padding[4096];
    padding[0] = static_cast<char>(depth & 0x7F);
    if (depth > 1000000) return padding[0];
    return burnStack(depth + 1) + padding[0];
}

void injectFault(unsigned int fault) {
    switch (static_cast<filo::FaultInjection>(fault)) {
        case filo::FaultInjection::AccessViolation: {
            volatile int* nowhere = nullptr;
            *nowhere = 42;                       // access violation
            break;
        }
        case filo::FaultInjection::InfiniteLoop: {
            for (;;) {
                volatile int spin = 0;
                (void)spin;                      // never ends: the watchdog has to step in
            }
        }
        case filo::FaultInjection::StackOverflow:
            burnStack(0);
            break;
        case filo::FaultInjection::Abort:
            RaiseException(0xE0000001, EXCEPTION_NONCONTINUABLE, 0, nullptr);
            break;
        case filo::FaultInjection::None:
        default:
            break;
    }
}

}  // namespace

extern "C" int filoExtractRaw(unsigned int kind, const char* input,
                              unsigned long long inputSize, char* output,
                              unsigned long long outputCapacity,
                              unsigned long long* outputSize, unsigned int fault) {
    *outputSize = 0;
    if (fault != 0) injectFault(fault);

    if (inputSize == 0) return static_cast<int>(filo::WorkerStatus::Empty);

    const auto contentKind = static_cast<filo::ContentKind>(kind);
    std::string text;

    if (contentKind == filo::ContentKind::OfficeXml ||
        contentKind == filo::ContentKind::OpenDocument) {
        std::string extracted;
        switch (filo::extractOfficeXml(input, static_cast<size_t>(inputSize), &extracted)) {
            case filo::OfficeResult::Ok:
                break;
            case filo::OfficeResult::Encrypted:
                return static_cast<int>(filo::WorkerStatus::Encrypted);
            case filo::OfficeResult::Empty:
                return static_cast<int>(filo::WorkerStatus::Empty);
            default:
                return static_cast<int>(filo::WorkerStatus::Failed);
        }
        // The extracted text is already UTF-8 (XML is, by definition), but it
        // still needs the same cleanup as every other format: that is what
        // guarantees the paragraph boundaries are the ones the chunker expects
        // to find.
        if (!filo::normalizeToUtf8(extracted.data(), extracted.size(),
                                   filo::TextEncoding::Utf8,
                                   filo::ContentKind::PlainText, &text)) {
            return static_cast<int>(filo::WorkerStatus::Empty);
        }
    } else if (contentKind == filo::ContentKind::Markup ||
               contentKind == filo::ContentKind::RichText ||
               contentKind == filo::ContentKind::Ebook) {
        std::string extracted;
        bool ok = false;
        if (contentKind == filo::ContentKind::Markup) {
            ok = filo::extractHtml(input, static_cast<size_t>(inputSize), &extracted);
        } else if (contentKind == filo::ContentKind::RichText) {
            ok = filo::extractRtf(input, static_cast<size_t>(inputSize), &extracted);
        } else {
            ok = filo::extractEpub(input, static_cast<size_t>(inputSize), &extracted);
        }
        if (!ok || extracted.empty()) return static_cast<int>(filo::WorkerStatus::Empty);

        // The text coming out of the extractors is already UTF-8: all that is
        // needed here is the common cleanup, the one that guarantees the
        // paragraph boundaries the chunker expects.
        if (!filo::normalizeToUtf8(extracted.data(), extracted.size(),
                                   filo::TextEncoding::Utf8,
                                   filo::ContentKind::PlainText, &text)) {
            return static_cast<int>(filo::WorkerStatus::Empty);
        }
#if FILO_HAVE_PDFIUM
    } else if (contentKind == filo::ContentKind::Pdf) {
        std::string extracted;
        int pages = 0, pagesWithText = 0;
        switch (filo::extractPdf(input, static_cast<size_t>(inputSize), &extracted,
                                 &pages, &pagesWithText)) {
            case filo::PdfResult::Ok:
                break;
            case filo::PdfResult::Encrypted:
                return static_cast<int>(filo::WorkerStatus::Encrypted);
            case filo::PdfResult::NeedsOcr:
                // A scan: images, not text. No engine will ever pull a word
                // out of it, and saying so is the only way the user does not
                // conclude the search is broken.
                return static_cast<int>(filo::WorkerStatus::NeedsOcr);
            default:
                return static_cast<int>(filo::WorkerStatus::Failed);
        }
        if (!filo::normalizeToUtf8(extracted.data(), extracted.size(),
                                   filo::TextEncoding::Utf8,
                                   filo::ContentKind::PlainText, &text)) {
            return static_cast<int>(filo::WorkerStatus::Empty);
        }
#endif
    } else {
        const filo::TextEncoding encoding =
            filo::detectEncoding(input, static_cast<size_t>(inputSize));
        if (!filo::normalizeToUtf8(input, static_cast<size_t>(inputSize), encoding,
                                   contentKind, &text)) {
            return static_cast<int>(filo::WorkerStatus::Failed);
        }
    }

    if (text.empty()) return static_cast<int>(filo::WorkerStatus::Empty);

    // The extracted text can outgrow the return buffer: it gets truncated, but
    // we SAY SO, so the interface can report "document only partly indexed"
    // instead of letting the user believe it is all in there.
    bool truncated = false;
    if (text.size() > outputCapacity) {
        text.resize(static_cast<size_t>(outputCapacity));
        truncated = true;
    }
    std::memcpy(output, text.data(), text.size());
    *outputSize = text.size();

    return static_cast<int>(truncated ? filo::WorkerStatus::TooBig
                                      : filo::WorkerStatus::Ok);
}
