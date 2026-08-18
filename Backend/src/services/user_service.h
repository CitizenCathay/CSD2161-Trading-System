#pragma once

#include "database/db.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace services
{
  enum class RegisterResult
  {
    ok,
    exists,
    error
  };

  enum class LoginStatus
  {
    ok,
    invalid_credentials,
    error
  };

  struct LoginResult
  {
    LoginStatus status{};
    std::string token;
  };

  class UserService
  {
  public:
    explicit UserService(database::Db& db);

    RegisterResult register_user(const std::string& username, const std::string& password, std::int64_t created_at_seconds);
    LoginResult login_user(const std::string& username, const std::string& password, std::int64_t now_seconds);
    std::optional<std::vector<database::UserSummary>> list_users() const;

  private:
    database::Db& db_;
  };
}
