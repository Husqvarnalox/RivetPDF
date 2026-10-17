// SPDX-License-Identifier: MPL-2.0
#include "app/QaScript.hpp"

#include "app/DocumentWorkspace.hpp"
#include "app/MenuModel.hpp"
#include "app/ShellController.hpp"
#include "core/Error.hpp"
#include "editor/DocumentSession.hpp"
#include "render/PageLayout.hpp"
#include "render/PageTransform.hpp"
#include "ui/PdfViewport.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <sstream>
#include <stop_token>
#include <thread>
#include <utility>

namespace rivet::app {

namespace qa {

namespace {

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0) text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0) text.remove_suffix(1);
    return text;
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string_view> splitWords(std::string_view text) {
    std::vector<std::string_view> words;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) != 0) ++i;
        const std::size_t start = i;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])) == 0) ++i;
        if (i > start) words.push_back(text.substr(start, i - start));
    }
    return words;
}

std::optional<double> parseNumber(std::string_view word) {
    double value = 0.0;
    const char* first = word.data();
    const char* last = word.data() + word.size();
    if (!word.empty() && word.front() == '+') ++first;
    const auto [ptr, ec] = std::from_chars(first, last, value);
    if (ec != std::errc{} || ptr != last || !std::isfinite(value)) return std::nullopt;
    return value;
}

// Expands ${NAME}; an unset variable is reported through `error`.
std::optional<std::string> expand(std::string_view line, const EnvLookup& env, std::string& error) {
    std::string out;
    std::size_t i = 0;
    while (i < line.size()) {
        if (line[i] == '$' && i + 1 < line.size() && line[i + 1] == '{') {
            const std::size_t close = line.find('}', i + 2);
            if (close == std::string_view::npos) {
                error = "unterminated ${";
                return std::nullopt;
            }
            const std::string_view name = line.substr(i + 2, close - i - 2);
            std::optional<std::string> value = env ? env(name) : std::nullopt;
            if (!value.has_value()) {
                error = std::format("environment variable '{}' is not set", name);
                return std::nullopt;
            }
            out += *value;
            i = close + 1;
            continue;
        }
        out += line[i];
        ++i;
    }
    return out;
}

// Trailing modifier words of mouse commands ("shift", "cmd+alt", ...).
std::optional<ui::ModifierFlags> parseModifiers(const std::vector<std::string_view>& words, std::size_t from,
                                                bool primaryIsCommand) {
    ui::ModifierFlags flags;
    for (std::size_t i = from; i < words.size(); ++i) {
        std::string_view rest = words[i];
        while (!rest.empty()) {
            const std::size_t plus = rest.find('+');
            const std::string part = lower(rest.substr(0, plus));
            rest = plus == std::string_view::npos ? std::string_view{} : rest.substr(plus + 1);
            if (part == "cmd" || part == "primary") {
                (primaryIsCommand ? flags.command : flags.control) = true;
            } else if (part == "ctrl" || part == "control") {
                flags.control = true;
            } else if (part == "shift") {
                flags.shift = true;
            } else if (part == "alt" || part == "option" || part == "opt") {
                flags.option = true;
            } else if (part == "meta" || part == "command") {
                flags.command = true;
            } else {
                return std::nullopt;
            }
        }
    }
    return flags;
}

constexpr bool kPrimaryIsCommand =
#if defined(__APPLE__)
    true;
#else
    false;
#endif

struct Spec {
    StepKind kind;
    std::size_t numbers;     // leading numeric arguments
    bool page;               // first number is a page (counted in numbers)
    bool textRest;           // remainder is free text (required)
    bool allowModifiers;     // trailing words are modifiers
};

const std::map<std::string, Spec, std::less<>>& specs() {
    static const std::map<std::string, Spec, std::less<>> table{
        {"open", {StepKind::Open, 0, false, true, false}},
        {"cmd", {StepKind::Cmd, 0, false, true, false}},
        {"assert-enabled", {StepKind::AssertEnabled, 0, false, true, false}},
        {"assert-disabled", {StepKind::AssertDisabled, 0, false, true, false}},
        {"click", {StepKind::Click, 2, false, false, true}},
        {"dblclick", {StepKind::DoubleClick, 2, false, false, true}},
        {"rclick", {StepKind::RightClick, 2, false, false, true}},
        {"drag", {StepKind::Drag, 4, false, false, true}},
        {"scroll", {StepKind::Scroll, 4, false, false, true}},
        {"clickpage", {StepKind::ClickPage, 3, true, false, true}},
        {"dblclickpage", {StepKind::DoubleClickPage, 3, true, false, true}},
        {"dragpage", {StepKind::DragPage, 5, true, false, true}},
        {"key", {StepKind::Key, 0, false, true, false}},
        {"type", {StepKind::Type, 0, false, true, false}},
        {"wait", {StepKind::Wait, 1, false, false, false}},
        {"idle", {StepKind::Idle, 0, false, false, false}},
        {"shot", {StepKind::Shot, 0, false, true, false}},
        {"resize", {StepKind::Resize, 2, false, false, false}},
        {"theme", {StepKind::Theme, 0, false, true, false}},
        {"answer", {StepKind::Answer, 0, false, true, false}},
        {"assert-status", {StepKind::AssertStatus, 0, false, true, false}},
        {"assert-find", {StepKind::AssertFind, 0, false, true, false}},
        {"assert-clipboard", {StepKind::AssertClipboard, 0, false, true, false}},
        {"assert-pages", {StepKind::AssertPages, 1, false, false, false}},
        {"assert-tabs", {StepKind::AssertTabs, 1, false, false, false}},
        {"log-state", {StepKind::LogState, 0, false, false, false}},
        {"log", {StepKind::Log, 0, false, true, false}},
        {"quit", {StepKind::Quit, 0, false, false, false}},
    };
    return table;
}

