// SPDX-License-Identifier: MPL-2.0
#pragma once

// Mermaid source -> MermaidDiagram (ADR-0032). Supported:
//
//   flowchart / graph [TD|TB|BT|LR|RL]
//     nodes      id, id[rect], id(round), id([stadium]), id{diamond},
//                id((circle)); "quoted" labels; <br> line breaks;
//                [[..]], [(..)], {{..}}, >..] map to the nearest shape
//     edges      -->  ---  -.->  -.-  ==>  ===  (longer variants too),
//                labels as A -->|text| B or A -- text --> B (also -. .-> and
//                == ==>), chains A --> B --> C, groups A & B --> C
//     subgraph   subgraph id[Title] ... end (one level), direction ignored
//     ignored    classDef, class, style, linkStyle, click, accTitle, accDescr
//
//   sequenceDiagram
//     participant / actor X [as Label], autonumber,
//     messages   A->B  A-->B  A->>B  A-->>B  A-xB  A--xB  A-)B  A--)B  (": text"),
//                +/- activation suffixes are accepted and ignored
//     notes      Note left of|right of|over A[,B]: text
//     blocks     loop/alt/opt/par/critical/break/rect ... else/and/option ... end
//     ignored    activate, deactivate, title, accTitle, accDescr
//
// `%%` comment lines, `%%{...}%%` directives and a leading `---` front-matter
// block are skipped. Anything else is an error: the caller renders the source
// as a code block with a one-line notice. Error reasons are short and never
// contain document text (they may be logged).
//
// Pure and thread-safe.

#include "markdown/mermaid/MermaidModel.hpp"

#include <expected>
#include <string>
#include <string_view>

namespace rivet::markdown::mermaid {

std::expected<MermaidDiagram, std::string> parseMermaid(std::string_view source, const MermaidLimits& limits = {});

// Whether a fenced code block's info string selects Mermaid ("mermaid",
// case-insensitive, optionally followed by more words).
bool isMermaidFenceInfo(std::string_view info);

} // namespace rivet::markdown::mermaid
