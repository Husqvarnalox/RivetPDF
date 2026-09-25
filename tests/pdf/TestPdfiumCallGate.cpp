#include "RivetTest.h"

// The gate is compiled directly into this test binary (no FPDF_* dependency),
// so these tests run with RIVET_WITH_PDFIUM=OFF as well.
#include "PdfiumCallGate.hpp"

#include <string>
#include <thread>
#include <vector>

namespace {

// Deliberately non-atomic: only the gate protects it.
int& sharedCounter() {
    static int value = 0;
    return value;
}

} // namespace

// 8 threads x 1000 increments of a plain int under the gate: the final value
// must be exactly 8000, proving invocations never overlap.
RIVET_TEST(pdfiumCallGateSerializesInvocations) {
    constexpr int kThreads = 8;
    constexpr int kIterations = 1000;
    int counter = 0;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kIterations; ++i) {
                rivet::pdf::globalPdfiumCallGate().invoke([&counter] { ++counter; });
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }

    CHECK_EQ(counter, kThreads * kIterations);
}

// invoke() forwards the callable's result unchanged: by value, by reference
// (decltype(auto) must preserve the value category), and void.
RIVET_TEST(pdfiumCallGateForwardsReturnValues) {
    const int byValue = rivet::pdf::globalPdfiumCallGate().invoke([] { return 41 + 1; });
    CHECK_EQ(byValue, 42);

    const std::string byValueString =
        rivet::pdf::globalPdfiumCallGate().invoke([] { return std::string("gate"); });
    CHECK_EQ(byValueString, "gate");

    int& byReference =
        rivet::pdf::globalPdfiumCallGate().invoke([]() -> int& { return sharedCounter(); });
    byReference = 7;
    CHECK_EQ(sharedCounter(), 7);
    CHECK(&byReference == &sharedCounter());

    bool voidRan = false;
    rivet::pdf::globalPdfiumCallGate().invoke([&voidRan] { voidRan = true; });
    CHECK(voidRan);
}

// The gate is a process-wide singleton: the same instance is returned from
// multiple threads.
RIVET_TEST(pdfiumCallGateSingletonIsProcessWide) {
    const rivet::pdf::PdfiumCallGate* fromMainThread = nullptr;
    const rivet::pdf::PdfiumCallGate* fromWorker = nullptr;

    std::thread worker([&fromWorker] { fromWorker = &rivet::pdf::globalPdfiumCallGate(); });
    fromMainThread = &rivet::pdf::globalPdfiumCallGate();
    worker.join();

    CHECK(fromWorker != nullptr);
    CHECK_EQ(fromMainThread, fromWorker);
}