std::optional<std::string> validateAnswer(Step& step) {
    const std::size_t space = step.text.find(' ');
    if (space == std::string::npos) return "answer needs a kind and a value";
    step.answerKind = lower(step.text.substr(0, space));
    step.text = std::string(trim(std::string_view(step.text).substr(space + 1)));
    const std::string value = lower(step.text);
    if (step.answerKind == "save" || step.answerKind == "open" || step.answerKind == "text") return std::nullopt;
    if (step.answerKind == "confirm") {
        if (value == "yes" || value == "no") return std::nullopt;
        return "answer confirm expects yes|no";
    }
    if (step.answerKind == "savechanges") {
        if (value == "save" || value == "dontsave" || value == "cancel") return std::nullopt;
        return "answer savechanges expects save|dontsave|cancel";
    }
    if (step.answerKind == "review") {
        if (value == "review" || value == "discard" || value == "cancel") return std::nullopt;
        return "answer review expects review|discard|cancel";
    }
    return std::format("unknown answer kind '{}'", step.answerKind);
}

} // namespace

std::string normalizeMenuTitle(std::string_view title) {
    std::string text = lower(trim(title));
    constexpr std::string_view kEllipsis = "\xE2\x80\xA6";
    if (text.ends_with(kEllipsis)) text.resize(text.size() - kEllipsis.size());
    else if (text.ends_with("...")) text.resize(text.size() - 3);
    return std::string(trim(text));
}

std::optional<ui::KeyEvent> parseKeyCombo(std::string_view combo, bool primaryIsCommand) {
    combo = trim(combo);
    if (combo.empty()) return std::nullopt;
    ui::KeyEvent event;
    // The key is the last '+'-separated part; a lone "+" (or "cmd++") is the
    // plus key itself.
    std::string_view keyPart = combo;
    std::string_view modifierPart;
    if (combo.size() > 1 && combo.ends_with("++")) {
        keyPart = "+";
        modifierPart = combo.substr(0, combo.size() - 2);
    } else if (const std::size_t plus = combo.rfind('+'); plus != std::string_view::npos && combo.size() > 1) {
        keyPart = combo.substr(plus + 1);
        modifierPart = combo.substr(0, plus);
    }
    if (!modifierPart.empty()) {
        const auto flags = parseModifiers({modifierPart}, 0, primaryIsCommand);
        if (!flags.has_value()) return std::nullopt;
        event.modifiers = *flags;
    }
    const std::string key = lower(keyPart);
    static const std::map<std::string, ui::Key, std::less<>> named{
        {"up", ui::Key::Up},         {"down", ui::Key::Down},         {"left", ui::Key::Left},
        {"right", ui::Key::Right},   {"pageup", ui::Key::PageUp},     {"pagedown", ui::Key::PageDown},
        {"home", ui::Key::Home},     {"end", ui::Key::End},           {"escape", ui::Key::Escape},
        {"esc", ui::Key::Escape},    {"return", ui::Key::Enter},      {"enter", ui::Key::Enter},
        {"tab", ui::Key::Tab},       {"backspace", ui::Key::Backspace}, {"delete", ui::Key::Delete},
        {"space", ui::Key::Space},   {"plus", ui::Key::Plus},         {"minus", ui::Key::Minus},
    };
    if (const auto it = named.find(key); it != named.end()) {
        event.key = it->second;
        if (event.key == ui::Key::Space) event.text = " ";
        if (event.key == ui::Key::Plus) event.text = "+";
        if (event.key == ui::Key::Minus) event.text = "-";
        return event;
    }
    if (key == "+" || key == "=") {
        event.key = ui::Key::Plus;
        event.text = key;
        return event;
    }
    if (key == "-") {
        event.key = ui::Key::Minus;
        event.text = "-";
        return event;
    }
    // A single character (ASCII or one UTF-8 code point).
    const std::vector<ui::KeyEvent> chars = keyEventsForText(keyPart);
    if (chars.size() != 1 || chars.front().key != ui::Key::Character) return std::nullopt;
    event.key = ui::Key::Character;
    event.text = chars.front().text;
    // AppKit reports the shifted character for Shift+letter.
    if (event.modifiers.shift && event.text.size() == 1) {
        event.text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(event.text[0])));
    }
    return event;
}

std::vector<ui::KeyEvent> keyEventsForText(std::string_view text) {
    std::vector<ui::KeyEvent> events;
    std::size_t i = 0;
    while (i < text.size()) {
        ui::KeyEvent event;
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte == '\\' && i + 1 < text.size()) {
            const char next = text[i + 1];
            if (next == 'n' || next == 't' || next == '\\') {
                if (next == 'n') event.key = ui::Key::Enter;
                if (next == 't') event.key = ui::Key::Tab;
                if (next == '\\') {
                    event.key = ui::Key::Character;
                    event.text = "\\";
                }
                events.push_back(std::move(event));
                i += 2;
                continue;
            }
        }
        std::size_t length = 1;
        if (byte >= 0xF0) length = 4;
        else if (byte >= 0xE0) length = 3;
        else if (byte >= 0xC0) length = 2;
        length = std::min(length, text.size() - i);
        event.text = std::string(text.substr(i, length));
        event.key = event.text == " " ? ui::Key::Space : ui::Key::Character;
        events.push_back(std::move(event));
        i += length;
    }
    return events;
}

