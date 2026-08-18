#include "controllers/user_controller.h"
#include "database/db.h"
#include "routes/routes.h"
#include "server/http_server.h"
#include "server/router.h"
#include "services/user_service.h"
#include "controllers/trade_controller.h"
#include "services/trade_service.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

static std::string trim(std::string s)
{
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n'))
  {
    s.pop_back();
  }
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n'))
  {
    ++start;
  }
  if (start > 0)
  {
    s.erase(0, start);
  }
  return s;
}

static void load_dotenv()
{
  namespace fs = std::filesystem;

  fs::path current = fs::current_path();
  for (int depth = 0; depth < 10; ++depth)
  {
    const fs::path candidate = current / ".env";
    if (fs::exists(candidate))
    {
      std::ifstream in(candidate);
      std::string line;
      while (std::getline(in, line))
      {
        if (!line.empty() && line.back() == '\r')
        {
          line.pop_back();
        }
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#')
        {
          continue;
        }

        const auto eq = trimmed.find('=');
        if (eq == std::string::npos || eq == 0)
        {
          continue;
        }

        std::string key = trim(trimmed.substr(0, eq));
        std::string value = trim(trimmed.substr(eq + 1));
        if (!value.empty() && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')) && value.size() >= 2)
        {
          value = value.substr(1, value.size() - 2);
        }

        if (!key.empty())
        {
          SetEnvironmentVariableA(key.c_str(), value.c_str());
        }
      }
      break;
    }

    if (!current.has_parent_path())
    {
      break;
    }
    const fs::path parent = current.parent_path();
    if (parent == current)
    {
      break;
    }
    current = parent;
  }
}

int main()
{
  constexpr unsigned short port = 8080;

  load_dotenv();

  auto db = database::Db::open(database::default_db_path());
  if (!db)
  {
    std::cerr << "Failed to open SQLite database\n";
    return 1;
  }

  services::UserService user_service(*db);
  controllers::UserController user_controller(user_service);
  services::TradeService trade_service(*db);
  controllers::TradeController trade_controller(trade_service);

  server::Router router;
  routes::register_routes(router, user_controller);

  router.add_route("POST",   "/orders",  [&](const server::HttpRequest& req){ return trade_controller.place_order(req);       });
  router.add_route("DELETE", "/orders",  [&](const server::HttpRequest& req){ return trade_controller.cancel_order(req);      });
  router.add_route("GET",    "/orders",  [&](const server::HttpRequest& req){ return trade_controller.get_order_book(req);    });
  router.add_route("GET",  "/trades",    [&](const server::HttpRequest& req){ return trade_controller.get_trade_history(req); });
router.add_route("GET",  "/my-orders", [&](const server::HttpRequest& req){ return trade_controller.get_user_orders(req);   });
  router.add_route("GET",  "/account",   [&](const server::HttpRequest& req){ return trade_controller.get_account(req);       });
  router.add_route("GET",  "/positions", [&](const server::HttpRequest& req){ return trade_controller.get_positions(req);    });
  router.add_route("GET",  "/markets",   [&](const server::HttpRequest& req){ return trade_controller.get_markets(req);      });
  router.add_route("GET",  "/snapshot",  [&](const server::HttpRequest& req){ return trade_controller.get_snapshot(req);     });

  server::HttpServer http_server(port);
  return http_server.run(router);
}

