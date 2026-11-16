#include "constant_replacer.h"

#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

namespace {

const std::vector<PackageSource>& externalPackages() {
    static const std::vector<PackageSource> packages{
        {"", "PACKAGE_FIRST", R"SQL(create or replace package package_first as
  c_status constant varchar2(10) := 'ACTIVE';
  c_limit constant number := 100;
end;
)SQL", {}, ""},
        {"", "PACKAGE_SECOND", R"SQL(create or replace package package_second as
  c_status constant varchar2(10) := package_first.c_status;
  c_another_limit constant number := 20;
end;
)SQL", {}, ""}
    };
    return packages;
}

bool expectReplacement(const char* testName, const std::string& source,
                       const std::string& selection,
                       const std::vector<PackageSource>& packages,
                       const std::string& expected, std::size_t expectedReplacements,
                       std::size_t expectedConstants) {
    const ReplacementResult result = replaceConstants(source, selection, packages);
    if (result.text == expected && result.replacements == expectedReplacements &&
        result.constantsFound == expectedConstants && result.ambiguousReferences == 0) {
        return true;
    }

    std::cerr << testName << " FAILED\nReplacements: " << result.replacements
              << "\nConstants: " << result.constantsFound
              << "\nAmbiguous: " << result.ambiguousReferences
              << "\nOutput:\n" << result.text;
    return false;
}

bool testQualifiedPlSql() {
    const std::string input = R"SQL(begin
  dbms_output.put_line(package_first.c_status); -- 'ACTIVE'
  dbms_output.put_line(package_second.c_status); -- 'ACTIVE'
  dbms_output.put_line(package_first.c_limit); -- 100
  dbms_output.put_line(package_second.c_another_limit); -- 20
end;
)SQL";
    const std::string expected = R"SQL(begin
  dbms_output.put_line('ACTIVE'); -- 'ACTIVE'
  dbms_output.put_line('ACTIVE'); -- 'ACTIVE'
  dbms_output.put_line(100); -- 100
  dbms_output.put_line(20); -- 20
end;
)SQL";

    const std::vector<PackageReference> dependencies = referencedPackages(externalPackages()[1].text);
    const std::vector<PackageReference> expectedDependencies{{"", "PACKAGE_FIRST"}};
    if (dependencies != expectedDependencies) {
        std::cerr << "qualified_plsql FAILED\nPACKAGE_FIRST dependency was not found in PACKAGE_SECOND";
        return false;
    }

    return expectReplacement("qualified_plsql", input, input, externalPackages(), expected, 4, 4);
}

bool testQualifiedSql() {
    const std::string input = R"SQL(select package_first.c_status from dual; -- 'ACTIVE'
select package_second.c_status from dual; -- 'ACTIVE'
)SQL";
    const std::string expected = R"SQL(select 'ACTIVE' from dual; -- 'ACTIVE'
select 'ACTIVE' from dual; -- 'ACTIVE'
)SQL";
    return expectReplacement("qualified_sql", input, input, externalPackages(), expected, 2, 4);
}

const std::string& sameWindowPackage() {
    static const std::string source = R"SQL(create or replace package package_first is
  c_status constant varchar2(10) := 'ACTIVE';
  c_limit constant number := 100;
  c_day constant date := date '2026-01-01';
end;
/
)SQL";
    return source;
}

bool testUnqualifiedPlSqlSameWindow() {
    const std::string input = R"SQL(begin
  dbms_output.put_line(c_status); -- 'ACTIVE'
  dbms_output.put_line(c_limit); -- 100
  dbms_output.put_line(c_day); -- date '2026-01-01'
end;
)SQL";
    const std::string expected = R"SQL(begin
  dbms_output.put_line('ACTIVE'); -- 'ACTIVE'
  dbms_output.put_line(100); -- 100
  dbms_output.put_line(date '2026-01-01'); -- date '2026-01-01'
end;
)SQL";
    return expectReplacement("unqualified_plsql_same_window", sameWindowPackage() + input,
                             input, {}, expected, 3, 3);
}

bool testUnqualifiedSqlSameWindow() {
    const std::string input = R"SQL(select c_status from dual; -- 'ACTIVE'
select c_limit from dual; -- 100
select c_day from dual; -- date '2026-01-01'
)SQL";
    const std::string expected = R"SQL(select 'ACTIVE' from dual; -- 'ACTIVE'
select 100 from dual; -- 100
select date '2026-01-01' from dual; -- date '2026-01-01'
)SQL";
    return expectReplacement("unqualified_sql_same_window", sameWindowPackage() + input,
                             input, {}, expected, 3, 3);
}

bool testWholeWindowDeclarations() {
    const std::string source = sameWindowPackage() +
                               "select c_status, c_limit, c_status from dual;\n";
    const std::string expected = sameWindowPackage() +
                                 "select 'ACTIVE', 100, 'ACTIVE' from dual;\n";
    return expectReplacement("whole_window_declarations", source, source, {}, expected, 3, 3);
}

bool testDependencyDiscovery() {
    const std::vector<PackageReference> dependencies = referencedPackages(R"SQL(begin
  dbms_output.put_line(package_first.c_status);
end;
select first.second.third.fourth from dual;
select package_second.c_status from dual;
select package_first.c_limit from dual;
)SQL");
    const std::vector<PackageReference> expected{{"", "PACKAGE_FIRST"}, {"", "PACKAGE_SECOND"}};
    if (dependencies == expected) {
        return true;
    }
    std::cerr << "dependency_discovery FAILED\nUnexpected package dependency list";
    return false;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Expected exactly one test name";
        return 2;
    }

    const std::string testName = argv[1];
    if (testName == "qualified_plsql") {
        return testQualifiedPlSql() ? 0 : 1;
    }
    if (testName == "qualified_sql") {
        return testQualifiedSql() ? 0 : 1;
    }
    if (testName == "unqualified_plsql_same_window") {
        return testUnqualifiedPlSqlSameWindow() ? 0 : 1;
    }
    if (testName == "unqualified_sql_same_window") {
        return testUnqualifiedSqlSameWindow() ? 0 : 1;
    }
    if (testName == "whole_window_declarations") {
        return testWholeWindowDeclarations() ? 0 : 1;
    }
    if (testName == "dependency_discovery") {
        return testDependencyDiscovery() ? 0 : 1;
    }

    std::cerr << "Unknown test name: " << testName;
    return 2;
}
