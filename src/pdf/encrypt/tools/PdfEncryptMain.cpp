// SPDX-License-Identifier: MPL-2.0
// Dev/test tool: encrypts a PDF with the standard security handler (ADR-0046).
// Not shipped in the app. Passwords are never taken from argv: they come from
// RIVET_USER_PASSWORD / RIVET_OWNER_PASSWORD, or (with --passwords-stdin) from
// the first two lines of stdin (user, then owner; either may be empty).
//
//   rivet-pdf-encrypt --in a.pdf --out b.pdf [--cipher aes256|aes128]
//       [--passwords-stdin] [--no-encrypt-metadata]
//       [--deny print,print-hq,copy,modify,annotate,forms,assemble,accessibility]
//
// The input must be the byte output of a PDFium save (classic xref table, no
// object streams, not encrypted, not signed); anything else is refused.
//
// Exit codes: 0 ok, 1 operation failed, 2 usage error.

#include "core/Error.hpp"
#include "pdf/encrypt/PdfEncrypt.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

using namespace rivet;

int usage(const char* why) {
    std::fprintf(stderr, "rivet-pdf-encrypt: %s\n", why);
    std::fprintf(stderr,
                 "usage: rivet-pdf-encrypt --in a.pdf --out b.pdf [--cipher aes256|aes128] [--passwords-stdin]\n"
                 "       [--no-encrypt-metadata] [--deny print,print-hq,copy,modify,annotate,forms,assemble,accessibility]\n"
                 "passwords: RIVET_USER_PASSWORD / RIVET_OWNER_PASSWORD, or --passwords-stdin (2 lines)\n");
    return 2;
}

bool readFile(const std::string& path, std::vector<std::uint8_t>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char buf[1 << 16];
    while (in.read(buf, sizeof buf) || in.gcount() > 0) {
        out.insert(out.end(), buf, buf + in.gcount());
        if (in.eof()) break;
    }
    return !in.bad();
}

} // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> opt;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--passwords-stdin" || a == "--no-encrypt-metadata") {
            opt[a] = "1";
        } else if ((a == "--in" || a == "--out" || a == "--cipher" || a == "--deny") && i + 1 < argc) {
            opt[a] = argv[++i];
        } else {
            return usage(("unknown or incomplete option: " + a).c_str());
        }
    }
    if (!opt.count("--in") || !opt.count("--out")) return usage("--in and --out are required");

    pdf::encrypt::EncryptOptions o;
    if (opt.count("--cipher")) {
        if (opt["--cipher"] == "aes256") o.cipher = pdf::encrypt::Cipher::Aes256;
        else if (opt["--cipher"] == "aes128") o.cipher = pdf::encrypt::Cipher::Aes128;
        else return usage("--cipher must be aes256 or aes128");
    }
    o.encryptMetadata = !opt.count("--no-encrypt-metadata");

    if (opt.count("--deny")) {
        const std::string list = opt["--deny"] + ",";
        std::size_t start = 0;
        while (start < list.size()) {
            const std::size_t comma = list.find(',', start);
            const std::string item = list.substr(start, comma - start);
            start = comma + 1;
            auto& p = o.permissions;
            if (item == "print") p.print = false;
            else if (item == "print-hq") p.printHighQuality = false;
            else if (item == "copy") p.copy = false;
            else if (item == "modify") p.modify = false;
            else if (item == "annotate") p.annotate = false;
            else if (item == "forms") p.fillForms = false;
            else if (item == "assemble") p.assemble = false;
            else if (item == "accessibility") p.accessibility = false;
            else if (!item.empty()) return usage(("unknown permission: " + item).c_str());
        }
    }

    if (opt.count("--passwords-stdin")) {
        std::getline(std::cin, o.userPassword);
        std::getline(std::cin, o.ownerPassword);
        while (!o.userPassword.empty() && (o.userPassword.back() == '\r')) o.userPassword.pop_back();
        while (!o.ownerPassword.empty() && (o.ownerPassword.back() == '\r')) o.ownerPassword.pop_back();
    } else {
        if (const char* u = std::getenv("RIVET_USER_PASSWORD")) o.userPassword = u;
        if (const char* w = std::getenv("RIVET_OWNER_PASSWORD")) o.ownerPassword = w;
    }

    std::vector<std::uint8_t> input;
    if (!readFile(opt["--in"], input)) {
        std::fprintf(stderr, "rivet-pdf-encrypt: cannot read %s\n", opt["--in"].c_str());
        return 1;
    }
    auto encrypted = pdf::encrypt::encryptPdf(input, o);
    if (!encrypted) {
        std::fprintf(stderr, "rivet-pdf-encrypt: %s\n", core::describe(encrypted.error()).c_str());
        return 1;
    }
    std::ofstream out(opt["--out"], std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(encrypted->data()), static_cast<std::streamsize>(encrypted->size()));
    out.flush();
    if (!out) {
        std::fprintf(stderr, "rivet-pdf-encrypt: cannot write %s\n", opt["--out"].c_str());
        return 1;
    }
    return 0;
}
