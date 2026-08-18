#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <wincrypt.h>

#include <optional>
#include <string>
#include <vector>

#include "server/router.h"
#include "services/trade_service.h"
#include "models/order.h"
#include "models/trade.h"

namespace controllers
{
  class TradeController
  {
  public:
    explicit TradeController(services::TradeService& trade_service);

    server::HttpResponse place_order(const server::HttpRequest& request) const;
	  server::HttpResponse cancel_order(const server::HttpRequest& request);  // Not const because it modifies state (cancelling order)
    server::HttpResponse get_order_book(const server::HttpRequest& request) const;
server::HttpResponse get_trade_history(const server::HttpRequest& request) const;
    server::HttpResponse get_user_orders(const server::HttpRequest& request) const;
    server::HttpResponse get_account(const server::HttpRequest& request) const;
    server::HttpResponse get_positions(const server::HttpRequest& request) const;
    server::HttpResponse get_markets(const server::HttpRequest& request) const;
    server::HttpResponse get_snapshot(const server::HttpRequest& request) const;

  private:
    services::TradeService& trade_service_;
  };
}
