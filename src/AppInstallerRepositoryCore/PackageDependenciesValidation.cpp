// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.
#pragma once
#include "pch.h"
#include <AppInstallerVersions.h>
#include <winget/Manifest.h>
#include <winget/ManifestValidation.h>
#include <SQLiteWrapper.h>
#include <PackageDependenciesValidation.h>
#include <Microsoft/Schema/1_4/DependenciesTable.h>
#include "Microsoft/Schema/1_0/ManifestTable.h"
#include <winget/DependenciesGraph.h>

namespace AppInstaller::Repository
{
    using namespace Microsoft::Schema::V1_4;
    using namespace Microsoft::Schema::V1_0;

    namespace
    {
        struct DependentManifestInfo
        {
            Utility::NormalizedString Id;
            Utility::NormalizedString Version;
        };

        Manifest::DependencyList GetDependencies(
            const Manifest::Manifest& manifest, AppInstaller::Manifest::DependencyType dependencyType)
        {
            Manifest::DependencyList depList;
            std::vector<AppInstaller::Manifest::Dependency>  dependencies;

            for (const auto& installer : manifest.Installers)
            {
                installer.Dependencies.ApplyToType(dependencyType, [&](AppInstaller::Manifest::Dependency dependency)
                {
                    depList.Add(dependency);
                });
            }

            return depList;
        }

        std::optional<std::pair<SQLite::rowid_t, Utility::Version>> GetPackageLatestVersion(
            SQLiteIndex* index, Manifest::string_t packageId, std::set<Utility::Version> exclusions = {})
        {
            SearchRequest request;
            request.Filters.emplace_back(PackageMatchField::Id, MatchType::CaseInsensitive, packageId);

            auto results = index->Search(request);

            if (results.Matches.empty())
            {
                return {};
            }

            auto packageRowId = results.Matches[0].first;
            auto vac = index->GetVersionKeysById(packageRowId);

            if (vac.empty())
            {
                return {};
            }

            Utility::VersionAndChannel maxVersion(Utility::Version::CreateUnknown(), Utility::Channel(""));

            for (auto& v : vac)
            {
                auto currentVersion = v.GetVersion();
                if (exclusions.find(currentVersion) != exclusions.end())
                {
                    continue;
                }

                if (currentVersion > maxVersion.GetVersion())
                {
                    maxVersion = v;
                }
            }

            if (maxVersion.GetVersion().IsUnknown())
            {
                return {};
            }

            auto manifestRowId = index->GetManifestIdByKey(
                packageRowId, maxVersion.GetVersion().ToString(), maxVersion.GetChannel().ToString());

            return std::make_pair(manifestRowId.value(), maxVersion.GetVersion());
        }
    
        void ThrowOnManifestValidationFailed(
            std::vector<std::pair<DependentManifestInfo, Utility::Version>> failedManifests, std::string error)
        {
            auto itrStart = failedManifests.begin();
            std::string dependentPackages{ itrStart->first.Id + "." + itrStart->first.Version };

            std::for_each(
                itrStart + 1,
                failedManifests.end(),
                [&](std::pair<DependentManifestInfo, Utility::Version> current)
                {
                    dependentPackages.append(", " + current.first.Id + "." + current.first.Version);
                });

            error.append("\n" + dependentPackages);
            THROW_EXCEPTION(
                Manifest::ManifestException({ Manifest::ValidationError(error) },
                    APPINSTALLER_CLI_ERROR_DEPENDENCIES_VALIDATION_FAILED));
        }
    };

