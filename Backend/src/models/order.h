#pragma once

#include <cstdint>
#include <string>

namespace models
{
  enum class Side { Buy, Sell };
  enum class OrderStatus { Open, PartialFill, Filled, Cancelled };

  struct Order
  {
    std::int64_t  order_id{};
    std::string   user_id;
    std::string   symbol;
    Side          side{};
    double        price{};
    double        qty_total{};
    double        qty_remaining{};
    std::int64_t  arrival{};   // sequence number — lower = resting first
    OrderStatus   status{};
  };
}
