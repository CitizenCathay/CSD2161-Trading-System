#include "services/user_service.h"

#include "models/user.h"
#include "utils/jwt_helper.h"

#include <chrono>

namespace services
{
  UserService::UserService(database::Db& db) : db_(db) {}

  RegisterResult UserService::register_user(const std::string& username, const std::string& password, std::int64_t created_at_seconds)
  {
    const auto exists = db_.user_exists(username);
    if (!exists)
    {
      return RegisterResult::error;
    }
    if (*exists)
    {
      return RegisterResult::exists;
    }

    const auto record = models::make_user_record(password);
    if (!record)
    {
      return RegisterResult::error;
    }

    const auto inserted = db_.insert_user(username, *record, created_at_seconds);
    switch (inserted)
    {
    case database::InsertUserResult::ok:
      db_.upsert_account(username, 100000.0);
      return RegisterResult::ok;
    case database::InsertUserResult::exists:
      return RegisterResult::exists;
    default:
      return RegisterResult::error;
    }
  }

  LoginResult UserService::login_user(const std::string& username, const std::string& password, std::int64_t now_seconds)
  {
    const auto record = db_.get_user(username);
    if (!record || !models::verify_password(password, *record))
    {
      return LoginResult{ LoginStatus::invalid_credentials, {} };
    }

    const auto token = utils::jwt::make_hs256(username, now_seconds, now_seconds + 3600);
    if (!token)
    {
      return LoginResult{ LoginStatus::error, {} };
    }

    return LoginResult{ LoginStatus::ok, *token };
  }

  std::optional<std::vector<database::UserSummary>> UserService::list_users() const { return db_.list_users(); }
}
