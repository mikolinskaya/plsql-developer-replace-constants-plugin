#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "constant_replacer.h"

namespace {

using GetText = const char* (__cdecl*)();
using GetEditorHandle = HWND (__cdecl*)();
using GetReadOnly = BOOL (__cdecl*)();
using SetStatusMessage = BOOL (__cdecl*)(const char*);
using Connected = BOOL (__cdecl*)();
using SqlExecute = int (__cdecl*)(const char*);
using SqlEof = BOOL (__cdecl*)();
using SqlNext = int (__cdecl*)();
using SqlField = const char* (__cdecl*)(int);
using SqlFieldIndex = int (__cdecl*)(const char*);
using SqlErrorMessage = const char* (__cdecl*)();

GetText getText = nullptr;
GetText getSelectedText = nullptr;
GetEditorHandle getEditorHandle = nullptr;
GetReadOnly getReadOnly = nullptr;
SetStatusMessage setStatusMessage = nullptr;
Connected connected = nullptr;
SqlExecute sqlExecute = nullptr;
SqlEof sqlEof = nullptr;
SqlNext sqlNext = nullptr;
SqlField sqlField = nullptr;
SqlFieldIndex sqlFieldIndex = nullptr;
SqlErrorMessage sqlErrorMessage = nullptr;

constexpr const char* pluginName = "Replace Constants";
constexpr const char* menuTitle = "Replace PL/SQL constants";

HWND ownerWindow() {
    return getEditorHandle ? getEditorHandle() : nullptr;
}

void showMessage(const std::string& text, UINT type = MB_ICONINFORMATION) {
    MessageBoxA(ownerWindow(), text.c_str(), menuTitle, MB_OK | type);
}

std::optional<std::string> readSqlField(int field) {
    const char* value = sqlField(field);
    return value ? std::optional<std::string>(value) : std::nullopt;
}

std::string sqlLiteral(const std::string& value) {
    std::string result = "'";
    result.reserve(value.size() + 2);
    for (const char character : value) {
        result += character;
        if (character == '\'') {
            result += '\'';
        }
    }
    return result + "'";
}

std::string sqlInList(const std::vector<std::string>& values) {
    std::string result = "(";
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            result += ", ";
        }
        result += sqlLiteral(values[index]);
    }
    return result + ")";
}

std::string sqlError(const std::string& action, int errorNumber) {
    std::string message = action + " (Oracle error " + std::to_string(errorNumber) + ")";
    if (sqlErrorMessage) {
        const char* details = sqlErrorMessage();
        if (details && *details) {
            message += ": " + std::string(details);
        }
    }
    return message;
}

class PackageLoader {
public:
    PackageLoader(std::vector<PackageSource>& sources, std::string& error)
        : sources_(sources), error_(error) {
    }

    bool load(const std::string& text) {
        const std::vector<PackageReference> references = referencedPackages(text);
        if (references.empty()) {
            return loadCurrentSchema();
        }
        std::vector<Request> requests;
        requests.reserve(references.size());
        for (const PackageReference& reference : references) {
            requests.push_back(Request{reference, "", reference});
        }
        return resolveMany(requests, 0);
    }

private:
    struct Request {
        PackageReference target;
        std::string defaultOwner;
        PackageReference alias;
    };

    static constexpr std::size_t maxDepth = 32;

    static std::string makeKey(const std::string& owner, const std::string& name, bool includePublic) {
        return std::to_string(owner.size()) + ":" + owner + std::to_string(name.size()) + ":" + name +
               (includePublic ? ":P" : ":O");
    }

    static const std::string& effectiveOwner(const Request& request) {
        return request.target.owner.empty() ? request.defaultOwner : request.target.owner;
    }

    std::string requestKey(const Request& request) const {
        return makeKey(effectiveOwner(request), request.target.name, request.target.owner.empty());
    }

    void addAlias(std::size_t sourceIndex, const PackageReference& alias) {
        std::vector<PackageReference>& aliases = sources_[sourceIndex].aliases;
        for (const PackageReference& existing : aliases) {
            if (existing == alias) {
                return;
            }
        }
        aliases.push_back(alias);
    }

    std::size_t addSource(PackageSource source, const PackageReference& alias, bool& isNew) {
        const std::string actualKey = makeKey(source.owner, source.name, false);
        const auto existing = packageIndices_.find(actualKey);
        isNew = existing == packageIndices_.end();
        const std::size_t sourceIndex = isNew ? sources_.size() : existing->second;
        if (isNew) {
            packageIndices_.emplace(actualKey, sourceIndex);
            sources_.push_back(std::move(source));
        }
        addAlias(sourceIndex, alias);
        return sourceIndex;
    }

