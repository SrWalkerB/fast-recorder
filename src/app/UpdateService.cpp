#include "app/UpdateService.h"

#include <Windows.h>
#include <bcrypt.h>
#include <shlobj.h>
#include <winhttp.h>
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace fastrecord::updates {

namespace {

constexpr wchar_t kRepository[] = L"srviniaviz/fast-recorder";
constexpr std::uint64_t kMaximumInstallerSize = 200ull * 1024ull * 1024ull;
constexpr std::size_t kMaximumReleaseJsonSize = 1024ull * 1024ull;

class InternetHandle final {
public:
    explicit InternetHandle(HINTERNET handle = nullptr) : m_handle(handle) {}
    ~InternetHandle() {
        if (m_handle != nullptr) {
            WinHttpCloseHandle(m_handle);
        }
    }

    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    operator HINTERNET() const noexcept { return m_handle; }

private:
    HINTERNET m_handle;
};

class FileHandle final {
public:
    explicit FileHandle(HANDLE handle = INVALID_HANDLE_VALUE) : m_handle(handle) {}
    ~FileHandle() {
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
        }
    }

    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    operator HANDLE() const noexcept { return m_handle; }
    bool close() noexcept {
        if (m_handle == INVALID_HANDLE_VALUE) {
            return true;
        }
        const bool succeeded = CloseHandle(m_handle) != FALSE;
        if (succeeded) {
            m_handle = INVALID_HANDLE_VALUE;
        }
        return succeeded;
    }

private:
    HANDLE m_handle;
};

class ComApartment final {
public:
    ComApartment() : m_result(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(m_result)) {
            CoUninitialize();
        }
    }
    bool usable() const noexcept {
        return SUCCEEDED(m_result) || m_result == RPC_E_CHANGED_MODE;
    }

private:
    HRESULT m_result;
};

std::wstring windowsError(const wchar_t* operation) {
    return std::wstring(operation) + L" (erro " + std::to_wstring(GetLastError()) + L").";
}

std::wstring trimVersionPrefix(std::wstring_view version) {
    if (!version.empty() && (version.front() == L'v' || version.front() == L'V')) {
        version.remove_prefix(1);
    }
    return std::wstring(version);
}

bool parseVersion(std::wstring_view input, std::vector<std::uint64_t>& parts) {
    parts.clear();
    if (!input.empty() && (input.front() == L'v' || input.front() == L'V')) {
        input.remove_prefix(1);
    }
    if (input.empty()) {
        return false;
    }

    std::uint64_t value = 0;
    bool hasDigit = false;
    for (const wchar_t character : input) {
        if (character == L'.') {
            if (!hasDigit) {
                return false;
            }
            parts.push_back(value);
            value = 0;
            hasDigit = false;
            continue;
        }
        if (character < L'0' || character > L'9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - L'0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
        hasDigit = true;
    }
    if (!hasDigit) {
        return false;
    }
    parts.push_back(value);
    return true;
}

bool isSha256(std::wstring_view value) {
    if (value.size() != 71 || value.substr(0, 7) != L"sha256:") {
        return false;
    }
    return std::all_of(value.begin() + 7, value.end(), [](wchar_t character) {
        return (character >= L'0' && character <= L'9') ||
            (character >= L'a' && character <= L'f') ||
            (character >= L'A' && character <= L'F');
    });
}

std::wstring lowerAscii(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character) {
        return character >= L'A' && character <= L'Z'
            ? static_cast<wchar_t>(character - L'A' + L'a')
            : character;
    });
    return value;
}

struct UrlParts {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port{0};
    bool secure{false};
};

bool crackUrl(const std::wstring& url, UrlParts& parts) {
    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) {
        return false;
    }

    parts.host.assign(components.lpszHostName, components.dwHostNameLength);
    parts.path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    if (components.dwExtraInfoLength != 0) {
        parts.path.append(components.lpszExtraInfo, components.dwExtraInfoLength);
    }
    parts.port = components.nPort;
    parts.secure = components.nScheme == INTERNET_SCHEME_HTTPS;
    return true;
}

