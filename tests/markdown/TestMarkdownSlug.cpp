// SPDX-License-Identifier: MPL-2.0
#include "RivetTest.h"

#include "markdown/MarkdownSlug.hpp"

using namespace rivet::markdown;

RIVET_TEST(slugAsciiBasics) {
    CHECK_EQ(makeSlug("Hello World"), "hello-world");
    CHECK_EQ(makeSlug("Hello, World! (2024)"), "hello-world-2024");
    CHECK_EQ(makeSlug("snake_case and kebab-case"), "snake_case-and-kebab-case");
    CHECK_EQ(makeSlug("  leading"), "--leading"); // GitHub keeps one '-' per space
    CHECK_EQ(makeSlug("C++ & C#"), "c--c");
}

RIVET_TEST(slugKeepsUnicodeLetters) {
    CHECK_EQ(makeSlug("Привет, мир!"), "привет-мир");
    CHECK_EQ(makeSlug("Заголовок Второго Уровня"), "заголовок-второго-уровня");
    CHECK_EQ(makeSlug("Ёж"), "ёж");
    CHECK_EQ(makeSlug("Ünïcödé Straße"), "ünïcödé-straße");
    CHECK_EQ(makeSlug("日本語 テキスト"), "日本語-テキスト");
}

RIVET_TEST(slugDropsSymbolsAndEmoji) {
    CHECK_EQ(makeSlug("Done \xE2\x9C\x85 and \xF0\x9F\x98\x80"), "done--and-");
    CHECK_EQ(makeSlug("a \xE2\x80\x94 b"), "a--b"); // em dash
}

RIVET_TEST(slugEmptyFallsBack) {
    CHECK_EQ(makeSlug(""), "section");
    CHECK_EQ(makeSlug("!!!"), "section");
}

RIVET_TEST(slugInvalidUtf8IsIgnored) {
    CHECK_EQ(makeSlug("a\xFF" "b"), "ab");
}

RIVET_TEST(slugAllocatorDeduplicates) {
    SlugAllocator a;
    CHECK_EQ(a.allocate("Intro"), "intro");
    CHECK_EQ(a.allocate("Intro"), "intro-1");
    CHECK_EQ(a.allocate("Intro"), "intro-2");
    CHECK_EQ(a.allocate("Intro 1"), "intro-1-1"); // "intro-1" was taken by the duplicate
    CHECK_EQ(a.allocate("Intro-1"), "intro-1-2");
    CHECK_EQ(a.allocate("Привет"), "привет");
    CHECK_EQ(a.allocate("Привет"), "привет-1");
}
