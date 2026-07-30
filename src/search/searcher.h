#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "index/file_index.h"

namespace filo {

// Searches `lowerNeedle` across every record name, spreading the work over
// several cores.
//
// Returns the indices of the matching records, in ascending order. With
// threadCount = 0 it uses every available core.
std::vector<uint32_t> searchNames(const FileIndex& index,
                                  const std::wstring& lowerNeedle,
                                  unsigned threadCount = 0);

// How well a name answers the query. Higher = more relevant.
//
// Needed because searchNames returns results in MFT order, which has nothing to
// do with relevance: without this, "invoice.pdf" can land below
// "old-invoice-2019-copy (3).pdf" purely because it was created later.
double nameRelevance(std::wstring_view name, const std::wstring& lowerNeedle);

}  // namespace filo