bool getStatusCode(HINTERNET request, DWORD& statusCode) {
    DWORD size = sizeof(statusCode);
    return WinHttpQueryHeaders(
        request,
        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX,
        &statusCode,
        &size,
        WINHTTP_NO_HEADER_INDEX) != FALSE;
}

bool fetchReleaseJson(std::string& body, std::wstring& error) {
    InternetHandle session(WinHttpOpen(
        L"FastRecordUpdater/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (session == nullptr) {
        error = windowsError(L"Não foi possível iniciar a conexão segura");
        return false;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 15000);

    InternetHandle connection(WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (connection == nullptr) {
        error = windowsError(L"Não foi possível conectar ao GitHub");
        return false;
    }

    InternetHandle request(WinHttpOpenRequest(
        connection,
        L"GET",
        L"/repos/srviniaviz/fast-recorder/releases/latest",
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (request == nullptr) {
        error = windowsError(L"Não foi possível preparar a verificação de versão");
        return false;
    }

    constexpr wchar_t headers[] =
        L"Accept: application/vnd.github+json\r\n"
        L"X-GitHub-Api-Version: 2022-11-28\r\n";
    if (!WinHttpSendRequest(
            request,
            headers,
            static_cast<DWORD>(-1),
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        error = windowsError(L"Falha ao consultar a release mais recente");
        return false;
    }

    DWORD statusCode = 0;
    if (!getStatusCode(request, statusCode)) {
        error = windowsError(L"Não foi possível ler a resposta do GitHub");
        return false;
    }
    if (statusCode != 200) {
        error = L"GitHub respondeu com HTTP " + std::to_wstring(statusCode) + L".";
        return false;
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            error = windowsError(L"Falha ao ler os dados da release");
            return false;
        }
        if (available == 0) {
            break;
        }
        if (body.size() + available > kMaximumReleaseJsonSize) {
            error = L"A resposta do GitHub excedeu o tamanho esperado.";
            return false;
        }

        std::vector<char> buffer(available);
        DWORD bytesRead = 0;
        if (!WinHttpReadData(request, buffer.data(), available, &bytesRead)) {
            error = windowsError(L"Falha ao baixar os dados da release");
            return false;
        }
        body.append(buffer.data(), bytesRead);
    }
    return true;
}

bool utf8ToWide(const std::string& input, std::wstring& output) {
    if (input.empty()) {
        output.clear();
        return true;
    }
    const int characterCount = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        input.data(),
        static_cast<int>(input.size()),
        nullptr,
        0);
    if (characterCount <= 0) {
        return false;
    }
    output.resize(static_cast<std::size_t>(characterCount));
    return MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        input.data(),
        static_cast<int>(input.size()),
        output.data(),
        characterCount) == characterCount;
}

bool currentFinalUrlIsTrusted(HINTERNET request) {
    DWORD size = 0;
    if (WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &size) ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || size < sizeof(wchar_t)) {
        return false;
    }
    std::wstring finalUrl(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryOption(request, WINHTTP_OPTION_URL, finalUrl.data(), &size)) {
        return false;
    }
    while (!finalUrl.empty() && finalUrl.back() == L'\0') {
        finalUrl.pop_back();
    }

    UrlParts parts;
    if (!crackUrl(finalUrl, parts) || !parts.secure) {
        return false;
    }
    const std::wstring host = lowerAscii(std::move(parts.host));
    return host == L"github.com" ||
        host == L"release-assets.githubusercontent.com" ||
        host == L"objects.githubusercontent.com";
}

bool writeAll(HANDLE file, const void* data, DWORD size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    DWORD remaining = size;
    while (remaining != 0) {
        DWORD written = 0;
        if (!WriteFile(file, bytes, remaining, &written, nullptr) || written == 0) {
            return false;
        }
        bytes += written;
        remaining -= written;
    }
    return true;
}

bool getLocalUpdateDirectory(std::filesystem::path& directory, std::wstring& error) {
    PWSTR localAppData = nullptr;
    const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &localAppData);
    if (FAILED(result) || localAppData == nullptr) {
        if (localAppData != nullptr) {
            CoTaskMemFree(localAppData);
        }
        error = L"Não foi possível encontrar a pasta local do usuário.";
        return false;
    }
    directory = std::filesystem::path(localAppData) / L"Fast Record" / L"Updates";
    CoTaskMemFree(localAppData);

    std::error_code filesystemError;
    std::filesystem::create_directories(directory, filesystemError);
    if (filesystemError) {
        error = L"Não foi possível criar a pasta de atualização (erro " +
            std::to_wstring(filesystemError.value()) + L").";
        return false;
    }
    return true;
}

