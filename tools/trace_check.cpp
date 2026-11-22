// trace_check: every requirement traced to the tests that verify it, every risk to its tests
// (M6.1, ctest trace_check.EveryRequirementAndRiskIsTracedToTests).
//
//   trace_check --source-dir=DIR [--write]
//
// Finds the tests in DIR's sources (TEST / TEST_F in *.cpp, add_test(NAME ...) in CMakeLists.txt),
// reads DIR/docs/requirements.md and DIR/docs/risks.md, and fails when:
//   - a requirement has no test carrying its ID, or a test carries an ID that is not a requirement;
//   - a "Verified by" entry matches no test, or a test with the ID is missing from "Verified by";
//   - a risk is verified by no test, or names no requirement, or one that does not exist;
//   - DIR/docs/traceability.md is not what this run generates (--write regenerates it).
// Exits 0 and prints PASS only if all of that holds.

#include "trace.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot read " + path.string());
    }
    std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::erase(text, '\r');
    return text;
}

// Build trees, the repository's own metadata, and anything hidden are not sources.
bool skipped(const fs::path& dir) {
    const auto name = dir.filename().string();
    return name.starts_with('.') || name == "build" || name.starts_with("build-") || name == "_deps";
}

std::vector<msc::trace::DeclaredTest> findTests(const fs::path& root) {
    std::vector<msc::trace::DeclaredTest> tests;
    for (auto it = fs::recursive_directory_iterator(root); it != fs::recursive_directory_iterator(); ++it) {
        if (it->is_directory()) {
            if (skipped(it->path())) {
                it.disable_recursion_pending();
            }
            continue;
        }
        const auto& path = it->path();
        const auto relative = fs::relative(path, root).generic_string();
        if (path.extension() == ".cpp") {
            auto found = msc::trace::findTestsInCpp(readFile(path), relative);
            tests.insert(tests.end(), found.begin(), found.end());
        } else if (path.filename() == "CMakeLists.txt") {
            auto found = msc::trace::findTestsInCMake(readFile(path), relative);
            tests.insert(tests.end(), found.begin(), found.end());
        }
    }
    return tests;
}

int run(const fs::path& root, bool write) {
    const auto docs = root / "docs";
    const auto requirements = msc::trace::parseRequirements(readFile(docs / "requirements.md"));
    const auto risks = msc::trace::parseRisks(readFile(docs / "risks.md"));
    const auto tests = findTests(root);
    const auto generated = msc::trace::report(requirements, risks, tests);

    auto problems = msc::trace::check(requirements, risks, tests);

    const auto reportPath = docs / "traceability.md";
    if (write) {
        std::ofstream(reportPath, std::ios::binary) << generated;
        std::cout << "wrote " << reportPath.generic_string() << "\n";
    } else if (!fs::exists(reportPath) || readFile(reportPath) != generated) {
        problems.push_back("docs/traceability.md is out of date: run trace_check --source-dir=" +
                           root.generic_string() + " --write and commit the result");
    }

    std::cout << requirements.size() << " requirements, " << risks.size() << " risks, " << tests.size()
              << " tests found in " << root.generic_string() << "\n";
    for (const auto& p : problems) {
        std::cout << "FAIL: " << p << "\n";
    }
    if (!problems.empty()) {
        std::cout << problems.size() << " problem(s)\n";
        return 1;
    }
    std::cout << "PASS\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        fs::path root;
        bool write = false;
        for (const std::string_view arg : std::span(argv, static_cast<std::size_t>(argc)).subspan(1)) {
            if (arg.starts_with("--source-dir=")) {
                root = fs::path(std::string(arg.substr(13)));
            } else if (arg == "--write") {
                write = true;
            } else {
                std::cerr << "trace_check: unknown option " << arg
                          << "\nusage: trace_check --source-dir=DIR [--write]\n";
                return 2;
            }
        }
        if (root.empty()) {
            std::cerr << "usage: trace_check --source-dir=DIR [--write]\n";
            return 2;
        }
        return run(root, write);
    } catch (const std::exception& e) {
        std::cout << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
