// SPDX-License-Identifier: MPL-2.0
#include "LinuxFileDialog.hpp"

#include "LinuxAlertService.hpp"
#include "LinuxSubprocess.hpp"
#include "X11Window.hpp"

#include <cctype>
#include <cstdlib>
#include <system_error>
#include <utility>

namespace rivet::platform {

namespace {

constexpr const char* kSubsystem = "platform.linux";

std::string sanitize(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        const auto uc = static_cast<unsigned char>(c);
        if (uc >= 0x20 && uc != 0x7F) out += c;
    }
    return out;
}

// A path argument that can never be mistaken for an option.
std::string pathArgument(const std::filesystem::path& path) {
    std::string text = sanitize(path.string());
    if (text.empty()) return ".";
    if (text.front() == '-') text = "./" + text;
    return text;
}

std::string lower(std::string text) {
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

std::string upper(std::string text) {
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return text;
}

std::filesystem::path homeDirectory() {
    const char* home = std::getenv("HOME");
    return home != nullptr && home[0] == '/' ? std::filesystem::path(home) : std::filesystem::path("/");
}

core::Error cancelled() {
    return core::makeError(core::ErrorCode::Cancelled, "user dismissed the open dialog", kSubsystem);
}

std::vector<std::filesystem::path> parsePaths(const std::string& output) {
    std::vector<std::filesystem::path> paths;
    std::size_t start = 0;
    while (start < output.size()) {
        std::size_t end = output.find('\n', start);
        if (end == std::string::npos) end = output.size();
        std::string line = output.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line.front() == '/') paths.emplace_back(line);
    }
    return paths;
}

} // namespace

LinuxFileDialog::LinuxFileDialog(X11Connection& connection, LinuxAlertService* alerts)
    : connection_(connection), alerts_(alerts) {
    if (auto zenity = subprocess::findExecutable("zenity")) {
        tool_ = Tool::Zenity;
        toolPath_ = std::move(*zenity);
    } else if (auto kdialog = subprocess::findExecutable("kdialog")) {
        tool_ = Tool::Kdialog;
        toolPath_ = std::move(*kdialog);
    }
}

core::Result<std::vector<std::filesystem::path>> LinuxFileDialog::runOpen(const std::string& title,
                                                                         const Filter& filter, bool multiple) {
    if (tool_ == Tool::None) {
        return std::unexpected(core::makeError(core::ErrorCode::Unsupported,
                                               "no file chooser available: install zenity or kdialog", kSubsystem));
    }
    std::string patterns;
    for (const std::string& extension : filter.extensions) {
        if (!patterns.empty()) patterns += ' ';
        patterns += "*." + extension + " *." + upper(extension);
    }

    std::vector<std::string> args;
    if (tool_ == Tool::Zenity) {
        args = {"--file-selection", "--title=" + sanitize(title),
                "--file-filter=" + sanitize(filter.name) + " | " + patterns, "--file-filter=All files | *"};
        if (multiple) {
            args.emplace_back("--multiple");
            args.emplace_back("--separator=\n");
        }
    } else {
        args = {"--title", sanitize(title)};
        if (owner_ != nullptr) {
            args.emplace_back("--attach");
            args.emplace_back(std::to_string(owner_->id()));
        }
        args.emplace_back("--getopenfilename");
        args.emplace_back(pathArgument(homeDirectory()));
        args.emplace_back(patterns + "|" + sanitize(filter.name));
        if (multiple) {
            args.emplace_back("--multiple");
            args.emplace_back("--separate-output");
        }
    }

    const subprocess::Result result = subprocess::run(toolPath_, args, [this](int fd) {
        connection_.pump(X11Connection::Mode::Modal, nullptr, fd, -1);
    });
    if (!result.started) {
        return std::unexpected(core::makeError(core::ErrorCode::Internal, "cannot start the file chooser", kSubsystem));
    }
    if (result.exitCode == 1) return std::unexpected(cancelled());
    if (result.exitCode != 0) {
        return std::unexpected(core::makeError(core::ErrorCode::Internal, "the file chooser failed", kSubsystem));
    }
    std::vector<std::filesystem::path> paths = parsePaths(result.output);
    if (paths.empty()) return std::unexpected(cancelled());
    if (!multiple) paths.resize(1);
    return paths;
}

