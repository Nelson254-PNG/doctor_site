#include "httplib.h"
#include "sqlite3.h"
#include <iostream>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

std::mutex fileMutex;
std::mutex dbMutex;
sqlite3* db = nullptr;
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

void execSQL(const char* sql){
  char* err = nullptr;
  if(sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK){
    std::cerr << "SQL error: " << err << std::endl;
    sqlite3_free(err);
  } 
}
// database setup

void seedSlots(){
  sqlite3_stmt* st;
  sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM slots", -1, &st, nullptr);
  sqlite3_step(st);
  int count = sqlite3_column_int(st, 0);
  sqlite3_finalize(st);
  if (count > 0) return;

  const char* times[] = {"09:00", "11:00", "14:00", "16:00"};
  for (int d = 0; d < 7; d++){
    std::time_t t = std::time(nullptr) + d * 86400;
    char date[16];
    std::strftime(date, sizeof(date), "%Y-%m-%d", std::localtime(&t));
    for (const char* tm : times){
      sqlite3_stmt* ins;
      sqlite3_prepare_v2(db, "INSERT OR IGNORE INTO slots (slot_date, slot_time) VALUES (?, ?)", -1, &ins, nullptr);
      sqlite3_bind_text(ins, 1, date, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(ins, 2, tm, -1, SQLITE_TRANSIENT);
      sqlite3_step(ins);
      sqlite3_finalize(ins);
    }
  }
}

void initDatabase(){
  std::filesystem::create_directories("data");
  if (sqlite3_open("data/clinic.db", &db) != SQLITE_OK){
    std::cerr << "Cannot open database\n";
    std::exit(1);
  }
  execSQL("CREATE TABLE IF NOT EXISTS slots ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " slot_date TEXT NOT NULL,"
        " slot_time TEXT NOT NULL,"
        " booked INTEGER NOT NULL DEFAULT 0,"
        " UNIQUE(slot_date, slot_time));");

  execSQL("CREATE TABLE IF NOT EXISTS bookings ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " slot_id INTEGER NOT NULL REFERENCES slots(id),"
        " name TEXT NOT NULL,"
        " phone TEXT NOT NULL,"
        " address TEXT NOT NULL,"
        " treatment TEXT NOT NULL,"
        " notes TEXT,"
        " created_at TEXT NOT NULL,"
        " status TEXT NOT NULL DEFAULT 'pending',"
        " payment TEXT NOT NULL DEFAULT 'unpaid');");
  seedSlots();            
}

int main(){
  initDatabase();
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
    if (name.empty() || phone.empty() || message.empty()){
      res.status = 400;
      res.set_content("{\"ok\":false,\"error\":\"Please fill in name, phone and your concern.\"}", "application/json");
      return;
    }

    std::lock_guard<std::mutex> lock(fileMutex);
    std::filesystem::create_directories("data");
    std::ofstream logFile("data/consultations.tsv", std::ios::app);
    logFile << now() << " | Name: " << name << " | Phone: " << phone << " | Email: " << email << " | Category: " << category << " | Message: " << message << std::endl;

    res.set_content("{\"ok\": \"true\"}", "application/json");
  });

  //lists free slots
  svr.Get("/api/slots", [](const httplib::Request&, httplib::Response& res){
    std::lock_guard<std::mutex> lock(dbMutex);
    sqlite3_stmt* st;
    sqlite3_prepare_v2(db, "SELECT slot_date, slot_time FROM slots WHERE booked = 0 AND slot_date >= date('now','localtime') ORDER BY slot_date, slot_time", -1, &st, nullptr);
    std::string json = "[";
    bool first = true;
    while (sqlite3_step(st) == SQLITE_ROW){
      if (!first) json += ",";
      first = false;
      json += "{\"id\":" + std::to_string(sqlite3_column_int(st, 0)) +
              ",\"date\":\"" + (const char*)sqlite3_column_text(st, 1) +
              "\",\"time\":\"" + (const char*)sqlite3_column_text(st, 2) + "\"}";
    
    }
    json += "]";
    sqlite3_finalize(st);
    res.set_content(json, "application/json");
  });

  // book a slot
  svr.Post("/api/book", [](const httplib::Request& req, httplib::Response& res){
    std::string name = clean(req.get_param_value("name"), 100);
    std::string phone = clean(req.get_header_value("phone"), 30);
    std::string address = clean(req.get_header_value("address"), 300);
    std::string treatment = clean(req.get_header_value("treatment"), 100);
    std::string notes = clean(req.get_header_value("notes"), 1000);

    int slotId = 0;
    try { slotId = std::stoi(req.get_header_value("slot_id")); } catch (...) {}
    if (name.empty() || phone.empty() || address.empty() || treatment.empty() || slotId <= 0){
      res.status = 400;
      res.set_content("{\"ok\":false,\"error\":\"Please fill in all required fields.\"}", "application/json");
      return;
    }

    std::lock_guard<std::mutex> lock(dbMutex);
    execSQL("BEGIN");

    //claim the slot only ifit is still free
    sqlite3_stmt* upd;
    sqlite3_prepare_v2(db, "UPDATE slots SET booked = 1 WHERE id = ? AND booked = 0", -1, &upd, nullptr);
    sqlite3_bind_int(upd, 1, slotId);
    sqlite3_step(upd);
    sqlite3_finalize(upd);

    if (sqlite3_changes(db) == 0){
      execSQL("ROLLBACK");
      res.status = 409;
      res.set_content("{\"ok\":false,\"error\":\"Sorry, that time was just taken. Please pick another.\"}", "application/json");
      return;
    }
    sqlite3_stmt* ins;
    sqlite3_prepare_v2(db, "INSERT INTO bookings (slot_id, name, phone, address, treatment, notes, created_at) VALUES (?, ?, ?, ?, ?, ?, ?)", -1, &ins, nullptr);
    sqlite3_bind_int(ins, 1, slotId);
    sqlite3_bind_text(ins, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 3, phone.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 4, address.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 5, treatment.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ins, 6, notes.c_str(), -1, SQLITE_TRANSIENT);
    std::string created = now();
    sqlite3_bind_text(ins, 7, created.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(ins);
    sqlite3_finalize(ins);

    execSQL("COMMIT");
    res.set_content("{\"ok\":true}", "application/json");
  }); 

  std::cout << "Server listening on http://localhost:8080" << std::endl;
  svr.listen("localhost", 8080);
}