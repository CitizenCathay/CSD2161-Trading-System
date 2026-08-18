#pragma once

#include <cstdint>
#include <string>

namespace models
{
  struct Trade
  {
    std::int64_t  trade_id{};
    std::int64_t  buy_order_id{};
    std::int64_t  sell_order_id{};
    std::string   buyer_id;
    std::string   seller_id;
    std::string   symbol;
    double        qty{};
    double        price{};
    std::int64_t  timestamp{};
  };
}
