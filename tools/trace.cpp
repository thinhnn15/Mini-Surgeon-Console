#include "trace.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <format>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace msc::trace {

namespace {

bool isIdentChar(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && isSpace(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && isSpace(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

int lineAt(std::string_view text, std::size_t pos) {
    return 1 + static_cast<int>(std::ranges::count(text.substr(0, pos), '\n'));
}

// True if only spaces stand between the start of the line and `pos`.
bool startsLine(std::string_view text, std::size_t pos) {
    while (pos > 0) {
        const char c = text[pos - 1];
        if (c == '\n') {
            return true;
        }
        if (c != ' ' && c != '\t') {
            return false;
        }
        --pos;
    }
    return true;
}

std::size_t skipSpaces(std::string_view text, std::size_t pos) {
    while (pos < text.size() && isSpace(text[pos])) {
        ++pos;
    }
    return pos;
}

std::string_view identifierAt(std::string_view text, std::size_t pos) {
    std::size_t end = pos;
    while (end < text.size() && isIdentChar(text[end])) {
        ++end;
    }
    return text.substr(pos, end - pos);
}

// "REQ-" + capitals + "-" + three digits.
bool isRequirementId(std::string_view id) {
    if (!id.starts_with("REQ-")) {
        return false;
    }
    id.remove_prefix(4);
    const auto dash = id.find('-');
    if (dash == 0 || dash == std::string_view::npos) {
        return false;
    }
    const auto area = id.substr(0, dash);
    const auto number = id.substr(dash + 1);
    return std::ranges::all_of(area, [](char c) { return c >= 'A' && c <= 'Z'; }) && number.size() == 3 &&
           std::ranges::all_of(number, [](char c) { return c >= '0' && c <= '9'; });
}

std::vector<std::string> requirementIdsInText(std::string_view text) {
    std::vector<std::string> ids;
    for (std::size_t pos = text.find("REQ-"); pos != std::string_view::npos;
         pos = text.find("REQ-", pos + 1)) {
        std::size_t end = pos + 4;
        while (end < text.size() && (isIdentChar(text[end]) || text[end] == '-')) {
            ++end;
        }
        const std::string id(text.substr(pos, end - pos));
        if (isRequirementId(id) && std::ranges::find(ids, id) == ids.end()) {
            ids.push_back(id);
        }
    }
    return ids;
}

// Splits "| a | `b|c` | d |" into {"a", "`b|c`", "d"}: a '|' inside back quotes is text.
std::vector<std::string_view> cellsOf(std::string_view row) {
    row = trim(row);
    if (row.starts_with('|')) {
        row.remove_prefix(1);
    }
    if (row.ends_with('|')) {
        row.remove_suffix(1);
    }
    std::vector<std::string_view> cells;
    bool quoted = false;
    std::size_t start = 0;
    for (std::size_t i = 0; i < row.size(); ++i) {
        if (row[i] == '`') {
            quoted = !quoted;
        } else if (row[i] == '|' && !quoted) {
            cells.push_back(trim(row.substr(start, i - start)));
            start = i + 1;
        }
    }
    cells.push_back(trim(row.substr(start)));
    return cells;
}

bool isSeparatorRow(const std::vector<std::string_view>& cells) {
    return std::ranges::all_of(cells, [](std::string_view c) {
        return !c.empty() && std::ranges::all_of(c, [](char ch) { return ch == '-' || ch == ':'; });
    });
}

struct Table {
    std::vector<std::string> header;
    struct Row {
        std::vector<std::string_view> cells;
        int line = 0;
    };
    std::vector<Row> rows;

    [[nodiscard]] std::ptrdiff_t column(std::string_view name) const {
        const auto it = std::ranges::find(header, name);
        return it == header.end() ? -1 : it - header.begin();
    }
};

std::vector<Table> tablesIn(std::string_view markdown) {
    std::vector<Table> tables;
    bool inTable = false;
    int lineNumber = 0;
    std::size_t pos = 0;
    while (pos <= markdown.size()) {
        auto end = markdown.find('\n', pos);
        if (end == std::string_view::npos) {
            end = markdown.size();
        }
        const auto line = trim(markdown.substr(pos, end - pos));
        ++lineNumber;
        if (line.starts_with('|')) {
            auto cells = cellsOf(line);
            if (!inTable) {
                tables.emplace_back();
                for (const auto cell : cells) {
                    tables.back().header.emplace_back(cell);
                }
                inTable = true;
            } else if (!isSeparatorRow(cells)) {
                tables.back().rows.push_back({std::move(cells), lineNumber});
            }
        } else {
            inTable = false;
        }
        pos = end + 1;
    }
    return tables;
}

const Table& findTable(const std::vector<Table>& tables, std::initializer_list<std::string_view> columns,
                       std::string_view what) {
    for (const auto& table : tables) {
        if (std::ranges::all_of(columns, [&table](std::string_view c) { return table.column(c) >= 0; })) {
            return table;
        }
    }
    std::string names;
    for (const auto c : columns) {
        names += std::format("{}\"{}\"", names.empty() ? "" : ", ", c);
    }
    throw std::runtime_error(std::format("no {} table: no Markdown table has the columns {}", what, names));
}

std::string_view cell(const Table::Row& row, std::ptrdiff_t column) {
    const auto index = static_cast<std::size_t>(column);
    return index < row.cells.size() ? row.cells[index] : std::string_view{};
}

bool isFileName(std::string_view token) {
    static constexpr std::array kExtensions{".cpp", ".hpp", ".h", ".md", ".txt", ".qml", ".cmake", ".json"};
    return std::ranges::any_of(kExtensions, [token](std::string_view ext) { return token.ends_with(ext); });
}

bool looksLikeTestName(std::string_view token) {
    if (token.empty() ||
        (std::isalpha(static_cast<unsigned char>(token.front())) == 0 && token.front() != '_')) {
        return false;
    }
    if (token.find('.') == std::string_view::npos || token.ends_with('.') ||
        token.find("..") != std::string_view::npos) {
        return false;
    }
    return std::ranges::all_of(token, [](char c) { return isIdentChar(c) || c == '.' || c == '*'; }) &&
           !isFileName(token);
}

std::string where(const DeclaredTest& test) {
    return std::format("{}:{}", test.file, test.line);
}

std::vector<const DeclaredTest*> matching(const std::vector<DeclaredTest>& tests, std::string_view pattern) {
    std::vector<const DeclaredTest*> found;
    for (const auto& test : tests) {
        if (matches(pattern, test.name)) {
            found.push_back(&test);
        }
    }
    return found;
}

bool carries(const DeclaredTest& test, std::string_view id) {
    const auto ids = requirementIdsIn(test.name);
    return std::ranges::find(ids, id) != ids.end();
}

std::string lowerCase(std::string_view s) {
    std::string out(s);
    std::ranges::transform(out, out.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return out;
}

std::string underscored(std::string_view id) {
    std::string s(id);
    std::ranges::replace(s, '-', '_');
    return s;
}

}  // namespace

std::vector<DeclaredTest> findTestsInCpp(std::string_view text, std::string_view file) {
    std::vector<DeclaredTest> tests;
    for (std::size_t pos = text.find("TEST"); pos != std::string_view::npos;
         pos = text.find("TEST", pos + 1)) {
        if (!startsLine(text, pos)) {
            continue;
        }
        std::size_t p = pos + 4;
        if (text.substr(p, 2) == "_F") {
            p += 2;
        }
        p = skipSpaces(text, p);
        if (p >= text.size() || text[p] != '(') {
            continue;  // TEST_P, TESTING, a word that starts with TEST
        }
        p = skipSpaces(text, p + 1);
        const auto suite = identifierAt(text, p);
        p = skipSpaces(text, p + suite.size());
        if (suite.empty() || p >= text.size() || text[p] != ',') {
            continue;
        }
        p = skipSpaces(text, p + 1);
        const auto name = identifierAt(text, p);
        p = skipSpaces(text, p + name.size());
        if (name.empty() || p >= text.size() || text[p] != ')') {
            continue;
        }
        tests.push_back({std::format("{}.{}", suite, name), std::string(file), lineAt(text, pos)});
    }
    return tests;
}

std::vector<DeclaredTest> findTestsInCMake(std::string_view text, std::string_view file) {
    // Blank out comments, keeping every character's position so line numbers stay right.
    std::string code(text);
    bool comment = false;
    for (auto& c : code) {
        if (c == '\n') {
            comment = false;
        } else if (c == '#') {
            comment = true;
        }
        if (comment) {
            c = ' ';
        }
    }
    const std::string_view view(code);
    std::vector<DeclaredTest> tests;
    for (std::size_t pos = view.find("add_test"); pos != std::string_view::npos;
         pos = view.find("add_test", pos + 1)) {
        if ((pos > 0 && isIdentChar(view[pos - 1])) ||
            (pos + 8 < view.size() && isIdentChar(view[pos + 8]))) {
            continue;
        }
        std::size_t p = skipSpaces(view, pos + 8);
        if (p >= view.size() || view[p] != '(') {
            continue;
        }
        p = skipSpaces(view, p + 1);
        if (view.substr(p, 4) != "NAME" || p + 4 >= view.size() || !isSpace(view[p + 4])) {
            continue;  // the old add_test(name command) signature: not used here
        }
        p = skipSpaces(view, p + 4);
        std::size_t end = p;
        while (end < view.size() && !isSpace(view[end]) && view[end] != ')') {
            ++end;
        }
        if (end > p) {
            tests.push_back({std::string(view.substr(p, end - p)), std::string(file), lineAt(view, pos)});
        }
    }
    return tests;
}

std::vector<Requirement> parseRequirements(std::string_view markdown) {
    const auto tables = tablesIn(markdown);
    const auto& table = findTable(tables, {"ID", "Verified by"}, "requirements");
    const auto id = table.column("ID");
    const auto verified = table.column("Verified by");
    std::vector<Requirement> requirements;
    requirements.reserve(table.rows.size());
    for (const auto& row : table.rows) {
        requirements.push_back({std::string(cell(row, id)), testReferencesIn(cell(row, verified)), row.line});
    }
    return requirements;
}

std::vector<Risk> parseRisks(std::string_view markdown) {
    const auto tables = tablesIn(markdown);
    const auto& table = findTable(tables, {"ID", "Hazard", "Verified by", "Requirement"}, "risk");
    const auto id = table.column("ID");
    const auto hazard = table.column("Hazard");
    const auto verified = table.column("Verified by");
    const auto requirement = table.column("Requirement");
    std::vector<Risk> risks;
    risks.reserve(table.rows.size());
    for (const auto& row : table.rows) {
        risks.push_back({std::string(cell(row, id)), std::string(cell(row, hazard)),
                         testReferencesIn(cell(row, verified)), requirementIdsInText(cell(row, requirement)),
                         row.line});
    }
    return risks;
}

std::vector<std::string> testReferencesIn(std::string_view cellText) {
    std::vector<std::string> references;
    std::size_t pos = 0;
    while (true) {
        const auto open = cellText.find('`', pos);
        if (open == std::string_view::npos) {
            break;
        }
        const auto close = cellText.find('`', open + 1);
        if (close == std::string_view::npos) {
            break;
        }
        const auto token = trim(cellText.substr(open + 1, close - open - 1));
        if (looksLikeTestName(token)) {
            references.emplace_back(token);
        }
        pos = close + 1;
    }
    return references;
}

bool matches(std::string_view pattern, std::string_view name) {
    // Greedy '*' with one point of backtracking: linear for patterns like "Suite.REQ_X_*".
    std::size_t p = 0;
    std::size_t n = 0;
    std::size_t star = std::string_view::npos;
    std::size_t resume = 0;
    while (n < name.size()) {
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            resume = n;
        } else if (p < pattern.size() && pattern[p] == name[n]) {
            ++p;
            ++n;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            n = ++resume;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

std::vector<std::string> requirementIdsIn(std::string_view testName) {
    std::vector<std::string> ids;
    for (std::size_t pos = testName.find("REQ_"); pos != std::string_view::npos;
         pos = testName.find("REQ_", pos + 1)) {
        std::size_t p = pos + 4;
        const std::size_t areaStart = p;
        while (p < testName.size() && testName[p] >= 'A' && testName[p] <= 'Z') {
            ++p;
        }
        if (p == areaStart || p + 4 > testName.size() || testName[p] != '_') {
            continue;
        }
        const auto number = testName.substr(p + 1, 3);
        const bool digits = std::ranges::all_of(number, [](char c) { return c >= '0' && c <= '9'; });
        const bool ends = p + 4 == testName.size() || testName[p + 4] < '0' || testName[p + 4] > '9';
        if (!digits || !ends) {
            continue;
        }
        auto id = std::format("REQ-{}-{}", testName.substr(areaStart, p - areaStart), number);
        if (std::ranges::find(ids, id) == ids.end()) {
            ids.push_back(std::move(id));
        }
    }
    return ids;
}

namespace {

using Problems = std::vector<std::string>;

template <typename Range>
std::string joined(const Range& items) {
    std::string out;
    for (const auto& item : items) {
        out += std::format("{}{}", out.empty() ? "" : ", ", item);
    }
    return out;
}

bool isTested(const std::vector<DeclaredTest>& tests, std::string_view id) {
    return std::ranges::any_of(tests, [id](const DeclaredTest& t) { return carries(t, id); });
}

// Each test name once: CTest would run only one of two tests with the same name.
void checkTestNames(const std::vector<DeclaredTest>& tests, Problems& problems) {
    std::map<std::string, std::vector<std::string>> byName;
    for (const auto& test : tests) {
        byName[test.name].push_back(where(test));
    }
    for (const auto& [name, places] : byName) {
        if (places.size() > 1) {
            problems.push_back(std::format("test {} is declared more than once: {}", name, joined(places)));
        }
    }
}

// Well-formed and unique; returns the IDs that are.
std::set<std::string> checkRequirementIds(const std::vector<Requirement>& requirements, Problems& problems) {
    std::set<std::string> known;
    for (const auto& r : requirements) {
        if (!isRequirementId(r.id)) {
            problems.push_back(std::format(
                "requirements.md:{}: \"{}\" is not a requirement ID (REQ-AREA-123)", r.line, r.id));
        } else if (!known.insert(r.id).second) {
            problems.push_back(std::format("requirements.md:{}: {} is listed twice", r.line, r.id));
        }
    }
    return known;
}

// Carried by at least one test, and every "Verified by" entry names tests that carry the ID.
void checkRequirement(const Requirement& r, const std::vector<DeclaredTest>& tests, Problems& problems) {
    if (!isTested(tests, r.id)) {
        problems.push_back(std::format("{} is not tested: no test name carries {}", r.id, underscored(r.id)));
    }
    for (const auto& pattern : r.verifiedBy) {
        const auto found = matching(tests, pattern);
        if (found.empty()) {
            problems.push_back(std::format("{}: `{}` in \"Verified by\" matches no test", r.id, pattern));
        }
        for (const auto* t : found) {
            if (!carries(*t, r.id)) {
                problems.push_back(
                    std::format("{}: `{}` in \"Verified by\" matches {}, which does not carry {}", r.id,
                                pattern, t->name, underscored(r.id)));
            }
        }
    }
}

// A test that carries an ID: the ID exists, and its row lists the test.
void checkCarriedIds(const DeclaredTest& test, const std::vector<Requirement>& requirements,
                     Problems& problems) {
    for (const auto& id : requirementIdsIn(test.name)) {
        const auto row = std::ranges::find(requirements, id, &Requirement::id);
        if (row == requirements.end()) {
            problems.push_back(std::format("test {} ({}) carries {}, which is not in requirements.md",
                                           test.name, where(test), id));
        } else if (std::ranges::none_of(row->verifiedBy,
                                        [&test](const std::string& p) { return matches(p, test.name); })) {
            problems.push_back(std::format("{}: test {} ({}) carries the ID but is not in \"Verified by\"",
                                           id, test.name, where(test)));
        }
    }
}

// Verified by tests that exist, tied to requirements that exist.
void checkRisk(const Risk& risk, const std::set<std::string>& known, const std::vector<DeclaredTest>& tests,
               Problems& problems) {
    if (risk.verifiedBy.empty()) {
        problems.push_back(
            std::format("{} ({}) is not verified: no test in \"Verified by\"", risk.id, risk.hazard));
    }
    for (const auto& pattern : risk.verifiedBy) {
        if (matching(tests, pattern).empty()) {
            problems.push_back(std::format("{}: `{}` in \"Verified by\" matches no test", risk.id, pattern));
        }
    }
    if (risk.requirements.empty()) {
        problems.push_back(std::format("{} ({}) names no requirement", risk.id, risk.hazard));
    }
    for (const auto& id : risk.requirements) {
        if (!known.contains(id)) {
            problems.push_back(std::format("{} names {}, which is not in requirements.md", risk.id, id));
        }
    }
}

bool isVerified(const Risk& risk, const std::vector<DeclaredTest>& tests) {
    return !risk.verifiedBy.empty() && std::ranges::all_of(risk.verifiedBy, [&tests](const std::string& p) {
        return !matching(tests, p).empty();
    });
}

std::string summarySection(const std::vector<Requirement>& requirements, const std::vector<Risk>& risks,
                           const std::vector<DeclaredTest>& tests) {
    const auto carrying =
        std::ranges::count_if(tests, [](const DeclaredTest& t) { return !requirementIdsIn(t.name).empty(); });
    const auto tested =
        std::ranges::count_if(requirements, [&tests](const Requirement& r) { return isTested(tests, r.id); });
    const auto verified =
        std::ranges::count_if(risks, [&tests](const Risk& risk) { return isVerified(risk, tests); });
    std::string out;
    out += "# Traceability\n\n";
    out += "Generated by `trace_check --write` from the test sources, `requirements.md` and `risks.md`.\n";
    out += "Do not edit by hand: the `trace_check` test fails when this file is out of date.\n\n";
    out += std::format("- Requirements: {}, tested: {}\n", requirements.size(), tested);
    out += std::format("- Tests: {}, of which {} carry a requirement ID\n", tests.size(), carrying);
    out += std::format("- Risks: {}, verified by tests that exist: {}\n", risks.size(), verified);
    return out;
}

std::string requirementsSection(const std::vector<Requirement>& requirements, const std::vector<Risk>& risks,
                                const std::vector<DeclaredTest>& tests) {
    std::string out = "\n## Requirements → tests\n\n| Requirement | Tests | Risks |\n| --- | --- | --- |\n";
    for (const auto& r : requirements) {
        const auto count =
            std::ranges::count_if(tests, [&r](const DeclaredTest& t) { return carries(t, r.id); });
        std::vector<std::string> riskIds;
        for (const auto& risk : risks) {
            if (std::ranges::find(risk.requirements, r.id) != risk.requirements.end()) {
                riskIds.push_back(risk.id);
            }
        }
        // GitHub makes a heading's anchor lower case: "### REQ-VID-001" -> #req-vid-001.
        out += std::format("| [{}](#{}) | {} | {} |\n", r.id, lowerCase(r.id), count,
                           riskIds.empty() ? std::string("-") : joined(riskIds));
    }
    return out;
}

std::string risksSection(const std::vector<Risk>& risks, const std::vector<DeclaredTest>& tests) {
    std::string out =
        "\n## Risks → requirements → tests\n\n"
        "| Risk | Hazard | Requirements | Tests named in the risk table |\n| --- | --- | --- | --- |\n";
    for (const auto& risk : risks) {
        std::set<std::string> names;
        for (const auto& p : risk.verifiedBy) {
            for (const auto* t : matching(tests, p)) {
                names.insert(t->name);
            }
        }
        out += std::format("| {} | {} | {} | {} |\n", risk.id, risk.hazard,
                           risk.requirements.empty() ? std::string("-") : joined(risk.requirements),
                           names.empty() ? std::string("**none**") : std::to_string(names.size()));
    }
    return out;
}

std::string testsSection(const std::vector<Requirement>& requirements,
                         const std::vector<DeclaredTest>& tests) {
    std::string out = "\n## Tests by requirement\n";
    for (const auto& r : requirements) {
        out += std::format("\n### {}\n\n", r.id);
        const auto before = out.size();
        for (const auto& t : tests) {
            if (carries(t, r.id)) {
                out += std::format("- `{}` ({})\n", t.name, t.file);
            }
        }
        if (out.size() == before) {
            out += "- **no test**\n";
        }
    }
    return out;
}

}  // namespace

std::vector<std::string> check(const std::vector<Requirement>& requirements, const std::vector<Risk>& risks,
                               const std::vector<DeclaredTest>& tests) {
    Problems problems;
    checkTestNames(tests, problems);
    const auto known = checkRequirementIds(requirements, problems);
    for (const auto& r : requirements) {
        if (isRequirementId(r.id)) {
            checkRequirement(r, tests, problems);
        }
    }
    for (const auto& test : tests) {
        checkCarriedIds(test, requirements, problems);
    }
    std::set<std::string> riskIds;
    for (const auto& risk : risks) {
        if (!riskIds.insert(risk.id).second) {
            problems.push_back(std::format("risks.md:{}: {} is listed twice", risk.line, risk.id));
        }
        checkRisk(risk, known, tests, problems);
    }
    return problems;
}

std::string report(const std::vector<Requirement>& requirements, const std::vector<Risk>& risks,
                   const std::vector<DeclaredTest>& tests) {
    auto sorted = tests;
    std::ranges::sort(sorted, {}, &DeclaredTest::name);
    return summarySection(requirements, risks, sorted) + requirementsSection(requirements, risks, sorted) +
           risksSection(risks, sorted) + testsSection(requirements, sorted);
}

}  // namespace msc::trace