    void appendDependencies(std::size_t sourceIndex, std::vector<Request>& requests) {
        const PackageSource& loadedSource = sources_[sourceIndex];
        std::vector<PackageReference> dependencies = referencedPackages(loadedSource.text);
        const std::vector<PackageReference> bodyDependencies = referencedPackages(loadedSource.bodyText);
        dependencies.insert(dependencies.end(), bodyDependencies.begin(), bodyDependencies.end());
        const std::string packageOwner = loadedSource.owner;
        for (const PackageReference& dependency : dependencies) {
            requests.push_back(Request{dependency, packageOwner, dependency});
        }
    }

    bool loadCurrentSchema() {
        std::vector<PackageSource> loadedSources;
        if (!readPackages("", {}, false, loadedSources)) {
            return false;
        }

        std::vector<std::size_t> newSources;
        for (PackageSource& source : loadedSources) {
            const PackageReference alias{"", source.name};
            bool isNew = false;
            const std::size_t sourceIndex = addSource(std::move(source), alias, isNew);
            if (isNew) {
                newSources.push_back(sourceIndex);
            }
        }
        std::vector<Request> dependencies;
        for (const std::size_t sourceIndex : newSources) {
            appendDependencies(sourceIndex, dependencies);
        }
        return resolveMany(dependencies, 1);
    }

    struct PendingRequest {
        Request request;
        std::string key;
        std::vector<PackageReference> aliases;
        std::optional<PackageReference> synonym;
    };

    bool resolveMany(const std::vector<Request>& requests, std::size_t depth) {
        if (requests.empty()) {
            return true;
        }
        if (depth > maxDepth) {
            error_ = "Package dependency or synonym resolution exceeds the maximum depth of " +
                     std::to_string(maxDepth) + ".";
            return false;
        }

        std::vector<PendingRequest> pending;
        std::unordered_map<std::string, std::size_t> pendingIndices;
        for (const Request& request : requests) {
            const std::string key = requestKey(request);
            const auto cached = requestResults_.find(key);
            if (cached != requestResults_.end()) {
                if (cached->second) {
                    addAlias(*cached->second, request.alias);
                }
                continue;
            }

            const auto loaded = packageIndices_.find(
                makeKey(effectiveOwner(request), request.target.name, false));
            if (loaded != packageIndices_.end()) {
                addAlias(loaded->second, request.alias);
                requestResults_[key] = loaded->second;
                continue;
            }

            const auto existing = pendingIndices.find(key);
            if (existing != pendingIndices.end()) {
                pending[existing->second].aliases.push_back(request.alias);
                continue;
            }
            if (!resolving_.insert(key).second) {
                requestResults_[key] = std::nullopt;
                continue;
            }
            pendingIndices.emplace(key, pending.size());
            pending.push_back(PendingRequest{request, key, {request.alias}, std::nullopt});
        }
        if (pending.empty()) {
            return true;
        }

        std::vector<std::string> owners;
        std::unordered_map<std::string, std::vector<std::string>> namesByOwner;
        std::unordered_map<std::string, std::size_t> pendingTargets;
        for (std::size_t index = 0; index < pending.size(); ++index) {
            const PendingRequest& item = pending[index];
            const std::string owner = effectiveOwner(item.request);
            auto names = namesByOwner.find(owner);
            if (names == namesByOwner.end()) {
                owners.push_back(owner);
                names = namesByOwner.emplace(owner, std::vector<std::string>{}).first;
            }
            if (std::find(names->second.begin(), names->second.end(), item.request.target.name) ==
                names->second.end()) {
                names->second.push_back(item.request.target.name);
            }
            pendingTargets.emplace(makeKey(owner, item.request.target.name, false), index);
        }

        std::vector<Request> nextRequests;
        std::unordered_map<std::string, std::size_t> resolvedRequests;
        for (const std::string& owner : owners) {
            std::vector<PackageSource> matches;
            if (!readPackages(owner, namesByOwner[owner], true, matches)) {
                return false;
            }
            for (PackageSource& source : matches) {
                const std::string sourceName = source.name;
                const auto target = pendingTargets.find(makeKey(owner, sourceName, false));
                if (target == pendingTargets.end()) {
                    continue;
                }
                const PackageReference& firstAlias = pending[target->second].aliases.front();
                bool isNew = false;
                const std::size_t sourceIndex = addSource(std::move(source), firstAlias, isNew);
                resolvedRequests[makeKey(owner, sourceName, false)] = sourceIndex;
                if (isNew) {
                    appendDependencies(sourceIndex, nextRequests);
                }
            }
        }

        std::vector<std::size_t> missing;
        for (std::size_t index = 0; index < pending.size(); ++index) {
            PendingRequest& item = pending[index];
            const auto found = resolvedRequests.find(
                makeKey(effectiveOwner(item.request), item.request.target.name, false));
            if (found == resolvedRequests.end()) {
                missing.push_back(index);
                continue;
            }
            for (const PackageReference& alias : item.aliases) {
                addAlias(found->second, alias);
            }
            requestResults_[item.key] = found->second;
        }

        if (!missing.empty()) {
            std::vector<SynonymRequest> synonymRequests;
            synonymRequests.reserve(missing.size());
            for (const std::size_t index : missing) {
                const Request& request = pending[index].request;
                synonymRequests.push_back(SynonymRequest{
                    effectiveOwner(request), request.target.name, request.target.owner.empty()});
            }
            std::vector<SynonymRow> synonyms;
            if (!readSynonyms(synonymRequests, synonyms)) {
                return false;
            }
            for (const std::size_t index : missing) {
                PendingRequest& item = pending[index];
                const SynonymRequest request{effectiveOwner(item.request), item.request.target.name,
                                             item.request.target.owner.empty()};
                for (const SynonymRow& row : synonyms) {
                    if (!matchesSynonym(request, row)) {
                        continue;
                    }
                    if (row.dbLink.empty()) {
                        item.synonym = PackageReference{row.targetOwner, row.targetName};
                    }
                    break;
                }
                if (item.synonym) {
                    for (const PackageReference& alias : item.aliases) {
                        nextRequests.push_back(Request{*item.synonym, "", alias});
                    }
                } else {
                    requestResults_[item.key] = std::nullopt;
                }
            }
        }

        if (!resolveMany(nextRequests, depth + 1)) {
            return false;
        }
        for (PendingRequest& item : pending) {
            if (item.synonym) {
                const auto result = requestResults_.find(
                    makeKey(item.synonym->owner, item.synonym->name, false));
                requestResults_[item.key] = result == requestResults_.end()
                    ? std::optional<std::size_t>{} : result->second;
            }
            resolving_.erase(item.key);
        }
        return true;
    }

