// SPDX-License-Identifier: MPL-2.0
#include "app/OcrController.hpp"

#include "core/async/IMainThreadDispatcher.hpp"
#include "editor/ContentCommands.hpp"
#include "editor/DocumentSession.hpp"
#include "editor/OcrLayer.hpp"
#include "pdf/PdfEngine.hpp"
#include "platform/TextRecognizer.hpp"

#include <chrono>
#include <format>
#include <functional>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

namespace rivet::app {

namespace {

bool hasVisibleText(const pdf::PdfTextPage& page) {
    for (const pdf::TextChar& c : page.chars()) {
        if (c.unicode > 0x20 && c.unicode != 0xFFFD && c.unicode != 0xA0) return true;
    }
    return false;
}

} // namespace

// One page captured on the main thread (immutable inputs for the worker).
struct OcrController::PageTask {
    core::PageId id;
    std::shared_ptr<pdf::PdfDocument> source;
    std::size_t sourcePageIndex = 0;
    pdf::PdfPageView view;
    std::vector<std::uint32_t> hiddenAnnotations;
    pdf::PdfPageContentEditsPtr edits;
};

struct OcrController::PageResult {
    enum class Outcome { Recognized, SkippedHasText, Failed, Cancelled };
    core::PageId id;
    pdf::PdfPageView view;
    Outcome outcome = Outcome::Failed;
    std::vector<editor::OcrWord> words;
};

struct OcrController::Job {
    TabId tab;
    core::DocumentId document;
    std::size_t total = 0;
    std::size_t done = 0;
    std::size_t recognized = 0;
    std::size_t skippedText = 0;
    std::size_t failed = 0;
    bool cancelled = false;
    bool finished = false;
    bool workerDone = false;
    std::size_t pending = 0; // recognized pages not applied yet
    std::stop_source stop;
};

OcrController::OcrController(ShellContext& context, core::TaskScheduler& scheduler)
    : context_(context), scheduler_(scheduler), alive_(std::make_shared<std::atomic<bool>>(true)) {}

OcrController::~OcrController() {
    *alive_ = false;
    if (job_ != nullptr) job_->stop.request_stop();
    scope_.closeAndWait();
}

const std::vector<std::string>& OcrController::defaultLanguages() {
    static const std::vector<std::string> kLanguages{"en-US", "ru-RU"};
    return kLanguages;
}

bool OcrController::isOcrCommand(ContentCommand command) {
    return command == ContentCommand::RecognizeTextMissing || command == ContentCommand::RecognizeTextAll;
}

std::string OcrController::unavailableReason() const {
    if (context_.services.textRecognizer == nullptr) return "text recognition is not available on this platform";
    if (context_.services.mainDispatcher == nullptr) return "text recognition is not available";
    if (context_.readyPdfTab() == nullptr) return "open a PDF first";
    if (job_ != nullptr) return "text recognition is already running";
    return {};
}

bool OcrController::canPerform(ContentCommand command) const {
    return isOcrCommand(command) && unavailableReason().empty();
}

void OcrController::perform(ContentCommand command) {
    if (!isOcrCommand(command)) return;
    const std::string reason = unavailableReason();
    if (!reason.empty()) {
        context_.setStatus(std::format("Recognize Text: {}", reason));
        return;
    }
    start(command == ContentCommand::RecognizeTextAll);
}

bool OcrController::requestCancel() {
    if (job_ == nullptr || job_->cancelled) return false;
    job_->cancelled = true;
    job_->stop.request_stop();
    context_.setStatus("Cancelling text recognition…");
    return true;
}

void OcrController::cancelForTab(TabId tab) {
    if (job_ != nullptr && job_->tab == tab) {
        job_->cancelled = true;
        job_->stop.request_stop();
    }
}

void OcrController::start(bool allPages) {
    DocumentTab* tab = context_.readyPdfTab();
    if (tab == nullptr) return;
    editor::DocumentSession& session = *tab->session();
    const editor::PageSnapshotPtr snapshot = session.pageSnapshot();
    if (snapshot == nullptr || snapshot->size() == 0) return;
    std::optional<core::AsyncScope::Token> entered = scope_.enter();
    if (!entered.has_value()) return;
    auto token = std::make_shared<core::AsyncScope::Token>(std::move(*entered));

    auto tasks = std::make_shared<std::vector<PageTask>>();
    tasks->reserve(snapshot->size());
    for (const editor::PageEntry& entry : snapshot->entries()) {
        PageTask task;
        task.id = entry.id;
        task.source = entry.source;
        task.sourcePageIndex = entry.sourcePageIndex;
        task.view = entry.view;
        task.edits = entry.contentEdits;
        if (entry.annotations != nullptr) {
            for (const std::uint32_t index : entry.annotations->suppressed) task.hiddenAnnotations.push_back(index);
        }
        tasks->push_back(std::move(task));
    }

    auto job = std::make_shared<Job>();
    job->tab = tab->id();
    job->document = session.id();
    job->total = tasks->size();
    job_ = job;
    context_.setStatus(std::format("Recognizing text… 0 of {} (Esc to cancel)", job->total));

    const platform::ITextRecognizer* recognizer = context_.services.textRecognizer;
    core::IMainThreadDispatcher* dispatcher = context_.services.mainDispatcher;
    const std::stop_token stop = job->stop.get_token();
    // The token is declared first: it is released last, after the worker body.
    scheduler_.post([this, token, tasks, job, recognizer, dispatcher, stop, allPages, alive = alive_] {
        for (const PageTask& task : *tasks) {
            PageResult result;
            result.id = task.id;
            result.view = task.view;
            if (stop.stop_requested()) {
                result.outcome = PageResult::Outcome::Cancelled;
            } else if (task.source == nullptr) {
                result.outcome = PageResult::Outcome::Failed;
            } else {
                bool skip = false;
                if (!allPages) {
                    auto text = task.source->textPage(task.sourcePageIndex, task.view, task.edits);
                    skip = text.has_value() && *text != nullptr && hasVisibleText(**text);
                }
                if (skip) {
                    result.outcome = PageResult::Outcome::SkippedHasText;
                } else {
                    const core::Size size = pdf::displaySize(task.view);
                    auto bitmap = task.source->renderPage(task.sourcePageIndex, task.view, task.hiddenAnnotations,
                                                          task.edits, core::Rect{core::Point{0, 0}, size},
                                                          kPixelsPerPoint);
                    if (!bitmap.has_value() || stop.stop_requested()) {
                        result.outcome = stop.stop_requested() ? PageResult::Outcome::Cancelled
                                                               : PageResult::Outcome::Failed;
                    } else {
                        auto page = recognizer->recognize(*bitmap, defaultLanguages(), stop);
                        if (!page.has_value()) {
                            result.outcome = page.error().code == core::ErrorCode::Cancelled
                                                 ? PageResult::Outcome::Cancelled
                                                 : PageResult::Outcome::Failed;
                        } else {
                            result.outcome = PageResult::Outcome::Recognized;
                            for (const platform::RecognizedLine& line : page->lines) {
                                for (const platform::RecognizedWord& word : line.words) {
                                    result.words.push_back(editor::ocrWordFromBitmap(
                                        word.text, word.quad, bitmap->width(), bitmap->height(), size));
                                }
                            }
                        }
                    }
                }
            }
            const bool cancelled = result.outcome == PageResult::Outcome::Cancelled;
            if (!*alive) return;
            auto shared = std::make_shared<PageResult>(std::move(result));
            dispatcher->post([this, job, shared, alive] {
                if (!*alive) return;
                onPage(job, std::move(*shared));
            });
            if (cancelled) break;
        }
        if (!*alive) return;
        dispatcher->post([this, job, alive] {
            if (!*alive) return;
            workerDone(job);
        });
    });
}

void OcrController::onPage(const std::shared_ptr<Job>& job, PageResult result) {
    if (job->finished) return;
    switch (result.outcome) {
    case PageResult::Outcome::SkippedHasText:
        ++job->skippedText;
        pageDone(job);
        break;
    case PageResult::Outcome::Failed:
        ++job->failed;
        pageDone(job);
        break;
    case PageResult::Outcome::Cancelled: pageDone(job); break;
    case PageResult::Outcome::Recognized:
        ++job->pending;
        apply(job, std::make_shared<PageResult>(std::move(result)), 0);
        break;
    }
}

void OcrController::apply(const std::shared_ptr<Job>& job, const std::shared_ptr<PageResult>& result,
                          std::size_t attempt) {
    if (job->finished) return;
    DocumentTab* tab = context_.workspace.tabById(job->tab);
    const bool tabGone = tab == nullptr || tab->session() == nullptr || tab->state() != DocumentTab::State::Ready ||
                         tab->session()->id() != job->document ||
                         tab->session()->pageIndexFor(result->id) == editor::DocumentSession::kInvalidPage;
    if (job->cancelled || tabGone) {
        --job->pending;
        if (!job->cancelled) ++job->failed; // the page went away
        pageDone(job);
        return;
    }
    auto edit = editor::addInvisibleText(*tab->session(), result->id, result->view, result->words);
    if (!edit.has_value() && edit.error().code == core::ErrorCode::NotAvailable && attempt < kMaxApplyAttempts) {
        // The page content is still being extracted (the attempt above asked
        // for it): retry shortly. The job stays open through `pending`.
        std::optional<core::AsyncScope::Token> entered = scope_.enter();
        if (entered.has_value()) {
            auto token = std::make_shared<core::AsyncScope::Token>(std::move(*entered));
            core::IMainThreadDispatcher* dispatcher = context_.services.mainDispatcher;
            scheduler_.post([this, token, dispatcher, job, result, attempt, alive = alive_] {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                if (!*alive) return;
                dispatcher->post([this, job, result, attempt, alive] {
                    if (!*alive) return;
                    apply(job, result, attempt + 1);
                });
            });
            return;
        }
    }
    --job->pending;
    if (!edit.has_value() || !tab->session()->execute(std::move(edit->command)).has_value()) {
        ++job->failed; // nothing readable, locked session, ... (no text in any message)
    } else {
        ++job->recognized;
    }
    pageDone(job);
}

void OcrController::pageDone(const std::shared_ptr<Job>& job) {
    ++job->done;
    if (!job->cancelled && !job->finished && job_ == job) {
        context_.setStatus(std::format("Recognizing text… {} of {} (Esc to cancel)", job->done, job->total));
    }
    maybeFinish(job);
}

void OcrController::maybeFinish(const std::shared_ptr<Job>& job) {
    if (job->workerDone && job->pending == 0 && !job->finished) finish(job);
}

void OcrController::workerDone(const std::shared_ptr<Job>& job) {
    job->workerDone = true;
    maybeFinish(job);
}

void OcrController::finish(const std::shared_ptr<Job>& job) {
    job->finished = true;
    if (job_ == job) job_.reset();
    std::string text;
    if (job->cancelled) {
        text = std::format("Text recognition cancelled ({} of {} pages done)", job->recognized, job->total);
    } else {
        text = std::format("Recognized text on {} of {} pages", job->recognized, job->total);
        if (job->skippedText > 0) text += std::format(", {} already had text", job->skippedText);
        if (job->failed > 0) text += std::format(", {} without readable text or failed", job->failed);
    }
    context_.setStatus(std::move(text));
}

} // namespace rivet::app
