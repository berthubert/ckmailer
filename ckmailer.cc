#include <fmt/printf.h>
#include <fmt/ranges.h>
#include <fmt/os.h>
#include "support.hh"
#include "sqlwriter.hh"
#include "inja.hpp"
#include "argparse/argparse.hpp"
#include <regex>
#include <signal.h>
using namespace std;

/*
  First you make a 'message', which is markdown
  A message can be delivered directly to an email address
  Or it can be delivered to a channel

  There is an email sending infra which needs to turn the markdown into:
    text
    html
  Both affixed with an unsubscribe link. The text version also needs an HTML link.

  To do so, we take the markdown and add an inja {{ unsubscribeLink }} to the end.

  THEN we turn the markdown into html and text.
*/

string markdownToX(const std::string& input, const std::string& options)
{
  string tmp = getLargeId()+".md";
  {
    auto out = fmt::output_file(tmp);
    out.print("{}", input);
  }
  string command = "pandoc -f markdown " + options +" < " + tmp;
  
  FILE* pfp = popen(command.c_str(), "r");
  if(!pfp)
    throw runtime_error("Unable to perform conversion for '"+command+"': "+string(strerror(errno)));
  
  shared_ptr<FILE> fp(pfp, pclose);
  char buffer[4096];
  string ret;
  for(;;) {
    int len = fread(buffer, 1, sizeof(buffer), fp.get());
    if(!len)
      break;
    ret.append(buffer, len);
  }
  if(ferror(fp.get()))
    throw runtime_error("Unable to perform markdownToText: "+string(strerror(errno)));

  unlink(tmp.c_str());
  return ret;
}

string htmlToText(const std::string& input)
{
  string tmp = getLargeId()+".html";
  {
    auto out = fmt::output_file(tmp);
    out.print("{}", input);
  }
  string command = "links -html-numbered-links 1 -dump file://"+tmp;
  
  FILE* pfp = popen(command.c_str(), "r");
  if(!pfp)
    throw runtime_error("Unable to perform conversion for '"+command+"': "+string(strerror(errno)));
  
  shared_ptr<FILE> fp(pfp, pclose);
  char buffer[4096];
  string ret;
  for(;;) {
    int len = fread(buffer, 1, sizeof(buffer), fp.get());
    if(!len)
      break;
    ret.append(buffer, len);
  }
  if(ferror(fp.get()))
    throw runtime_error("Unable to perform html2text: "+string(strerror(errno)));

  unlink(tmp.c_str());
  return ret;
}


string markdownToText(const std::string& input)
{
  string html =  markdownToX(input, "-t html");
  return htmlToText(html);
}

string markdownToHTML(const std::string& input)
{
  return markdownToX(input, "-t html");
}
// don't put " in the title
string markdownToWeb(const std::string& input, const std::string& title)
{
  return markdownToX(input, "-t html -s --embed-resources --metadata title=\""+title+"\"");
}


