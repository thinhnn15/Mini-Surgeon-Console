#include "trace.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// trace_check scans this file too. The C++ and CMake snippets below are inside string literals
// that never start a line, so they are not taken for real tests, and no real test here carries a
// requirement ID.

namespace {

using msc::trace::DeclaredTest;

std::vector<std::string> names(const std::vector<DeclaredTest>& tests) {
    std::vector<std::string> out;
    out.reserve(tests.size());
    for (const auto& t : tests) {
        out.push_back(t.name);
    }
    return out;
}

bool mentions(const std::vector<std::string>& problems, std::string_view text) {
    return std::ranges::any_of(problems,
                               [text](const std::string& p) { return p.find(text) != std::string::npos; });
}

const std::string kRequirements =
    "# Requirements\n\nSome text | not a table\n\n"
    "| ID | Requirement | Milestone | Verified by |\n"
    "| --- | --- | --- | --- |\n"
    "| REQ-COM-001 | Packets carry a CRC. | M3 | `Crc32.REQ_COM_001_*`; `arm_sim` as a process |\n"
    "| REQ-SAF-001 | The arm holds. | M3 | `Arm.REQ_SAF_001_Holds`, see `arm.hpp` |\n";

const std::string kRisks =
    "| ID | Hazard | Sev | Verified by | Requirement |\n"
    "| --- | --- | --- | --- | --- |\n"
    "| RISK-001 | Arm keeps moving | 5 | `Arm.REQ_SAF_001_Holds` | REQ-SAF-001 |\n";

std::vector<DeclaredTest> cleanTests() {
    return {{"Crc32.REQ_COM_001_Check", "a.cpp", 1},
            {"Crc32.REQ_COM_001_Flip", "a.cpp", 5},
            {"Arm.REQ_SAF_001_Holds", "b.cpp", 3},
            {"Arm.Starts", "b.cpp", 9}};
}

}  // namespace

TEST(TraceFindTests, FindsTestAndTestFAtTheStartOfALine) {
    const std::string source = "#include <x>\nTEST(Crc32, Check) {}\n\n  TEST_F(Fixture, Holds) {\n}\n";
    const auto tests = msc::trace::findTestsInCpp(source, "dir/a.cpp");
    ASSERT_EQ(tests.size(), 2U);
    EXPECT_EQ(tests[0].name, "Crc32.Check");
    EXPECT_EQ(tests[0].file, "dir/a.cpp");
    EXPECT_EQ(tests[0].line, 2);
    EXPECT_EQ(tests[1].name, "Fixture.Holds");
    EXPECT_EQ(tests[1].line, 4);
}

TEST(TraceFindTests, ADeclarationMaySpanLines) {
    const std::string source = "TEST(Suite,\n     AVeryLongTestName) {}\n";
    EXPECT_EQ(names(msc::trace::findTestsInCpp(source, "a.cpp")),
              std::vector<std::string>{"Suite.AVeryLongTestName"});
}

TEST(TraceFindTests, IgnoresWhatIsNotADeclaration) {
    const std::string source =
        "// TEST(Commented, Out)\nauto s = \"TEST(InA, String)\";\nTEST_P(Param, Test) {}\nTESTING(A, B)\n"
        "TEST(OnlyOne)\n";
    EXPECT_TRUE(msc::trace::findTestsInCpp(source, "a.cpp").empty());
}

TEST(TraceFindTests, FindsAddTestNamesOutsideComments) {
    const std::string cmake =
        "if(BUILD_TESTING)\n    add_test(NAME arm_sim.version COMMAND arm_sim --version)\n"
        "    # add_test(NAME commented.out COMMAND x)\n"
        "    set_tests_properties(arm_sim.version PROPERTIES TIMEOUT 5)\n"
        "    add_test(\n        NAME console.Check\n        COMMAND check)\nendif()\n";
    const auto tests = msc::trace::findTestsInCMake(cmake, "arm_sim/CMakeLists.txt");
    ASSERT_EQ(tests.size(), 2U);
    EXPECT_EQ(tests[0].name, "arm_sim.version");
    EXPECT_EQ(tests[0].line, 2);
    EXPECT_EQ(tests[1].name, "console.Check");
    EXPECT_EQ(tests[1].line, 5);
}

TEST(TraceTables, ReadsTheRequirementsTableAndOnlyTheTestReferences) {
    const auto requirements = msc::trace::parseRequirements(kRequirements);
    ASSERT_EQ(requirements.size(), 2U);
    EXPECT_EQ(requirements[0].id, "REQ-COM-001");
    EXPECT_EQ(requirements[0].verifiedBy, std::vector<std::string>{"Crc32.REQ_COM_001_*"});
    EXPECT_EQ(requirements[1].verifiedBy, std::vector<std::string>{"Arm.REQ_SAF_001_Holds"});
    EXPECT_EQ(requirements[1].line, 8);
}

