#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct ReplacementResult {
    std::string text;
    std::size_t constantsFound = 0;
    std::size_t replacements = 0;
    std::size_t ambiguousReferences = 0;
};

struct PackageReference {
    std::string owner;
    std::string name;
};

inline bool operator==(const PackageReference& left, const PackageReference& right) {
    return left.owner == right.owner && left.name == right.name;
}

struct PackageSource {
    std::string owner;
    std::string name;
    std::string text;
    std::vector<PackageReference> aliases;
    std::string bodyText;
};

ReplacementResult replaceConstants(
    const std::string& source,
    const std::string& selection,
    const std::vector<PackageSource>& packageSources = {});

std::vector<PackageReference> referencedPackages(const std::string& text);
