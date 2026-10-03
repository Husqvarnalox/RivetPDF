// SPDX-License-Identifier: MPL-2.0
// Opt-in command-line probe (RIVET_BUILD_PERF_PROBES, PDFium builds): runs
// the Phase 5 content-editing pipeline over arbitrary real PDFs supplied at
// run time, to find crashes, hangs and inconsistencies that generated
// fixtures cannot. Not registered with ctest; the PDFs are never part of the
// repository.
//
//   rivet_probe_content_corpus <dir-or-file>... [--max-pages N] [--timeout-ms N]
//
// For every *.pdf (directories are searched recursively, sorted) the probe
// opens a session, and for the first N pages awaits the extracted content,
// moves one movable object, retypes one editable text block, saves to a
// temporary file, reloads it in a fresh session and compares the object
// counts. Only the file NAME, aggregate counts and error categories are
// printed: never document text, passwords or page content. A factory
// refusal (core::Error) is not a failure; a crash, sanitizer report, hang,
// timeout or inconsistency is.
#include "core/Error.hpp"
#include "core/async/TaskScheduler.hpp"
#include "editor/ContentCommands.hpp"
#include "editor/ContentObjects.hpp"
#include "editor/ContentService.hpp"
#include "editor/DocumentSaver.hpp"
#include "editor/DocumentSession.hpp"
#include "pdf/PdfEngine.hpp"
#include "pdf/PdfSystem.hpp"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace ed = rivet::editor;
namespace pdf = rivet::pdf;
namespace core = rivet::core;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

struct Options {
    std::vector<fs::path> inputs;
    std::size_t maxPages = 3;
    std::chrono::milliseconds timeout{60000};
};

using Counter = std::map<std::string, std::size_t>;

struct Totals {
    std::size_t files = 0;
    std::size_t opened = 0;
    std::size_t pages = 0;
    std::size_t editablePages = 0;
    std::size_t moves = 0;
    std::size_t retypes = 0;
    std::size_t roundTripOk = 0;
    std::size_t roundTripMismatch = 0;
    std::size_t roundTripSkipped = 0;
    std::size_t inconsistencies = 0;
    Counter inconsistencyKinds;
    std::size_t timeouts = 0;
    std::size_t exceptions = 0;
    Counter openErrors;
    Counter readOnlyReasons;
    Counter refusals;
};

struct FileResult {
    std::string open = "ok";
    std::size_t pages = 0;
    std::size_t objects = 0;
    std::size_t editablePages = 0;
    std::size_t readOnlyPages = 0;
    Counter readOnlyReasons;
    std::size_t moved = 0;
    std::size_t retyped = 0;
    std::size_t refused = 0;
    Counter refusals;
    std::string roundTrip = "skipped";
    std::size_t inconsistencies = 0;
    Counter inconsistencyKinds;
    bool timedOut = false;
};

std::string category(const core::Error& error) {
    return std::string(core::toString(error.code));
}

std::string joined(const Counter& counter) {
    if (counter.empty()) return "-";
    std::string out;
    for (const auto& [key, count] : counter) {
        if (!out.empty()) out += ';';
        out += key + "x" + std::to_string(count);
    }
    return out;
}

void merge(Counter& into, const Counter& from) {
    for (const auto& [key, count] : from) into[key] += count;
}