    bool readPackages(const std::string& owner, const std::vector<std::string>& names,
                      bool includeBodies, std::vector<PackageSource>& sources) {
        std::string query = "select owner, name, type, text from all_source where ";
        query += owner.empty() ? "owner = user" : "owner = " + sqlLiteral(owner);
        query += includeBodies ? " and type in ('PACKAGE', 'PACKAGE BODY')" : " and type = 'PACKAGE'";
        if (names.size() == 1) {
            query += " and name = " + sqlLiteral(names.front());
        } else if (!names.empty()) {
            query += " and name in " + sqlInList(names);
        }
        query += " order by owner, name, case when type = 'PACKAGE' then 0 else 1 end, line";

        const int executeResult = sqlExecute(query.c_str());
        if (executeResult != 0) {
            error_ = sqlError("Cannot read a package source from ALL_SOURCE", executeResult);
            return false;
        }

        const int ownerField = sqlFieldIndex("OWNER");
        const int nameField = sqlFieldIndex("NAME");
        const int typeField = sqlFieldIndex("TYPE");
        const int textField = sqlFieldIndex("TEXT");
        if (ownerField < 0 || nameField < 0 || typeField < 0 || textField < 0) {
            error_ = "ALL_SOURCE did not return the expected OWNER, NAME, TYPE and TEXT columns.";
            return false;
        }

        std::string currentPackage;
        while (!sqlEof()) {
            const std::optional<std::string> ownerValue = readSqlField(ownerField);
            const std::optional<std::string> nameValue = readSqlField(nameField);
            const std::optional<std::string> typeValue = readSqlField(typeField);
            const std::optional<std::string> textValue = readSqlField(textField);
            if (!ownerValue || !nameValue || !typeValue) {
                error_ = "Cannot read a package owner, name or type from ALL_SOURCE.";
                return false;
            }

            const std::string packageKey = makeKey(*ownerValue, *nameValue, false);
            if (sources.empty() || packageKey != currentPackage) {
                sources.push_back(PackageSource{*ownerValue, *nameValue, "", {}, ""});
                currentPackage = packageKey;
            }
            if (textValue) {
                std::string& target = *typeValue == "PACKAGE BODY"
                    ? sources.back().bodyText : sources.back().text;
                target += *textValue;
            }

            const int nextResult = sqlNext();
            if (nextResult != 0) {
                error_ = sqlError("Cannot fetch the next package source row", nextResult);
                return false;
            }
        }
        return true;
    }

