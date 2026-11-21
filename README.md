# Replace Constants for PL/SQL Developer

**English** | [Русский](README.ru.md)

The plugin replaces references to PL/SQL constants with their values in the selected code fragment. If nothing is selected, it processes the entire active window. Constant declarations are read from the full text of the active window and from the specifications and bodies of referenced packages in accessible schemas through the `ALL_SOURCE` view. Private and public synonyms are resolved through `ALL_SYNONYMS`, and package dependencies are loaded recursively.

This is an independent third-party plugin, not a product of Allround Automations.

## Examples

### Packages in another window

Given the following package declarations:

```sql
create or replace package package_first is
  c_status constant varchar2(10) := 'ACTIVE';
  c_limit  constant number := 100;
  c_day    constant date := date '2026-01-01';
end;
/

create or replace package package_second is
  c_status        constant varchar2(10) := package_first.c_status;
  c_another_limit constant number := 20;
end;
/
```

The packages do not have to be open in the active editor. When connected, the plugin reads referenced packages from `ALL_SOURCE` and recursively loads their dependencies. For example, it transforms:

```sql
select package_first.c_status from dual;
select package_second.c_status from dual;
```

into:

```sql
select 'ACTIVE' from dual;
select 'ACTIVE' from dual;
```

Qualified references are supported in PL/SQL blocks as well as SQL statements.

### Synonym for a package in another schema

Given a package owned by `another_app_user`:

```sql
create or replace package package_third is
  c_status_inactive constant varchar2(10) := 'INACTIVE';
  c_limit           constant number := 30;
end;
/

grant execute on package_third to app_user;
```

and a synonym owned by `app_user`:

```sql
create or replace synonym package_synonym for another_app_user.package_third;
```

when connected as `app_user`, the plugin resolves the synonym to its target schema and package. For example, it transforms:

```sql
select package_synonym.c_limit from dual;

begin
  dbms_output.put_line(package_synonym.c_status_inactive);
  dbms_output.put_line(package_synonym.c_limit);
  dbms_output.put_line(another_app_user.package_third.c_limit);
end;
/
```

into:

```sql
select 30 from dual;

begin
  dbms_output.put_line('INACTIVE');
  dbms_output.put_line(30);
  dbms_output.put_line(30);
end;
/
```

References through the synonym and through the fully qualified target name can be used in the same window.

### Package in the same window

If the package is declared in the same window, its prefix can be omitted in PL/SQL blocks and SQL statements. For example, given:

```sql
create or replace package body app_api as
  c_status constant varchar2(10) := 'ACTIVE';
  c_limit  constant number := 100;
  c_day    constant date := date '2026-01-01';

  function get_items return sys_refcursor is
  begin
    -- ...
  end;
end;
```

selecting the following fragment elsewhere in the same window:

```sql
select *
  from app_items
 where status = c_status
   and created_at >= c_day
   and rownum <= c_limit
```

and running the plugin produces:

```sql
select *
  from app_items
 where status = 'ACTIVE'
   and created_at >= date '2026-01-01'
   and rownum <= 100
```

## Features

- Unquoted constant names are recognized case-insensitively.
- Local constants and references in the `package.constant` and `schema.package.constant` forms are supported.
- When connected, the plugin loads specifications and bodies of packages from the current and explicitly referenced schemas through `ALL_SOURCE`. Private and public synonyms, including synonyms for packages in other schemas, are resolved through `ALL_SYNONYMS`, with a private synonym taking precedence over a public one.
- Qualified references such as `package.constant` and `schema.package.constant` are resolved using the package specification only. Constants declared only in a package body are available through unqualified names.
- Packages referenced by loaded specifications and bodies are loaded recursively. Unqualified names inside a package are resolved relative to its owner's schema. Duplicate loading and cycles are prevented, and resolution depth is limited to 32 levels.
- If the processed code contains no package-qualified references, specifications of all packages in the current schema are loaded in one query so that unqualified constants can also be resolved outside package windows.
- String, numeric, date, and other SQL-compatible values are inserted without changing the original expression.
- Compound expressions are enclosed in parentheses: `c_size constant number := 10 * 2` becomes `(10 * 2)`.
- Constants used in other constant initializers are resolved recursively. Duplicate names are considered unambiguous when their resolved values are identical.
- Identifiers inside string literals and comments are left unchanged.
- Constant declarations are preserved even when the entire document is processed.
- Bind variables `:name` and substitution variables `&name` are left unchanged.
- Regular strings, `n'...'`, alternative quoting syntax such as `q'[...]'`, and quoted identifiers are supported.
- Changes can be undone with the standard **Undo** command (`Ctrl+Z`).

