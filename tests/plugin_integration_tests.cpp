#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

extern "C" {
void __cdecl RegisterCallback(int index, void* function);
void __cdecl OnMenuClick(int index);
}

namespace {

constexpr const char* packageFirst = R"SQL(create or replace package package_first is
  c_status constant varchar2(10) := 'ACTIVE';
  c_limit  constant number := 100;
  c_day    constant date := date '2026-01-01';
end;
/
)SQL";

constexpr const char* packageSecond = R"SQL(create or replace package package_second is
  c_status constant varchar2(10) := package_first.c_status;
  c_another_limit constant number := 20;
end;
/
)SQL";

using SqlRow = std::array<const char*, 5>;

HWND editor = nullptr;
std::vector<SqlRow> sqlRows;
std::size_t sqlRow = 0;
int packageFirstLoads = 0;
int packageSecondLoads = 0;
int packageSourceQueries = 0;
bool synonymQuery = false;
bool useSynonym = false;

BOOL __cdecl connected() {
    return TRUE;
}

std::string windowText() {
    const int length = GetWindowTextLengthA(editor);
    std::string text(static_cast<std::size_t>(length) + 1, '\0');
    GetWindowTextA(editor, text.data(), length + 1);
    text.resize(static_cast<std::size_t>(length));
    return text;
}

const char* __cdecl getText() {
    static std::string text;
    text = windowText();
    return text.c_str();
}

const char* __cdecl getSelectedText() {
    static std::string selection;
    DWORD begin = 0;
    DWORD end = 0;
    SendMessageA(editor, EM_GETSEL, reinterpret_cast<WPARAM>(&begin),
                 reinterpret_cast<LPARAM>(&end));
    const std::string text = windowText();
    selection = text.substr(begin, end - begin);
    return selection.c_str();
}

HWND __cdecl getEditorHandle() {
    return editor;
}

int __cdecl sqlExecute(const char* queryValue) {
    const std::string query = queryValue;
    sqlRows.clear();
    sqlRow = 0;
    synonymQuery = query.find("from all_synonyms") != std::string::npos;

    if (query.find("from all_source") != std::string::npos) {
        ++packageSourceQueries;
        if (query.find("'PACKAGE_FIRST'") != std::string::npos) {
            ++packageFirstLoads;
            sqlRows.push_back({"TEST_SCHEMA", "PACKAGE_FIRST", "PACKAGE", packageFirst, nullptr});
        }
        if (query.find("'PACKAGE_SECOND'") != std::string::npos) {
            ++packageSecondLoads;
            sqlRows.push_back({"TEST_SCHEMA", "PACKAGE_SECOND", "PACKAGE", packageSecond, nullptr});
        }
    } else if (synonymQuery && useSynonym &&
               query.find("'PACKAGE_ALIAS'") != std::string::npos) {
        sqlRows.push_back({"TEST_SCHEMA", "PACKAGE_FIRST", nullptr,
                           "TEST_SCHEMA", "PACKAGE_ALIAS"});
    }
    return 0;
}

BOOL __cdecl sqlEof() {
    return sqlRow >= sqlRows.size() ? TRUE : FALSE;
}

int __cdecl sqlNext() {
    if (sqlRow < sqlRows.size()) {
        ++sqlRow;
    }
    return 0;
}

const char* __cdecl sqlField(int index) {
    static std::string value;
    if (sqlRow >= sqlRows.size() || index < 0 || index >= 5) {
        return nullptr;
    }
    const char* field = sqlRows[sqlRow][static_cast<std::size_t>(index)];
    if (!field) {
        return nullptr;
    }
    value = field;
    return value.c_str();
}

int __cdecl sqlFieldIndex(const char* name) {
    if (synonymQuery) {
        if (std::strcmp(name, "TABLE_OWNER") == 0) {
            return 0;
        }
        if (std::strcmp(name, "TABLE_NAME") == 0) {
            return 1;
        }
        if (std::strcmp(name, "DB_LINK") == 0) {
            return 2;
        }
        if (std::strcmp(name, "OWNER") == 0) {
            return 3;
        }
        if (std::strcmp(name, "SYNONYM_NAME") == 0) {
            return 4;
        }
        return -1;
    }
    if (std::strcmp(name, "OWNER") == 0) {
        return 0;
    }
    if (std::strcmp(name, "NAME") == 0) {
        return 1;
    }
    if (std::strcmp(name, "TYPE") == 0) {
        return 2;
    }
    if (std::strcmp(name, "TEXT") == 0) {
        return 3;
    }
    return -1;
}