core::Result<std::filesystem::path> LinuxFileDialog::openPdf() {
    auto chosen = openPdfs(OpenOptions{});
    if (!chosen) return std::unexpected(chosen.error());
    return std::move(chosen->front());
}

core::Result<std::filesystem::path> LinuxFileDialog::openDocument() {
    auto chosen = runOpen("Open", Filter{"Documents", {"pdf", "md", "markdown", "mdown"}}, false);
    if (!chosen) return std::unexpected(chosen.error());
    return std::move(chosen->front());
}

core::Result<std::vector<std::filesystem::path>> LinuxFileDialog::openPdfs(const OpenOptions& options) {
    return runOpen(options.title, Filter{"PDF documents", {"pdf"}}, options.allowMultiple);
}

core::Result<std::filesystem::path> LinuxFileDialog::openImage() {
    auto chosen = runOpen("Replace Image", Filter{"Images", {"png", "jpg", "jpeg"}}, false);
    if (!chosen) return std::unexpected(chosen.error());
    return std::move(chosen->front());
}

std::optional<std::filesystem::path> LinuxFileDialog::runSavePanel(const ISaveDialog::Options& options) {
    if (tool_ == Tool::None) return std::nullopt;
    std::vector<std::string> extensions = options.allowedExtensions;
    if (extensions.empty()) extensions.emplace_back("pdf");
    std::string patterns;
    for (const std::string& extension : extensions) {
        if (!patterns.empty()) patterns += ' ';
        patterns += "*." + extension;
    }
    const std::filesystem::path directory =
        options.initialDirectory.empty() ? homeDirectory() : options.initialDirectory;
    const std::filesystem::path start =
        options.suggestedName.empty() ? directory : directory / sanitize(options.suggestedName);

    std::vector<std::string> args;
    if (tool_ == Tool::Zenity) {
        std::string startArg = pathArgument(start);
        if (options.suggestedName.empty() && startArg.back() != '/') startArg += '/';
        args = {"--file-selection",
                "--save",
                "--confirm-overwrite",
                "--title=" + sanitize(options.title),
                "--filename=" + startArg,
                "--file-filter=" + upper(extensions.front()) + " | " + patterns};
    } else {
        args = {"--title", sanitize(options.title)};
        if (owner_ != nullptr) {
            args.emplace_back("--attach");
            args.emplace_back(std::to_string(owner_->id()));
        }
        args.emplace_back("--getsavefilename");
        args.emplace_back(pathArgument(start));
        args.emplace_back(patterns + "|" + upper(extensions.front()));
    }

    const subprocess::Result result = subprocess::run(toolPath_, args, [this](int fd) {
        connection_.pump(X11Connection::Mode::Modal, nullptr, fd, -1);
    });
    if (!result.started || result.exitCode != 0) return std::nullopt;
    const std::vector<std::filesystem::path> paths = parsePaths(result.output);
    if (paths.empty()) return std::nullopt;
    std::filesystem::path chosen = paths.front();
    if (chosen.filename().empty()) return std::nullopt;

    // Append the default extension when the typed name has none of ours.
    const std::string extension = lower(chosen.extension().string());
    bool known = false;
    for (const std::string& allowed : extensions) {
        if (extension == "." + allowed) known = true;
    }
    bool appended = false;
    if (!known) {
        chosen += "." + extensions.front();
        appended = true;
    }

    std::error_code error;
    const bool exists = std::filesystem::exists(chosen, error);
    if (exists && (appended || tool_ == Tool::Kdialog)) {
        // Never overwrite silently: no alert service means no.
        if (alerts_ == nullptr) return std::nullopt;
        const std::string name = chosen.filename().string();
        if (!alerts_->confirm("\xE2\x80\x9C" + name + "\xE2\x80\x9D already exists. Do you want to replace it?",
                              "A file with the same name already exists. Replacing it will overwrite its "
                              "current contents.",
                              "Replace")) {
            return std::nullopt;
        }
    }
    return chosen;
}

} // namespace rivet::platform