    struct SynonymRequest {
        std::string owner;
        std::string name;
        bool includePublic;
    };

    struct SynonymRow {
        std::string owner;
        std::string synonymName;
        std::string targetOwner;
        std::string targetName;
        std::string dbLink;
    };

    static bool matchesSynonym(const SynonymRequest& request, const SynonymRow& row) {
        if (request.name != row.synonymName) {
            return false;
        }
        if (row.owner == "PUBLIC") {
            return request.owner == "PUBLIC" || request.includePublic;
        }
        return request.owner.empty() || request.owner == row.owner;
    }

    bool readSynonyms(const std::vector<SynonymRequest>& requests, std::vector<SynonymRow>& rows) {
        std::vector<std::string> names;
        std::vector<std::string> owners;
        bool includeCurrentUser = false;
        bool includePublic = false;
        auto addUnique = [](std::vector<std::string>& values, const std::string& value) {
            if (std::find(values.begin(), values.end(), value) == values.end()) {
                values.push_back(value);
            }
        };
        for (const SynonymRequest& request : requests) {
            addUnique(names, request.name);
            if (request.owner.empty()) {
                includeCurrentUser = true;
            } else {
                addUnique(owners, request.owner);
            }
            includePublic = includePublic || (request.includePublic && request.owner != "PUBLIC");
        }
        if (includePublic) {
            addUnique(owners, "PUBLIC");
        }

        std::string ownerCondition;
        if (includeCurrentUser) {
            ownerCondition = "owner = user";
        }
        if (!owners.empty()) {
            if (!ownerCondition.empty()) {
                ownerCondition += " or ";
            }
            ownerCondition += owners.size() == 1
                ? "owner = " + sqlLiteral(owners.front())
                : "owner in " + sqlInList(owners);
        }
        const std::string query =
            "select table_owner, table_name, db_link, owner, synonym_name from all_synonyms "
            "where synonym_name in " + sqlInList(names) + " and (" + ownerCondition +
            ") order by synonym_name, case when owner = 'PUBLIC' then 1 else 0 end";

        const int executeResult = sqlExecute(query.c_str());
        if (executeResult != 0) {
            error_ = sqlError("Cannot resolve package synonyms through ALL_SYNONYMS", executeResult);
            return false;
        }

        const int targetOwnerField = sqlFieldIndex("TABLE_OWNER");
        const int targetNameField = sqlFieldIndex("TABLE_NAME");
        const int linkField = sqlFieldIndex("DB_LINK");
        const int ownerField = sqlFieldIndex("OWNER");
        const int synonymNameField = sqlFieldIndex("SYNONYM_NAME");
        if (targetOwnerField < 0 || targetNameField < 0 || linkField < 0 ||
            ownerField < 0 || synonymNameField < 0) {
            error_ = "ALL_SYNONYMS did not return the expected synonym and target columns.";
            return false;
        }
        while (!sqlEof()) {
            const std::optional<std::string> targetOwnerValue = readSqlField(targetOwnerField);
            const std::optional<std::string> targetNameValue = readSqlField(targetNameField);
            const std::optional<std::string> linkValue = readSqlField(linkField);
            const std::optional<std::string> ownerValue = readSqlField(ownerField);
            const std::optional<std::string> synonymNameValue = readSqlField(synonymNameField);
            if (!targetOwnerValue || !targetNameValue || !ownerValue || !synonymNameValue) {
                error_ = "Cannot read a synonym target from ALL_SYNONYMS.";
                return false;
            }
            rows.push_back(SynonymRow{*ownerValue, *synonymNameValue, *targetOwnerValue,
                                      *targetNameValue, linkValue ? *linkValue : ""});
            const int nextResult = sqlNext();
            if (nextResult != 0) {
                error_ = sqlError("Cannot fetch the next synonym row", nextResult);
                return false;
            }
        }
        return true;
    }

    std::vector<PackageSource>& sources_;
    std::string& error_;
    std::unordered_map<std::string, std::optional<std::size_t>> requestResults_;
    std::unordered_map<std::string, std::size_t> packageIndices_;
    std::unordered_set<std::string> resolving_;
};

bool loadPackageSources(const std::string& text, std::vector<PackageSource>& sources, std::string& error) {
    if (!connected || !sqlExecute || !sqlEof || !sqlNext || !sqlField || !sqlFieldIndex || !connected()) {
        return true;
    }
    PackageLoader loader(sources, error);
    return loader.load(text);
}

