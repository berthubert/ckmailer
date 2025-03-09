#include <fmt/printf.h>
#include <fmt/ranges.h>
#include <fmt/chrono.h>
#include <fmt/os.h>
#include "support.hh"
#include "sqlwriter.hh"
#include "jsonhelper.hh"
#include "inja.hpp"
#include "argparse/argparse.hpp"
#include "pugixml.hpp"

#include "thingpool.hh"
#include <regex>
#define CPPHTTPLIB_USE_POLL
#define CPPHTTPLIB_THREAD_POOL_COUNT 32

#include "httplib.h"

using namespace std;


// english -> empty return
// dutch -> .nl

string bestLang(const auto& req)
{
  string lang = req.has_header("Accept-Language") ? req.get_header_value("Accept-Language") : "";
  if(lang.empty())
    return "";
  auto parts = splitString(lang, ",");
  fmt::print("langs: {}\n", parts);
  if(parts[0].find("nl") != string::npos)
    return ".nl";
  return "";
}

static auto prepRSS(auto& doc, const std::string& title, const std::string& desc, const std::string& baseURL)
{
  doc.append_attribute("standalone") = "yes";
  doc.append_attribute("version") = "1.0";
  
  doc.append_attribute("encoding") = "utf-8";
  pugi::xml_node rss = doc.append_child("rss");
  rss.append_attribute("version")="2.0";
  rss.append_attribute("xmlns:atom")="http://www.w3.org/2005/Atom";
  
  pugi::xml_node channel = rss.append_child("channel");
  channel.append_child("title").append_child(pugi::node_pcdata).set_value(title.c_str());
  channel.append_child("description").append_child(pugi::node_pcdata).set_value(desc.c_str());
  channel.append_child("link").append_child(pugi::node_pcdata).set_value(baseURL.c_str());
  channel.append_child("generator").append_child(pugi::node_pcdata).set_value("CKMailer");
  return channel;
}