Script parseScript(std::string_view source, const EnvLookup& env) {
    Script script;
    int lineNumber = 0;
    std::size_t pos = 0;
    while (pos <= source.size()) {
        const std::size_t end = std::min(source.find('\n', pos), source.size());
        std::string_view raw = source.substr(pos, end - pos);
        pos = end + 1;
        ++lineNumber;
        if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
        const std::string_view stripped = trim(raw);
        if (stripped.empty() || stripped.front() == '#') {
            if (end == source.size()) break;
            continue;
        }
        std::string error;
        const std::optional<std::string> expanded = expand(stripped, env, error);
        if (!expanded.has_value()) {
            script.errors.push_back({lineNumber, error});
            if (end == source.size()) break;
            continue;
        }
        const std::string_view line = *expanded;
        const std::size_t space = line.find_first_of(" \t");
        const std::string verb = lower(line.substr(0, space));
        const std::string_view rest = space == std::string_view::npos ? std::string_view{} : trim(line.substr(space));
        const auto it = specs().find(verb);
        if (it == specs().end()) {
            script.errors.push_back({lineNumber, std::format("unknown command '{}'", verb)});
            if (end == source.size()) break;
            continue;
        }
        const Spec& spec = it->second;
        Step step;
        step.kind = spec.kind;
        step.line = lineNumber;
        step.source = std::string(line);
        std::optional<std::string> problem;
        if (spec.textRest) {
            if (rest.empty()) problem = std::format("'{}' needs an argument", verb);
            // `type` keeps its text verbatim (leading blanks included after
            // the single separator).
            step.text = verb == "type" && space != std::string_view::npos ? std::string(line.substr(space + 1))
                                                                           : std::string(rest);
            if (!problem && spec.kind == StepKind::Answer) problem = validateAnswer(step);
            if (!problem && spec.kind == StepKind::Theme) {
                step.text = lower(step.text);
                if (step.text != "light" && step.text != "dark" && step.text != "system") {
                    problem = "theme expects light|dark|system";
                }
            }
            if (!problem && spec.kind == StepKind::Key && !parseKeyCombo(step.text, kPrimaryIsCommand)) {
                problem = std::format("bad key combo '{}'", step.text);
            }
            if (!problem && (spec.kind == StepKind::Cmd || spec.kind == StepKind::AssertEnabled ||
                             spec.kind == StepKind::AssertDisabled) &&
                step.text.find('/') == std::string::npos) {
                problem = "expected <Menu>/<Item>";
            }
        } else {
            const std::vector<std::string_view> words = splitWords(rest);
            if (words.size() < spec.numbers || (!spec.allowModifiers && words.size() != spec.numbers)) {
                problem = std::format("'{}' expects {} number(s)", verb, spec.numbers);
            } else {
                for (std::size_t i = 0; i < spec.numbers && !problem; ++i) {
                    const auto value = parseNumber(words[i]);
                    if (!value.has_value()) problem = std::format("bad number '{}'", words[i]);
                    else step.numbers.push_back(*value);
                }
                if (!problem && spec.allowModifiers) {
                    const auto flags = parseModifiers(words, spec.numbers, kPrimaryIsCommand);
                    if (!flags.has_value()) problem = "bad modifier";
                    else step.modifiers = *flags;
                }
                if (!problem && spec.page) {
                    const double page = step.numbers.front();
                    if (page < 1.0 || page != std::floor(page)) problem = "page numbers are 1-based integers";
                    else step.page = static_cast<int>(page);
                    step.numbers.erase(step.numbers.begin());
                }
                if (!problem && spec.kind == StepKind::Wait && step.numbers.front() < 0.0) {
                    problem = "wait expects a non-negative duration";
                }
                if (!problem && spec.kind == StepKind::Resize &&
                    (step.numbers[0] <= 0.0 || step.numbers[1] <= 0.0)) {
                    problem = "resize expects positive sizes";
                }
            }
        }
        if (problem) script.errors.push_back({lineNumber, *problem});
        else script.steps.push_back(std::move(step));
        if (end == source.size()) break;
    }
    return script;
}

} // namespace qa

// --- Session -----------------------------------------------------------------

namespace {

void qaLog(std::string_view text) {
    std::string line = "[qa] ";
    line += text;
    line += '\n';
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
}

std::optional<std::string> environmentValue(std::string_view name) {
    const std::string key(name);
    const char* value = std::getenv(key.c_str()); // NOLINT(concurrency-mt-unsafe): main thread, startup
    if (value == nullptr) return std::nullopt;
    return std::string(value);
}

} // namespace

struct QaSession::Impl {
    // Answers queued by `answer` lines, consumed by the doubles.
    std::map<std::string, std::deque<std::string>, std::less<>> answers;
    int failures = 0;
    int stepsRun = 0;
    bool loadFailed = false;
    std::string name;
    qa::Script script;

