#include "RivetTest.h"
#include "pdf/PdfSystem.hpp"
#include "pdf/PdfPageGeometry.hpp"
#include <cstdio>
#include <memory>
using namespace rivet;
RIVET_TEST(dbgRotation) {
    std::unique_ptr<pdf::PdfEngine> engine = pdf::createEngine();
    if (!engine || !engine->isAvailable()) { std::puts("no pdfium"); return; }
    auto doc = engine->openDocument("tests/pdf/fixtures/markers-5.pdf", {});
    if (!doc) { std::puts("open failed"); return; }
    auto info = (*doc)->pageInfo(0);
    std::printf("orig rot=%d size=%fx%f crop=%f %f %f %f media=%f %f %f %f\n",
        (int)info->rotation, info->sizePoints.width, info->sizePoints.height,
        info->view.cropBox.left, info->view.cropBox.bottom, info->view.cropBox.right, info->view.cropBox.top,
        info->mediaBox.left, info->mediaBox.bottom, info->mediaBox.right, info->mediaBox.top);
}