## Building

Requirements: Windows, CMake 3.20 or later, and a C++17 compiler. The primary option is Visual Studio 2022 with the **Desktop development with C++** workload.

### 64-bit DLL

Open **x64 Native Tools Command Prompt for VS 2022** in the project directory:

```bat
cmake -S . -B build\x64 -A x64
cmake --build build\x64 --config Release
```

Output file: `build\x64\Release\ReplaceConstants.dll`.

### 32-bit DLL

Open **x86 Native Tools Command Prompt for VS 2022**:

```bat
cmake -S . -B build\x86 -A Win32
cmake --build build\x86 --config Release
```

Output file: `build\x86\Release\ReplaceConstants.dll`.

The DLL architecture must always match PL/SQL Developer; the IDE ignores plugins with a different architecture. Both 32-bit and 64-bit variants of the IDE exist, including versions 16 and 17.

## Installation

1. Close PL/SQL Developer.
2. Copy `ReplaceConstants.dll` with the appropriate architecture to the `PlugIns` subdirectory of the PL/SQL Developer installation directory. For example:

   ```text
   C:\Program Files\PLSQL Developer 16\PlugIns\ReplaceConstants.dll
   ```

   The actual path may differ. Administrator privileges may be required to install into `Program Files`.
3. Start PL/SQL Developer.
4. Open **Tools → Configure Plug-Ins…** and make sure **Replace Constants** is listed and enabled.
5. In PL/SQL Developer 11 and earlier, the command is available under **Edit → Replace PL → SQL constants**. In version 12 and later, use **Plug-Ins → Replace Constants → SQL constants**.

A separate Oracle Client is not required: database access uses the SQL API of the current PL/SQL Developer instance.

## Usage

1. Open PL/SQL code in a Program Window or another PL/SQL Developer editor.
2. Select the SQL query or fragment in which constants should be replaced. Clear the selection to process the entire window.
3. Choose **Edit → Replace PL → SQL constants** in PL/SQL Developer 11 and earlier, or **Plug-Ins → Replace Constants → SQL constants** in version 12 and later.
4. Review the result and run the query. The status bar shows the number of replacements.
5. If necessary, press `Ctrl+Z` to restore the original text.

If there are no matches, the text remains unchanged and an explanatory message is displayed.

## Limitations

- Loading packages from other schemas and resolving synonyms require the current user to have access to the objects and to `ALL_SOURCE` and `ALL_SYNONYMS`. Synonyms that point to a remote database through `DB_LINK` are not resolved. Without a connection, declarations from the active window remain available.
- The plugin does not evaluate expressions or calculate them in Oracle. For example, `to_date('2026-01-01', 'YYYY-MM-DD')` is inserted as `(to_date('2026-01-01', 'YYYY-MM-DD'))`.
- The initializer must be a valid SQL expression at the insertion point. Values of PL/SQL-specific types, such as `boolean`, may not be suitable for regular SQL.
- If the same name is declared with different values in several scopes, ambiguous references are skipped. Qualify package constants as `package.constant`. Always review the converted SQL before running it.
- The plugin replaces lexical references to known constants rather than performing a full semantic query analysis. If a column has the same name as a constant, review that fragment manually.

## Project structure

- `src/plugin.cpp` — PL/SQL Developer DLL interface, `ALL_SOURCE` access, and replacement of the selection or entire window through `EM_REPLACESEL`.
- `src/constant_replacer.cpp` — lexical parsing of declarations and value substitution.
- `src/constant_replacer.h` — Windows-independent core interface.
- `src/replace_constants.def` — exported function names.

The plugin interface documentation is located in the `PlugInDoc` directory of the PL/SQL Developer installation. The list of official and third-party plugins is available at <https://www.allroundautomations.com/products/pl-sql-developer/plug-ins/>.

## License

This project is distributed under the [MIT License](LICENSE).
