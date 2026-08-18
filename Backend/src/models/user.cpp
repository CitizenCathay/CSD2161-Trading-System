#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "models/user.h"

#include <Windows.h>

#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace models
{
  static bool constant_time_equals(std::string_view a, std::string_view b)
  {
    if (a.size() != b.size())
    {
      return false;
    }
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
      diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
    }
    return diff == 0;
  }

  static std::optional<std::string> base64_encode(const std::vector<unsigned char>& bytes)
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

  static std::optional<std::vector<unsigned char>> base64_decode(std::string_view base64)
  {
    if (base64.empty())
    {
      return std::vector<unsigned char>{};
    }

    DWORD needed = 0;
    if (!CryptStringToBinaryA(base64.data(), static_cast<DWORD>(base64.size()), CRYPT_STRING_BASE64, nullptr, &needed, nullptr, nullptr))
    {
      return std::nullopt;
    }

    std::vector<unsigned char> out;
    out.resize(static_cast<size_t>(needed));
    if (!CryptStringToBinaryA(base64.data(), static_cast<DWORD>(base64.size()), CRYPT_STRING_BASE64, out.data(), &needed, nullptr, nullptr))
    {
      return std::nullopt;
    }

    out.resize(static_cast<size_t>(needed));
    return out;
  }

  static std::optional<std::vector<unsigned char>> hmac_sha256(std::string_view key, std::string_view message)
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

  static std::optional<std::vector<unsigned char>> pbkdf2_hmac_sha256(
    std::string_view password,
    const std::vector<unsigned char>& salt,
    ULONG iterations,
    ULONG derived_key_len)
  {
    BCRYPT_ALG_HANDLE alg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (status < 0)
    {
      return std::nullopt;
    }

    std::vector<unsigned char> derived;
    derived.resize(derived_key_len);
    status = BCryptDeriveKeyPBKDF2(
      alg,
      reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
      static_cast<ULONG>(password.size()),
      reinterpret_cast<PUCHAR>(const_cast<unsigned char*>(salt.data())),
      static_cast<ULONG>(salt.size()),
      iterations,
      derived.data(),
      static_cast<ULONG>(derived.size()),
      0);
    BCryptCloseAlgorithmProvider(alg, 0);

    if (status >= 0)
    {
      return derived;
    }

    if (iterations == 0)
    {
      return std::nullopt;
    }

    constexpr size_t h_len = 32;
    const size_t blocks = (derived_key_len + (h_len - 1)) / h_len;

    std::vector<unsigned char> out;
    out.reserve(static_cast<size_t>(blocks) * h_len);

    std::vector<unsigned char> salt_block;
    salt_block.reserve(salt.size() + 4);

    for (size_t block_index = 1; block_index <= blocks; ++block_index)
    {
      salt_block.assign(salt.begin(), salt.end());
      const unsigned char i1 = static_cast<unsigned char>((block_index >> 24) & 0xFF);
      const unsigned char i2 = static_cast<unsigned char>((block_index >> 16) & 0xFF);
      const unsigned char i3 = static_cast<unsigned char>((block_index >> 8) & 0xFF);
      const unsigned char i4 = static_cast<unsigned char>(block_index & 0xFF);
      salt_block.push_back(i1);
      salt_block.push_back(i2);
      salt_block.push_back(i3);
      salt_block.push_back(i4);

      const auto u1 = hmac_sha256(password, std::string_view(reinterpret_cast<const char*>(salt_block.data()), salt_block.size()));
      if (!u1 || u1->size() != h_len)
      {
        return std::nullopt;
      }

      std::vector<unsigned char> t = *u1;
      std::vector<unsigned char> u = *u1;

      for (ULONG iter = 2; iter <= iterations; ++iter)
      {
        const auto u_next = hmac_sha256(password, std::string_view(reinterpret_cast<const char*>(u.data()), u.size()));
        if (!u_next || u_next->size() != h_len)
        {
          return std::nullopt;
        }
        u = *u_next;
        for (size_t j = 0; j < h_len; ++j)
        {
          t[j] ^= u[j];
        }
      }

      out.insert(out.end(), t.begin(), t.end());
    }

    out.resize(derived_key_len);
    return out;
  }

  static std::optional<std::vector<unsigned char>> random_bytes(size_t size)
  {
    std::vector<unsigned char> out(size);
    if (size == 0)
    {
      return out;
    }
    const NTSTATUS status = BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0)
    {
      return std::nullopt;
    }
    return out;
  }

  std::optional<UserRecord> make_user_record(std::string_view password)
  {
    constexpr ULONG iterations = 100000;
    constexpr ULONG derived_len = 32;
    const auto salt = random_bytes(16);
    if (!salt)
    {
      return std::nullopt;
    }
    const auto derived = pbkdf2_hmac_sha256(password, *salt, iterations, derived_len);
    if (!derived)
    {
      return std::nullopt;
    }
    const auto salt_b64 = base64_encode(*salt);
    const auto hash_b64 = base64_encode(*derived);
    if (!salt_b64 || !hash_b64)
    {
      return std::nullopt;
    }
    return UserRecord{ *salt_b64, *hash_b64 };
  }

  bool verify_password(std::string_view password, const UserRecord& record)
  {
    constexpr ULONG iterations = 100000;
    constexpr ULONG derived_len = 32;

    const auto salt = base64_decode(record.salt_b64);
    const auto expected = base64_decode(record.hash_b64);
    if (!salt || !expected || expected->size() != derived_len)
    {
      return false;
    }

    const auto derived = pbkdf2_hmac_sha256(password, *salt, iterations, derived_len);
    if (!derived)
    {
      return false;
    }

    return constant_time_equals(
      std::string_view(reinterpret_cast<const char*>(derived->data()), derived->size()),
      std::string_view(reinterpret_cast<const char*>(expected->data()), expected->size()));
  }
}

