#include "constant_replacer.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

enum class TokenKind {
    Identifier,
    QuotedIdentifier,
    Number,
    String,
    Symbol
};

struct Token {
    TokenKind kind;
    std::size_t begin;
    std::size_t end;
};

struct Span {
    std::size_t begin;
    std::size_t end;
};

struct Constant {
    std::string name;
    std::string expression;
    Span declaration;
};

bool isIdentifier(const Token& token) {
    return token.kind == TokenKind::Identifier || token.kind == TokenKind::QuotedIdentifier;
}

char asciiLower(char value) {
    if (value >= 'A' && value <= 'Z') {
        return static_cast<char>(value - 'A' + 'a');
    }
    return value;
}

char asciiUpper(char value) {
    if (value >= 'a' && value <= 'z') {
        return static_cast<char>(value - 'a' + 'A');
    }
    return value;
}

bool equalsIgnoreCase(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
               return asciiLower(a) == asciiLower(b);
           });
}

std::string upper(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), asciiUpper);
    return result;
}

std::string trim(std::string_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::isspace(static_cast<unsigned char>(value[begin]))) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return std::string(value.substr(begin, end - begin));
}

bool isIdentifierStart(char value) {
    const unsigned char byte = static_cast<unsigned char>(value);
    return std::isalpha(byte) || value == '_' || byte >= 0x80;
}

bool isIdentifierPart(char value) {
    const unsigned char byte = static_cast<unsigned char>(value);
    return std::isalnum(byte) || value == '_' || value == '$' || value == '#' || byte >= 0x80;
}

std::size_t quotedStringEnd(const std::string& text, std::size_t begin) {
    std::size_t position = begin + 1;
    while (position < text.size()) {
        if (text[position] != '\'') {
            ++position;
            continue;
        }
        if (position + 1 < text.size() && text[position + 1] == '\'') {
            position += 2;
        } else {
            return position + 1;
        }
    }
    return text.size();
}

std::optional<std::size_t> alternativeQuotedStringEnd(const std::string& text, std::size_t begin) {
    std::size_t position = begin;
    if (position < text.size() && (text[position] == 'n' || text[position] == 'N')) {
        ++position;
    }
    if (position + 2 >= text.size() || (text[position] != 'q' && text[position] != 'Q') ||
        text[position + 1] != '\'') {
        return std::nullopt;
    }

    const char opening = text[position + 2];
    char closing = opening;
    switch (opening) {
        case '[': closing = ']'; break;
        case '{': closing = '}'; break;
        case '(': closing = ')'; break;
        case '<': closing = '>'; break;
        default: break;
    }

    position += 3;
    while (position + 1 < text.size()) {
        if (text[position] == closing && text[position + 1] == '\'') {
            return position + 2;
        }
        ++position;
    }
    return text.size();
}

std::vector<Token> tokenize(const std::string& text) {
    std::vector<Token> tokens;
    std::size_t position = 0;
    while (position < text.size()) {
        const char value = text[position];
        if (std::isspace(static_cast<unsigned char>(value))) {
            ++position;
            continue;
        }
        if (value == '-' && position + 1 < text.size() && text[position + 1] == '-') {
            position += 2;
            while (position < text.size() && text[position] != '\n' && text[position] != '\r') {
                ++position;
            }
            continue;
        }
        if (value == '/' && position + 1 < text.size() && text[position + 1] == '*') {
            const std::size_t closing = text.find("*/", position + 2);
            position = closing == std::string::npos ? text.size() : closing + 2;
            continue;
        }
        if (value == '\'') {
            const std::size_t end = quotedStringEnd(text, position);
            tokens.push_back({TokenKind::String, position, end});
            position = end;
            continue;
        }
        if (const auto end = alternativeQuotedStringEnd(text, position)) {
            tokens.push_back({TokenKind::String, position, *end});
            position = *end;
            continue;
        }
        if (value == '"') {
            const std::size_t begin = position++;
            while (position < text.size()) {
                if (text[position] != '"') {
                    ++position;
                } else if (position + 1 < text.size() && text[position + 1] == '"') {
                    position += 2;
                } else {
                    ++position;
                    break;
                }
            }
            tokens.push_back({TokenKind::QuotedIdentifier, begin, position});
            continue;
        }
        if (isIdentifierStart(value)) {
            const std::size_t begin = position++;
            while (position < text.size() && isIdentifierPart(text[position])) {
                ++position;
            }
            tokens.push_back({TokenKind::Identifier, begin, position});
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(value)) ||
            (value == '.' && position + 1 < text.size() &&
             std::isdigit(static_cast<unsigned char>(text[position + 1])))) {
            const std::size_t begin = position++;
            while (position < text.size() &&
                   std::isdigit(static_cast<unsigned char>(text[position]))) {
                ++position;
            }
            if (position < text.size() && text[position] == '.') {
                ++position;
                while (position < text.size() &&
                       std::isdigit(static_cast<unsigned char>(text[position]))) {
                    ++position;
                }
            }
            if (position < text.size() && (text[position] == 'e' || text[position] == 'E')) {
                std::size_t exponent = position + 1;
                if (exponent < text.size() && (text[exponent] == '+' || text[exponent] == '-')) {
                    ++exponent;
                }
                const std::size_t digits = exponent;
                while (exponent < text.size() &&
                       std::isdigit(static_cast<unsigned char>(text[exponent]))) {
                    ++exponent;
                }
                if (exponent > digits) {
                    position = exponent;
                }
            }
            if (position < text.size() &&
                (text[position] == 'd' || text[position] == 'D' ||
                 text[position] == 'f' || text[position] == 'F')) {
                ++position;
            }
            tokens.push_back({TokenKind::Number, begin, position});
            continue;
        }

        const std::size_t begin = position++;
        if (position < text.size()) {
            const std::string_view pair(text.data() + begin, 2);
            if (pair == ":=" || pair == "||" || pair == "=>" || pair == "**" ||
                pair == "!=" || pair == "~=" || pair == "^=" || pair == "<=" || pair == ">=") {
                ++position;
            }
        }
        tokens.push_back({TokenKind::Symbol, begin, position});
    }
    return tokens;
}

