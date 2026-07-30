#pragma once

#include <cstddef>
#include <string>

namespace filo {

// Rules for pulling text out of an XML document.
//
// The lists are terminated by a null pointer.
struct XmlTextRules {
    const char* const* textTags = nullptr;   // text inside these elements is kept
    const char* const* skipTags = nullptr;   // text inside these is thrown away
    const char* const* breakTags = nullptr;  // closing these separates paragraphs
};

// Tag-scanning text extractor that builds no tree at all.
//
// Why not a real XML parser:
//  - In a real document.xml the formatting elements outnumber the text ones by
//    10-50 times. A DOM would allocate a node for every one of them.
//  - By expanding neither DTDs nor custom entities we are immune BY
//    CONSTRUCTION to the "billion laughs" and XXE family of attacks, which has
//    repeatedly hit general-purpose parsers. We are about to feed a parser
//    files of arbitrary origin.
//
// This is not an XML parser: it is a text extractor. It validates nothing, and
// using it for anything else would be wrong.
void scanXmlText(const char* data, size_t size, const XmlTextRules& rules,
                 std::string* out);

}  // namespace filo
