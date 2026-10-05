// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <string>
#include <string_view>
#include <unordered_set>

namespace rivet::markdown {

// GitHub-like heading slug, deterministic and locale independent:
//  - letters are lower-cased (ASCII, Latin-1/Extended-A, Greek, Cyrillic);
//  - non-ASCII letters (Cyrillic, CJK, ...) and digits are kept;
//  - every space/tab becomes '-' (runs are not collapsed, as on GitHub);
//  - '-' and '_' are kept; all other punctuation/symbols are dropped.
// An empty result becomes "section".
std::string makeSlug(std::string_view headingText);

// Hands out document-unique slugs: the first "foo" stays "foo", then "foo-1",
// "foo-2", ... (skipping candidates that are already taken).
class SlugAllocator {
public:
    std::string allocate(std::string_view headingText);

private:
    std::unordered_set<std::string> used_;
};

} // namespace rivet::markdown
