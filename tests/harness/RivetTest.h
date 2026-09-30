#pragma once

// Minimal internal test harness for Rivet (no third-party test framework).
//
// Usage:
//     RIVET_TEST(myTest) {
//         CHECK(1 + 1 == 2);
//         CHECK_EQ(42, answer);
//         CHECK_NEAR(0.5, 0.5 + 1e-12, 1e-9);
//     }
//
// Tests self-register; TestMain.cpp runs every registered test and returns
// a non-zero exit code if any test failed.

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace rivet::test {

struct TestCase {
    std::string name;
    void (*fn)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct CheckFailure : std::runtime_error {
    explicit CheckFailure(const char* what) : std::runtime_error(what) {}
};

inline void reportCheck(bool ok, const char* expr, const char* file, int line) {
    if (!ok) {
        std::fprintf(stderr, "    CHECK failed at %s:%d: %s\n", file, line, expr);
        throw CheckFailure(expr);
    }
}

template <typename A, typename B>
void reportEq(const A& a, const B& b, const char* ea, const char* eb, const char* file, int line) {
    if (!(a == b)) {
        std::fprintf(stderr, "    CHECK_EQ failed at %s:%d: %s == %s\n", file, line, ea, eb);
        throw CheckFailure("CHECK_EQ");
    }
}

// UTF-32 text from a (valid) UTF-8 narrow literal. Tests use this instead of
// U"..." literals: GCC on Darwin (seen with Homebrew GCC 16) emits non-char
// string literals into __TEXT,__const under an assembler-local "L" label, so
// with .subsections_via_symbols the literal belongs to whatever symbol
// precedes it in that section. When that symbol is a weak one (e.g. a
// typeinfo name), the linker coalesces it with another object's copy and the
// literal's address resolves into unrelated data. Narrow literals live in
// __cstring, which the linker splits by content, so they are unaffected.
inline std::u32string utf32(std::string_view utf8) {
    std::u32string out;
    for (std::size_t i = 0; i < utf8.size();) {
        const auto lead = static_cast<unsigned char>(utf8[i]);
        const std::size_t length = lead < 0x80u ? 1 : lead < 0xE0u ? 2 : lead < 0xF0u ? 3 : 4;
        char32_t codePoint = length == 1 ? lead : length == 2 ? (lead & 0x1Fu) : length == 3 ? (lead & 0x0Fu) : (lead & 0x07u);
        for (std::size_t k = 1; k < length && i + k < utf8.size(); ++k) {
            codePoint = (codePoint << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3Fu);
        }
        out.push_back(codePoint);
        i += length;
    }
    return out;
}

inline int runAll() {
    std::size_t failedTests = 0;
    for (const auto& testCase : registry()) {
        try {
            testCase.fn();
            std::fprintf(stderr, "[ ok ] %s\n", testCase.name.c_str());
        } catch (const CheckFailure&) {
            ++failedTests;
            std::fprintf(stderr, "[FAIL] %s\n", testCase.name.c_str());
        } catch (const std::exception& e) {
            ++failedTests;
            std::fprintf(stderr, "[FAIL] %s (uncaught exception: %s)\n", testCase.name.c_str(), e.what());
        } catch (...) {
            ++failedTests;
            std::fprintf(stderr, "[FAIL] %s (unknown exception)\n", testCase.name.c_str());
        }
    }
    std::fprintf(stderr, "%zu test(s), %zu failed\n", registry().size(), failedTests);
    return failedTests == 0 ? 0 : 1;
}

} // namespace rivet::test

#define RIVET_TEST(name)                                                        \
    static void rivet_test_fn_##name();                                         \
    static const bool rivet_test_reg_##name = [] {                              \
        rivet::test::registry().push_back({#name, &rivet_test_fn_##name});      \
        return true;                                                            \
    }();                                                                        \
    static void rivet_test_fn_##name()

// Variadic so expressions containing braced initializers (which the
// preprocessor treats as argument commas) still work: CHECK(Point{1, 2}).
#define CHECK(...) rivet::test::reportCheck(static_cast<bool>(__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)

#define CHECK_EQ(a, b) rivet::test::reportEq((a), (b), #a, #b, __FILE__, __LINE__)

#define CHECK_GE(a, b) rivet::test::reportCheck((a) >= (b), #a " >= " #b, __FILE__, __LINE__)

#define CHECK_GT(a, b) rivet::test::reportCheck((a) > (b), #a " > " #b, __FILE__, __LINE__)

#define CHECK_LE(a, b) rivet::test::reportCheck((a) <= (b), #a " <= " #b, __FILE__, __LINE__)

#define CHECK_LT(a, b) rivet::test::reportCheck((a) < (b), #a " < " #b, __FILE__, __LINE__)

#define CHECK_NEAR(a, b, eps)                                                   \
    rivet::test::reportCheck(std::abs((a) - (b)) <= (eps), #a " ~= " #b, __FILE__, __LINE__)