std::string_view tokenText(const std::string& text, const Token& token) {
    return std::string_view(text).substr(token.begin, token.end - token.begin);
}

std::string normalizeIdentifier(const std::string& text, const Token& token) {
    if (token.kind == TokenKind::QuotedIdentifier) {
        std::string value;
        for (std::size_t position = token.begin + 1; position + 1 < token.end; ++position) {
            value += text[position];
            if (text[position] == '"' && position + 2 < token.end && text[position + 1] == '"') {
                ++position;
            }
        }
        return "Q:" + value;
    }
    std::string value(tokenText(text, token));
    std::transform(value.begin(), value.end(), value.begin(), asciiUpper);
    return "U:" + value;
}

bool parseQualifiedName(const std::string& text, const std::vector<Token>& tokens,
                        std::size_t& position, std::vector<std::string>& parts) {
    if (position >= tokens.size() || !isIdentifier(tokens[position])) {
        return false;
    }
    parts.push_back(normalizeIdentifier(text, tokens[position]));
    ++position;
    while (position + 1 < tokens.size() && tokenText(text, tokens[position]) == "." &&
           isIdentifier(tokens[position + 1])) {
        parts.push_back(normalizeIdentifier(text, tokens[position + 1]));
        position += 2;
    }
    return true;
}

std::string joinName(const std::vector<std::string>& parts, std::size_t begin = 0) {
    std::string result;
    for (std::size_t index = begin; index < parts.size(); ++index) {
        if (!result.empty()) {
            result += '.';
        }
        result += parts[index];
    }
    return result;
}

struct ParsedDeclarations {
    std::vector<Token> tokens;
    std::vector<Span> declarations;
    std::vector<std::string> qualifiedNames;
    std::vector<std::string> shortQualifiedNames;
};

ParsedDeclarations parseDeclarations(const std::string& text, std::vector<Constant>& constants) {
    ParsedDeclarations result;
    result.tokens = tokenize(text);
    const std::vector<Token>& tokens = result.tokens;
    std::vector<std::string> packagePath;

    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (equalsIgnoreCase(tokenText(text, tokens[index]), "package")) {
            std::size_t position = index + 1;
            if (position < tokens.size() &&
                equalsIgnoreCase(tokenText(text, tokens[position]), "body")) {
                ++position;
            }
            std::vector<std::string> name;
            if (parseQualifiedName(text, tokens, position, name)) {
                packagePath = std::move(name);
            }
        }

        if (!isIdentifier(tokens[index]) || index + 1 >= tokens.size() ||
            !equalsIgnoreCase(tokenText(text, tokens[index + 1]), "constant")) {
            continue;
        }

        std::size_t assignment = tokens.size();
        std::size_t semicolon = tokens.size();
        int parentheses = 0;
        for (std::size_t position = index + 2; position < tokens.size(); ++position) {
            const std::string_view value = tokenText(text, tokens[position]);
            if (value == "(") {
                ++parentheses;
            } else if (value == ")" && parentheses > 0) {
                --parentheses;
            } else if (parentheses == 0 && value == ":=" && assignment == tokens.size()) {
                assignment = position;
            } else if (parentheses == 0 && value == ";") {
                semicolon = position;
                break;
            }
        }
        if (assignment == tokens.size() || semicolon == tokens.size() || assignment + 1 >= semicolon) {
            continue;
        }

        const std::size_t expressionBegin = tokens[assignment].end;
        const std::size_t expressionEnd = tokens[semicolon].begin;
        const std::string expression = trim(
            std::string_view(text).substr(expressionBegin, expressionEnd - expressionBegin));
        if (expression.empty()) {
            continue;
        }

        const std::string name = normalizeIdentifier(text, tokens[index]);
        constants.push_back({name, expression, {tokens[index].begin, tokens[semicolon].end}});
        result.declarations.push_back({tokens[index].begin, tokens[semicolon].end});
        result.qualifiedNames.emplace_back();
        result.shortQualifiedNames.emplace_back();
        if (!packagePath.empty()) {
            std::vector<std::string> qualified = packagePath;
            qualified.push_back(name);
            result.qualifiedNames.back() = joinName(qualified);
            if (qualified.size() == 3) {
                result.shortQualifiedNames.back() = joinName(qualified, 1);
            }
        }
        index = semicolon;
    }
    return result;
}

