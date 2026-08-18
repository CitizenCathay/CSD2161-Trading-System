#pragma once

#include "controllers/user_controller.h"
#include "server/router.h"

namespace routes
{
  void register_routes(server::Router& router, const controllers::UserController& user_controller);
}

