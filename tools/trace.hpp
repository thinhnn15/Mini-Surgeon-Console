#pragma once

// Traceability (M6.1): which tests verify which requirement, and which risks are verified by
// which tests, worked out from the sources and the docs, so a script answers it, not a person.
//
// The tests are found in the sources, not asked from CTest: some are registered only where there
// is a display (console_app's screen checks), so CTest's list differs from machine to machine,
// while the question "is this requirement tested" must have one answer.
//
// A test verifies a requirement when its name carries the ID with underscores:
// REQ-COM-001 -> TEST(Crc32, REQ_COM_001_MatchesStandardCheckValue) -> "Crc32.REQ_COM_001_...".

#include <string>
#include <string_view>
#include <vector>

namespace msc::trace {

// A test as CTest names it: "Suite.Name" for Google Test, the NAME of add_test() otherwise.
struct DeclaredTest {
    std::string name{};
    std::string file{};  // relative to the source directory, '/' separated
    int line = 0;
};

// TEST(Suite, Name) and TEST_F(Fixture, Name), each at the start of a line.
std::vector<DeclaredTest> findTestsInCpp(std::string_view text, std::string_view file);
// add_test(NAME name ...), outside comments.
std::vector<DeclaredTest> findTestsInCMake(std::string_view text, std::string_view file);

// One row of the requirements table (docs/requirements.md).
struct Requirement {
    std::string id{};                       // "REQ-COM-001"
    std::vector<std::string> verifiedBy{};  // test names or patterns from the "Verified by" column
    int line = 0;
};

// One row of the risk table (docs/risks.md).
struct Risk {
    std::string id{};  // "RISK-001"
    std::string hazard{};
    std::vector<std::string> verifiedBy{};
    std::vector<std::string> requirements{};  // "REQ-..." IDs from the "Requirement" column
    int line = 0;
};

// Reads the first Markdown table whose header has the columns needed ("ID" and "Verified by";
// for risks also "Hazard" and "Requirement"). Throws std::runtime_error if there is none.
std::vector<Requirement> parseRequirements(std::string_view markdown);
std::vector<Risk> parseRisks(std::string_view markdown);

// The test references in a "Verified by" cell: every `back-quoted` Suite.Name, '*' allowed.
// Other back-quoted words (`arm_sim`, `--check-hud`) are not tests and are left out.
std::vector<std::string> testReferencesIn(std::string_view cell);

// '*' matches any run of characters, everything else itself.
bool matches(std::string_view pattern, std::string_view name);

// The requirement IDs a test name carries: "Crc32.REQ_COM_001_X" -> {"REQ-COM-001"}.
std::vector<std::string> requirementIdsIn(std::string_view testName);

// Everything wrong with the tables against the tests, one sentence each, in a stable order.
// Empty means every requirement and every risk is traced.
std::vector<std::string> check(const std::vector<Requirement>& requirements, const std::vector<Risk>& risks,
                               const std::vector<DeclaredTest>& tests);

// docs/traceability.md: requirement -> tests, risk -> requirements -> tests. Deterministic
// (sorted, no line numbers), so it changes only when the traceability does.
std::string report(const std::vector<Requirement>& requirements, const std::vector<Risk>& risks,
                   const std::vector<DeclaredTest>& tests);

}  // namespace msc::trace
