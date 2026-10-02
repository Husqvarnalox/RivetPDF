// SPDX-License-Identifier: MPL-2.0
// Post-edit search policy: only changes that can alter the text results
// (order / page set / content) restart a search; annotation-only and
// raster-only changes keep the found matches and the active match.
#include "RivetTest.h"

#include "fakes/AnnotationTestSupport.hpp"

#include "editor/PageModel.hpp"
#include "editor/TextSearchController.hpp"

#include <cstddef>
#include <optional>

using rivet::editor::PageModelChange;
using rivet::editor::TextSearchController;
using rivet::test::AnnotationFixture;
using rivet::test::settle;

namespace {

struct Searched {
    Searched() : search(*f.session, f.session->textService()) {
        search.start("PAGE-");
        CHECK(settle(f.dispatcher, [&] { return !search.searching(); }));
        CHECK_EQ(search.matchCount(), std::size_t{3});
        search.setCurrentIndex(std::size_t{2});
    }
    AnnotationFixture f{3};
    TextSearchController search;
};

} // namespace

RIVET_TEST(searchKeepsMatchesOnAnnotationOnlyChange) {
    Searched s;
    PageModelChange change;
    change.annotationsChanged = {s.f.id(0)};
    s.search.handlePageModelChanged(change);
    CHECK(!s.search.searching());
    CHECK_EQ(s.search.matchCount(), std::size_t{3});
    CHECK(s.search.currentIndex() == std::optional<std::size_t>{2});
}

RIVET_TEST(searchKeepsMatchesOnRasterOnlyChange) {
    Searched s;
    PageModelChange change;
    change.rasterChanged = {s.f.id(1)};
    s.search.handlePageModelChanged(change);
    CHECK(!s.search.searching());
    CHECK_EQ(s.search.matchCount(), std::size_t{3});
    CHECK(s.search.currentIndex() == std::optional<std::size_t>{2});
}

RIVET_TEST(searchRestartsOnOrderAddRemoveAndContentChanges) {
    {
        Searched s;
        PageModelChange change;
        change.orderChanged = true;
        s.search.handlePageModelChanged(change);
        CHECK(s.search.searching());
        CHECK(settle(s.f.dispatcher, [&] { return !s.search.searching(); }));
        CHECK_EQ(s.search.matchCount(), std::size_t{3});
    }
    {
        Searched s;
        PageModelChange change;
        change.removed = {s.f.id(0)};
        s.search.handlePageModelChanged(change);
        CHECK(s.search.searching());
        CHECK(settle(s.f.dispatcher, [&] { return !s.search.searching(); }));
    }
    {
        Searched s;
        PageModelChange change;
        change.added = {s.f.id(0)};
        s.search.handlePageModelChanged(change);
        CHECK(s.search.searching());
        CHECK(settle(s.f.dispatcher, [&] { return !s.search.searching(); }));
    }
    {
        Searched s;
        PageModelChange change;
        change.contentChanged = {s.f.id(0)};
        s.search.handlePageModelChanged(change);
        CHECK(s.search.searching());
        CHECK(settle(s.f.dispatcher, [&] { return !s.search.searching(); }));
    }
}
