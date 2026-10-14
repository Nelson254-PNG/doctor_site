#include "httplib.h"
#include <iostream>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

std::mutex fileMutex;
std::string clean(std::string s, size_t maxlen){
  if (s.size() > maxlen) s.resize(maxlen);
  for (char& c : s){
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  }
  return s;
}

std::string now() {
  std::time_t t = std::time(nullptr);
  char buf[32];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
  return buf;

}

int main(){
  httplib::Server svr;
  svr.set_mount_point("/", "./public");
  svr.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("{\"status\": \"ok\"}", "application/json");
  });
  svr.Post("/api/consultation", [](const httplib::Request& req, httplib::Response& res) {
    std::string name = clean(req.get_param_value("name"), 100);
    std::string phone = clean(req.get_param_value("phone"), 30);
    std::string email = clean(req.get_param_value("email"), 100);
    std::string category = clean(req.get_param_value("category"), 50);
    std::string message = clean(req.get_param_value("message"), 2000);

    std::lock_guard<std::mutex> lock(fileMutex);
    std::ofstream logFile("consultations.log", std::ios::app);
    logFile << now() << " | Name: " << name << " | Phone: " << phone << " | Email: " << email << " | Category: " << category << " | Message: " << message << std::endl;

    res.set_content("{\"status\": \"received\"}", "application/json");
  });
  std::cout << "Server listening on http://localhost:8080" << std::endl;
  svr.listen("localhost", 8080);
}