bool hashToHex(BCRYPT_ALG_HANDLE algorithm, HANDLE file, std::wstring& hash) {
    DWORD objectSize = 0;
    DWORD returned = 0;
    DWORD hashSize = 0;
    if (BCryptGetProperty(
            algorithm,
            BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize),
            sizeof(objectSize),
            &returned,
            0) < 0 ||
        BCryptGetProperty(
            algorithm,
            BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&hashSize),
            sizeof(hashSize),
            &returned,
            0) < 0) {
        return false;
    }

    std::vector<UCHAR> object(objectSize);
    std::vector<UCHAR> digest(hashSize);
    BCRYPT_HASH_HANDLE hashHandle = nullptr;
    if (BCryptCreateHash(algorithm, &hashHandle, object.data(), objectSize, nullptr, 0, 0) < 0) {
        return false;
    }

    bool success = true;
    std::array<UCHAR, 64 * 1024> buffer{};
    LARGE_INTEGER zero{};
    success = SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) != FALSE;
    while (success) {
        DWORD bytesRead = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr)) {
            success = false;
            break;
        }
        if (bytesRead == 0) {
            break;
        }
        success = BCryptHashData(hashHandle, buffer.data(), bytesRead, 0) >= 0;
    }
    if (success) {
        success = BCryptFinishHash(hashHandle, digest.data(), hashSize, 0) >= 0;
    }
    BCryptDestroyHash(hashHandle);
    if (!success) {
        return false;
    }

    constexpr wchar_t digits[] = L"0123456789abcdef";
    hash.clear();
    hash.reserve(digest.size() * 2);
    for (const UCHAR value : digest) {
        hash.push_back(digits[value >> 4]);
        hash.push_back(digits[value & 0x0F]);
    }
    return true;
}

} // namespace

bool isNewerVersion(std::wstring_view current, std::wstring_view candidate) {
    std::vector<std::uint64_t> currentParts;
    std::vector<std::uint64_t> candidateParts;
    if (!parseVersion(current, currentParts) || !parseVersion(candidate, candidateParts)) {
        return false;
    }

    const std::size_t count = std::max(currentParts.size(), candidateParts.size());
    for (std::size_t index = 0; index < count; ++index) {
        const std::uint64_t currentPart = index < currentParts.size() ? currentParts[index] : 0;
        const std::uint64_t candidatePart = index < candidateParts.size() ? candidateParts[index] : 0;
        if (candidatePart != currentPart) {
            return candidatePart > currentPart;
        }
    }
    return false;
}