struct LookupEntry {
    std::vector<std::size_t> constants;
};

class Replacer {
public:
    Replacer(const std::string& source, const std::vector<PackageSource>& packageSources) {
        ParsedDeclarations sourceDeclarations = parseDeclarations(source, constants_);
        declarations_ = std::move(sourceDeclarations.declarations);
        sourceTokens_ = std::move(sourceDeclarations.tokens);
        for (std::size_t index = 0; index < constants_.size(); ++index) {
            addLookup(constants_[index].name, index);
        }
        addPackageNames(sourceDeclarations, 0);

        for (const PackageSource& packageSource : packageSources) {
            std::vector<Constant> packageConstants;
            ParsedDeclarations packageDeclarations = parseDeclarations(packageSource.text, packageConstants);
            const std::size_t offset = constants_.size();
            for (const Constant& constant : packageConstants) {
                constants_.push_back(constant);
                addLookup(constant.name, constants_.size() - 1);
            }
            if (packageSource.owner.empty() || packageSource.name.empty()) {
                addPackageNames(packageDeclarations, offset);
            } else {
                addDatabasePackageNames(packageSource, packageConstants, offset);
            }

            std::vector<Constant> bodyConstants;
            parseDeclarations(packageSource.bodyText, bodyConstants);
            for (const Constant& constant : bodyConstants) {
                constants_.push_back(constant);
                addLookup(constant.name, constants_.size() - 1);
            }
        }
        resolved_.resize(constants_.size());
        rendered_.resize(constants_.size());
        states_.resize(constants_.size(), 0);
    }

    ReplacementResult run(const std::string& source, const std::string& selection) {
        ReplacementResult result;
        result.constantsFound = constants_.size();
        if (&source == &selection || source == selection) {
            result.text = replaceTokens(selection, sourceTokens_, declarations_, true);
        } else {
            std::vector<Constant> ignored;
            const ParsedDeclarations selectionDeclarations = parseDeclarations(selection, ignored);
            result.text = replaceTokens(selection, selectionDeclarations.tokens,
                                        selectionDeclarations.declarations, true);
        }
        result.replacements = replacements_;
        result.ambiguousReferences = ambiguousReferences_;
        return result;
    }

private:
    void addPackageNames(const ParsedDeclarations& declarations, std::size_t constantOffset) {
        for (std::size_t index = 0; index < declarations.qualifiedNames.size(); ++index) {
            if (!declarations.qualifiedNames[index].empty()) {
                addLookup(declarations.qualifiedNames[index], constantOffset + index);
            }
            if (!declarations.shortQualifiedNames[index].empty()) {
                addLookup(declarations.shortQualifiedNames[index], constantOffset + index);
            }
        }
    }

    static std::vector<std::string> databaseIdentifierKeys(const std::string& value) {
        std::vector<std::string> keys;
        if (upper(value) == value) {
            keys.push_back("U:" + value);
        }
        keys.push_back("Q:" + value);
        return keys;
    }

    struct DatabasePackageName {
        std::vector<std::string> prefixes;
    };