    std::optional<std::string> takeAnswer(std::string_view kind) {
        const auto it = answers.find(kind);
        if (it == answers.end() || it->second.empty()) return std::nullopt;
        std::string value = std::move(it->second.front());
        it->second.pop_front();
        return value;
    }
    void unexpected(std::string_view what) {
        ++failures;
        qaLog(std::format("ERROR: unexpected {} (no queued answer) -> cancelled", what));
    }

    // Doubles -----------------------------------------------------------------
    class FileDialog final : public platform::IFileDialog, public platform::ISaveDialog {
    public:
        explicit FileDialog(Impl& owner) : owner_(owner) {}
        core::Result<std::filesystem::path> openPdf() override { return take("open dialog"); }
        core::Result<std::filesystem::path> openDocument() override { return take("open dialog"); }
        core::Result<std::vector<std::filesystem::path>> openPdfs(const OpenOptions& options) override {
            auto one = take(options.title);
            if (!one) return std::unexpected(one.error());
            return std::vector<std::filesystem::path>{std::move(*one)};
        }
        core::Result<std::filesystem::path> openImage() override { return take("image open dialog"); }
        std::optional<std::filesystem::path> runSavePanel(const Options& options) override {
            const auto answer = owner_.takeAnswer("save");
            if (!answer.has_value()) {
                owner_.unexpected(std::format("save panel '{}'", options.title));
                return std::nullopt;
            }
            qaLog(std::format("dialog: save panel '{}' -> {}", options.title, *answer));
            return std::filesystem::path(*answer);
        }

    private:
        core::Result<std::filesystem::path> take(std::string_view what) {
            const auto answer = owner_.takeAnswer("open");
            if (!answer.has_value()) {
                owner_.unexpected(what);
                return std::unexpected(core::makeError(core::ErrorCode::Cancelled, "qa: no answer", "qa"));
            }
            qaLog(std::format("dialog: {} -> {}", what, *answer));
            return std::filesystem::path(*answer);
        }
        Impl& owner_;
    };

    class Alerts final : public platform::IAlertService {
    public:
        explicit Alerts(Impl& owner) : owner_(owner) {}
        platform::SaveChangesChoice askSaveChanges(std::string_view documentTitle) override {
            const auto answer = owner_.takeAnswer("savechanges");
            if (!answer.has_value()) {
                owner_.unexpected(std::format("save-changes alert for '{}'", documentTitle));
                return platform::SaveChangesChoice::Cancel;
            }
            qaLog(std::format("alert: save changes? -> {}", *answer));
            const std::string value = answerLower(*answer);
            if (value == "save") return platform::SaveChangesChoice::Save;
            if (value == "dontsave") return platform::SaveChangesChoice::DontSave;
            return platform::SaveChangesChoice::Cancel;
        }
        platform::ReviewChangesChoice askReviewUnsavedChanges(std::size_t count) override {
            const auto answer = owner_.takeAnswer("review");
            if (!answer.has_value()) {
                owner_.unexpected(std::format("review-changes alert ({} documents)", count));
                return platform::ReviewChangesChoice::Cancel;
            }
            const std::string value = answerLower(*answer);
            if (value == "review") return platform::ReviewChangesChoice::Review;
            if (value == "discard") return platform::ReviewChangesChoice::DiscardAll;
            return platform::ReviewChangesChoice::Cancel;
        }
        void showError(std::string_view title, std::string_view message) override {
            qaLog(std::format("alert: error '{}': {}", title, message));
        }
        std::optional<std::string> promptForText(std::string_view title, std::string_view message,
                                                 std::string_view defaultText) override {
            (void)message;
            (void)defaultText;
            auto answer = owner_.takeAnswer("text");
            if (!answer.has_value()) owner_.unexpected(std::format("text prompt '{}'", title));
            else qaLog(std::format("alert: text prompt '{}' answered", title));
            return answer;
        }
        bool confirm(std::string_view title, std::string_view message, std::string_view okLabel) override {
            (void)message;
            const auto answer = owner_.takeAnswer("confirm");
            if (!answer.has_value()) {
                owner_.unexpected(std::format("confirmation '{}'", title));
                return false;
            }
            const bool yes = answerLower(*answer) == "yes";
            qaLog(std::format("alert: confirm '{}' [{}] -> {}", title, okLabel, yes ? "yes" : "no"));
            return yes;
        }

    private:
        static std::string answerLower(std::string_view text) {
            std::string out(text);
            for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }
        Impl& owner_;
    };

    class Appearance final : public platform::IAppearance {
    public:
        explicit Appearance(platform::IAppearance* inner) : inner_(inner) {
            if (inner_ != nullptr) inner_->onChange([this] { notify(); });
        }
        ~Appearance() override {
            if (inner_ != nullptr) inner_->onChange({});
        }
        Appearance(const Appearance&) = delete;
        Appearance& operator=(const Appearance&) = delete;
        bool isDark() const override {
            if (override_.has_value()) return *override_;
            return inner_ != nullptr && inner_->isDark();
        }
        std::optional<platform::AccentColor> accentColor() const override {
            return inner_ != nullptr ? inner_->accentColor() : std::nullopt;
        }
        void onChange(std::function<void()> callback) override { callback_ = std::move(callback); }
        void setOverride(std::optional<bool> dark) {
            override_ = dark;
            notify();
        }

