#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace models
{
  struct UserRecord
  {
    std::string salt_b64;
    std::string hash_b64;
  };

  std::optional<UserRecord> make_user_record(std::string_view password);
  bool verify_password(std::string_view password, const UserRecord& record);
}