// Polls the lazily extracted content of the page until it is loaded or the
// deadline passes (nullptr).
ed::PageContentViewPtr awaitLoaded(ed::DocumentSession& session, core::PageId page, Clock::time_point deadline) {
    while (Clock::now() < deadline) {
        auto view = session.contentService().content(page);
        if (view != nullptr && view->loaded) return view;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    return nullptr;
}

// Why a page offers nothing to edit, as a short fixed category (the engine's
// reasons are fixed strings and never carry document text). Empty = editable.
std::string readOnlyCategory(const ed::PageContentView& view) {
    if (view.truncated) return "truncated";
    if (!view.regenerationSafe) {
        return "regeneration:" + (view.regenerationIssue.empty() ? std::string("unknown") : view.regenerationIssue);
    }
    if (view.objects.empty()) return "";
    for (const auto& object : view.objects) {
        if (object.capability != ed::ContentCapability::ReadOnly) return "";
    }
    return "all-objects-readonly";
}

fs::path scratchDir() {
    static const fs::path dir = [] {
        fs::path path = fs::temp_directory_path() / ("rivet-probe-corpus-" + std::to_string(::getpid()));
        std::error_code ignored;
        fs::create_directories(path, ignored);
        return path;
    }();
    return dir;
}

bool readFile(const fs::path& path, std::string& bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

bool writeFile(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

FileResult probeFile(pdf::PdfEngine& engine, core::TaskScheduler& scheduler, const fs::path& source,
                     const Options& options) {
    FileResult result;
    const auto fileStart = Clock::now();
    const auto deadline = fileStart + options.timeout;
    const auto expired = [&] { return Clock::now() >= deadline; };

    // Work on a private copy so the original is never touched, even by a bug.
    std::string bytes;
    if (!readFile(source, bytes)) {
        result.open = "err:io-read";
        return result;
    }
    const fs::path work = scratchDir() / "work.pdf";
    if (!writeFile(work, bytes)) {
        result.open = "err:io-write";
        return result;
    }
    bytes.clear();
    bytes.shrink_to_fit();

    auto created = ed::DocumentSession::create(engine, scheduler, nullptr, work);
    if (!created.has_value()) {
        result.open = "err:" + category(created.error());
        return result;
    }
    if (expired()) {
        result.timedOut = true;
        result.open = "timeout";
        return result;
    }
    auto session = std::move(*created);

    const std::size_t pageCount = std::min(session->pageCount(), options.maxPages);
    std::vector<std::size_t> editedCounts;
    bool abandoned = false;
    for (std::size_t index = 0; index < pageCount && !abandoned; ++index) {
        const core::PageId page = session->pageId(index);
        auto view = awaitLoaded(*session, page, deadline);
        if (view == nullptr) {
            result.timedOut = true;
            abandoned = true;
            break;
        }
        ++result.pages;
        result.objects += view->objects.size();
        const std::string readOnly = readOnlyCategory(*view);
        if (!readOnly.empty()) {
            ++result.readOnlyPages;
            ++result.readOnlyReasons[readOnly];
        } else if (!view->objects.empty()) {
            ++result.editablePages;
        }

        // Move one movable object by (1, 1).
        const std::size_t objectsBefore = view->objects.size();
        core::ObjectId movable;
        bool haveMovable = false;
        for (const auto& object : view->objects) {
            if (object.capability != ed::ContentCapability::ReadOnly) {
                movable = object.id;
                haveMovable = true;
                break;
            }
        }
        if (haveMovable) {
            auto move = ed::moveContent(*session, page, {movable}, core::Point{1.0, 1.0});
            if (!move.has_value()) {
                ++result.refused;
                ++result.refusals["move:" + category(move.error())];
            } else if (!session->execute(std::move(move->command)).has_value()) {
                ++result.refused;
                ++result.refusals["move-execute"];
            } else {
                ++result.moved;
                view = awaitLoaded(*session, page, deadline);
                if (view == nullptr) {
                    result.timedOut = true;
                    abandoned = true;
                    break;
                }
                if (view->objects.size() != objectsBefore) {
                    ++result.inconsistencies;
                    ++result.inconsistencyKinds["move-changed-object-count"];
                }
            }
        }

        // Retype the first editable block: its text with one character appended.
        const ed::TextBlockView* editable = nullptr;
        for (const auto& block : view->blocks) {
            if (block.capability == ed::ContentCapability::Replaceable ||
                block.capability == ed::ContentCapability::FullyEditable) {
                editable = &block;
                break;
            }
        }
        if (editable != nullptr) {
            ed::TextBlockPatch patch;
            patch.text = editable->text + "x";
            auto edit = ed::editTextBlock(*session, page, editable->id, patch);
            if (!edit.has_value()) {
                ++result.refused;
                ++result.refusals["retype:" + category(edit.error())];
            } else if (!session->execute(std::move(edit->command)).has_value()) {
                ++result.refused;
                ++result.refusals["retype-execute"];
            } else {
                ++result.retyped;
                view = awaitLoaded(*session, page, deadline);
                if (view == nullptr) {
                    result.timedOut = true;
                    abandoned = true;
                    break;
                }
                if (view->blocks.empty()) {
                    ++result.inconsistencies;
                    ++result.inconsistencyKinds["retype-lost-all-blocks"];
                }
            }
        }
        editedCounts.push_back(view->objects.size());
    }

    if (abandoned || result.pages == 0) {
        result.roundTrip = "skipped";
        return result;
    }

    // Save, reload in a fresh session, compare the object counts per page.
    const fs::path saved = scratchDir() / "saved.pdf";
    bool compared = false;
    bool match = true;
    {
        auto job = ed::makeSaveJob(*session, saved);
        if (!job.has_value()) {
            ++result.refused;
            ++result.refusals["save-job:" + category(job.error())];
        } else {
            const auto written = ed::runDocumentWrite(engine, *job);
            if (!written.written.has_value()) {
                ++result.refused;
                ++result.refusals["save:" + category(written.written.error())];
            } else {
                auto reloaded = ed::DocumentSession::create(engine, scheduler, nullptr, saved);
                if (!reloaded.has_value()) {
                    ++result.refused;
                    ++result.refusals["reload:" + category(reloaded.error())];
                    match = false;
                    compared = true;
                } else {
                    auto second = std::move(*reloaded);
                    if (second->pageCount() < editedCounts.size()) {
                        match = false;
                        compared = true;
                    } else {
                        compared = true;
                        for (std::size_t index = 0; index < editedCounts.size(); ++index) {
                            auto view = awaitLoaded(*second, second->pageId(index), deadline);
                            if (view == nullptr) {
                                result.timedOut = true;
                                compared = false;
                                break;
                            }
                            if (view->objects.size() != editedCounts[index]) match = false;
                        }
                    }
                }
            }
        }
    }
    std::error_code ignored;
    fs::remove(saved, ignored);
    if (compared) result.roundTrip = match ? "ok" : "mismatch";
    return result;
}

void collect(const fs::path& input, std::vector<fs::path>& files) {
    std::error_code ec;
    if (fs::is_directory(input, ec)) {
        fs::recursive_directory_iterator it(input, fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code fileEc;
            if (!it->is_regular_file(fileEc)) continue;
            std::string ext = it->path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
            if (ext == ".pdf") files.push_back(it->path());
        }
    } else if (fs::is_regular_file(input, ec)) {
        files.push_back(input);
    }
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--max-pages" && i + 1 < argc) {
            options.maxPages = static_cast<std::size_t>(std::max(1L, std::strtol(argv[++i], nullptr, 10)));
        } else if (arg == "--timeout-ms" && i + 1 < argc) {
            options.timeout = std::chrono::milliseconds(std::max(1L, std::strtol(argv[++i], nullptr, 10)));
        } else if (arg.rfind("--", 0) == 0) {
            std::fprintf(stderr, "unknown option %s\n", arg.c_str());
            return 64;
        } else {
            options.inputs.emplace_back(arg);
        }
    }
    if (options.inputs.empty()) {
        std::fputs("usage: rivet_probe_content_corpus <dir-or-file>... [--max-pages N] [--timeout-ms N]\n", stderr);
        return 64;
    }

    auto engine = pdf::createEngine();
    if (!engine || !engine->isAvailable()) {
        std::puts("no PDFium");
        return 2;
    }

    std::vector<fs::path> files;
    for (const auto& input : options.inputs) collect(input, files);
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());

    core::TaskScheduler scheduler(2);
    Totals totals;
    for (const auto& file : files) {
        FileResult result;
        try {
            result = probeFile(*engine, scheduler, file, options);
        } catch (const std::exception&) {
            result.open = "exception";
            ++totals.exceptions;
        } catch (...) {
            result.open = "exception";
            ++totals.exceptions;
        }
        ++totals.files;
        if (result.open == "ok") ++totals.opened;
        if (result.open.rfind("err:", 0) == 0) ++totals.openErrors[result.open.substr(4)];
        totals.pages += result.pages;
        totals.editablePages += result.editablePages;
        merge(totals.readOnlyReasons, result.readOnlyReasons);
        totals.moves += result.moved;
        totals.retypes += result.retyped;
        merge(totals.refusals, result.refusals);
        totals.inconsistencies += result.inconsistencies;
        merge(totals.inconsistencyKinds, result.inconsistencyKinds);
        if (result.timedOut) ++totals.timeouts;
        if (result.roundTrip == "ok") ++totals.roundTripOk;
        else if (result.roundTrip == "mismatch") ++totals.roundTripMismatch;
        else ++totals.roundTripSkipped;

        std::size_t refused = result.refused;
        std::printf("%s | open=%s%s | pages=%zu | objects=%zu | editable_pages=%zu | readonly=%zu(%s) | moved=%zu | "
                    "retyped=%zu | refused=%zu(%s) | roundtrip=%s%s%s\n",
                    file.filename().string().c_str(), result.open.c_str(), result.timedOut ? "+timeout" : "",
                    result.pages, result.objects, result.editablePages, result.readOnlyPages,
                    joined(result.readOnlyReasons).c_str(), result.moved, result.retyped, refused,
                    joined(result.refusals).c_str(),
                    result.roundTrip.c_str(), result.inconsistencies != 0 ? (" | INCONSISTENCY(" + joined(result.inconsistencyKinds) + ")").c_str() : "",
                    result.timedOut ? " | TIMEOUT" : "");
        std::fflush(stdout);
    }
    std::error_code ignored;
    fs::remove_all(scratchDir(), ignored);

    std::puts("\n== summary ==");
    std::printf("files:                 %zu\n", totals.files);
    std::printf("opened:                %zu\n", totals.opened);
    std::printf("open errors:           %s\n", joined(totals.openErrors).c_str());
    std::printf("pages probed:          %zu\n", totals.pages);
    std::printf("editable pages:        %zu\n", totals.editablePages);
    std::printf("read-only pages:       %s\n", joined(totals.readOnlyReasons).c_str());
    std::printf("moves:                 %zu\n", totals.moves);
    std::printf("retypes:               %zu\n", totals.retypes);
    std::printf("refusals:              %s\n", joined(totals.refusals).c_str());
    std::printf("round-trip ok:         %zu\n", totals.roundTripOk);
    std::printf("round-trip mismatch:   %zu\n", totals.roundTripMismatch);
    std::printf("round-trip skipped:    %zu\n", totals.roundTripSkipped);
    std::printf("inconsistencies:       %zu (%s)\n", totals.inconsistencies, joined(totals.inconsistencyKinds).c_str());
    std::printf("timeouts:              %zu\n", totals.timeouts);
    std::printf("exceptions:            %zu\n", totals.exceptions);
    const bool failed = totals.inconsistencies != 0 || totals.roundTripMismatch != 0 || totals.timeouts != 0 ||
                        totals.exceptions != 0;
    return failed ? 1 : 0;
}