    private:
        void notify() {
            if (callback_) callback_();
        }
        platform::IAppearance* inner_;
        std::optional<bool> override_;
        std::function<void()> callback_;
    };

    std::unique_ptr<FileDialog> fileDialog;
    std::unique_ptr<Alerts> alerts;
    std::unique_ptr<Appearance> appearance;

    // Runner state ------------------------------------------------------------
    using Clock = std::chrono::steady_clock;
    enum class Phase : std::uint8_t { NotStarted, WaitIdle, Sleep, Done };

    ShellController* shell = nullptr;
    platform::ShellServices services;
    std::function<void(int)> quit;
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
    std::jthread ticker;
    std::atomic<bool> tickQueued{false};
    Phase phase = Phase::NotStarted;
    std::size_t next = 0;
    int consecutiveIdle = 0;
    int currentLine = 0;
    Clock::time_point phaseStart;
    Clock::time_point sleepUntil;
    std::chrono::milliseconds idleTimeout{30000};
    bool running = false; // re-entrancy guard
    int exitCode = 0;
    std::vector<qa::Step> steps;
    std::vector<Menu> menuBar;
    std::vector<const MenuItem*> menuItems;

    void fail(const qa::Step& step, std::string_view why) {
        ++failures;
        qaLog(std::format("line {}: FAIL {} -- {}", step.line, step.source, why));
    }
    void ok(const qa::Step& step, std::string_view detail = {}) {
        if (detail.empty()) qaLog(std::format("line {}: ok   {}", step.line, step.source));
        else qaLog(std::format("line {}: ok   {} ({})", step.line, step.source, detail));
    }

    void finish(int code) {
        if (phase == Phase::Done) return;
        phase = Phase::Done;
        exitCode = code;
        qaLog(std::format("done: {} step(s) run, {} failure(s), exit code {}", stepsRun, failures, code));
        ticker.request_stop();
        if (quit) {
            auto handler = std::move(quit);
            quit = nullptr;
            handler(code);
        }
    }

    void tick() {
        tickQueued.store(false, std::memory_order_release);
        if (running || shell == nullptr || phase == Phase::Done) return;
        running = true;
        advance();
        running = false;
    }

    void advance() {
        const auto now = Clock::now();
        switch (phase) {
        case Phase::NotStarted:
            phase = Phase::WaitIdle;
            phaseStart = now;
            consecutiveIdle = 0;
            return;
        case Phase::Sleep:
            if (now < sleepUntil) return;
            phase = Phase::WaitIdle;
            phaseStart = now;
            consecutiveIdle = 0;
            return;
        case Phase::WaitIdle:
            if (shell->isIdle()) {
                ++consecutiveIdle;
            } else {
                consecutiveIdle = 0;
            }
            if (consecutiveIdle < 3) {
                if (now - phaseStart > idleTimeout) {
                    ++failures;
                    qaLog(std::format("line {}: ERROR idle timeout after {} ms (continuing)", currentLine,
                                      idleTimeout.count()));
                } else {
                    return;
                }
            }
            break;
        case Phase::Done:
            return;
        }
        // Ready for the next step.
        if (next >= steps.size()) {
            finish(failures == 0 ? 0 : 1);
            return;
        }
        const qa::Step step = steps[next++];
        currentLine = step.line;
        ++stepsRun;
        phase = Phase::WaitIdle;
        consecutiveIdle = 0;
        execute(step);
        phaseStart = Clock::now();
    }

    // Input synthesis -----------------------------------------------------------
    void pointer(ui::PointerEventType type, core::Point point, int button, const ui::ModifierFlags& modifiers,
                 core::Point scrollDelta = {}) {
        ui::PointerEvent event;
        event.type = type;
        event.position = point;
        event.button = button;
        event.modifiers = modifiers;
        event.scrollDelta = scrollDelta;
        shell->rootWidget().onMouse(event);
    }
    void clickAt(core::Point point, int button, const ui::ModifierFlags& modifiers, int count) {
        pointer(ui::PointerEventType::Move, point, 0, modifiers);
        for (int i = 0; i < count; ++i) {
            pointer(ui::PointerEventType::Down, point, button, modifiers);
            pointer(ui::PointerEventType::Up, point, button, modifiers);
        }
    }
    void dragBetween(core::Point from, core::Point to, const ui::ModifierFlags& modifiers) {
        pointer(ui::PointerEventType::Move, from, 0, modifiers);
        pointer(ui::PointerEventType::Down, from, 1, modifiers);
        constexpr int kSteps = 12;
        for (int i = 1; i <= kSteps; ++i) {
            const double t = static_cast<double>(i) / kSteps;
            pointer(ui::PointerEventType::Move,
                    core::Point{from.x + (to.x - from.x) * t, from.y + (to.y - from.y) * t}, 1, modifiers);
        }
        pointer(ui::PointerEventType::Up, to, 1, modifiers);
    }