TEST(TraceTables, ReadsTheRiskTable) {
    const auto risks = msc::trace::parseRisks(kRisks);
    ASSERT_EQ(risks.size(), 1U);
    EXPECT_EQ(risks[0].id, "RISK-001");
    EXPECT_EQ(risks[0].hazard, "Arm keeps moving");
    EXPECT_EQ(risks[0].verifiedBy, std::vector<std::string>{"Arm.REQ_SAF_001_Holds"});
    EXPECT_EQ(risks[0].requirements, std::vector<std::string>{"REQ-SAF-001"});
}

TEST(TraceTables, APipeInsideBackQuotesDoesNotSplitTheCell) {
    const std::string markdown =
        "| ID | Verified by |\n| --- | --- |\n| REQ-COM-001 | `a|b` then `Crc32.REQ_COM_001_X` |\n";
    const auto requirements = msc::trace::parseRequirements(markdown);
    ASSERT_EQ(requirements.size(), 1U);
    EXPECT_EQ(requirements[0].verifiedBy, std::vector<std::string>{"Crc32.REQ_COM_001_X"});
}

TEST(TraceTables, AMissingTableIsAnError) {
    EXPECT_THROW((void)msc::trace::parseRequirements("| ID | Text |\n| --- | --- |\n"), std::runtime_error);
    EXPECT_THROW((void)msc::trace::parseRisks(kRequirements), std::runtime_error);
}

TEST(TraceTables, TestReferencesLeaveOutCommandsAndFiles) {
    EXPECT_EQ(msc::trace::testReferencesIn("`arm_sim`, `--check-hud`, `pose.hpp`, `docs/x.md`, `A.B*`"),
              std::vector<std::string>{"A.B*"});
}

TEST(TraceMatch, StarMatchesAnyRun) {
    EXPECT_TRUE(msc::trace::matches("Crc32.Check", "Crc32.Check"));
    EXPECT_TRUE(msc::trace::matches("Crc32.*", "Crc32.Check"));
    EXPECT_TRUE(msc::trace::matches("*.Check", "Crc32.Check"));
    EXPECT_TRUE(msc::trace::matches("C*2.*k", "Crc32.Check"));
    EXPECT_FALSE(msc::trace::matches("Crc32.Check", "Crc32.CheckMore"));
    EXPECT_FALSE(msc::trace::matches("Crc32.*", "Crc3.Check"));
    EXPECT_FALSE(msc::trace::matches("", "Crc32.Check"));
}

TEST(TraceIds, ReadsEveryIdATestNameCarries) {
    EXPECT_EQ(msc::trace::requirementIdsIn("Crc32.REQ_COM_001_Check"),
              std::vector<std::string>{"REQ-COM-001"});
    EXPECT_EQ(msc::trace::requirementIdsIn("Arm.REQ_SAF_001_And_REQ_COM_001_Too"),
              (std::vector<std::string>{"REQ-SAF-001", "REQ-COM-001"}));
    EXPECT_TRUE(msc::trace::requirementIdsIn("Crc32.Check").empty());
    EXPECT_TRUE(msc::trace::requirementIdsIn("Crc32.REQ_COM_0011_FourDigits").empty());
    EXPECT_TRUE(msc::trace::requirementIdsIn("Crc32.REQ_com_001_LowerCase").empty());
}

TEST(TraceCheck, ConsistentTablesHaveNoProblems) {
    EXPECT_TRUE(msc::trace::check(msc::trace::parseRequirements(kRequirements),
                                  msc::trace::parseRisks(kRisks), cleanTests())
                    .empty());
}

TEST(TraceCheck, ARequirementWithoutATestIsReported) {
    auto requirements = msc::trace::parseRequirements(kRequirements);
    requirements.push_back({"REQ-VID-001", {}, 9});
    const auto problems = msc::trace::check(requirements, msc::trace::parseRisks(kRisks), cleanTests());
    EXPECT_TRUE(mentions(problems, "REQ-VID-001 is not tested")) << problems.size();
}

TEST(TraceCheck, ATestWithAnUnknownIdIsReported) {
    auto tests = cleanTests();
    tests.push_back({"Video.REQ_VID_009_Typo", "v.cpp", 4});
    const auto problems = msc::trace::check(msc::trace::parseRequirements(kRequirements),
                                            msc::trace::parseRisks(kRisks), tests);
    EXPECT_TRUE(mentions(problems, "Video.REQ_VID_009_Typo (v.cpp:4) carries REQ-VID-009, which is not in"));
}

