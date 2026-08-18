#include "controllers/user_controller.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace controllers
{
  namespace
  {
    static server::HttpResponse make_json_response(int status_code, const nlohmann::json& body)
    {
      return server::HttpResponse{ status_code, "application/json; charset=utf-8", body.dump() };
    }

    static bool starts_with_ci(std::string_view value, std::string_view prefix)
    {
      if (value.size() < prefix.size())
      {
        return false;
      }
      for (size_t i = 0; i < prefix.size(); ++i)
      {
        if (std::tolower(static_cast<unsigned char>(value[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
        {
          return false;
        }
      }
      return true;
    }

    static std::string url_decode(std::string_view input)
    {
      std::string out;
      out.reserve(input.size());

      auto hex_value = [](unsigned char c) -> int {
        if (c >= '0' && c <= '9')
        {
          return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
          return 10 + (c - 'a');
        }
        if (c >= 'A' && c <= 'F')
        {
          return 10 + (c - 'A');
        }
        return -1;
      };

      for (size_t i = 0; i < input.size(); ++i)
      {
        const char c = input[i];
        if (c == '+')
        {
          out.push_back(' ');
          continue;
        }
        if (c == '%' && (i + 2) < input.size())
        {
          const int hi = hex_value(static_cast<unsigned char>(input[i + 1]));
          const int lo = hex_value(static_cast<unsigned char>(input[i + 2]));
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

    static std::unordered_map<std::string, std::string> parse_form_urlencoded(std::string_view body)
    {
      std::unordered_map<std::string, std::string> out;
      size_t pos = 0;
      while (pos < body.size())
      {
        const auto amp = body.find('&', pos);
        const std::string_view pair = (amp == std::string_view::npos) ? body.substr(pos) : body.substr(pos, amp - pos);
        pos = (amp == std::string_view::npos) ? body.size() : (amp + 1);
        if (pair.empty())
        {
          continue;
        }
        const auto eq = pair.find('=');
        const std::string_view key = (eq == std::string_view::npos) ? pair : pair.substr(0, eq);
        const std::string_view value = (eq == std::string_view::npos) ? std::string_view{} : pair.substr(eq + 1);
        out.emplace(url_decode(key), url_decode(value));
      }
      return out;
    }

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

    static std::unordered_map<std::string, std::string> parse_multipart_formdata(std::string_view body, std::string_view boundary)
    {
      std::unordered_map<std::string, std::string> out;
      if (boundary.empty())
      {
        return out;
      }

      const std::string delimiter = "--" + std::string(boundary);
      size_t pos = body.find(delimiter);
      while (pos != std::string_view::npos)
      {
        size_t part_start = pos + delimiter.size();
        if (part_start + 1 < body.size() && body[part_start] == '-' && body[part_start + 1] == '-')
        {
          break;
        }
        if (part_start + 1 < body.size() && body[part_start] == '\r' && body[part_start + 1] == '\n')
        {
          part_start += 2;
        }

        const auto next = body.find(delimiter, part_start);
        if (next == std::string_view::npos)
        {
          break;
        }

        std::string_view part = body.substr(part_start, next - part_start);
        if (part.size() >= 2 && part.substr(part.size() - 2) == "\r\n")
        {
          part = part.substr(0, part.size() - 2);
        }

        const auto header_end = part.find("\r\n\r\n");
        if (header_end != std::string_view::npos)
        {
          const std::string_view headers = part.substr(0, header_end);
          std::string_view content = part.substr(header_end + 4);
          if (content.size() >= 2 && content.substr(content.size() - 2) == "\r\n")
          {
            content = content.substr(0, content.size() - 2);
          }

          const auto name_pos = headers.find("name=\"");
          if (name_pos != std::string_view::npos)
          {
            const auto key_start = name_pos + 6;
            const auto key_end = headers.find('"', key_start);
            if (key_end != std::string_view::npos)
            {
              const std::string key(headers.substr(key_start, key_end - key_start));
              out.emplace(key, std::string(content));
            }
          }
        }

        pos = next;
      }
      return out;
    }

    static std::optional<std::pair<std::string, std::string>> extract_credentials(const server::HttpRequest& request)
    {
      const auto parsed = nlohmann::json::parse(request.body, nullptr, false);
      if (parsed.is_object())
      {
        const auto u = parsed.find("username");
        const auto p = parsed.find("password");
        if (u != parsed.end() && p != parsed.end() && u->is_string() && p->is_string())
        {
          return std::make_pair(u->get<std::string>(), p->get<std::string>());
        }
      }

      const auto content_type = server::header_value_ci(request.headers, "Content-Type");
      const std::string ct = content_type ? *content_type : std::string();

      if (starts_with_ci(ct, "application/x-www-form-urlencoded"))
      {
        const auto fields = parse_form_urlencoded(request.body);
        const auto u = fields.find("username");
        const auto p = fields.find("password");
        if (u != fields.end() && p != fields.end())
        {
          return std::make_pair(u->second, p->second);
        }
        return std::nullopt;
      }

      if (starts_with_ci(ct, "multipart/form-data"))
      {
        std::string boundary;
        const auto bpos = ct.find("boundary=");
        if (bpos != std::string::npos)
        {
          boundary = ct.substr(bpos + 9);
          boundary = trim(boundary);
          if (!boundary.empty() && boundary.front() == '"')
          {
            boundary.erase(boundary.begin());
          }
          if (!boundary.empty() && boundary.back() == '"')
          {
            boundary.pop_back();
          }
        }

        const auto fields = parse_multipart_formdata(request.body, boundary);
        const auto u = fields.find("username");
        const auto p = fields.find("password");
        if (u != fields.end() && p != fields.end())
        {
          return std::make_pair(u->second, p->second);
        }
        return std::nullopt;
      }

      const auto fields = parse_form_urlencoded(request.body);
      const auto u = fields.find("username");
      const auto p = fields.find("password");
      if (u != fields.end() && p != fields.end())
      {
        return std::make_pair(u->second, p->second);
      }

      return std::nullopt;
    }

    static std::int64_t now_seconds()
    {
      return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
  }

  UserController::UserController(services::UserService& user_service) : user_service_(user_service) {}

  server::HttpResponse UserController::get_users(const server::HttpRequest&) const
  {
    const auto users = user_service_.list_users();
    if (!users)
    {
      return make_json_response(500, nlohmann::json{ { "error", "db error" } });
    }

    nlohmann::json list = nlohmann::json::array();
    for (const auto& u : *users)
    {
      list.push_back(nlohmann::json{ { "username", u.username }, { "created_at", u.created_at } });
    }

    return make_json_response(200, nlohmann::json{ { "users", list } });
  }

  server::HttpResponse UserController::register_user(const server::HttpRequest& request) const
  {
    const auto creds = extract_credentials(request);
    if (!creds || creds->first.empty() || creds->second.empty())
    {
      return make_json_response(400, nlohmann::json{ { "error", "username and password required" } });
    }

    const auto res = user_service_.register_user(creds->first, creds->second, now_seconds());
    switch (res)
    {
    case services::RegisterResult::ok:
      return make_json_response(201, nlohmann::json{ { "message", "registered" } });
    case services::RegisterResult::exists:
      return make_json_response(409, nlohmann::json{ { "error", "user already exists" } });
    default:
      return make_json_response(500, nlohmann::json{ { "error", "db error" } });
    }
  }

  server::HttpResponse UserController::login_user(const server::HttpRequest& request) const
  {
    const auto creds = extract_credentials(request);
    if (!creds || creds->first.empty() || creds->second.empty())
    {
      return make_json_response(400, nlohmann::json{ { "error", "username and password required" } });
    }

    const auto res = user_service_.login_user(creds->first, creds->second, now_seconds());
    switch (res.status)
    {
    case services::LoginStatus::ok:
      return make_json_response(200, nlohmann::json{ { "token", res.token } });
    case services::LoginStatus::invalid_credentials:
      return make_json_response(401, nlohmann::json{ { "error", "invalid credentials" } });
    default:
      return make_json_response(500, nlohmann::json{ { "error", "failed to mint token" } });
    }
  }
}
