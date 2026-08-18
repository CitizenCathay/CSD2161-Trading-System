#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

#pragma comment(lib, "ws2_32.lib")

static std::filesystem::path get_executable_dir()
{
  char buffer[MAX_PATH]{};
  const DWORD length = GetModuleFileNameA(nullptr, buffer, static_cast<DWORD>(sizeof(buffer)));
  if (length == 0 || length >= sizeof(buffer))
  {
    return std::filesystem::current_path();
  }
  return std::filesystem::path(std::string(buffer, buffer + length)).parent_path();
}

static std::string trim_query_and_fragment(std::string path)
{
  const auto query_pos = path.find_first_of("?#");
  if (query_pos != std::string::npos)
  {
    path.resize(query_pos);
  }
  return path;
}

namespace mvc
{
  struct HttpRequest
  {
    std::string method;
    std::string path;
  };

  struct HttpResponse
  {
    int status_code{};
    std::string content_type;
    std::string body;
  };

  class HttpView
  {
  public:
    static std::string to_wire(const HttpResponse& response)
    {
      std::ostringstream out;
      out << "HTTP/1.1 " << response.status_code << ' ' << status_text(response.status_code) << "\r\n";
      out << "Content-Type: " << response.content_type << "\r\n";
      out << "Content-Length: " << response.body.size() << "\r\n";
      out << "Connection: close\r\n";
      out << "\r\n";
      out << response.body;
      return out.str();
    }

  private:
    static std::string status_text(int status_code)
    {
      switch (status_code)
      {
      case 200:
        return "OK";
      case 404:
        return "Not Found";
      case 405:
        return "Method Not Allowed";
      default:
        return "OK";
      }
    }
  };

  class RouteModel
  {
  public:
    RouteModel()
    {
      get_routes_.emplace("/", "login.html");
      get_routes_.emplace("/login", "login.html");
      get_routes_.emplace("/login/", "login.html");
      get_routes_.emplace("/login.html", "login.html");
      get_routes_.emplace("/register", "register.html");
      get_routes_.emplace("/register/", "register.html");
      get_routes_.emplace("/register.html", "register.html");
      get_routes_.emplace("/index.html", "index.html");
      get_routes_.emplace("/markets", "markets.html");
      get_routes_.emplace("/markets/", "markets.html");
      get_routes_.emplace("/markets.html", "markets.html");
      get_routes_.emplace("/portfolio", "portfolio.html");
      get_routes_.emplace("/portfolio/", "portfolio.html");
      get_routes_.emplace("/portfolio.html", "portfolio.html");
      get_routes_.emplace("/trade", "trade.html");
      get_routes_.emplace("/trade/", "trade.html");
      get_routes_.emplace("/trade.html", "trade.html");
    }

    std::optional<std::string> resolve_view_file(HttpRequest request) const
    {
      if (request.method != "GET")
      {
        return std::nullopt;
      }

      request.path = trim_query_and_fragment(std::move(request.path));

      const auto it = get_routes_.find(request.path);
      if (it == get_routes_.end())
      {
        return std::nullopt;
      }
      return it->second;
    }

  private:
    std::unordered_map<std::string, std::string> get_routes_;
  };

  class StaticFileModel
  {
  public:
    explicit StaticFileModel(std::filesystem::path web_root)
      : web_root_(std::move(web_root))
    {
    }

    const std::filesystem::path& web_root() const
    {
      return web_root_;
    }

    std::optional<std::string> read_file_text(const std::string& relative_file) const
    {
      const auto full_path = web_root_ / relative_file;
      std::ifstream file(full_path, std::ios::in | std::ios::binary);
      if (!file)
      {
        return std::nullopt;
      }

      std::ostringstream contents;
      contents << file.rdbuf();
      return contents.str();
    }

  private:
    std::filesystem::path web_root_;
  };

  class FrontendController
  {
  public:
    explicit FrontendController(StaticFileModel files)
      : files_(std::move(files))
    {
    }