CheckResult parseLatestReleaseJson(std::wstring_view json, std::wstring_view currentVersion) {
    CheckResult result;
    const ComApartment apartment;
    if (!apartment.usable()) {
        result.error = L"Não foi possível inicializar o componente de leitura de versões.";
        return result;
    }
    try {
        const std::wstring jsonText(json);
        const auto release = winrt::Windows::Data::Json::JsonObject::Parse(winrt::hstring(jsonText));
        const std::wstring tag = release.GetNamedString(L"tag_name", L"").c_str();
        result.version = trimVersionPrefix(tag);
        if (result.version.empty()) {
            result.error = L"A release do GitHub não informou uma versão válida.";
            return result;
        }

        std::vector<std::uint64_t> versionParts;
        if (!parseVersion(result.version, versionParts)) {
            result.error = L"A versão publicada no GitHub tem um formato inválido.";
            return result;
        }
        result.success = true;
        if (!isNewerVersion(currentVersion, result.version)) {
            return result;
        }

        if (!release.HasKey(L"assets")) {
            result.success = false;
            result.error = L"A release mais recente não contém os arquivos de instalação.";
            return result;
        }

        const std::wstring expectedAsset = L"FastRecord-" + result.version + L"-x64-Setup.exe";
        const auto assets = release.GetNamedArray(L"assets");
        for (const auto& value : assets) {
            const auto asset = value.GetObject();
            const std::wstring assetName = asset.GetNamedString(L"name", L"").c_str();
            if (assetName != expectedAsset) {
                continue;
            }

            const std::wstring expectedUrl =
                L"https://github.com/" + std::wstring(kRepository) + L"/releases/download/" +
                tag + L"/" + expectedAsset;
            result.downloadUrl = asset.GetNamedString(L"browser_download_url", L"").c_str();
            const std::wstring digest = asset.GetNamedString(L"digest", L"").c_str();
            if (result.downloadUrl != expectedUrl) {
                result.success = false;
                result.error = L"O endereço do instalador na release não passou na validação.";
                return result;
            }
            if (!isSha256(digest)) {
                result.success = false;
                result.error = L"A release não publicou um SHA-256 válido para o instalador.";
                return result;
            }
            result.sha256 = lowerAscii(digest.substr(7));

            const double assetSize = asset.GetNamedNumber(L"size", 0.0);
            if (assetSize <= 0.0 || std::floor(assetSize) != assetSize ||
                assetSize > static_cast<double>(kMaximumInstallerSize)) {
                result.success = false;
                result.error = L"O tamanho informado para o instalador é inválido.";
                return result;
            }
            result.sizeBytes = static_cast<std::uint64_t>(assetSize);
            result.updateAvailable = true;
            return result;
        }

        result.success = false;
        result.error = L"A release mais recente não contém o instalador do Fast Record.";
        return result;
    } catch (const winrt::hresult_error&) {
        result.success = false;
        result.error = L"Não foi possível interpretar a resposta de versões do GitHub.";
        return result;
    } catch (...) {
        result.success = false;
        result.error = L"Ocorreu um erro ao validar a versão mais recente.";
        return result;
    }
}

CheckResult checkForUpdate(std::wstring_view currentVersion) {
    std::string response;
    std::wstring error;
    if (!fetchReleaseJson(response, error)) {
        CheckResult result;
        result.error = std::move(error);
        return result;
    }

    std::wstring json;
    if (!utf8ToWide(response, json)) {
        CheckResult result;
        result.error = L"A resposta do GitHub não está em UTF-8 válido.";
        return result;
    }
    return parseLatestReleaseJson(json, currentVersion);
}