TEST(TraceCheck, AStaleVerifiedByEntryIsReported) {
    auto tests = cleanTests();
    std::erase_if(tests, [](const DeclaredTest& t) { return t.name == "Arm.REQ_SAF_001_Holds"; });
    const auto problems = msc::trace::check(msc::trace::parseRequirements(kRequirements),
                                            msc::trace::parseRisks(kRisks), tests);
    EXPECT_TRUE(
        mentions(problems, "REQ-SAF-001: `Arm.REQ_SAF_001_Holds` in \"Verified by\" matches no test"));
    EXPECT_TRUE(mentions(problems, "REQ-SAF-001 is not tested"));
    EXPECT_TRUE(mentions(problems, "RISK-001: `Arm.REQ_SAF_001_Holds` in \"Verified by\" matches no test"));
}

TEST(TraceCheck, ATestMissingFromVerifiedByIsReported) {
    auto tests = cleanTests();
    tests.push_back({"Link.REQ_SAF_001_Brakes", "l.cpp", 2});
    const auto problems = msc::trace::check(msc::trace::parseRequirements(kRequirements),
                                            msc::trace::parseRisks(kRisks), tests);
    EXPECT_TRUE(mentions(problems,
                         "REQ-SAF-001: test Link.REQ_SAF_001_Brakes (l.cpp:2) carries the ID but is not in"));
}

TEST(TraceCheck, AVerifiedByEntryForAnotherRequirementIsReported) {
    const std::string markdown =
        "| ID | Verified by |\n| --- | --- |\n"
        "| REQ-COM-001 | `Crc32.REQ_COM_001_*`, `Arm.Starts` |\n| REQ-SAF-001 | `Arm.REQ_SAF_001_Holds` |\n";
    const auto problems = msc::trace::check(msc::trace::parseRequirements(markdown),
                                            msc::trace::parseRisks(kRisks), cleanTests());
    EXPECT_TRUE(mentions(
        problems, "`Arm.Starts` in \"Verified by\" matches Arm.Starts, which does not carry REQ_COM_001"));
}

TEST(TraceCheck, RiskProblemsAreReported) {
    const std::string risks =
        "| ID | Hazard | Verified by | Requirement |\n| --- | --- | --- | --- |\n"
        "| RISK-001 | Unverified | | REQ-SAF-001 |\n"
        "| RISK-002 | Unknown requirement | `Arm.Starts` | REQ-XYZ-123 |\n"
        "| RISK-003 | No requirement | `Arm.Starts` | |\n"
        "| RISK-003 | Twice | `Arm.Starts` | REQ-SAF-001 |\n";
    const auto problems = msc::trace::check(msc::trace::parseRequirements(kRequirements),
                                            msc::trace::parseRisks(risks), cleanTests());
    EXPECT_TRUE(mentions(problems, "RISK-001 (Unverified) is not verified"));
    EXPECT_TRUE(mentions(problems, "RISK-002 names REQ-XYZ-123, which is not in requirements.md"));
    EXPECT_TRUE(mentions(problems, "RISK-003 (No requirement) names no requirement"));
    EXPECT_TRUE(mentions(problems, "RISK-003 is listed twice"));
}

TEST(TraceCheck, DuplicatesAreReported) {
    auto tests = cleanTests();
    tests.push_back({"Arm.Starts", "c.cpp", 7});
    auto requirements = msc::trace::parseRequirements(kRequirements);
    requirements.push_back(requirements.front());
    requirements.push_back({"REQ-1", {}, 12});
    const auto problems = msc::trace::check(requirements, msc::trace::parseRisks(kRisks), tests);
    EXPECT_TRUE(mentions(problems, "test Arm.Starts is declared more than once: b.cpp:9, c.cpp:7"));
    EXPECT_TRUE(mentions(problems, "REQ-COM-001 is listed twice"));
    EXPECT_TRUE(mentions(problems, "\"REQ-1\" is not a requirement ID"));
}

TEST(TraceReport, ListsTestsPerRequirementAndDoesNotDependOnOrder) {
    const auto requirements = msc::trace::parseRequirements(kRequirements);
    const auto risks = msc::trace::parseRisks(kRisks);
    auto tests = cleanTests();
    const auto first = msc::trace::report(requirements, risks, tests);
    std::ranges::reverse(tests);
    EXPECT_EQ(msc::trace::report(requirements, risks, tests), first);
    EXPECT_NE(first.find("| [REQ-COM-001](#req-com-001) | 2 | - |"), std::string::npos) << first;
    EXPECT_NE(first.find("| [REQ-SAF-001](#req-saf-001) | 1 | RISK-001 |"), std::string::npos) << first;
    EXPECT_NE(first.find("- `Crc32.REQ_COM_001_Check` (a.cpp)"), std::string::npos) << first;
    EXPECT_NE(first.find("- Tests: 4, of which 3 carry a requirement ID"), std::string::npos) << first;
}