void registerCallbacks() {
    RegisterCallback(11, reinterpret_cast<void*>(&connected));
    RegisterCallback(30, reinterpret_cast<void*>(&getText));
    RegisterCallback(31, reinterpret_cast<void*>(&getSelectedText));
    RegisterCallback(33, reinterpret_cast<void*>(&getEditorHandle));
    RegisterCallback(40, reinterpret_cast<void*>(&sqlExecute));
    RegisterCallback(42, reinterpret_cast<void*>(&sqlEof));
    RegisterCallback(43, reinterpret_cast<void*>(&sqlNext));
    RegisterCallback(44, reinterpret_cast<void*>(&sqlField));
    RegisterCallback(46, reinterpret_cast<void*>(&sqlFieldIndex));
}

bool expectReplacement(const char* testName, const std::string& input, const std::string& expected,
                       int expectedFirstLoads, int expectedSecondLoads, int expectedSourceQueries,
                       bool withSynonym = false) {
    const HMODULE instance = GetModuleHandleA(nullptr);
    HWND parent = CreateWindowExA(0, "STATIC", "", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                  CW_USEDEFAULT, 800, 600, nullptr, nullptr, instance, nullptr);
    editor = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", input.c_str(),
                             WS_CHILD | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                             0, 0, 780, 580, parent, nullptr, instance, nullptr);
    if (!parent || !editor) {
        std::cerr << testName << " FAILED\nCannot create the test editor";
        if (parent) {
            DestroyWindow(parent);
        }
        editor = nullptr;
        return false;
    }

    packageFirstLoads = 0;
    packageSecondLoads = 0;
    packageSourceQueries = 0;
    useSynonym = withSynonym;
    SendMessageA(editor, EM_SETSEL, 0, -1);
    OnMenuClick(1);
    const std::string actual = windowText();
    DestroyWindow(parent);
    editor = nullptr;

    if (actual == expected && packageFirstLoads == expectedFirstLoads &&
        packageSecondLoads == expectedSecondLoads && packageSourceQueries == expectedSourceQueries) {
        return true;
    }

    std::cerr << testName << " FAILED\nPACKAGE_FIRST loads: " << packageFirstLoads
              << "\nPACKAGE_SECOND loads: " << packageSecondLoads
              << "\nALL_SOURCE queries: " << packageSourceQueries
              << "\nOutput:\n" << actual;
    return false;
}

bool testQualifiedSqlOtherWindow() {
    const std::string input = R"SQL(select package_first.c_status from dual; -- 'ACTIVE'
select package_second.c_status from dual; -- 'ACTIVE'
)SQL";
    const std::string expected = R"SQL(select 'ACTIVE' from dual; -- 'ACTIVE'
select 'ACTIVE' from dual; -- 'ACTIVE'
)SQL";
    return expectReplacement("qualified_sql_other_window", input, expected, 1, 1, 1);
}

bool testQualifiedPlSqlOtherWindow() {
    const std::string input = R"SQL(begin
  dbms_output.put_line(package_first.c_status);
  dbms_output.put_line(package_second.c_status);
  dbms_output.put_line(package_first.c_limit);
  dbms_output.put_line(package_second.c_another_limit);
end;
)SQL";
    const std::string expected = R"SQL(begin
  dbms_output.put_line('ACTIVE');
  dbms_output.put_line('ACTIVE');
  dbms_output.put_line(100);
  dbms_output.put_line(20);
end;
)SQL";
    return expectReplacement("qualified_plsql_other_window", input, expected, 1, 1, 1);
}

bool testPackageFirstOnlyOtherWindow() {
    const std::string input = "select package_first.c_status from dual; -- 'ACTIVE'\n";
    const std::string expected = "select 'ACTIVE' from dual; -- 'ACTIVE'\n";
    return expectReplacement("package_first_only_other_window", input, expected, 1, 0, 1);
}

bool testPackageDependencyOtherWindow() {
    const std::string input = "select package_second.c_status from dual; -- 'ACTIVE'\n";
    const std::string expected = "select 'ACTIVE' from dual; -- 'ACTIVE'\n";
    return expectReplacement("package_dependency_other_window", input, expected, 1, 1, 2);
}

bool testSynonymOtherWindow() {
    const std::string input = "select package_alias.c_status from dual; -- 'ACTIVE'\n";
    const std::string expected = "select 'ACTIVE' from dual; -- 'ACTIVE'\n";
    return expectReplacement("synonym_other_window", input, expected, 1, 0, 2, true);
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Expected exactly one test name";
        return 2;
    }

    registerCallbacks();
    const std::string testName = argv[1];
    if (testName == "qualified_sql_other_window") {
        return testQualifiedSqlOtherWindow() ? 0 : 1;
    }
    if (testName == "qualified_plsql_other_window") {
        return testQualifiedPlSqlOtherWindow() ? 0 : 1;
    }
    if (testName == "package_first_only_other_window") {
        return testPackageFirstOnlyOtherWindow() ? 0 : 1;
    }
    if (testName == "package_dependency_other_window") {
        return testPackageDependencyOtherWindow() ? 0 : 1;
    }
    if (testName == "synonym_other_window") {
        return testSynonymOtherWindow() ? 0 : 1;
    }

    std::cerr << "Unknown test name: " << testName;
    return 2;
}
