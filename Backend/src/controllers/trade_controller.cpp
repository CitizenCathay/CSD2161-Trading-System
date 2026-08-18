#include "controllers/trade_controller.h"

#include "utils/jwt_helper.h"

#include <nlohmann/json.hpp>
#include <chrono>
#include <string>

namespace controllers
{
  namespace
  {
    static server::HttpResponse make_json(int status, const nlohmann::json& body)
    {
      return server::HttpResponse{ status, "application/json; charset=utf-8", body.dump() };
    }

    // Extract "sub" claim from a Bearer JWT in the Authorization header.
    // Returns nullopt if the token is missing or invalid.
    static std::optional<std::string> extract_user(const server::HttpRequest& request)
    {
      const auto auth = server::header_value_ci(request.headers, "Authorization");
      if (!auth || auth->size() < 8) return std::nullopt;

      // Expect "Bearer <token>"
      const std::string prefix = "Bearer ";
      if (auth->substr(0, prefix.size()) != prefix) return std::nullopt;
      const std::string token = auth->substr(prefix.size());

      // Split header.payload.sig
      const auto dot1 = token.find('.');
      const auto dot2 = token.find('.', dot1 + 1);
      if (dot1 == std::string::npos || dot2 == std::string::npos) return std::nullopt;

      // Verify signature
      const std::string signing_input = token.substr(0, dot2);
      const auto expected_sig = utils::jwt::hmac_sha256(utils::jwt::secret(), signing_input);
      if (!expected_sig) return std::nullopt;

      const auto expected_b64 = utils::jwt::base64url_encode(*expected_sig);
      if (!expected_b64) return std::nullopt;

      const std::string actual_sig = token.substr(dot2 + 1);
      if (actual_sig != *expected_b64) return std::nullopt;

      // Decode payload (base64url to JSON)
      const std::string payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);

      // base64url to base64
      std::string b64 = payload_b64;
      std::replace(b64.begin(), b64.end(), '-', '+');
      std::replace(b64.begin(), b64.end(), '_', '/');
      while (b64.size() % 4 != 0) b64.push_back('=');

      // decode
      DWORD needed = 0;
      if (!CryptStringToBinaryA(b64.data(), static_cast<DWORD>(b64.size()),
            CRYPT_STRING_BASE64, nullptr, &needed, nullptr, nullptr))
        return std::nullopt;
      std::vector<unsigned char> decoded(needed);
      if (!CryptStringToBinaryA(b64.data(), static_cast<DWORD>(b64.size()),
            CRYPT_STRING_BASE64, decoded.data(), &needed, nullptr, nullptr))
        return std::nullopt;
      decoded.resize(needed);

      const std::string payload_json(decoded.begin(), decoded.end());
      const auto parsed = nlohmann::json::parse(payload_json, nullptr, false);
      if (!parsed.is_object()) return std::nullopt;

      const auto sub = parsed.find("sub");
      if (sub == parsed.end() || !sub->is_string()) return std::nullopt;

      // Reject expired tokens: check "exp" claim (Unix timestamp) against current time
      const auto exp = parsed.find("exp");
      if (exp != parsed.end() && exp->is_number_integer())
      {
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count(); // Current Unix time in seconds

        if (static_cast<std::int64_t>(now) > exp->get<std::int64_t>())  // Token has expired
          return std::nullopt;
      }

      return sub->get<std::string>();
    }

    static const char* side_str(models::Side s)
    {
      return s == models::Side::Buy ? "buy" : "sell";
    }

    static const char* status_str(models::OrderStatus s)
    {
      switch (s)
      {
      case models::OrderStatus::PartialFill: return "partial_fill";
      case models::OrderStatus::Filled:      return "filled";
      case models::OrderStatus::Cancelled:   return "cancelled";
      default:                               return "open";
      }
    }

    static nlohmann::json order_to_json(const models::Order& o)
    {
      return nlohmann::json{
        { "order_id",       o.order_id       },
        { "user_id",        o.user_id        },
        { "symbol",         o.symbol         },
        { "side",           side_str(o.side) },
        { "price",          o.price          },
        { "qty_total",      o.qty_total      },
        { "qty_remaining",  o.qty_remaining  },
        { "status",         status_str(o.status) },
      };
    }

