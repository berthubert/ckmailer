#include <fmt/printf.h>
#include <fmt/ranges.h>
#include <fmt/os.h>
#include "support.hh"
#include "sqlwriter.hh"
#include "jsonhelper.hh"
#include "inja.hpp"
#include "argparse/argparse.hpp"

#include "thingpool.hh"
#include <regex>
#define CPPHTTPLIB_USE_POLL
#define CPPHTTPLIB_THREAD_POOL_COUNT 32

#include "httplib.h"

using namespace std;

int main(int argc, char** argv)
{
  signal(SIGPIPE, SIG_IGN); // every TCP application needs this

  argparse::ArgumentParser args("ckmserv", "0.0");
  args.add_argument("--smtp-server").help("IP address of SMTP smart host. If empty, no mail will get sent").default_value("");
  
  SQLiteWriter db("bmail.sqlite3", { {"users", {{"email", "collate nocase"}}}});
  try {
    db.queryT("create unique index if not exists emailidx on users(email)");
  }catch(...)
    {
      cout<< "Could not yet create index on email db, is ok\n"<< endl;
    }
  
  try {
    args.parse_args(argc, argv);
  }
  catch (const std::runtime_error& err) {
    std::cout << err.what() << std::endl << args;
    std::exit(1);
  }

  httplib::Server svr;
  svr.set_mount_point("/", "./html/");
  svr.set_keep_alive_max_count(1); // Default is 5
  svr.set_keep_alive_timeout(1);  // Default is 5
  ThingPool<SQLiteWriter> tp("bmail.sqlite3");

  SQLiteWriter userdb("bmail.sqlite3", {
      {"subscriptions",
       {
	 {"userId", "NOT NULL REFERENCES users(id) ON DELETE CASCADE"},
	 {"channelId", "NOT NULL REFERENCES channel(id) ON DELETE CASCADE"}
       }
      },
      {"users",
       {
	 {"id", "PRIMARY KEY"},
	 {"email", "collate nocase"}
       }
      },
      {"channels",
       {
	 {"id", "PRIMARY KEY"}
       }
      }
    } );

  
  
  svr.Get(R"(/msg/:msgid)", [&tp](const httplib::Request &req, httplib::Response &res) {
    cout<<"Called!"<<endl;
    string msgid = req.path_params.at("msgid");
    auto pages = tp.getLease()->query("select webVersion from msgs where id=?", {msgid});
    if(pages.empty()) {
      res.status = 404;
      res.set_content(fmt::format("No such message {}", msgid), "text/plain");
      return;
    }
    cout<<"Got "<<pages.size()<<" pages";
    res.set_content(pages[0]["webversion"], "text/html");
  });

  svr.Get(R"(/signinon.html)", [&tp](const httplib::Request &req, httplib::Response &res) {
    nlohmann::json data = nlohmann::json::object();
    inja::Environment e;
    e.set_html_autoescape(true); 
    
    data["pagemeta"]["title"]="Sign-in or sign-on";
    data["og"]["title"] = "Sign-on or sign-on";
    auto channels = tp.getLease()->queryT("select * from channels");
    data["channels"] = packResultsJson(channels);
    
    res.set_content(e.render_file("./partials/signinon.html", data), "text/html");
  });

  svr.Get(R"(/start.html)", [&tp](const httplib::Request &req, httplib::Response &res) {
    nlohmann::json data = nlohmann::json::object();
    inja::Environment e;
    e.set_html_autoescape(true); 
    
    data["pagemeta"]["title"]="Sign-in or sign-on";
    data["og"]["title"] = "Sign-on or sign-on";
    auto channels = tp.getLease()->queryT("select * from channels order by name");
    data["channels"] = packResultsJson(channels);
    
    res.set_content(e.render_file("./partials/signinon.html", data), "text/html");
  });

  
  svr.Get(R"(/manage.html)", [&tp](const httplib::Request &req, httplib::Response &res) {
    string timsi = req.get_param_value("timsi");
    auto user = tp.getLease()->queryT("select * from users where timsi=?", {timsi});
    if(user.empty()) {
      res.status = 404;
      res.set_content(fmt::format("No such user with timsi {}", timsi), "text/plain");
      return;
    }
    string userId = eget(user[0], "id");
    decltype(user) channels;
    try {
      channels = tp.getLease()->queryT("select channels.id, name,description,channelid not null as subscribed from channels left join subscriptions on subscriptions.channelId = channels.id and subscriptions.userId = ? order by name",
				       {userId});
      cout<<"Got " << channels.size()<<" channels\n";
    }
    catch(std::exception& e) {
      cout<<"No subscriptions yet, only show channels: "<< e.what() << endl;
      channels = tp.getLease()->queryT("select *, 0 as subscribed from channels");
    }
    
    nlohmann::json data = nlohmann::json::object();
    inja::Environment e;
    e.set_html_autoescape(true); 
    data["timsi"]=timsi;
    data["email"] = eget(user[0], "email");
    data["pagemeta"]["title"]="Account";
    data["og"]["title"] = "Account";
    data["channels"] = packResultsJson(channels);
    cout << packResultsJsonStr(channels) << endl;
    res.set_content(e.render_file("./partials/manage.html", data), "text/html");
  });

  svr.Post(R"(/unsubscribe/:userid/:channel)", [&tp](const httplib::Request &req, httplib::Response &res) {
    string userId = req.path_params.at("userid");
    string channelId = req.path_params.at("channel");
    tp.getLease()->queryT("delete from subscriptions where userId=? and channelId=?",
				       {userId, channelId});
    cout<<"Just unsubscribed user id "<<userId<<" from channel "<<channelId<<endl;
  });

  
  svr.Post(R"(/change-subscription)", [&tp](const httplib::Request &req, httplib::Response &res) {
    string timsi = req.get_file_value("timsi").content;
    string channelId = req.get_file_value("channelid").content;
    string to = req.get_file_value("to").content;
    cout<<"Got change request, to: "<<to<<endl;
    auto users = tp.getLease()->queryT("select * from users where timsi=?", {timsi});
    nlohmann::json j;
    if(users.empty()) {
      j["ok"]=0;
      j["message"] = "No such user";
      res.set_content(j.dump(), "application/json");
      return;
    }
    string userId = eget(users[0], "id");
    if(to=="subscribed") {
      cout<<"Trying to subscribe "<<userId<<" "<<eget(users[0], "email") <<" to "<<channelId<<endl;
      tp.getLease()->addOrReplaceValue({{"userId", userId}, {"channelId", channelId}}, "subscriptions");
      j["newstate"]="subscribed";
    }
    else {
      cout<<"Trying to unsubscribe "<<userId<<" " <<eget(users[0], "email")<<" from "<<channelId<<endl;
      tp.getLease()->queryT("delete from subscriptions where userid=? and channelid=?", {
	  userId, channelId});
      j["newstate"]="unsubscribed";
    }
    j["ok"]=1;
    res.set_content(j.dump(), "application/json");
  });
  

  svr.Post(R"(/send-user-account-link)", [&tp](const httplib::Request &req, httplib::Response &res) {
    string email = req.get_file_value("email").content;
    nlohmann::json j;
    
    if(email.empty()) {
      j["ok"]=0;
      j["message"] = "User field empty";
      res.set_content(j.dump(), "application/json");
      return;
    }
    
    cout << "Possible new user "<<email<<endl;
    auto db = tp.getLease();
    auto user = db->queryT("select * from users where email=?", {email});
    string timsi;
    bool newuser=false;
    
    if(user.empty()) {
      newuser=true;
      timsi = getLargeId();
      db->addValue({{"id", getLargeId()}, {"timsi", timsi}, {"email", email}}, "users");
    }
    else
      timsi = eget(user[0], "timsi");
    
    string url = "https://berthub.eu/ckmailer/manage.html?timsi=" +timsi;
    string textmsg, htmlmsg;
    if(newuser) {
      textmsg="Om je account te activeren, klik hier: "+url+" en selecteer dan in de webbrowser\nwelke nieuwsbrieven je wil ontvangen. Met vriendelijke groeten,\nCKMailer\n";
      htmlmsg="<p>Om je account te activeren, klik hier <a href=\""+url+"\">"+url+"</a> en selecteer dan in de webbrowser welke nieuwsbrieven je wil ontvangen.</p><p> Met vriendelijke groeten,<br>CKMailer</p>";
    }
    else {
      textmsg="Om je account te beheren, klik hier: "+url+" en selecteer dan in de webbrowser\nwelke nieuwsbrieven je wil ontvangen. Met vriendelijke groeten,\nCKMailer\n";
      htmlmsg="<p>Om je account te beheren, klik hier <a href=\""+url+"\">"+url+"</a> en selecteer dan in de webbrowser welke nieuwsbrieven je wil ontvangen.</p><p> Met vriendelijke groeten,<br>CKMailer</p>";
    }
    
    sendEmail("10.0.0.2",  // system setting
	      "bert@hubertnet.nl", // channel setting really
	      email,        
	      newuser ? "Activatielink voor je account op berthub.eu/ckmailer" :
	      "Beheer je account op berthub.eu/ckmailer" , // subject
	      textmsg,
	      htmlmsg);
    
    j["ok"]=1;
    res.set_content(j.dump(), "application/json");
  });

  
  svr.set_exception_handler([](const auto& req, auto& res, std::exception_ptr ep) {
    auto fmt = "<h1>Error 500</h1><p>%s</p>";
    string buf;
    try {
      std::rethrow_exception(ep);
    } catch (std::exception &e) {
      buf = fmt::sprintf(fmt, htmlEscape(e.what()).c_str());
    } catch (...) { // See the following NOTE
      buf = fmt::sprintf(fmt, "Unknown exception");
    }
    cout<<"Error: '"<<buf<<"'"<<endl;
    res.set_content(buf, "text/html");
    res.status = 500; 
  });
  
  svr.set_pre_routing_handler([&tp](const auto& req, auto& res) {
    fmt::print("Req: {} {} {} max-db {}\n", req.path, req.params, req.has_header("User-Agent") ? req.get_header_value("User-Agent") : "",
	       (unsigned int)tp.d_maxout);
    return httplib::Server::HandlerResponse::Unhandled;
  });
  
  
  svr.listen("0.0.0.0", 1848);
}
