#include "httplib.h"
#include <iostream>
int main(){
  httplib::Server svr;
  svr.set_mount_point("/", "./public");
  svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("{\"status\": \"ok\"}", "application/json");
  });
  std::cout << "Server listening on http://localhost:8080" << std::endl;
  svr.listen("localhost", 8080);
}