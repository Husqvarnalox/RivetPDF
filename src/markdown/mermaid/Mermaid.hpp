// SPDX-License-Identifier: MPL-2.0
#pragma once

// Mermaid in Markdown (ADR-0032): source -> paint-ready DiagramScene.
//
//   if (isMermaidFenceInfo(info)) {
//       auto scene = renderMermaid(code, measurer);
//       if (!scene) { /* show the code block plus a notice: scene.error() */ }
//   }
//
// The scene is measured with `measurer` using diagramTextStyle(); the painter
// and the PDF exporter must draw text with the same styles. Pure and
// thread-safe: may run on the async parse path.

#include "markdown/MarkdownLayout.hpp"
#include "markdown/mermaid/DiagramScene.hpp"
#include "markdown/mermaid/FlowchartLayout.hpp"
#include "markdown/mermaid/MermaidModel.hpp"
#include "markdown/mermaid/MermaidParser.hpp"
#include "markdown/mermaid/SequenceLayout.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace rivet::markdown::mermaid {

std::expected<DiagramScene, std::string> renderMermaid(std::string_view source, const ITextMeasurer& measurer,
                                                       const DiagramStyle& style = {},
                                                       const MermaidLimits& limits = {});

} // namespace rivet::markdown::mermaid