void replaceSelection() {
    if (!getText || !getSelectedText || !getEditorHandle) {
        showMessage("The PL/SQL Developer editor API is not available.", MB_ICONERROR);
        return;
    }
    if (getReadOnly && getReadOnly()) {
        showMessage("The current editor is read-only.", MB_ICONWARNING);
        return;
    }

    const char* selectionValue = getSelectedText();
    const std::string selectedText = selectionValue ? selectionValue : "";
    const char* sourceValue = getText();
    if (!sourceValue) {
        showMessage("Cannot read the current editor contents.", MB_ICONERROR);
        return;
    }
    const std::string sourceText = sourceValue;
    const bool wholeWindow = selectedText.empty();
    const std::string& targetText = wholeWindow ? sourceText : selectedText;

    std::vector<PackageSource> packageSources;
    std::string databaseError;
    if (!loadPackageSources(targetText, packageSources, databaseError)) {
        showMessage(databaseError, MB_ICONERROR);
        return;
    }

    const ReplacementResult result = replaceConstants(sourceText, targetText, packageSources);
    if (result.constantsFound == 0) {
        showMessage("No CONSTANT declarations with initializers were found in the current window or referenced packages.");
        return;
    }
    if (result.replacements == 0) {
        std::string message = wholeWindow
            ? "No unambiguous constant references were found in the current window."
            : "No unambiguous constant references were found in the selection.";
        if (result.ambiguousReferences > 0) {
            message += " Qualify duplicate constant names with their package name.";
        }
        showMessage(message);
        return;
    }

    const HWND editor = getEditorHandle();
    if (!editor) {
        showMessage("The current window has no editor.", MB_ICONERROR);
        return;
    }
    if (wholeWindow) {
        SendMessageA(editor, EM_SETSEL, 0, -1);
    }
    SendMessageA(editor, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(result.text.c_str()));

    if (setStatusMessage) {
        std::string status = "Replace Constants: " + std::to_string(result.replacements) + " replacement(s)";
        if (result.ambiguousReferences > 0) {
            status += ", " + std::to_string(result.ambiguousReferences) + " ambiguous reference(s) skipped";
        }
        setStatusMessage(status.c_str());
    }
}

} // namespace

extern "C" {

__declspec(dllexport) const char* __cdecl IdentifyPlugIn(int) {
    return pluginName;
}

__declspec(dllexport) const char* __cdecl CreateMenuItem(int index) {
    return index == 1 ? "Edit / Replace PL/SQL constants" : "";
}

__declspec(dllexport) void __cdecl RegisterCallback(int index, void* function) {
    switch (index) {
        case 11:
            connected = reinterpret_cast<Connected>(function);
            break;
        case 26:
            getReadOnly = reinterpret_cast<GetReadOnly>(function);
            break;
        case 30:
            getText = reinterpret_cast<GetText>(function);
            break;
        case 31:
            getSelectedText = reinterpret_cast<GetText>(function);
            break;
        case 33:
            getEditorHandle = reinterpret_cast<GetEditorHandle>(function);
            break;
        case 35:
            setStatusMessage = reinterpret_cast<SetStatusMessage>(function);
            break;
        case 40:
            sqlExecute = reinterpret_cast<SqlExecute>(function);
            break;
        case 42:
            sqlEof = reinterpret_cast<SqlEof>(function);
            break;
        case 43:
            sqlNext = reinterpret_cast<SqlNext>(function);
            break;
        case 44:
            sqlField = reinterpret_cast<SqlField>(function);
            break;
        case 46:
            sqlFieldIndex = reinterpret_cast<SqlFieldIndex>(function);
            break;
        case 48:
            sqlErrorMessage = reinterpret_cast<SqlErrorMessage>(function);
            break;
        default:
            break;
    }
}

__declspec(dllexport) void __cdecl OnMenuClick(int index) {
    if (index != 1) {
        return;
    }
    try {
        replaceSelection();
    } catch (const std::exception& error) {
        showMessage(std::string("Replacement failed: ") + error.what(), MB_ICONERROR);
    } catch (...) {
        showMessage("Replacement failed because of an unexpected error.", MB_ICONERROR);
    }
}

__declspec(dllexport) const char* __cdecl About() {
    return "Replace Constants 1.0.0\n"
           "Replaces references to PL/SQL constants in the selection or current window.\n"
           "Declarations are read from the editor and recursively loaded package specifications and bodies, including other schemas and synonyms.";
}

}