int main(int argc, char** argv)
{
  signal(SIGPIPE, SIG_IGN); // every TCP application needs this

  argparse::ArgumentParser args("ckmserv", "0.0");
  args.add_argument("--smtp-server").help("IP address of SMTP smart host. If empty, no mail will get sent").default_value("");
  map<string, string> settings;
  args.add_argument("--base-url").help("Base URL of our website").default_value("").store_into(settings["base-url"]);
  args.add_argument("--save-settings").help("store settings from this command line to the database").flag();
  
  SQLiteWriter db("ckmailer.sqlite3", { {"users", {{"email", "collate nocase"}}}});
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

  if (args["--save-settings"] == true) {
    auto doSetting = [&](const std::string& name) {
      if(args.is_used("--"+name)) {
	string value = settings[name];
	cout<<"Storing --"<<name<< " as "<< value <<endl;
	db.addOrReplaceValue({{"name", name}, {"value", value}}, "settings");
      }
    };
    for(auto& [name, value] : settings) {
      doSetting(name);
    }

    db.queryT("delete from settings where value=''");
    return EXIT_SUCCESS;
  }

  auto getSetting = [&](const std::string& name) {
    if(!args.is_used("--"+name)) {
      auto rows = db.queryT("select value from settings where name=?", {name});
      if(!rows.empty()) {
	settings[name] = eget(rows[0], "value");
	cout<<name<<" " <<settings[name]<<endl;
      }
      
    }
  };
  for(auto& [name, value] : settings) {
    getSetting(name);
  }

  httplib::Server svr;
  svr.set_mount_point("/", "./html/");
  svr.set_keep_alive_max_count(1); // Default is 5
  svr.set_keep_alive_timeout(1);  // Default is 5
  ThingPool<SQLiteWriter> tp("ckmailer.sqlite3", SQLWFlag::NoTransactions);

  SQLiteWriter userdb("ckmailer.sqlite3", {
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


  svr.Get(R"(/start.html)", [&tp](const httplib::Request &req, httplib::Response &res) {
    string lang = bestLang(req);
    nlohmann::json data = nlohmann::json::object();
    inja::Environment e;
    e.set_html_autoescape(true); 

    data["highlight"] = req.get_param_value("hl");
    
    data["pagemeta"]["title"]= "Sign-in or sign-on";
    data["og"]["title"] = "Sign-on or sign-on";
    auto channels = tp.getLease()->queryT("select *, 'c'||rowid as cid from channels order by name");
    data["channels"] = packResultsJson(channels);
    data["lang"] = lang.empty() ? lang : lang.substr(1); // skip the .
    
    res.set_content(e.render_file("./partials/signinon" + lang+".html", data), "text/html");
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
      channels = tp.getLease()->queryT("select channels.id, name,description, 'c' || channels.rowid cid, channelid not null as subscribed from channels left join subscriptions on subscriptions.channelId = channels.id and subscriptions.userId = ? order by name",
				       {userId});
    }
    catch(std::exception& e) {
      cout<<"No subscriptions yet, only show channels: "<< e.what() << endl;
      channels = tp.getLease()->queryT("select *, 0 as subscribed, 'c'||rowid as cid from channels");
    }
    
    nlohmann::json data = nlohmann::json::object();
    inja::Environment e;
    e.set_html_autoescape(true); 
    data["timsi"]=timsi;
    data["highlight"] = req.get_param_value("hl");
    data["email"] = eget(user[0], "email");
    data["pagemeta"]["title"]="Account";
    data["og"]["title"] = "Account";
    data["channels"] = packResultsJson(channels);
    string lang = bestLang(req);
    data["lang"] = lang.empty() ? lang : lang.substr(1); // skip the .
    res.set_content(e.render_file("./partials/manage"+lang+".html", data), "text/html");
  });

  svr.Get(R"(/unsubscribe.html)", [&tp](const httplib::Request &req, httplib::Response &res) {
    string userId = req.get_param_value("userId");
    string channelId = req.get_param_value("channelId");

    auto channel = tp.getLease()->queryT("select name from channels where id=?", {channelId});
    auto user = tp.getLease()->queryT("select email from users where id=?", {userId});
    if(user.empty()) {
      res.status = 404;
      res.set_content(fmt::format("Could not find user {}\n", userId), "text/plain");
      return;
    }
    if(channel.empty()) {
      res.status = 404;
      res.set_content(fmt::format("Could not find channel {}\n", channelId), "text/plain");
      return;
    }
    
    nlohmann::json data = nlohmann::json::object();
    inja::Environment e;
    e.set_html_autoescape(true); 
    data["userId"]=userId;
    data["channelId"] = channelId;
    data["pagemeta"]["title"]="Unsubscribe";
    data["og"]["title"] = "Unsubscribe";
    auto subscription = tp.getLease()->queryT("select * from subscriptions where userId=? and channelId=?", {userId, channelId});
    data["subscribed"] = !subscription.empty();
    data["channelName"] = eget(channel.at(0), "name");

    data["email"] = eget(user.at(0), "email");
    string lang = bestLang(req);
    data["lang"] = lang.empty() ? lang : lang.substr(1); // skip the .
    
    res.set_content(e.render_file("./partials/unsubscribe.html", data), "text/html");
  });

  const string baseURL = settings["base-url"];
  svr.Get(R"(/channel.html)", [&tp, baseURL](const httplib::Request &req, httplib::Response &res) {
    string channelId = req.get_param_value("channelId");
    string lang = bestLang(req);
    
    auto channel = tp.getLease()->queryT("select * from channels where id=?", {channelId});
    if(channel.empty()) {
      res.status = 404;
      res.set_content(fmt::format("Could not find channel {}\n", channelId), "text/plain");
      return;
    }
    nlohmann::json data = nlohmann::json::object();
    inja::Environment e;
    e.set_html_autoescape(true); 
    data["channelId"] = channelId;
    data["pagemeta"]["title"]="Channel information";
    data["og"]["title"] = "Channel information";
    auto subscription = tp.getLease()->queryT("select * from subscriptions where channelId=?", {channelId});
    data["numsubscribers"] =subscription.size();
    data["channelName"] = eget(channel.at(0), "name");
    data["channelDescription"] = eget(channel.at(0), "description");
    data["rssURL"] = concatUrl(baseURL, "channel-index.xml?channelId="+ channelId);
    data["posts"] = packResultsJson(tp.getLease()->queryT("select * from launches where channelId=? order by timestamp desc", {channelId}));
    data["lang"] = lang.empty() ? lang : lang.substr(1); // skip the .
    res.set_content(e.render_file("./partials/channel"+lang+".html", data), "text/html");
  });

  
  svr.Post(R"(/unsubscribe/:userid/:channel)", [&tp](const httplib::Request &req, httplib::Response &res) {
    string userId = req.path_params.at("userid");
    string channelId = req.path_params.at("channel");
    tp.getLease()->queryT("delete from subscriptions where userId=? and channelId=?",
				       {userId, channelId});
    tp.getLease()->addValue({{"timestamp", time(0)}, {"action", "post-oneclick-unsubscribe"}, {"channelId", channelId}, {"userId", userId}}, "log");
    cout<<"Just post-oneclick-unsubscribed user id "<<userId<<" from channel "<<channelId<<endl;
  });

  // we get this when people *click* on the automated POST unsubscribe link..
  svr.Get(R"(/unsubscribe/:userid/:channel)", [&tp](const httplib::Request &req, httplib::Response &res) {
    string userId = req.path_params.at("userid");
    string channelId = req.path_params.at("channel");

    auto channel = tp.getLease()->queryT("select name from channels where id=?", {channelId});
    auto user = tp.getLease()->queryT("select email from users where id=?", {userId});
    if(user.empty()) {
      res.status = 404;
      res.set_content(fmt::format("Could not find user {}\n", userId), "text/plain");
      return;
    }
    if(channel.empty()) {
      res.status = 404;
      res.set_content(fmt::format("Could not find channel {}\n", channelId), "text/plain");
      return;
    }

    res.status = 301;
    res.set_header("Location", "../../unsubscribe.html?userId="+userId+"&channelId="+channelId);
  });

  // https://berthub.eu/tkconv/search.html?q=bert+hubert&twomonths=false&soorten=alles
  svr.Get("/channel-index.xml", [&tp, baseURL](const httplib::Request &req, httplib::Response &res) {
    string channelId;
    string channelName= "all";
    
    if(req.has_param("channelId")) {
      channelId = req.get_param_value("channelId");

      auto chan = tp.getLease()->queryT("select * from channels where id=?", {channelId});
      if(chan.empty()) {
	res.status = 404;
	res.set_content(fmt::format("Could not find channel {}\n", channelId), "text/plain");
	return;
      }
      channelName=eget(chan[0],"name");
    }

    
    auto posts = channelId.empty() ?
      tp.getLease()->queryT("select * from launches,channels where channels.id=launches.channelId order by timestamp desc") : 
      tp.getLease()->queryT("select * from launches,channels where channels.id=launches.channelId and channelId=? order by timestamp desc", {channelId});

    pugi::xml_document doc;
    pugi::xml_node channel = prepRSS(doc, "Channel "+ channelName, "Channel "+ channelName, baseURL);
    
    bool first = true;
    
    
    for(auto& p : posts) {
      pugi::xml_node item = channel.append_child("item");
      string onderwerp = eget(p, "subject");
      item.append_child("title").append_child(pugi::node_pcdata).set_value(onderwerp.c_str());
      onderwerp = eget(p, "name")+" | " +onderwerp;
      item.append_child("description").append_child(pugi::node_pcdata).set_value(onderwerp.c_str());

      
      item.append_child("link").append_child(pugi::node_pcdata).set_value(
									  concatUrl(baseURL, eget(p,"msgId")).c_str());
      item.append_child("guid").append_child(pugi::node_pcdata).set_value(("ckmailer_"+eget(p, "msgId")).c_str());

      // 2024-12-06T06:01:10.2530000
      time_t then = std::get<int64_t>(p["timestamp"]);
     
      //      <pubDate>Fri, 13 Dec 2024 14:13:41 +0000</pubDate>
      string date = fmt::format("{:%a, %d %b %Y %H:%M:%S %z}", fmt::localtime(then));
      item.append_child("pubDate").append_child(pugi::node_pcdata).set_value(date.c_str());

      if(first) {
	channel.prepend_child("lastBuildDate").append_child(pugi::node_pcdata).set_value(date.c_str());
	first=false;
      }
      
    }

    if(first) {
      string date = fmt::format("{:%a, %d %b %Y %H:%M:%S %z}", fmt::localtime(time(0)));
      channel.append_child("pubDate").append_child(pugi::node_pcdata).set_value(date.c_str());
    }
    
    ostringstream str;
    doc.save(str);
    res.set_content(str.str(), "application/xml");
  });

  
  svr.Get(R"(/)", [&tp](const httplib::Request &req, httplib::Response &res) {
    res.status = 302; // temporary
    res.set_header("Location", "start.html");
  });
  
  svr.Post(R"(/change-subscription)", [&tp](const httplib::Request &req, httplib::Response &res) {
    string timsi = req.get_file_value("timsi").content;
    string channelId = req.get_file_value("channelid").content;
    string to = req.get_file_value("to").content;
    //    cout<<"Got change request, to: "<<to<<endl;
    auto users = tp.getLease()->queryT("select * from users where timsi=?", {timsi});
    nlohmann::json j;
    if(users.empty()) {
      j["ok"]=0;
      j["message"] = "No such user";
      res.set_content(j.dump(), "application/json");
      return;
    }
    auto channels = tp.getLease()->queryT("select * from channels where id=?", {channelId});
    if(users.empty()) {
      j["ok"]=0;
      j["message"] = "No such channel";
      res.set_content(j.dump(), "application/json");
      return;
    }
    string email = eget(users[0], "email");
    string userId = eget(users[0], "id");
    string channelName = eget(channels[0], "name");
    if(to=="subscribed") {
      cout<<"Trying to subscribe "<<userId<<" "<<email <<" to "<< channelName << " " <<channelId<<endl;
      tp.getLease()->addOrReplaceValue({{"userId", userId}, {"channelId", channelId}}, "subscriptions");
      j["newstate"]="subscribed";
    }
    else {
      cout<<"Trying to unsubscribe "<<userId<<" " << email <<" from "<<channelName<<" " <<channelId<<endl;
      tp.getLease()->queryT("delete from subscriptions where userid=? and channelid=?", {
	  userId, channelId});
      j["newstate"]="unsubscribed";
    }
    j["ok"]=1;

    tp.getLease()->addValue({{"timestamp", time(0)}, {"action", "change-subscribe"}, {"userId", userId}, {"email", email}, {"channelName", channelName}, {"channelId", channelId}, 
			     {"newstate", (string)j["newstate"]}}, "log");
    
    res.set_content(j.dump(), "application/json");
  });  

  svr.Post(R"(/send-user-account-link)", [&tp](const httplib::Request &req, httplib::Response &res) {
    string email = req.get_file_value("email").content;
    string highlight = req.get_file_value("highlight").content;
    nlohmann::json j;

    std::regex re("c[0-9]*");
    if(!highlight.empty() && !regex_match(highlight, re)) {
      j["ok"]=0;
      j["message"] = "Invalid channeld id";
      res.set_content(j.dump(), "application/json");
      return;
    }
    
    if(email.empty()) {
      j["ok"]=0;
      j["message"] = "User field empty";
      res.set_content(j.dump(), "application/json");
      return;
    }
    string lang = bestLang(req); // .nl dutch, empty string for english    
    cout << "Possible new user "<<email<<", lang '"<<lang<<"'"<<endl;
    auto db = tp.getLease();
    auto user = db->queryT("select * from users where email=?", {email});
    string timsi;
    bool newuser=false;
    string userId;
    bool created = false;
    if(user.empty()) {
      cout<<"New user!"<<endl;
      newuser=true;
      timsi = getLargeId();
      userId = getLargeId();
      db->addValue({{"id", userId}, {"timsi", timsi}, {"email", email}}, "users");
      created=true;
    }
    else {
      userId = eget(user[0], "id");
      timsi = eget(user[0], "timsi");
      created = false;
    }
    
    string url = "https://berthub.eu/ckmailer/manage.html?timsi=" +timsi;
    if(!highlight.empty()) {
      url += "&hl="+highlight;
    }
    string textmsg, htmlmsg, subject;

    if(newuser) {
      if(lang==".nl") {
 	textmsg="Om je account te activeren, klik hier: "+url+" en selecteer dan in de webbrowser\nwelke nieuwsbrieven je wil ontvangen. Met vriendelijke groeten,\nCKMailer\n";
	htmlmsg="<p>Om je account te activeren, klik hier <a href=\""+url+"\">"+url+"</a> en selecteer dan in de webbrowser welke nieuwsbrieven je wil ontvangen.</p><p> Met vriendelijke groeten,<br>CKMailer</p>";
	
	subject = "Activatielink voor je account op berthub.eu/ckmailer";
      }
      else {
	textmsg="To activate your account, click here: "+url+" and on the linked page, select\nwhich mailing lists you want to subscribe to. With kind regards,\nCKMailer\n";
	htmlmsg="<p>To activate your account, click here: <a href=\""+url+"\">"+url+"</a> and on the linked page select which mailing lists you want to subscribe to.</p><p>With kind regards,<br>CKMailer</p>";

	subject = "Activate your account on berthub.eu/ckmailer";

      }
    }
    else {
      if(lang==".nl") {
	textmsg="Om je account te beheren, klik hier: "+url+" en selecteer dan in de webbrowser\nwelke nieuwsbrieven je wil ontvangen. Met vriendelijke groeten,\nCKMailer\n";
	htmlmsg="<p>Om je account te beheren, klik hier <a href=\""+url+"\">"+url+"</a> en selecteer dan in de webbrowser welke nieuwsbrieven je wil ontvangen.</p><p> Met vriendelijke groeten,<br>CKMailer</p>";
	subject = "Beheer je account op berthub.eu/ckmailer";
      }
      else {
	textmsg="To manage your account, click here: "+url+" and on the linked page\nselect which mailing lists you want to receive. With kind regards,\nCKMailer\n";
	htmlmsg="<p>To manage your account, click here <a href=\""+url+"\">"+url+"</a> and on the linked page, select which mailing lists you want to receive.</p><p>With kind regards,<br>CKMailer</p>";
	subject = "Manage your account on berthub.eu/ckmailer";
      }
    }
    tp.getLease()->addValue({{"timestamp", time(0)}, {"action", "user-invite"}, {"userId", userId}, {"email", email}, {"created", created}, {"lang", lang}}, "log");
    sendEmail("10.0.0.2",  // system setting
	      "bert@hubertnet.nl", // channel setting really
	      email,
	      subject,
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
    fmt::print("Req: {} {} {} {} max-db {}\n", req.path, req.params,
	       req.has_header("User-Agent") ? req.get_header_value("User-Agent") : "",
	       req.has_header("Accept-Language") ? req.get_header_value("Accept-Language") : "",
	       (unsigned int)tp.d_maxout);
    return httplib::Server::HandlerResponse::Unhandled;
  });
  
  svr.set_socket_options([](socket_t sock) {
    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
	       reinterpret_cast<const void *>(&yes), sizeof(yes));
  });

  cout<<"Going live on http://127.0.0.1:1848/\n";
  svr.listen("0.0.0.0", 1848);
}