bool downloadAndVerifyInstaller(
    const CheckResult& update,
    std::filesystem::path& installerPath,
    std::wstring& error) {
    if (!update.success || !update.updateAvailable || update.downloadUrl.empty() ||
        update.sha256.size() != 64 || update.sizeBytes == 0 ||
        update.sizeBytes > kMaximumInstallerSize) {
        error = L"Os dados de atualização não são válidos.";
        return false;
    }

    UrlParts urlParts;
    if (!crackUrl(update.downloadUrl, urlParts) || !urlParts.secure ||
        lowerAscii(urlParts.host) != L"github.com") {
        error = L"O endereço do instalador não é uma URL HTTPS válida do GitHub.";
        return false;
    }

    InternetHandle session(WinHttpOpen(
        L"FastRecordUpdater/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (session == nullptr) {
        error = windowsError(L"Não foi possível iniciar o download seguro");
        return false;
    }
    WinHttpSetTimeouts(session, 5000, 5000, 15000, 30000);

    InternetHandle connection(WinHttpConnect(session, urlParts.host.c_str(), urlParts.port, 0));
    if (connection == nullptr) {
        error = windowsError(L"Não foi possível conectar ao servidor do instalador");
        return false;
    }
    InternetHandle request(WinHttpOpenRequest(
        connection,
        L"GET",
        urlParts.path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE));
    if (request == nullptr) {
        error = windowsError(L"Não foi possível preparar o download do instalador");
        return false;
    }
    constexpr wchar_t headers[] = L"Accept: application/octet-stream\r\n";
    if (!WinHttpSendRequest(
            request,
            headers,
            static_cast<DWORD>(-1),
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        error = windowsError(L"Falha ao baixar o instalador");
        return false;
    }

    DWORD statusCode = 0;
    if (!getStatusCode(request, statusCode) || statusCode != 200) {
        error = statusCode == 0
            ? windowsError(L"Não foi possível validar a resposta do instalador")
            : L"O servidor do instalador respondeu com HTTP " + std::to_wstring(statusCode) + L".";
        return false;
    }
    if (!currentFinalUrlIsTrusted(request)) {
        error = L"O download foi redirecionado para um endereço não confiável.";
        return false;
    }

    std::filesystem::path updateDirectory;
    if (!getLocalUpdateDirectory(updateDirectory, error)) {
        return false;
    }
    const std::wstring fileName = L"FastRecord-" + update.version + L"-x64-Setup.exe";
    installerPath = updateDirectory / fileName;
    std::filesystem::path partialPath = installerPath;
    partialPath += L".download";
    FileHandle file(CreateFileW(
        partialPath.c_str(),
        GENERIC_WRITE | GENERIC_READ,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY,
        nullptr));
    if (file == INVALID_HANDLE_VALUE) {
        error = windowsError(L"Não foi possível criar o arquivo temporário do instalador");
        return false;
    }

    bool downloadSucceeded = true;
    std::uint64_t totalBytes = 0;
    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            error = windowsError(L"Falha ao receber o instalador");
            downloadSucceeded = false;
            break;
        }
        if (available == 0) {
            break;
        }
        std::array<std::uint8_t, 64 * 1024> buffer{};
        const DWORD requested = std::min<DWORD>(available, static_cast<DWORD>(buffer.size()));
        DWORD bytesRead = 0;
        if (!WinHttpReadData(request, buffer.data(), requested, &bytesRead) || bytesRead == 0) {
            error = windowsError(L"Falha ao receber os dados do instalador");
            downloadSucceeded = false;
            break;
        }
        totalBytes += bytesRead;
        if (totalBytes > kMaximumInstallerSize || !writeAll(file, buffer.data(), bytesRead)) {
            error = totalBytes > kMaximumInstallerSize
                ? L"O instalador baixado ultrapassou o tamanho máximo permitido."
                : windowsError(L"Não foi possível gravar o instalador");
            downloadSucceeded = false;
            break;
        }
    }

    if (downloadSucceeded && totalBytes != update.sizeBytes) {
        error = L"O tamanho do instalador baixado não corresponde ao informado pela release.";
        downloadSucceeded = false;
    }
    if (downloadSucceeded && !FlushFileBuffers(file)) {
        error = windowsError(L"Não foi possível finalizar a gravação do instalador");
        downloadSucceeded = false;
    }
    if (downloadSucceeded) {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            error = L"O Windows não conseguiu preparar a validação SHA-256.";
            downloadSucceeded = false;
        } else {
            std::wstring actualHash;
            downloadSucceeded = hashToHex(algorithm, file, actualHash);
            BCryptCloseAlgorithmProvider(algorithm, 0);
            if (!downloadSucceeded) {
                error = L"Não foi possível validar o SHA-256 do instalador.";
            } else if (actualHash != lowerAscii(update.sha256)) {
                error = L"O SHA-256 do instalador não corresponde ao publicado pelo GitHub.";
                downloadSucceeded = false;
            }
        }
    }

    if (!file.close()) {
        error = windowsError(L"Não foi possível fechar o instalador baixado");
        DeleteFileW(partialPath.c_str());
        return false;
    }
    if (!downloadSucceeded) {
        DeleteFileW(partialPath.c_str());
        return false;
    }

    if (!MoveFileExW(
            partialPath.c_str(),
            installerPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = windowsError(L"Não foi possível concluir a instalação baixada");
        DeleteFileW(partialPath.c_str());
        return false;
    }
    return true;
}

} // namespace fastrecord::updates
