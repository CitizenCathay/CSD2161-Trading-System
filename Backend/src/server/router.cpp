#include "server/router.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace server
{
  static std::string to_lower(std::string s)
  {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
  }

  std::optional<std::string> header_value_ci(const std::unordered_map<std::string, std::string>& headers, std::string_view name)
  {
    const std::string needle = to_lower(std::string(name));
    for (const auto& [k, v] : headers)
    {
      if (to_lower(k) == needle)
      {
        return v;
      }
    }
    return std::nullopt;
  }

  std::string Router::make_key(std::string_view method, std::string_view path)
  {
    std::string key;
    key.reserve(method.size() + 1 + path.size());
    key.append(method);
    key.push_back(' ');
    key.append(path);
    return key;
  }

  void Router::add_route(std::string method, std::string path, RouteHandler handler)
  {
    routes_.insert_or_assign(make_key(method, path), std::move(handler));
  }

  HttpResponse Router::route(const HttpRequest& request) const
  {
    if (request.method == "OPTIONS")
    {
      return HttpResponse{ 204, "application/json; charset=utf-8", "" };
    }

    const auto it = routes_.find(make_key(request.method, request.path));
    if (it == routes_.end())
    {
      return HttpResponse{ 404, "application/json; charset=utf-8", R"({"error":"not found"})" };
    }

    return it->second(request);
  }
}

