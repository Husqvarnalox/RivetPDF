// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "app/UrlPolicy.hpp"

using rivet::app::isAllowedExternalUrlScheme;

RIVET_TEST(allowedSchemesPass) {
    CHECK(isAllowedExternalUrlScheme("http://example.com"));
    CHECK(isAllowedExternalUrlScheme("https://example.com/rivet?a=1"));
    CHECK(isAllowedExternalUrlScheme("mailto:someone@example.com"));
}

RIVET_TEST(unsafeSchemesAreRejected) {
    CHECK(!isAllowedExternalUrlScheme("file:///etc/passwd"));
    CHECK(!isAllowedExternalUrlScheme("javascript:alert(1)"));
    CHECK(!isAllowedExternalUrlScheme("shell:rm -rf /"));
    CHECK(!isAllowedExternalUrlScheme("x-rivet://launch"));
    CHECK(!isAllowedExternalUrlScheme("https:example.com"));   // malformed
    CHECK(!isAllowedExternalUrlScheme("//example.com"));        // scheme-less
    CHECK(!isAllowedExternalUrlScheme(""));
}
