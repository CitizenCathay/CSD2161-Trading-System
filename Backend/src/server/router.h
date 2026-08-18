#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace server
{
  struct HttpRequest
  {
    std::string method;
    std::string target;
    std::string path;
    std::unordered_map<std::string, std::string> query;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
  };

  struct HttpResponse
  {
    int status_code{};
    std::string content_type;
    std::string body;
  };

  using RouteHandler = std::function<HttpResponse(const HttpRequest&)>;

  std::optional<std::string> header_value_ci(const std::unordered_map<std::string, std::string>& headers, std::string_view name);

  class Router
  {
  public:
    void add_route(std::string method, std::string path, RouteHandler handler);
    HttpResponse route(const HttpRequest& request) const;

  private:
    std::unordered_map<std::string, RouteHandler> routes_;
    static std::string make_key(std::string_view method, std::string_view path);
  };
}