int main(int argc, char** argv)
{
  signal(SIGPIPE, SIG_IGN); // every TCP application needs this
  argparse::ArgumentParser args("bmailer", "0.0");
  map<string, string> settings;
  args.add_argument("--smtp-server").help("IP address of SMTP smart host. If empty, no mail will get sent").default_value("").store_into(settings["smtp-server"]);
  args.add_argument("--imap-server").help("IMAP server to query").default_value("").store_into(settings["imap-server"]);
  args.add_argument("--imap-user").help("IMAP server to query").default_value("").store_into(settings["imap-user"]);
  args.add_argument("--imap-password").help("IMAP server to query").default_value("").store_into(settings["imap-password"]);
    
  args.add_argument("--sender-email").help("From address of email we send").default_value("").store_into(settings["sender-email"]);
  args.add_argument("--save-settings").help("store settings from this command line to the database").flag();
  
  argparse::ArgumentParser channel_command("channel");
  channel_command.add_description("Manage channels");

  argparse::ArgumentParser channel_create_command("create");
  channel_create_command.add_description("Create a new channel");
  channel_create_command.add_argument("name").help("a short name for the channel").required();
  channel_create_command.add_argument("description").help("a slightly longer description").required();
  channel_command.add_subparser(channel_create_command);

  argparse::ArgumentParser channel_list_command("list");
  channel_list_command.add_description("List all channels");
  channel_command.add_subparser(channel_list_command);

  argparse::ArgumentParser channel_ls_command("ls");
  channel_ls_command.add_description("List all channels");
  channel_command.add_subparser(channel_ls_command);

  args.add_subparser(channel_command);

  argparse::ArgumentParser user_command("user");
  user_command.add_description("Manage users");

  argparse::ArgumentParser user_add_command("add");
  user_add_command.add_description("Add a new user");
  user_add_command.add_argument("email").help("the user's email address").required();
  user_command.add_subparser(user_add_command);

  argparse::ArgumentParser user_subscribe_command("subscribe");
  user_subscribe_command.add_description("Subscribe an email address to a channel");
  user_subscribe_command.add_argument("email").help("the user's email address").required();
  user_subscribe_command.add_argument("channel").help("a channel identifier like c2").required();
  user_command.add_subparser(user_subscribe_command);

  argparse::ArgumentParser user_list_command("list");
  user_list_command.add_description("List all users");
  user_command.add_subparser(user_list_command);

  argparse::ArgumentParser user_ls_command("ls");
  user_ls_command.add_description("List all users");
  user_command.add_subparser(user_ls_command);

  args.add_subparser(user_command);

  argparse::ArgumentParser msg_command("msg");
  msg_command.add_description("Manage msgs");

  argparse::ArgumentParser msg_read_command("read");
  msg_read_command.add_description("Read a Markdown file into the database as a message");
  msg_read_command.add_argument("filename").help("file containing a body in Markdown").required();
  msg_read_command.add_argument("language").help("language the message is written in").choices("nl", "en").required();
  msg_command.add_subparser(msg_read_command);

  argparse::ArgumentParser msg_list_command("list");
  msg_list_command.add_description("List all messages");
  msg_command.add_subparser(msg_list_command);

  argparse::ArgumentParser msg_ls_command("ls");
  msg_ls_command.add_description("List all messages");
  msg_command.add_subparser(msg_ls_command);

  argparse::ArgumentParser msg_send_command("send");
  msg_send_command.add_description("Send a message to an email address immediately");
  msg_send_command.add_argument("message").help("a message identifier like m2").required();
  msg_send_command.add_argument("destination").help("an email address").required();
  msg_command.add_subparser(msg_send_command);

  argparse::ArgumentParser msg_launch_command("launch");
  msg_launch_command.add_description("Queue a message to be sent to all subscribers");
  msg_launch_command.add_argument("message").help("a message identifier like m2").required();
  msg_launch_command.add_argument("channel").help("a channel identifier like c2").required();
  msg_launch_command.add_argument("subject").help("the message subject").required();
  msg_command.add_subparser(msg_launch_command);
  
  args.add_subparser(msg_command);

  argparse::ArgumentParser init_command("init");
  init_command.add_description("Initialize database");
  args.add_subparser(init_command);

  argparse::ArgumentParser queue_command("queue");
  queue_command.add_description("Queue management");

  argparse::ArgumentParser queue_list_command("list");
  queue_list_command.add_description("List all queued messages");
  queue_command.add_subparser(queue_list_command);

  argparse::ArgumentParser queue_ls_command("ls");
  queue_ls_command.add_description("List all queued messages");
  queue_command.add_subparser(queue_ls_command);

  argparse::ArgumentParser queue_run_command("run");
  queue_run_command.add_description("Send all messages in the queue");
  queue_command.add_subparser(queue_run_command);

  args.add_subparser(queue_command);

  argparse::ArgumentParser imap_command("imap");
  imap_command.add_description("Imap management");
  imap_command.add_argument("--active").help("act on messages, don't just observe").flag();
  args.add_subparser(imap_command);

  
  SQLiteWriter db("ckmailer.sqlite3", {
      {"subscriptions",
       {
	 {"userId", "NOT NULL REFERENCES users(id) ON DELETE CASCADE"},
	 {"channelId", "NOT NULL REFERENCES channels(id) ON DELETE CASCADE"}
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
      },
      {"settings",
	  {
	    {"name", "PRIMARY KEY"}
	  }
      }
    }, SQLWFlag::NoTransactions );

  try {
    string userId=getLargeId(), channelId=getLargeId();
    db.addValue({{"id", userId}, {"timsi", getLargeId()}, {"email", getLargeId()}}, "users");
    db.queryT("create unique index if not exists emailidx on users(email)");
    db.addValue({{"id", channelId}, {"name", "naam"}}, "channels");
    db.addOrReplaceValue({{"userId", userId}, {"channelId", channelId}}, "subscriptions");

    db.addOrReplaceValue({{"name", ""}, {"value", ""}}, "settings");
    db.queryT("delete from settings where name=''");
    
    db.queryT("create unique index if not exists subindex on subscriptions(userId, channelId)");
    db.queryT("delete from users where id=?", {userId});
    db.queryT("delete from channels where id=?", {channelId});
    
  }catch(std::exception& e)
    {
      cout<< "Error during init: "<<e.what()<<endl;
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
	//	cout<<"Retrieved "<<name<<" from database"<<endl;
	settings[name] = eget(rows[0], "value");
      }
      
    }
  };
  for(auto& [name, value] : settings) {
    getSetting(name);
  }
  
  fmt::print("settings: {}\n", settings);
  if(args.is_subcommand_used(user_command)) {
    if(user_command.is_subcommand_used(user_add_command)) {
      db.addValue({{"id", getLargeId()}, {"timsi", getLargeId()}, {"email", user_add_command.get("email")}}, "users");
    }
    else if(user_command.is_subcommand_used(user_subscribe_command)) {
      string email = user_subscribe_command.get("email");
      string channel = user_subscribe_command.get("channel");
      auto user = db.queryT("select * from users where email=?", {email});
      string userId;
      if(user.empty()) {
	userId=getLargeId();
	db.addValue({{"id", userId}, {"timsi", getLargeId()}, {"email", email}}, "users");
	cout << "Created user"<<endl;
      }
      else
	userId = eget(user[0], "id");

      int64_t rowid=atoi(channel.substr(1).c_str());
      auto channelr = db.queryT("select * from channels where rowid=?", {rowid});
      if(channelr.empty()) {
	cout<<"Can't find channel "<<channel<<endl;
	return EXIT_FAILURE;
      }
      db.addOrReplaceValue({{"userId", userId}, {"channelId", eget(channelr[0], "id")}}, "subscriptions");
      cout<<"User is now subscribed to "<<eget(channelr[0], "name")<<endl;
    }
    else if(user_command.is_subcommand_used(user_list_command) || user_command.is_subcommand_used(user_ls_command)) {
      auto rows = db.query("select rowid,* from users");
      for(auto& r : rows)
	cout << 'u'<< r["rowid"] << '\t' << r["email"]<< '\t' << r["id"]<<'\n';
    }
    else
      cout<<user_command<<endl;
  }
  else if(args.is_subcommand_used(msg_command)) {
    if(msg_command.is_subcommand_used(msg_read_command)) {
      string filename = msg_read_command.get("filename");
      string markdown = getContentsOfFile(filename);
      string lang = msg_read_command.get("language");

      // no prefix, no postfix, no replacements
      string webVersion = markdownToWeb(markdown, "Een CKMailer nieuwsbrief / a CKMailer newsletter");
      
      regex img_regex(R"(!\[\]\(([^)]*)\))");  // XXX needs to be adjusted for captions!!
      auto begin = 
        std::sregex_iterator(markdown.begin(), markdown.end(), img_regex);
      auto end = std::sregex_iterator();
      struct item
      {
	string cid;
	string newlink;
	string fname;
      };
      map<string,item> repl;
      for (std::sregex_iterator i = begin; i != end; ++i) {
	cout << "Hit: "<<i->str() <<" " <<i->size()<< " " << (*i)[1].str() << endl;
	string cid = getLargeId();
	repl[i->str()]={cid, "![](cid:"+cid+")", (*i)[1].str()};
      }
      for(auto& [from, to] : repl) {
	replaceSubstring(markdown, from, to.newlink);
      }
      //      cout<<"Markdown now:\n"<<markdown<<endl;
      
      string textVersion;
      if(lang == "nl")
	textVersion = "Klik op {{weblink}} om deze mail op het web te bekijken\n\n";
      else
	textVersion = "Click here {{weblink}} to view this message on the web\n\n";
      textVersion += markdownToText(markdown);

      if(lang == "nl") 
	textVersion += "\nKlik op {{unsubscribelink}} om je af te melden voor de email lijst {{channelName}} of om je abonnementen te beheren. Op {{channelLink}} kan je het archief fincen, en kunnen aneren zich ook inschrijven.\n";
      else
	textVersion += "\nClick here {{unsubscribelink}} to unsubscribe from list {{channelName}} or to manage your subcriptions. On {{channelLink}} you'll find the archive, and links for other people to subscribe to the list.\n";
      
      string htmlVersion = markdownToHTML(markdown);

      if(lang =="nl") 
	htmlVersion += "\n<p>Klik <a href=\"{{unsubscribelink}}\">hier</a> om je af te melden van lijst {{channelName}} of om je abonnementen te beheren. Op <a href=\"{{channelLink}}\">deze pagina</a> kan je het archief vinden, en kunnen anderen zich ook inschrijven.\n</p>";
      else
	htmlVersion += "\n<p>Click <a href=\"{{unsubscribelink}}\">here</a> to unsubscribe from list {{channelName}} or to manage your subscriptions. On <a href=\"{{channelLink}}\">this page</a> you'll find the archive, and links for other people to subscribe to the list.\n</p>";	

      string id = getLargeId();
      db.addValue({{"id", id}, {"markdown", markdown}, {"textversion", textVersion}, {"htmlversion", htmlVersion}, {"webversion", webVersion}}, "msgs");
      auto res = db.query("select last_insert_rowid() rid");
      cout<<"created new message m"<<res[0]["rid"]<<", https://berthub.eu/ckmailer/msg/"<<id<<endl;

      for(auto& [from, to] : repl) {
	db.addValue({{"id", to.cid}, {"msgId", id}, {"filename", to.fname}}, "attachments");
      }
    }
    else if(msg_command.is_subcommand_used(msg_list_command) || msg_command.is_subcommand_used(msg_ls_command)) {
      auto rows = db.query("select rowid,* from msgs");
      for(auto& r : rows)
	cout << 'm'<< r["rowid"] << '\t' << r["markdown"].length()<< '\t' << r["id"]<<'\n';
    }
    else if(msg_command.is_subcommand_used(msg_send_command)) {
      string rowid = msg_send_command.get("message").substr(1);
      string dest = msg_send_command.get("destination");
      auto rows = db.query("select * from msgs where rowid=?", {rowid});

      nlohmann::json data = nlohmann::json::object();
      inja::Environment e;
      e.set_html_autoescape(false); // NOTE WELL!
      data["weblink"] = "https://berthub.eu/ckmailer/msg/"+rows[0]["id"];
      data["unsubscribelink"] = "NA";
      data["channelName"] = "NA";
      data["channelLink"] = "NA";
      
      string textmsg = e.render(rows[0]["textversion"], data);
      e.set_html_autoescape(true); // NOTE WELL!
      string htmlmsg = e.render(rows[0]["htmlversion"], data);
      fmt::print("Should send {} to {}: {}\n", rows[0]["id"], dest, textmsg);

      auto attrows= db.query("select * from attachments where msgId=?", {rows[0]["id"]});
      vector<pair<string,string>> att;
      for(auto& r : attrows)
	att.push_back({r["id"], r["filename"]});
            
      sendEmail(settings["smtp-server"],  // system setting
		"bert@hubertnet.nl", // channel setting really
		dest,        
		"test email", // subject
		textmsg,
		htmlmsg,
		"",
		"bmailer+"+getLargeId()+"@hubertnet.nl", att);
    }
    else if(msg_command.is_subcommand_used(msg_launch_command)) {
      // launch m1 c2 "Welkom"
      int64_t msgId = atoi(msg_launch_command.get("message").substr(1).c_str());
      int64_t channelId = atoi(msg_launch_command.get("channel").substr(1).c_str());
      string subject = msg_launch_command.get("subject");
      
      auto msg = db.queryT("select * from msgs where rowid=?", {msgId});
      if(msg.empty()) {
	cout <<"No such message m"<<msgId<<endl;
	return EXIT_FAILURE;
      }
      auto channel = db.queryT("select * from channels where rowid=?", {channelId});
      if(channel.empty()) {
	cout <<"No such channel c"<<msgId<<endl;
	return EXIT_FAILURE;
      }
      cout<<"Going to launch to c"<<channelId<<": "<<eget(channel[0], "name")<<endl;
      
      auto dests = db.queryT("select users.id userId, users.timsi timsi, channels.id channelId, users.email from users,subscriptions,channels where users.id=subscriptions.userId and subscriptions.channelId=channels.id and channels.rowid=?", {channelId});

      for(auto& d: dests) {

	string msgId=eget(msg[0], "id");
	string userId=eget(d, "userId");
	auto seen=db.queryT("select id from queue where msgId=? and userId=?", {msgId, userId});
	if(!seen.empty()) {
	  cout<<"Skipping message to "<<eget(d,"email")<<", sent them a copy already perhaps on another list"<<endl;
	  continue;
	}
	else
	  cout<<"Queued to "<<eget(d,"email")<< " with subject " <<subject<<endl;	
	
	db.addValue({
	    {"id", getLargeId()},
	    {"msgId", msgId},
	    {"subject", subject},
	    {"sent", false},
	    {"bounced", false},
	    {"channelId", eget(d, "channelId")},
	    {"channelName", eget(channel[0], "name")},
	    {"destination", eget(d, "email")},
	    {"userId", eget(d, "userId")},
	    {"timsi", eget(d, "timsi")},
	    {"timestamp", time(0)}
	  }, "queue");
      }
      db.addValue({{"channelId", eget(channel[0], "id")}, {"msgId", eget(msg[0], "id")}, {"timestamp", time(0)}, {"subject", subject}}, "launches");
    }
    else 
      cout<<msg_command<<endl;
  }
  else if(args.is_subcommand_used(channel_command)) {
    if(channel_command.is_subcommand_used(channel_create_command)) {
      string channel = channel_create_command.get("name");
      string description = channel_create_command.get("description");
      string id = getLargeId();

      db.addValue({{"id", id}, {"name", channel}, {"description", description}}, "channels");
      auto res = db.query("select last_insert_rowid() rid");
      cout<<"created new channel c"<<res[0]["rid"]<<", https://berthub.eu/ckmailer/channel/"<<id<<endl;
    }
    else if(channel_command.is_subcommand_used(channel_list_command) || channel_command.is_subcommand_used(channel_ls_command)) {
      auto rows = db.query("select rowid,* from channels");
      for(auto& r : rows)
	cout << 'c'<< r["rowid"] << '\t' << r["name"]<< '\t' << r["description"]<<"\t"<< r["id"]<<'\n';
    }
    else {
      cout<<channel_command<<endl;
    }
  }
  else if(args.is_subcommand_used(queue_command)) {
    if(queue_command.is_subcommand_used(queue_list_command) || queue_command.is_subcommand_used(queue_ls_command)) {
      auto queued = db.queryT("select * from queue where sent=0");
      for(auto& q: queued) {
	cout << "Should send to "<<eget(q, "destination") << endl;
      }
    }
    else if(queue_command.is_subcommand_used(queue_run_command)) {
      auto queued = db.queryT("select queue.id queueId, msgId, channelId, channelName, timsi, userId, destination, subject, textversion, htmlversion from queue,msgs where sent=0 and msgs.id=queue.msgId");

      for(auto& q: queued) {
	inja::Environment e;
	e.set_html_autoescape(false); // NOTE WELL!
	nlohmann::json data;
	data["weblink"] = "https://berthub.eu/ckmailer/msg/"+eget(q, "msgId");
	data["unsubscribelink"] = "https://berthub.eu/ckmailer/manage.html?timsi="+eget(q, "timsi");
	data["channelName"] = eget(q, "channelName");
	data["channelLink"] = "https://berthub.eu/ckmailer/channel.html?channelId="+eget(q, "channelId");

	string textmsg = e.render(eget(q, "textversion"), data);
	e.set_html_autoescape(true); // NOTE WELL!
	string htmlmsg = e.render(eget(q, "htmlversion"), data);

	auto attrows= db.query("select * from attachments where msgId=?", {eget(q, "msgId")});
	vector<pair<string,string>> att;
	for(auto& r : attrows)
	  att.push_back({r["id"], r["filename"]});
	cout<<"Sending to "<< eget(q, "destination") <<endl;

	vector<pair<string,string>> headers = {
	  {"List-Unsubscribe", "<https://berthub.eu/ckmailer/unsubscribe/"+eget(q, "userId")+"/"+eget(q, "channelId")+">, <mailto:bmailer+"+eget(q, "queueId")+"@hubertnet.nl?subject="+eget(q, "userId")+"/"+eget(q, "channelId")+">"},
	  {"List-Unsubscribe-Post", "List-Unsubscribe=One-Click"},
	  {"List-ID", eget(q, "channelName") + " <"+eget(q, "channelId")+">"}
	};
	
	if(1)
	sendEmail(settings["smtp-server"],  // system setting
		  settings["sender-email"], // channel setting really
		  eget(q, "destination"),        
		  eget(q, "subject"), // subject
		  textmsg,
		  htmlmsg,
		  "",
		  "bmailer+"+ eget(q, "queueId") +"@hubertnet.nl", att, headers);

	db.queryT("update queue set sent=1 where id=?", {eget(q, "queueId")});
	sleep(1);
      }
    }
    else {
      cout<<queue_command<<endl;
    }
  }
  else if(args.is_subcommand_used(imap_command)) {

    auto uhdrs = imapGetMessages(ComboAddress(settings["imap-server"], 993),
				 settings["imap-user"],
				 settings["imap-password"]);
    //    fmt::print("Got this from imap:\n{}\n", uhdrs);
    set<uint32_t> handled;
    for(auto& [uid, hdrs]: uhdrs) {
      if(hdrs.count("Subject")) {
	auto s = splitString(hdrs["Subject"], "/");
	if(s.size() == 2) {
	  auto hits = db.queryT("select * from subscriptions where userId=? and channelId=?",
				{s[0], s[1]});
	  cout<<"Hits for unsubscribe of "<<s[0]<<" / " <<s[1]<<" " << hits.size()<<endl;
	  db.queryT("delete from subscriptions where userId=? and channelId=?",
		    {s[0], s[1]});
	  handled.insert(uid);
	}
      }

      string to = hdrs["To"];
      auto pluspos = to.find('+') ;
      auto atpos = to.find('@') ;
      if(hdrs["Return-Path"] =="<>" && pluspos != string::npos && atpos != string::npos) {

	string id = to.substr(pluspos+1, atpos - pluspos -1);
	cout <<"Maybe we got a bounce: "<<id<<" from '"<<to<<"'"<<endl;
	auto res = db.query("select * from queue where id=?", {id});
	if(!res.empty()) {
	  cout<<"Bounce was for "<<res[0]["destination"] << ", noting"<<endl;
	  if (imap_command["--active"] == true)
	    db.query("update queue set bounced=1 where id=?", {id});
	  handled.insert(uid);
	}
	else
	  cout<<"Unknown bounce"<<endl;
	
      }
    }
    if (imap_command["--active"] == true && !handled.empty()) {
      imapMove(ComboAddress(settings["imap-server"], 993), "bmailer", settings["imap-password"], handled);
    }
  } else {
    cout<<args<<endl;
  }
}
