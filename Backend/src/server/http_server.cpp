#define WIN32_LEAN_AND_MEAN
#include "server/http_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

#pragma comment(lib, "ws2_32.lib")

namespace server
{
  HttpServer::HttpServer(unsigned short port) : port_(port) {}

  static std::string trim_query_and_fragment(std::string path)
  {
    const auto query_pos = path.find_first_of("?#");
    if (query_pos != std::string::npos)
    {
      path.resize(query_pos);
    }
    return path;
  }

  static int hex_value(char c)
  {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
  }

  static std::string url_decode(std::string_view in)
  {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i)
    {
      const char c = in[i];
      if (c == '%' && i + 2 < in.size())
      {
        const int hi = hex_value(in[i + 1]);
        const int lo = hex_value(in[i + 2]);
        if (hi >= 0 && lo >= 0)
        {
          out.push_back(static_cast<char>((hi << 4) | lo));
          i += 2;
          continue;
        }
      }
      out.push_back(c);
    }
    return out;
  }

  static void parse_query_string(std::string_view target, std::unordered_map<std::string, std::string>& out)
  {
    const auto qpos = target.find('?');
    if (qpos == std::string_view::npos) return;
    auto end = target.find('#', qpos + 1);
    if (end == std::string_view::npos) end = target.size();
    std::string_view qs = target.substr(qpos + 1, end - (qpos + 1));
    while (!qs.empty())
    {
      const auto amp = qs.find('&');
      const std::string_view pair = (amp == std::string_view::npos) ? qs : qs.substr(0, amp);
      if (amp == std::string_view::npos) qs = std::string_view{};
      else qs.remove_prefix(amp + 1);

      if (pair.empty()) continue;
      const auto eq = pair.find('=');
      const std::string_view k = (eq == std::string_view::npos) ? pair : pair.substr(0, eq);
      const std::string_view v = (eq == std::string_view::npos) ? std::string_view{} : pair.substr(eq + 1);
      if (k.empty()) continue;
      out.insert_or_assign(url_decode(k), url_decode(v));
    }
  }

  static std::string to_lower(std::string s)
  {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
  }

  static std::optional<size_t> try_parse_content_length(const std::unordered_map<std::string, std::string>& headers)
  {
    for (const auto& [k, v] : headers)
    {
      if (to_lower(k) != "content-length")
      {
        continue;
      }
      try
      {
        return static_cast<size_t>(std::stoll(v));
      }
      catch (...)
      {
        return std::nullopt;
      }
    }
    return std::nullopt;
  }

  static std::optional<HttpRequest> parse_request(const std::string& raw_request)
  {
    const auto header_end = raw_request.find("\r\n\r\n");
    if (header_end == std::string::npos)
    {
      return std::nullopt;
    }

    const std::string_view headers_part(raw_request.data(), header_end);
    const std::string_view body_part(raw_request.data() + header_end + 4, raw_request.size() - (header_end + 4));

    const auto line_end = headers_part.find("\r\n");
    const std::string_view first_line = (line_end == std::string_view::npos) ? headers_part : headers_part.substr(0, line_end);

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
    request.target = std::string(first_line.substr(first_space + 1, second_space - (first_space + 1)));
    request.path = trim_query_and_fragment(request.target);
    parse_query_string(request.target, request.query);

    size_t offset = (line_end == std::string_view::npos) ? headers_part.size() : (line_end + 2);
    while (offset < headers_part.size())
    {
      const auto next_end = headers_part.find("\r\n", offset);
      const std::string_view line = (next_end == std::string_view::npos) ? headers_part.substr(offset) : headers_part.substr(offset, next_end - offset);
      offset = (next_end == std::string_view::npos) ? headers_part.size() : (next_end + 2);
      if (line.empty())
      {
        break;
      }
      const auto colon = line.find(':');
      if (colon == std::string_view::npos)
      {
        continue;
      }
      std::string name(line.substr(0, colon));
      std::string value(line.substr(colon + 1));
      while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
      {
        value.erase(value.begin());
      }
      request.headers.emplace(std::move(name), std::move(value));
    }

    request.body = std::string(body_part);
    return request;
  }

  static std::optional<std::string> receive_http_request(SOCKET client_socket)
  {
    std::string buffer;
    buffer.reserve(8192);

    std::string temp;
    temp.resize(4096);

    bool headers_complete = false;
    size_t total_expected = 0;

    while (true)
    {
      const int received = ::recv(client_socket, temp.data(), static_cast<int>(temp.size()), 0);
      if (received <= 0)
      {
        break;
      }
      buffer.append(temp.data(), temp.data() + received);

      if (!headers_complete)
      {
        const auto header_end = buffer.find("\r\n\r\n");
        if (header_end != std::string::npos)
        {
          headers_complete = true;
          const auto parsed = parse_request(buffer);
          if (parsed)
          {
            const auto content_length = try_parse_content_length(parsed->headers);
            if (content_length)
            {
              total_expected = header_end + 4 + *content_length;
            }
            else
            {
              return buffer;
            }
          }
          else
          {
            return buffer;
          }
        }
      }

      if (headers_complete && total_expected > 0 && buffer.size() >= total_expected)
      {
        return buffer;
      }

      if (buffer.size() >= 1024 * 1024)
      {
        return std::nullopt;
      }
    }

    return headers_complete ? std::optional<std::string>(buffer) : std::nullopt;
  }

  std::string HttpView::to_wire(const HttpResponse& response)
  {
    std::ostringstream out;
    out << "HTTP/1.1 " << response.status_code << ' ' << status_text(response.status_code) << "\r\n";
    out << "Content-Type: " << response.content_type << "\r\n";
    out << "Content-Length: " << response.body.size() << "\r\n";
    out << "Connection: close\r\n";
    out << "Access-Control-Allow-Origin: *\r\n";
    out << "Access-Control-Allow-Headers: Authorization, Content-Type\r\n";
    out << "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n";
    out << "\r\n";
    out << response.body;
    return out.str();
  }

  std::string HttpView::status_text(int status_code)
  {
    switch (status_code)
    {
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 204:
      return "No Content";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 404:
      return "Not Found";
    case 409:
      return "Conflict";
    case 500:
      return "Internal Server Error";
    default:
      return "OK";
    }
  }

  int HttpServer::run(const Router& router) const
  {
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
    address.sin_port = htons(port_);

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

    std::cout << "Backend server running at http://localhost:" << port_ << "\n";

    while (true)
    {
      sockaddr_in client_address{};
      int client_len = sizeof(client_address);
      const SOCKET client_socket = accept(server_socket, reinterpret_cast<sockaddr*>(&client_address), &client_len);
      if (client_socket == INVALID_SOCKET)
      {
        continue;
      }

      std::thread([client_socket, &router]()
      {
        const auto raw = receive_http_request(client_socket);
        if (!raw)
        {
          const auto response = HttpView::to_wire(HttpResponse{ 400, "application/json; charset=utf-8", R"({"error":"bad request"})" });
          ::send(client_socket, response.data(), static_cast<int>(response.size()), 0);
          closesocket(client_socket);
          return;
        }

        const auto request = parse_request(*raw);
        if (!request)
        {
          const auto response = HttpView::to_wire(HttpResponse{ 400, "application/json; charset=utf-8", R"({"error":"bad request"})" });
          ::send(client_socket, response.data(), static_cast<int>(response.size()), 0);
          closesocket(client_socket);
          return;
        }

        const HttpResponse response = router.route(*request);
        const auto wire = HttpView::to_wire(response);
        ::send(client_socket, wire.data(), static_cast<int>(wire.size()), 0);
        closesocket(client_socket);
      }).detach();
    }
  }
}

