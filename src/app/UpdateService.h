#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace fastrecord::updates {

struct CheckResult {
    bool success{false};
    bool updateAvailable{false};
    std::wstring version;
    std::wstring downloadUrl;
    std::wstring sha256;
    std::uint64_t sizeBytes{0};
    std::wstring error;
};

bool isNewerVersion(std::wstring_view current, std::wstring_view candidate);
CheckResult parseLatestReleaseJson(std::wstring_view json, std::wstring_view currentVersion);
CheckResult checkForUpdate(std::wstring_view currentVersion);
bool downloadAndVerifyInstaller(
    const CheckResult& update,
    std::filesystem::path& installerPath,
    std::wstring& error);

} // namespace fastrecord::updates