    // PDF user space of page `page` (1-based) -> root coordinates; scrolls the
    // point into view first when needed.
    std::optional<core::Point> pagePoint(int page, core::Point user, std::string& error) {
        ui::PdfViewport& viewport = shell->pdfViewport();
        const render::PageLayout* layout = viewport.pageLayout();
        if (layout == nullptr) {
            error = "no PDF document in the viewport";
            return std::nullopt;
        }
        const auto index = static_cast<std::size_t>(page - 1);
        if (index >= layout->pages().size()) {
            error = std::format("page {} out of range (1..{})", page, layout->pages().size());
            return std::nullopt;
        }
        const auto map = [&]() -> std::optional<core::Point> {
            const std::optional<core::Rect> rect = viewport.pageRectInViewport(index);
            if (!rect.has_value()) return std::nullopt;
            const render::PageLayout::PageInfo& info = layout->pages()[index];
            const bool quarter = info.rotation == core::PageRotation::Clockwise90 ||
                                 info.rotation == core::PageRotation::Clockwise270;
            const core::Size unrotated = quarter ? core::Size{info.sizePoints.height, info.sizePoints.width}
                                                 : info.sizePoints;
            const double zoom = viewport.zoomFactor() > 0.0 ? viewport.zoomFactor() : 1.0;
            const render::PageTransform transform =
                render::PageTransform::make(*rect, unrotated, info.rotation, zoom, 1.0);
            return transform.pageToLogical(user);
        };
        std::optional<core::Point> local = map();
        const core::Rect bounds = viewport.bounds().inset(core::Insets{4.0, 4.0, 4.0, 4.0});
        if (!local.has_value() || !bounds.contains(*local)) {
            const std::optional<core::Rect> rect = viewport.pageRectInViewport(index);
            if (rect.has_value() && local.has_value()) {
                const double zoom = viewport.zoomFactor() > 0.0 ? viewport.zoomFactor() : 1.0;
                const core::Point display{(local->x - rect->origin.x) / zoom, (local->y - rect->origin.y) / zoom};
                viewport.revealContentRect(index, core::Rect{display.x - 1.0, display.y - 1.0, 2.0, 2.0});
            } else {
                viewport.goToPage(index);
            }
            local = map();
        }
        if (!local.has_value()) {
            error = "page is not laid out";
            return std::nullopt;
        }
        if (!viewport.bounds().contains(*local)) {
            error = "point is outside the visible viewport";
            return std::nullopt;
        }
        return viewport.originInRoot() + *local;
    }

    const MenuItem* findMenuItem(std::string_view path) const {
        const std::size_t slash = path.find('/');
        const std::string menu = qa::normalizeMenuTitle(path.substr(0, slash));
        const std::string item = qa::normalizeMenuTitle(path.substr(slash + 1));
        for (const Menu& m : menuBar) {
            if (qa::normalizeMenuTitle(m.title) != menu) continue;
            for (const MenuItem& entry : m.items) {
                if (qa::normalizeMenuTitle(entry.title) == item) return &entry;
            }
        }
        return nullptr;
    }

    // Menu key equivalents first (as AppKit / the in-window menu strips do),
    // then the shell's key routing.
    bool sendKey(const ui::KeyEvent& event, std::string& route) {
        const bool primary = qa::kPrimaryIsCommand ? event.modifiers.command : event.modifiers.control;
        if (primary && event.key == ui::Key::Character) {
            const Shortcut wanted{qa::normalizeMenuTitle(event.text), true, event.modifiers.shift, event.modifiers.option};
            for (const MenuItem* item : menuItems) {
                if (!item->shortcut.has_value() || *item->shortcut != wanted) continue;
                if (shell->canPerform(item->command)) {
                    route = std::format("menu '{}'", item->title);
                    shell->perform(item->command);
                    return true;
                }
            }
        }
        route = "shell";
        return shell->handleKeyEvent(event);
    }

    std::filesystem::path outputPath(const std::string& text) const {
        std::filesystem::path path(text);
        if (path.is_relative()) {
            if (const auto out = environmentValue("RIVET_QA_OUT"); out.has_value()) {
                path = std::filesystem::path(*out) / path;
            }
        }
        return path;
    }

