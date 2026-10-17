// SPDX-License-Identifier: MPL-2.0

#include "markdown/mermaid/Mermaid.hpp"

#include <variant>

namespace rivet::markdown::mermaid {

std::expected<DiagramScene, std::string> renderMermaid(std::string_view source, const ITextMeasurer& measurer,
                                                       const DiagramStyle& style, const MermaidLimits& limits) {
    auto parsed = parseMermaid(source, limits);
    if (!parsed) return std::unexpected(parsed.error());
    if (const auto* chart = std::get_if<Flowchart>(&*parsed)) {
        auto layout = layoutFlowchart(*chart, measurer, style, limits);
        if (!layout) return std::unexpected(layout.error());
        return flowchartScene(*chart, *layout, measurer, style);
    }
    const auto& sequence = std::get<SequenceDiagram>(*parsed);
    auto layout = layoutSequence(sequence, measurer, style, limits);
    if (!layout) return std::unexpected(layout.error());
    return sequenceScene(sequence, *layout, measurer, style);
}

} // namespace rivet::markdown::mermaid
