#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <Windows.h>

#include <bcrypt.h>
#include <wincrypt.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace utils::jwt
{
  inline std::string get_env_string(const char* name)
  {
    char buffer[4096]{};
    const DWORD length = GetEnvironmentVariableA(name, buffer, static_cast<DWORD>(sizeof(buffer)));
    if (length == 0 || length >= sizeof(buffer))
    {
      return {};
    }
    return std::string(buffer, buffer + length);
  }

  inline std::string secret()
  {
    auto value = get_env_string("JWT_SECRET");
    if (!value.empty())
    {
      return value;
    }
    return "dev-secret-change-me";
  }

  inline std::optional<std::string> base64_encode(const std::vector<unsigned char>& bytes)
  {
    if (bytes.empty())
    {
      return std::string();
    }

    DWORD needed = 0;
    if (!CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &needed))
    {
      return std::nullopt;
    }

    std::string out;
    out.resize(static_cast<size_t>(needed));
    if (!CryptBinaryToStringA(bytes.data(), static_cast<DWORD>(bytes.size()), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out.data(), &needed))
    {
      return std::nullopt;
    }

    if (!out.empty() && out.back() == '\0')
    {
      out.pop_back();
    }
    return out;
  }

  inline std::optional<std::string> base64url_encode(const std::vector<unsigned char>& bytes)
  {
    const auto b64 = base64_encode(bytes);
    if (!b64)
    {
      return std::nullopt;
    }

    std::string out = *b64;
    out.erase(std::remove(out.begin(), out.end(), '='), out.end());
    std::replace(out.begin(), out.end(), '+', '-');
    std::replace(out.begin(), out.end(), '/', '_');
    return out;
  }

  inline std::optional<std::vector<unsigned char>> hmac_sha256(std::string_view key, std::string_view message)
  {
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (status < 0)
    {
      return std::nullopt;
    }

    DWORD object_len = 0;
    DWORD bytes_done = 0;
    status = BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_len), sizeof(object_len), &bytes_done, 0);
    if (status < 0 || object_len == 0)
    {
      BCryptCloseAlgorithmProvider(alg, 0);
      return std::nullopt;
    }

    DWORD hash_len = 0;
    status = BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len), &bytes_done, 0);
    if (status < 0 || hash_len == 0)
    {
      BCryptCloseAlgorithmProvider(alg, 0);
      return std::nullopt;
    }

    std::vector<unsigned char> hash_object(object_len);
    BCRYPT_HASH_HANDLE hash = nullptr;
    status = BCryptCreateHash(
      alg,
      &hash,
      hash_object.data(),
      static_cast<ULONG>(hash_object.size()),
      reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
      static_cast<ULONG>(key.size()),
      0);
    if (status < 0)
    {
      BCryptCloseAlgorithmProvider(alg, 0);
      return std::nullopt;
    }

    status = BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(message.data())), static_cast<ULONG>(message.size()), 0);
    if (status < 0)
    {
      BCryptDestroyHash(hash);
      BCryptCloseAlgorithmProvider(alg, 0);
      return std::nullopt;
    }

    std::vector<unsigned char> out(hash_len);
    status = BCryptFinishHash(hash, out.data(), static_cast<ULONG>(out.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);

    if (status < 0)
    {
      return std::nullopt;
    }
    return out;
  }

  inline std::optional<std::string> make_hs256(std::string_view subject, std::int64_t now_seconds, std::int64_t exp_seconds)
  {
    const std::string header_json = nlohmann::json{ { "alg", "HS256" }, { "typ", "JWT" } }.dump();
    const std::string payload_json = nlohmann::json{
      { "sub", std::string(subject) },
      { "iat", now_seconds },
      { "exp", exp_seconds },
    }.dump();

    const auto header_b64 = base64url_encode(std::vector<unsigned char>(header_json.begin(), header_json.end()));
    const auto payload_b64 = base64url_encode(std::vector<unsigned char>(payload_json.begin(), payload_json.end()));
    if (!header_b64 || !payload_b64)
    {
      return std::nullopt;
    }

    const std::string signing_input = *header_b64 + "." + *payload_b64;
    const auto sig = hmac_sha256(secret(), signing_input);
    if (!sig)
    {
      return std::nullopt;
    }
    const auto sig_b64 = base64url_encode(*sig);
    if (!sig_b64)
    {
      return std::nullopt;
    }

    return signing_input + "." + *sig_b64;
  }
}
