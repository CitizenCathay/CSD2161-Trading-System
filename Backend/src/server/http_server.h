#pragma once

#include "server/router.h"

namespace server
{
  class HttpServer
  {
  public:
    explicit HttpServer(unsigned short port);
    int run(const Router& router) const;

  private:
    unsigned short port_{};
  };

  class HttpView
  {
  public:
    static std::string to_wire(const HttpResponse& response);

  private:
    static std::string status_text(int status_code);
  };
}

