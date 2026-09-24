#include "pdf/PdfSystem.hpp"

#include "PdfiumEngine.h"

#include <memory>
#include <utility>

namespace rivet::pdf {

std::unique_ptr<PdfEngine> createEngine() {
    return std::make_unique<PdfiumEngine>();
}

} // namespace rivet::pdf
