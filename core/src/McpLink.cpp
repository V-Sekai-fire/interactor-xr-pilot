// SPDX-License-Identifier: Apache-2.0 OR MIT

#include "xrpilot/McpLink.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <bcrypt.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/random.h>
#endif
#endif

namespace xrpilot
{

namespace
{

bool validToken(const std::string& token)
{
    if (token.size() != 64)
    {
        return false;
    }
    for (char c : token)
    {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        {
            return false;
        }
    }
    return true;
}

bool randomBytes(unsigned char* out, size_t n)
{
#if defined(_WIN32)
    return BCryptGenRandom(nullptr, out, ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    return getentropy(out, n) == 0;
#endif
}

#if defined(_WIN32)
std::wstring wide(const std::string& s)
{
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

// A protected DACL with one entry, full access for this user: no inherited group or admin entries.
std::wstring userOnlySddl()
{
    HANDLE process = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &process))
    {
        return {};
    }
    DWORD size = 0;
    GetTokenInformation(process, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> user(size);
    LPWSTR sid = nullptr;
    const bool ok = GetTokenInformation(process, TokenUser, user.data(), size, &size) &&
                    ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid);
    CloseHandle(process);
    if (!ok)
    {
        return {};
    }
    std::wstring sddl = L"D:P(A;;FA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    return sddl;
}

bool writeUserOnly(const std::string& path, const std::string& text)
{
    const std::wstring file = wide(path);
    DeleteFileW(file.c_str());
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, FALSE};
    const std::wstring sddl = userOnlySddl();
    if (sddl.empty() || !ConvertStringSecurityDescriptorToSecurityDescriptorW(
                            sddl.c_str(), SDDL_REVISION_1, &attributes.lpSecurityDescriptor, nullptr))
    {
        return false;
    }
    HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    LocalFree(attributes.lpSecurityDescriptor);
    if (h == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    DWORD written = 0;
    const bool ok = WriteFile(h, text.data(), DWORD(text.size()), &written, nullptr) && written == text.size();
    CloseHandle(h);
    return ok;
}
#else
bool writeUserOnly(const std::string& path, const std::string& text)
{
    unlink(path.c_str());
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd < 0)
    {
        return false;
    }
    const bool ok = fchmod(fd, S_IRUSR | S_IWUSR) == 0 && write(fd, text.data(), text.size()) == ssize_t(text.size());
    return close(fd) == 0 && ok;
}
#endif

std::filesystem::path fsPath(const std::string& path)
{
#if defined(_WIN32)
    return std::filesystem::path(wide(path));
#else
    return std::filesystem::path(path);
#endif
}

} // namespace

bool loadOrCreateMcpToken(const std::string& path, std::string& token, std::string* error)
{
    std::ifstream in(fsPath(path), std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    std::string kept = text.str();
    while (!kept.empty() && (kept.back() == '\n' || kept.back() == '\r' || kept.back() == ' '))
    {
        kept.pop_back();
    }
    if (validToken(kept))
    {
        token = kept;
        return true;
    }
    in.close();
    unsigned char bytes[32];
    if (!randomBytes(bytes, sizeof(bytes)))
    {
        if (error != nullptr)
        {
            *error = "the system has no random source";
        }
        return false;
    }
    std::string fresh;
    for (unsigned char b : bytes)
    {
        char hex[3];
        std::snprintf(hex, sizeof(hex), "%02x", b);
        fresh += hex;
    }
    std::error_code ignored;
    std::filesystem::create_directories(fsPath(path).parent_path(), ignored);
    if (!writeUserOnly(path, fresh + "\n"))
    {
        if (error != nullptr)
        {
            *error = "cannot write " + path;
        }
        return false;
    }
    token = fresh;
    return true;
}

std::string mcpClientConfig(int port, const std::string& token)
{
    return "{\"type\":\"http\",\"url\":\"http://127.0.0.1:" + std::to_string(port) +
           "/mcp\",\"headers\":{\"Authorization\":\"Bearer " + token + "\"}}";
}

} // namespace xrpilot
