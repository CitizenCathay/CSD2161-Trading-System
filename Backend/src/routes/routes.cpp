#include "routes/routes.h"

namespace routes
{
  void register_routes(server::Router& router, const controllers::UserController& user_controller)
  {
    // Users Endpoint
    router.add_route("GET", "/users", [&user_controller](const server::HttpRequest& req) { return user_controller.get_users(req); });

    // Register Endpoint
    router.add_route("POST", "/auth/register", [&user_controller](const server::HttpRequest& req) { return user_controller.register_user(req); });
    router.add_route("POST", "/register", [&user_controller](const server::HttpRequest& req) { return user_controller.register_user(req); });

    // Login Endpoint
    router.add_route("POST", "/auth/login", [&user_controller](const server::HttpRequest& req) { return user_controller.login_user(req); });
    router.add_route("POST", "/login", [&user_controller](const server::HttpRequest& req) { return user_controller.login_user(req); });
  }
}