    static DatabasePackageName makeDatabasePackageName(const PackageReference& package,
                                                       bool includeUnqualified) {
        const std::vector<std::string> names = databaseIdentifierKeys(package.name);
        const std::vector<std::string> owners = package.owner.empty()
            ? std::vector<std::string>{} : databaseIdentifierKeys(package.owner);
        DatabasePackageName result;
        result.prefixes.reserve(names.size() * (owners.size() + (includeUnqualified ? 1 : 0)));
        for (const std::string& name : names) {
            if (includeUnqualified) {
                result.prefixes.push_back(name + ".");
            }
            for (const std::string& owner : owners) {
                result.prefixes.push_back(owner + "." + name + ".");
            }
        }
        return result;
    }

    void addDatabasePackageNames(const PackageSource& packageSource,
                                 const std::vector<Constant>& packageConstants,
                                 std::size_t constantOffset) {
        const PackageReference actualName{packageSource.owner, packageSource.name};
        const DatabasePackageName actual = makeDatabasePackageName(
            actualName, packageSource.aliases.empty());
        std::vector<DatabasePackageName> aliases;
        aliases.reserve(packageSource.aliases.size());
        for (const PackageReference& alias : packageSource.aliases) {
            aliases.push_back(makeDatabasePackageName(alias, alias.owner.empty()));
        }
        for (std::size_t index = 0; index < packageConstants.size(); ++index) {
            const std::size_t constantIndex = constantOffset + index;
            for (const std::string& prefix : actual.prefixes) {
                addLookup(prefix + packageConstants[index].name, constantIndex);
            }
            for (const DatabasePackageName& alias : aliases) {
                for (const std::string& prefix : alias.prefixes) {
                    addLookup(prefix + packageConstants[index].name, constantIndex);
                }
            }
        }
    }

    void addLookup(const std::string& name, std::size_t constantIndex) {
        std::vector<std::size_t>& candidates = lookup_[name].constants;
        if (std::find(candidates.begin(), candidates.end(), constantIndex) == candidates.end()) {
            candidates.push_back(constantIndex);
        }
    }

    std::optional<std::size_t> findConstant(const std::vector<std::string>& chain,
                                            bool countAmbiguous, std::string& lookupKey) {
        lookupKey.clear();
        for (const std::string& part : chain) {
            if (!lookupKey.empty()) {
                lookupKey += '.';
            }
            lookupKey += part;
        }
        const auto entry = lookup_.find(lookupKey);
        if (entry == lookup_.end()) {
            return std::nullopt;
        }

        const std::vector<std::size_t>& candidates = entry->second.constants;
        if (candidates.size() == 1) {
            return candidates.front();
        }

        std::optional<std::size_t> result;
        std::optional<std::string> value;
        for (const std::size_t candidate : candidates) {
            const std::string candidateValue = trim(resolveConstant(candidate));
            if (!result) {
                result = candidate;
                value = candidateValue;
            } else if (*value != candidateValue) {
                if (countAmbiguous) {
                    ++ambiguousReferences_;
                }
                return std::nullopt;
            }
        }
        return result;
    }

    const std::string& resolveConstant(std::size_t index) {
        if (states_[index] == 2) {
            return resolved_[index];
        }
        if (states_[index] == 1) {
            return constants_[index].expression;
        }
        states_[index] = 1;
        resolved_[index] = replaceText(constants_[index].expression, {}, false);
        states_[index] = 2;
        return resolved_[index];
    }

    static bool isSimpleExpression(const std::string& expression) {
        const std::vector<Token> tokens = tokenize(expression);
        if (tokens.size() == 1) {
            return tokens[0].kind == TokenKind::String || tokens[0].kind == TokenKind::Number ||
                   tokens[0].kind == TokenKind::Identifier;
        }
        if (tokens.size() == 2) {
            const std::string_view first = tokenText(expression, tokens[0]);
            return ((first == "+" || first == "-") && tokens[1].kind == TokenKind::Number) ||
                   ((equalsIgnoreCase(first, "date") || equalsIgnoreCase(first, "timestamp")) &&
                    tokens[1].kind == TokenKind::String);
        }
        return false;
    }

    static std::string formatConstant(const std::string& resolvedExpression) {
        const std::string expression = trim(resolvedExpression);
        if (isSimpleExpression(expression)) {
            return expression;
        }
        return "(" + expression + ")";
    }

    void appendConstant(std::string& output, std::size_t index) {
        const std::string& expression = resolveConstant(index);
        if (states_[index] != 2) {
            output += formatConstant(expression);
            return;
        }
        if (!rendered_[index]) {
            rendered_[index] = formatConstant(expression);
        }
        output += *rendered_[index];
    }

    std::string replaceText(const std::string& text, const std::vector<Span>& declarations,
                            bool countStatistics) {
        return replaceTokens(text, tokenize(text), declarations, countStatistics);
    }