    static nlohmann::json trade_to_json(const models::Trade& t)
    {
      return nlohmann::json{
        { "trade_id",       t.trade_id      },
        { "buy_order_id",   t.buy_order_id  },
        { "sell_order_id",  t.sell_order_id },
        { "buyer_id",       t.buyer_id      },
        { "seller_id",      t.seller_id     },
        { "symbol",         t.symbol        },
        { "qty",            t.qty           },
        { "price",          t.price         },
        { "timestamp",      t.timestamp     },
      };
    }
  }

  TradeController::TradeController(services::TradeService& trade_service)
    : trade_service_(trade_service) {}

  // POST /orders
  // Body: { "symbol": "DBS", "side": "buy", "price": 110.0, "qty": 10 }
  server::HttpResponse TradeController::place_order(const server::HttpRequest& request) const
  {
    const auto user = extract_user(request);
    if (!user)
      return make_json(401, { { "error", "unauthorized" } });

    const auto parsed = nlohmann::json::parse(request.body, nullptr, false);
    if (!parsed.is_object())
      return make_json(400, { { "error", "invalid JSON" } });

    const auto sym   = parsed.find("symbol");
    const auto side  = parsed.find("side");
    const auto price = parsed.find("price");
    const auto qty   = parsed.find("qty");

    if (sym == parsed.end() || side == parsed.end() ||
        price == parsed.end() || qty == parsed.end())
      return make_json(400, { { "error", "symbol, side, price, qty required" } });

    if (!sym->is_string() || !side->is_string() || !price->is_number() || !qty->is_number())
      return make_json(400, { { "error", "invalid field types" } });

    const auto result = trade_service_.place_order(
      *user,
      sym->get<std::string>(),
      side->get<std::string>(),
      price->get<double>(),
      qty->get<double>());

    switch (result.status)
    {
    case services::PlaceOrderStatus::ok:
      return make_json(201, { { "message", "order placed" }, { "order_id", *result.order_id } });
    case services::PlaceOrderStatus::insufficient_funds:
      return make_json(400, { { "error", "insufficient funds" } });
    case services::PlaceOrderStatus::insufficient_holdings:
      return make_json(400, { { "error", "insufficient holdings" } });
    case services::PlaceOrderStatus::invalid_input:
      return make_json(400, { { "error", "invalid order parameters" } });
    default:
      return make_json(500, { { "error", "internal error" } });
    }
  }

  // DELETE /orders
  // Body: { "order_id": 123 }
  server::HttpResponse TradeController::cancel_order(const server::HttpRequest& request)
  {
	  // 1. Extract user from JWT; if missing/invalid return 401
      const auto user = extract_user(request);
      if (!user)
          return make_json(401, { { "error", "unauthorized" } });

	  // 2. Parse JSON body; if missing/invalid return 400
      const auto parsed = nlohmann::json::parse(request.body, nullptr, false);
      if (!parsed.is_object())
          return make_json(400, { { "error", "invalid JSON" } });

	  // 3. Extract order_id from body; if missing/invalid return 400
      const auto id = parsed.find("order_id");
      if (id == parsed.end() || !id->is_number_integer())
          return make_json(400, { { "error", "order_id required" } });

	  // 4. Call cancel_order() and return appropriate response based on result
      const auto status = trade_service_.cancel_order(*user, id->get<std::int64_t>());

      // Only the user who owns the order can cancel it and only if it's still open/partial_fill;
      // otherwise return 404 to avoid leaking info about other users' orders
      switch (status)
      {
          case services::CancelOrderStatus::ok:
              return make_json(200, { { "message", "order cancelled" } });
          case services::CancelOrderStatus::not_found:
              return make_json(404, { { "error", "order not found" } });
          case services::CancelOrderStatus::not_owner:
              return make_json(403, { { "error", "order does not belong to user" } });
          case services::CancelOrderStatus::already_closed:
              return make_json(409, { { "error", "order already filled or cancelled" } });
          default:
              return make_json(500, { { "error", "internal error" } }); 
      }
  }

  // GET /orders?symbol=DBS
  server::HttpResponse TradeController::get_order_book(const server::HttpRequest& request) const
  {
    auto it = request.query.find("symbol");
    const std::string symbol = (it != request.query.end() && !it->second.empty()) ? it->second : "BTC";
    const auto orders = trade_service_.get_order_book(symbol);

    nlohmann::json list = nlohmann::json::array();
    for (const auto& o : orders) list.push_back(order_to_json(o));
    return make_json(200, { { "orders", list } });
  }

  // GET /trades
  server::HttpResponse TradeController::get_trade_history(const server::HttpRequest& request) const
  {
    auto it = request.query.find("symbol");
    const std::string symbol = (it != request.query.end() && !it->second.empty()) ? it->second : "BTC";
    const auto trades = trade_service_.get_trade_history(symbol);

    nlohmann::json list = nlohmann::json::array();
    for (const auto& t : trades) list.push_back(trade_to_json(t));
    return make_json(200, { { "trades", list } });
  }

  // GET /my-orders  (requires JWT)
  server::HttpResponse TradeController::get_user_orders(const server::HttpRequest& request) const
  {
    const auto user = extract_user(request);
    if (!user)
      return make_json(401, { { "error", "unauthorized" } });

    const auto orders = trade_service_.get_user_orders(*user);
    nlohmann::json list = nlohmann::json::array();
    for (const auto& o : orders) list.push_back(order_to_json(o));
    return make_json(200, { { "orders", list } });
  }

  // GET /account  (requires JWT)
  server::HttpResponse TradeController::get_account(const server::HttpRequest& request) const
  {
    const auto user = extract_user(request);
    if (!user)
      return make_json(401, { { "error", "unauthorized" } });

    const auto account = trade_service_.get_account(*user);
    if (!account)
      return make_json(404, { { "error", "account not found" } });

    nlohmann::json positions = nlohmann::json::array();
    for (const auto& p : account->positions)
      positions.push_back({ { "symbol", p.symbol }, { "qty", p.qty } });
    return make_json(200, {
      { "username",  account->username },
      { "cash",      account->cash     },
      { "positions", positions         },
    });
  }

  // GET /positions (requires JWT)
  server::HttpResponse TradeController::get_positions(const server::HttpRequest& request) const
  {
    const auto user = extract_user(request);
    if (!user)
      return make_json(401, { { "error", "unauthorized" } });

    const auto positions = trade_service_.get_positions(*user);
    nlohmann::json list = nlohmann::json::array();
    for (const auto& p : positions)
    {
      list.push_back(nlohmann::json{ { "symbol", p.symbol }, { "qty", p.qty } });
    }
    return make_json(200, { { "positions", list } });
  }

  // GET /markets
  server::HttpResponse TradeController::get_markets(const server::HttpRequest& request) const
  {
    const auto markets = trade_service_.get_markets();
    nlohmann::json list = nlohmann::json::array();
    for (const auto& a : markets)
    {
      const double pct = (a.base > 0.0) ? ((a.price - a.base) / a.base * 100.0) : 0.0;
      list.push_back(nlohmann::json{
        { "symbol",   a.symbol },
        { "name",     a.name },
        { "category", a.category },
        { "icon",     a.icon },
        { "is_new",   a.is_new },
        { "price",    a.price },
        { "change",   pct },
        { "volume_24h_usd", a.volume_24h_usd },
      });
    }
    return make_json(200, { { "assets", list } });
  }

  // GET /snapshot?symbol=BTC
  server::HttpResponse TradeController::get_snapshot(const server::HttpRequest& request) const
  {
    auto it = request.query.find("symbol");
    const std::string symbol = (it != request.query.end() && !it->second.empty()) ? it->second : "BTC";

    nlohmann::json out;
    out["symbol"] = symbol;
    out["markets"] = nlohmann::json::object();

    {
      const auto markets = trade_service_.get_markets();
      nlohmann::json list = nlohmann::json::array();
      for (const auto& a : markets)
      {
        const double pct = (a.base > 0.0) ? ((a.price - a.base) / a.base * 100.0) : 0.0;
        list.push_back(nlohmann::json{
          { "symbol",   a.symbol },
          { "name",     a.name },
          { "category", a.category },
          { "icon",     a.icon },
          { "is_new",   a.is_new },
          { "price",    a.price },
          { "change",   pct },
          { "volume_24h_usd", a.volume_24h_usd },
        });
      }
      out["markets"]["assets"] = list;
    }

    {
      const auto orders = trade_service_.get_order_book(symbol);
      nlohmann::json list = nlohmann::json::array();
      for (const auto& o : orders) list.push_back(order_to_json(o));
      out["order_book"] = nlohmann::json{ { "orders", list } };
    }

    {
      const auto trades = trade_service_.get_trade_history(symbol);
      nlohmann::json list = nlohmann::json::array();
      for (const auto& t : trades) list.push_back(trade_to_json(t));
      out["trades"] = nlohmann::json{ { "trades", list } };
    }

    const auto user = extract_user(request);
    if (!user)
    {
      out["auth"] = nlohmann::json{ { "authenticated", false } };
      return make_json(200, out);
    }

    out["auth"] = nlohmann::json{ { "authenticated", true }, { "username", *user } };

    const auto account = trade_service_.get_account(*user);
    if (account)
    {
      out["account"] = nlohmann::json{ { "username", account->username }, { "cash", account->cash } };
    }

    {
      const auto positions = trade_service_.get_positions(*user);
      nlohmann::json list = nlohmann::json::array();
      for (const auto& p : positions) list.push_back(nlohmann::json{ { "symbol", p.symbol }, { "qty", p.qty } });
      out["positions"] = nlohmann::json{ { "positions", list } };
    }

    {
      const auto orders = trade_service_.get_user_orders(*user);
      nlohmann::json list = nlohmann::json::array();
      for (const auto& o : orders) list.push_back(order_to_json(o));
      out["my_orders"] = nlohmann::json{ { "orders", list } };
    }

    return make_json(200, out);
  }
}