    void execute(const qa::Step& step) {
        using qa::StepKind;
        switch (step.kind) {
        case StepKind::Open:
            shell->openDocument(std::filesystem::path(step.text));
            ok(step);
            return;
        case StepKind::Cmd:
        case StepKind::AssertEnabled:
        case StepKind::AssertDisabled: {
            const MenuItem* item = findMenuItem(step.text);
            if (item == nullptr) {
                fail(step, "no such menu item");
                return;
            }
            const bool enabled = shell->canPerform(item->command);
            if (step.kind == StepKind::AssertEnabled) {
                enabled ? ok(step) : fail(step, "item is disabled");
                return;
            }
            if (step.kind == StepKind::AssertDisabled) {
                enabled ? fail(step, "item is enabled") : ok(step);
                return;
            }
            if (!enabled) {
                fail(step, "menu item is disabled");
                return;
            }
            shell->perform(item->command);
            ok(step);
            return;
        }
        case StepKind::Click:
        case StepKind::DoubleClick:
        case StepKind::RightClick:
            clickAt(core::Point{step.numbers[0], step.numbers[1]}, step.kind == StepKind::RightClick ? 2 : 1,
                    step.modifiers, step.kind == StepKind::DoubleClick ? 2 : 1);
            ok(step);
            return;
        case StepKind::Drag:
            dragBetween(core::Point{step.numbers[0], step.numbers[1]}, core::Point{step.numbers[2], step.numbers[3]},
                        step.modifiers);
            ok(step);
            return;
        case StepKind::Scroll:
            pointer(ui::PointerEventType::Scroll, core::Point{step.numbers[0], step.numbers[1]}, 0, step.modifiers,
                    core::Point{step.numbers[2], step.numbers[3]});
            ok(step);
            return;
        case StepKind::ClickPage:
        case StepKind::DoubleClickPage: {
            std::string error;
            const auto point = pagePoint(step.page, core::Point{step.numbers[0], step.numbers[1]}, error);
            if (!point.has_value()) {
                fail(step, error);
                return;
            }
            clickAt(*point, 1, step.modifiers, step.kind == StepKind::DoubleClickPage ? 2 : 1);
            ok(step, std::format("at {:.0f},{:.0f}", point->x, point->y));
            return;
        }
        case StepKind::DragPage: {
            std::string error;
            const auto from = pagePoint(step.page, core::Point{step.numbers[0], step.numbers[1]}, error);
            const auto to = from.has_value()
                                ? pagePoint(step.page, core::Point{step.numbers[2], step.numbers[3]}, error)
                                : std::nullopt;
            // The second mapping may have scrolled: map the start again.
            const auto start = to.has_value()
                                   ? pagePoint(step.page, core::Point{step.numbers[0], step.numbers[1]}, error)
                                   : std::nullopt;
            if (!start.has_value() || !to.has_value()) {
                fail(step, error);
                return;
            }
            dragBetween(*start, *to, step.modifiers);
            ok(step, std::format("{:.0f},{:.0f} -> {:.0f},{:.0f}", start->x, start->y, to->x, to->y));
            return;
        }
        case StepKind::Key: {
            const auto event = qa::parseKeyCombo(step.text, qa::kPrimaryIsCommand);
            if (!event.has_value()) {
                fail(step, "bad key combo");
                return;
            }
            std::string route;
            const bool handled = sendKey(*event, route);
            ok(step, std::format("{} via {}", handled ? "handled" : "NOT handled", route));
            return;
        }
        case StepKind::Type: {
            std::size_t unhandled = 0;
            const std::vector<ui::KeyEvent> events = qa::keyEventsForText(step.text);
            for (const ui::KeyEvent& event : events) {
                if (!shell->handleKeyEvent(event)) ++unhandled;
            }
            if (unhandled != 0) ok(step, std::format("{} of {} keys not handled", unhandled, events.size()));
            else ok(step);
            return;
        }
        case StepKind::Wait:
            phase = Phase::Sleep;
            sleepUntil = Clock::now() + std::chrono::milliseconds(static_cast<long long>(step.numbers[0]));
            ok(step);
            return;
        case StepKind::Idle:
            ok(step);
            return;
        case StepKind::Shot: {
            const std::filesystem::path path = outputPath(step.text);
            if (!services.captureWindowPng) {
                qaLog(std::format("line {}: skip {} -- no screenshot hook on this platform", step.line, step.source));
                return;
            }
            std::error_code ec;
            if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
            if (services.captureWindowPng(path)) ok(step, path.string());
            else fail(step, "could not write the screenshot");
            return;
        }
        case StepKind::Resize:
            if (!services.resizeWindow) {
                qaLog(std::format("line {}: skip {} -- no resize hook on this platform", step.line, step.source));
                return;
            }
            services.resizeWindow(step.numbers[0], step.numbers[1]);
            ok(step, std::format("root now {:.0f}x{:.0f}", shell->rootWidget().frame().size.width,
                                 shell->rootWidget().frame().size.height));
            return;
        case StepKind::Theme:
            if (appearance == nullptr) {
                fail(step, "appearance double not installed");
                return;
            }
            appearance->setOverride(step.text == "system" ? std::nullopt
                                                          : std::optional<bool>(step.text == "dark"));
            ok(step);
            return;
        case StepKind::Answer:
            answers[step.answerKind].push_back(step.text);
            ok(step);
            return;
        case StepKind::AssertStatus: {
            const std::string& status = shell->statusText();
            if (status.find(step.text) != std::string::npos) ok(step);
            else fail(step, std::format("status is '{}'", status));
            return;
        }
        case StepKind::AssertFind: {
            const std::string count = shell->searchCountText();
            if (count.find(step.text) != std::string::npos) ok(step);
            else fail(step, std::format("find result is '{}'", count));
            return;
        }
        case StepKind::AssertClipboard: {
            if (services.clipboard == nullptr) {
                fail(step, "no clipboard service");
                return;
            }
            // The clipboard holds document text: only report a match.
            const std::string text = services.clipboard->text();
            if (text.find(step.text) != std::string::npos) ok(step);
            else fail(step, std::format("clipboard ({} bytes) does not contain the text", text.size()));
            return;
        }
        case StepKind::AssertPages: {
            const DocumentTab* tab = shell->workspace().activeTab();
            if (tab == nullptr || tab->session() == nullptr) {
                fail(step, "no ready PDF tab");
                return;
            }
            const std::size_t count = tab->session()->pageCount();
            if (static_cast<double>(count) == step.numbers[0]) ok(step);
            else fail(step, std::format("page count is {}", count));
            return;
        }
        case StepKind::AssertTabs: {
            const std::size_t count = shell->workspace().tabCount();
            if (static_cast<double>(count) == step.numbers[0]) ok(step);
            else fail(step, std::format("tab count is {}", count));
            return;
        }
        case StepKind::LogState:
            qaLog(std::format("line {}: state {}", step.line, describeState()));
            return;
        case StepKind::Log:
            qaLog(std::format("line {}: log {}", step.line, step.text));
            return;
        case StepKind::Quit:
            finish(failures == 0 ? 0 : 1);
            return;
        }
    }