    std::string replaceTokens(const std::string& text, const std::vector<Token>& tokens,
                              const std::vector<Span>& declarations, bool countStatistics) {
        std::string output;
        output.reserve(text.size());
        std::size_t copiedUntil = 0;
        std::size_t declarationIndex = 0;
        std::string lookupKey;

        for (std::size_t index = 0; index < tokens.size(); ++index) {
            if (!isIdentifier(tokens[index])) {
                continue;
            }
            while (declarationIndex < declarations.size() &&
                   tokens[index].begin >= declarations[declarationIndex].end) {
                ++declarationIndex;
            }
            if (declarationIndex < declarations.size() &&
                tokens[index].begin >= declarations[declarationIndex].begin) {
                continue;
            }
            if (index > 0) {
                const std::string_view previous = tokenText(text, tokens[index - 1]);
                if (previous == ":" || previous == "&") {
                    continue;
                }
            }

            std::size_t endIndex = index;
            std::vector<std::string> chain;
            chain.reserve(3);
            chain.push_back(normalizeIdentifier(text, tokens[index]));
            while (endIndex + 2 < tokens.size() && tokenText(text, tokens[endIndex + 1]) == "." &&
                   isIdentifier(tokens[endIndex + 2])) {
                endIndex += 2;
                chain.push_back(normalizeIdentifier(text, tokens[endIndex]));
            }
            if (endIndex + 1 < tokens.size() && tokenText(text, tokens[endIndex + 1]) == "(") {
                index = endIndex;
                continue;
            }

            const auto constant = findConstant(chain, countStatistics, lookupKey);
            if (!constant) {
                index = endIndex;
                continue;
            }

            output.append(text, copiedUntil, tokens[index].begin - copiedUntil);
            appendConstant(output, *constant);
            copiedUntil = tokens[endIndex].end;
            if (countStatistics) {
                ++replacements_;
            }
            index = endIndex;
        }
        output.append(text, copiedUntil, text.size() - copiedUntil);
        return output;
    }

    std::vector<Constant> constants_;
    std::vector<Span> declarations_;
    std::vector<Token> sourceTokens_;
    std::unordered_map<std::string, LookupEntry> lookup_;
    std::vector<std::string> resolved_;
    std::vector<std::optional<std::string>> rendered_;
    std::vector<int> states_;
    std::size_t replacements_ = 0;
    std::size_t ambiguousReferences_ = 0;
};

} // namespace

ReplacementResult replaceConstants(const std::string& source, const std::string& selection,
                                   const std::vector<PackageSource>& packageSources) {
    Replacer replacer(source, packageSources);
    return replacer.run(source, selection);
}

struct PackageReferenceHash {
    std::size_t operator()(const PackageReference& reference) const {
        const std::size_t ownerHash = std::hash<std::string>{}(reference.owner);
        const std::size_t nameHash = std::hash<std::string>{}(reference.name);
        return ownerHash ^ (nameHash + (ownerHash << 6) + (ownerHash >> 2));
    }
};

std::vector<PackageReference> referencedPackages(const std::string& text) {
    const std::vector<Token> tokens = tokenize(text);
    std::vector<PackageReference> packages;
    std::unordered_set<PackageReference, PackageReferenceHash> seen;

    auto identifierName = [&](const Token& token) {
        if (token.kind != TokenKind::QuotedIdentifier) {
            return upper(tokenText(text, token));
        }
        std::string name;
        for (std::size_t position = token.begin + 1; position + 1 < token.end; ++position) {
            name += text[position];
            if (text[position] == '"' && position + 2 < token.end && text[position + 1] == '"') {
                ++position;
            }
        }
        return name;
    };

    for (std::size_t index = 0; index < tokens.size(); ++index) {
        if (!isIdentifier(tokens[index])) {
            continue;
        }
        std::size_t endIndex = index;
        std::vector<Token> chain{tokens[index]};
        chain.reserve(3);
        while (endIndex + 2 < tokens.size() && tokenText(text, tokens[endIndex + 1]) == "." &&
               isIdentifier(tokens[endIndex + 2])) {
            endIndex += 2;
            chain.push_back(tokens[endIndex]);
        }
        if (chain.size() >= 2) {
            const bool functionCall = endIndex + 1 < tokens.size() &&
                                      tokenText(text, tokens[endIndex + 1]) == "(";
            if (chain.size() <= 3 && !functionCall) {
                const Token& package = chain[chain.size() - 2];
                PackageReference reference{"", identifierName(package)};
                if (chain.size() == 3) {
                    reference.owner = identifierName(chain[0]);
                }
                if (seen.insert(reference).second) {
                    packages.push_back(std::move(reference));
                }
            }
            index = endIndex;
        }
    }
    return packages;
}
