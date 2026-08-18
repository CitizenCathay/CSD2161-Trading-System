#pragma once

#include "server/router.h"
#include "services/user_service.h"

namespace controllers
{
  class UserController
  {
  public:
    explicit UserController(services::UserService& user_service);

    server::HttpResponse get_users(const server::HttpRequest& request) const;
    server::HttpResponse register_user(const server::HttpRequest& request) const;
    server::HttpResponse login_user(const server::HttpRequest& request) const;

  private:
    services::UserService& user_service_;
  };
}