    std::string describeState() const {
        std::ostringstream out;
        DocumentWorkspace& workspace = shell->workspace();
        out << "tabs=" << workspace.tabCount();
        if (const DocumentTab* tab = workspace.activeTab(); tab != nullptr) {
            out << " active='" << tab->title() << "'";
            out << " kind=" << (tab->isPdf() ? "pdf" : tab->isMarkdown() ? "markdown" : "comparison");
            out << " dirty=" << (tab->isDirty() ? "yes" : "no");
            if (tab->session() != nullptr) {
                const ui::PdfViewport& viewport = shell->pdfViewport();
                out << " pages=" << tab->session()->pageCount() << " page=" << (viewport.currentPageIndex() + 1)
                    << std::format(" zoom={:.0f}%", viewport.zoom().zoom() * 100.0)
                    << std::format(" scroll={:.0f},{:.0f}", viewport.scrollOffsetPoints().x,
                                   viewport.scrollOffsetPoints().y);
            }
        }
        out << " tool=" << static_cast<int>(shell->toolMode());
        const core::Size size = shell->rootWidget().frame().size;
        out << std::format(" root={:.0f}x{:.0f}", size.width, size.height);
        out << " status='" << shell->statusText() << "'";
        if (const std::string find = shell->searchCountText(); !find.empty()) out << " find='" << find << "'";
        return out.str();
    }
};

QaSession::QaSession() : impl_(std::make_shared<Impl>()) {}

QaSession::~QaSession() {
    *impl_->alive = false;
    impl_->ticker.request_stop();
    if (impl_->ticker.joinable()) impl_->ticker.join();
}

std::unique_ptr<QaSession> QaSession::fromEnvironment() {
    const auto file = environmentValue("RIVET_QA_SCRIPT");
    if (!file.has_value() || file->empty()) return nullptr;
    std::ifstream in(*file, std::ios::binary);
    if (!in) {
        auto session = std::unique_ptr<QaSession>(new QaSession());
        session->impl_->name = *file;
        session->impl_->loadFailed = true;
        qaLog(std::format("ERROR: cannot read script {}", *file));
        return session;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return fromSource(text.str(), *file);
}

std::unique_ptr<QaSession> QaSession::fromSource(std::string_view source, std::string name) {
    auto session = std::unique_ptr<QaSession>(new QaSession());
    Impl& impl = *session->impl_;
    impl.name = std::move(name);
    impl.script = qa::parseScript(source, environmentValue);
    for (const qa::ParseError& error : impl.script.errors) {
        qaLog(std::format("{}:{}: parse error: {}", impl.name, error.line, error.message));
    }
    if (const auto timeout = environmentValue("RIVET_QA_IDLE_TIMEOUT_MS"); timeout.has_value()) {
        int ms = 0;
        const auto [ptr, ec] = std::from_chars(timeout->data(), timeout->data() + timeout->size(), ms);
        if (ec == std::errc{} && ms > 0) impl.idleTimeout = std::chrono::milliseconds(ms);
    }
    return session;
}

void QaSession::installDoubles(platform::ShellServices& services) {
    Impl& impl = *impl_;
    impl.fileDialog = std::make_unique<Impl::FileDialog>(impl);
    impl.alerts = std::make_unique<Impl::Alerts>(impl);
    impl.appearance = std::make_unique<Impl::Appearance>(services.appearance);
    services.fileDialog = impl.fileDialog.get();
    services.saveDialog = impl.fileDialog.get();
    services.alerts = impl.alerts.get();
    services.appearance = impl.appearance.get();
}

void QaSession::start(ShellController& shell, const platform::ShellServices& services,
                      std::function<void(int)> quit) {
    Impl& impl = *impl_;
    impl.shell = &shell;
    impl.services = services;
    impl.quit = std::move(quit);
    impl.menuBar = buildMenuBar();
    impl.menuItems = flattenMenuItems(impl.menuBar);
    qaLog(std::format("script {} ({} step(s))", impl.name, impl.script.steps.size()));
    if (impl.loadFailed || !impl.script.errors.empty() || services.mainDispatcher == nullptr) {
        if (services.mainDispatcher == nullptr) qaLog("ERROR: no main-thread dispatcher");
        impl.finish(2);
        return;
    }
    impl.steps = std::move(impl.script.steps);
    // The ticker only posts; every step runs on the main thread. It holds
    // the dispatcher (outlives the session) and a weak view of the session.
    core::IMainThreadDispatcher* dispatcher = services.mainDispatcher;
    std::weak_ptr<Impl> weak = impl_;
    std::shared_ptr<bool> alive = impl.alive;
    impl.ticker = std::jthread([dispatcher, weak, alive](const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
            const std::shared_ptr<Impl> self = weak.lock();
            if (self == nullptr) return;
            if (self->tickQueued.exchange(true, std::memory_order_acq_rel)) continue;
            dispatcher->post([weak, alive] {
                // Main thread: the session dies on the main thread too.
                if (!*alive) return;
                if (const std::shared_ptr<Impl> target = weak.lock()) target->tick();
            });
        }
    });
}

int QaSession::exitCode() const { return impl_->exitCode; }

bool QaSession::finished() const { return impl_->phase == Impl::Phase::Done; }

} // namespace rivet::app