    std::string handle_raw_request(const std::string& raw_request) const
    {
      const auto request = parse_request(raw_request);
      if (!request)
      {
        return HttpView::to_wire(HttpResponse{ 404, "text/plain; charset=utf-8", "Not Found" });
      }

      const auto view_file = routes_.resolve_view_file(*request);
      if (!view_file)
      {
        const auto status = (request->method == "GET") ? 404 : 405;
        const auto body = (status == 404) ? "Not Found" : "Method Not Allowed";
        return HttpView::to_wire(HttpResponse{ status, "text/plain; charset=utf-8", body });
      }

      const auto contents = files_.read_file_text(*view_file);
      if (!contents)
      {
        return HttpView::to_wire(HttpResponse{ 404, "text/plain; charset=utf-8", "Not Found" });
      }

      return HttpView::to_wire(HttpResponse{ 200, "text/html; charset=utf-8", *contents });
    }

  private:
    static std::optional<HttpRequest> parse_request(const std::string& raw_request)
    {
      const auto line_end = raw_request.find("\r\n");
      const std::string_view first_line = (line_end == std::string::npos) ? std::string_view(raw_request) : std::string_view(raw_request).substr(0, line_end);

      const auto first_space = first_line.find(' ');
      if (first_space == std::string_view::npos)
      {
        return std::nullopt;
      }
      const auto second_space = first_line.find(' ', first_space + 1);
      if (second_space == std::string_view::npos)
      {
        return std::nullopt;
      }

      HttpRequest request;
      request.method = std::string(first_line.substr(0, first_space));
      request.path = std::string(first_line.substr(first_space + 1, second_space - (first_space + 1)));
      return request;
    }

    RouteModel routes_;
    StaticFileModel files_;
  };
}

static std::filesystem::path resolve_web_root()
{
  const auto exe_root = get_executable_dir() / "public";
  if (std::filesystem::exists(exe_root))
  {
    return exe_root;
  }

#ifdef FRONTEND_SOURCE_DIR
  const auto source_root = std::filesystem::path(FRONTEND_SOURCE_DIR) / "public";
  if (std::filesystem::exists(source_root))
  {
    return source_root;
  }
#endif

  return std::filesystem::current_path() / "public";
}

int main()
{
  constexpr unsigned short port = 8081;

  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
  {
    std::cerr << "WSAStartup failed\n";
    return 1;
  }

  const SOCKET server_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (server_socket == INVALID_SOCKET)
  {
    std::cerr << "socket() failed\n";
    WSACleanup();
    return 1;
  }

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = INADDR_ANY;
  address.sin_port = htons(port);

  const int reuse = 1;
  setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  if (bind(server_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
  {
    std::cerr << "bind() failed\n";
    closesocket(server_socket);
    WSACleanup();
    return 1;
  }

  if (listen(server_socket, SOMAXCONN) == SOCKET_ERROR)
  {
    std::cerr << "listen() failed\n";
    closesocket(server_socket);
    WSACleanup();
    return 1;
  }

  const auto web_root = resolve_web_root();
  const mvc::FrontendController controller{ mvc::StaticFileModel{ web_root } };
  std::cout << "Frontend server running at http://localhost:" << port << "\n";
  std::cout << "Web root: " << web_root.string() << "\n";

  while (true)
  {
    sockaddr_in client_address{};
    int client_len = sizeof(client_address);
    const SOCKET client_socket = accept(server_socket, reinterpret_cast<sockaddr*>(&client_address), &client_len);
    if (client_socket == INVALID_SOCKET)
    {
      continue;
    }

    std::string request;
    request.resize(8192);
    const int received = ::recv(client_socket, request.data(), static_cast<int>(request.size()), 0);
    if (received <= 0)
    {
      closesocket(client_socket);
      continue;
    }
    request.resize(static_cast<size_t>(received));

    const auto response = controller.handle_raw_request(request);
    ::send(client_socket, response.data(), static_cast<int>(response.size()), 0);
    closesocket(client_socket);
  }
}