    bool PackageDependenciesValidation::ValidateManifestDependencies(SQLiteIndex* index, const Manifest::Manifest manifest)
    {
        using namespace Manifest;

        Dependency rootId(DependencyType::Package, manifest.Id, manifest.Version);
        std::vector<ValidationError> dependenciesError;
        bool foundErrors = false;

        DependencyGraph graph(rootId, [&](const Dependency& node) {

            DependencyList depList;
            if (node.Id == rootId.Id)
            {
                return GetDependencies(manifest, DependencyType::Package);
            }

            auto packageLatest = GetPackageLatestVersion(index, node.Id);
            if (!packageLatest.has_value())
            {
                std::string error = ManifestError::MissingManifestDependenciesNode;
                error.append(" ").append(node.Id);
                dependenciesError.emplace_back(ValidationError(error));
                foundErrors = true;
                return depList;
            }

            if (node.MinVersion > packageLatest.value().second)
            {
                std::string error = ManifestError::NoSuitableMinVersion;
                error.append(" ").append(node.Id);
                dependenciesError.emplace_back(ValidationError(error));
                foundErrors = true;
                return depList;
            }

            auto packageLatestDependencies = index->GetDependenciesByManifestRowId(packageLatest.value().first);
            std::for_each(
                packageLatestDependencies.begin(),
                packageLatestDependencies.end(),
                [&](std::pair<SQLite::rowid_t, Utility::NormalizedString> row)
                {
                    auto manifestRowId = index->GetManifestIdByKey(row.first, "", "");
                    auto packageId = index->GetPropertyByManifestId(manifestRowId.value(), PackageVersionProperty::Id);
                    Dependency dep(DependencyType::Package, packageId.value(), row.second);
                    depList.Add(dep);
                });

            return depList;
            });

        graph.BuildGraph();

        if (foundErrors)
        {
            THROW_EXCEPTION(ManifestException(std::move(dependenciesError), APPINSTALLER_CLI_ERROR_DEPENDENCIES_VALIDATION_FAILED));
        }

        if (graph.HasLoop())
        {
            std::string error = ManifestError::FoundLoop;
            dependenciesError.emplace_back(error);
            THROW_EXCEPTION(ManifestException(std::move(dependenciesError), APPINSTALLER_CLI_ERROR_DEPENDENCIES_VALIDATION_FAILED));
        }

        return true;
    }

    bool PackageDependenciesValidation::VerifyDependenciesStructureForManifestDelete(SQLiteIndex* index, const Manifest::Manifest manifest)
    {
        auto dependentsSet = index->GetDependentsById(manifest.Id);

        if (!dependentsSet.size())
        {
            // all good this manifest is not a dependency of any manifest.
            return true;
        }

        std::vector<std::pair<DependentManifestInfo, Utility::Version>> dependentManifestInfoToVersionPair;
        std::for_each(
            dependentsSet.begin(),
            dependentsSet.end(),
            [&](std::pair<SQLite::rowid_t, Utility::Version> current)
            {
                DependentManifestInfo dependentManifestInfo;
                dependentManifestInfo.Id = index->GetPropertyByManifestId(current.first, PackageVersionProperty::Id).value();
                dependentManifestInfo.Version = index->GetPropertyByManifestId(current.first, PackageVersionProperty::Version).value();

                dependentManifestInfoToVersionPair.emplace_back(std::make_pair(dependentManifestInfo, current.second));
            });

        auto packageLatest = GetPackageLatestVersion(index, manifest.Id);

        if (!packageLatest.has_value())
        {
            // this is a fatal error, a manifest should exists in the very least(including the current manifest being deleted),
            // since this is a delete operation. 
            THROW_HR(APPINSTALLER_CLI_ERROR_MISSING_PACKAGE);
        }

        if (Utility::Version(manifest.Version) < packageLatest.value().second)
        {
            // all good, since it's min version the criteria is still satisfied.
            return true;
        }

        auto nextLatestAfterDelete = GetPackageLatestVersion(index, manifest.Id, { packageLatest.value().second });

        if (!nextLatestAfterDelete.has_value())
        {
            ThrowOnManifestValidationFailed(
                dependentManifestInfoToVersionPair, Manifest::ManifestError::SingleManifestPackageHasDependencies);
        }

        std::vector<std::pair<DependentManifestInfo, Utility::Version>> breakingManifests;

        // Gets breaking manifests.
        std::copy_if(
            dependentManifestInfoToVersionPair.begin(),
            dependentManifestInfoToVersionPair.end(),
            std::back_inserter(breakingManifests),
            [&](std::pair<DependentManifestInfo, Utility::Version> current)
            {
                return current.second > nextLatestAfterDelete.value().second;
            }
        );

        if (breakingManifests.size())
        {
            ThrowOnManifestValidationFailed(
                breakingManifests, Manifest::ManifestError::MultiManifestPackageHasDependencies);
        }

        return true;
    }
}