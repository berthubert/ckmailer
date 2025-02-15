#include "support.hh"
#include "support.hh"
#include <fmt/format.h>
#include <fmt/printf.h>
#include <fmt/chrono.h>
#include <sys/stat.h>
#include <vector>
#include <random>
#include <sclasses.hh>
#include <regex>
#include "base64.hpp"
using namespace std;

uint64_t getRandom64()
{
  static std::random_device rd; // 32 bits at a time. At least on recent Linux and gcc this does not block
  return ((uint64_t)rd() << 32) | rd();
}

string getLargeId()
{
  uint64_t id = getRandom64();
  string ret = base64::to_base64(std::string((char*)&id, sizeof(id)));
  ret.resize(ret.size()-1); // this base64url implementation pads, somehow
  id = getRandom64();
  ret += base64::to_base64(std::string((char*)&id, sizeof(id)));
  ret.resize(ret.size()-1); // this base64url implementation pads, somehow

  for(auto& c : ret) {
    if(c == '/')
      c = '_';
    else if(c == '+')
      c = '-';
  }
  return ret;
}

string getContentsOfFile(const std::string& fname)
{
  FILE* pfp = fopen(fname.c_str(), "r");
  if(!pfp)
    throw runtime_error("Unable to get document "+fname+": "+string(strerror(errno)));
  
  shared_ptr<FILE> fp(pfp, fclose);
  char buffer[4096];
  string ret;
  for(;;) {
    int len = fread(buffer, 1, sizeof(buffer), fp.get());
    if(!len)
      break;
    ret.append(buffer, len);
  }
  if(!ferror(fp.get())) {
    return ret;
  }
  return "";
}


// do not put \r in in
string toQuotedPrintable(const std::string& in)
{
  string out;
  string line;

  for(const auto& c: in) {
    if(c=='\n') {
      out += line;
      out += "\r\n"; // really
      line.clear();
      continue;
    }
    string part;
    if(isprint(c) && c != '=')
      part=c;
    else
      part = fmt::sprintf("=%02X", (int)(unsigned char)c);
    
    if(line.length() + part.length() >= 76) {
      out += line;
      out += "=\r\n";
      line.clear();
    }
    line += part;
  }
  out += line;
  return out;
}

std::vector<std::string> splitString(const std::string& str, const std::string& delimiter) {
    std::vector<std::string> tokens;
    std::regex regex(delimiter);  // Create regex from the delimiter string
    std::sregex_token_iterator it(str.begin(), str.end(), regex, -1);
    std::sregex_token_iterator end;

    while (it != end) {
        tokens.push_back(*it);
        ++it;
    }

    return tokens;
}


void replaceSubstring(std::string &originalString, const std::string &searchString, const std::string &replaceString) {
  size_t pos = originalString.find(searchString);
  
  while (pos != std::string::npos) {
    originalString.replace(pos, searchString.length(), replaceString);
    pos = originalString.find(searchString, pos + replaceString.length());
  }
}
// Function to check if a string ends with a particular suffix
bool endsWith(const std::string& str, const std::string& suffix) {
    // Check if the suffix is longer than the string itself
    if (suffix.size() > str.size()) {
        return false;
    }

    // Compare the end of the string with the suffix
    return std::equal(suffix.rbegin(), suffix.rend(), str.rbegin());
}


void sendEmail(const std::string& server, const int serverPort, const std::string& from, const std::string& to, const std::string& subject, const std::string& textBody, const std::string& htmlBody, const std::string& bcc, const std::string& envelopeFrom, const std::vector<std::pair<std::string, std::string>>& att,
	       const std::vector<std::pair<std::string, std::string>>& headers)
{
  string rEnvelopeFrom = envelopeFrom.empty() ? from : envelopeFrom;

  const char* allowed="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_+-.@=";
  if(from.find_first_not_of(allowed) != string::npos || to.find_first_not_of(allowed) != string::npos) {
    throw std::runtime_error("Illegal character in from or to address");
  }

  ComboAddress mailserver(server, serverPort);
  Socket s(mailserver.sin4.sin_family, SOCK_STREAM);

  SocketCommunicator sc(s);
  sc.connect(mailserver);
  string line;
  auto sponge= [&](int expected) {
    while(sc.getLine(line)) {
      if(line.size() < 4)
        throw std::runtime_error("Invalid response from SMTP server: '"+line+"'");
      if(stoi(line.substr(0,3)) != expected)
        throw std::runtime_error("Unexpected response from SMTP server: '"+line+"'");
      if(line.at(3) == ' ')
        break;
    }
  };

  sponge(220);
  sc.writen("EHLO outer2.berthub.eu\r\n");
  sponge(250);

  sc.writen("MAIL From:<"+rEnvelopeFrom+">\r\n");
  sponge(250);

  sc.writen("RCPT To:<"+to+">\r\n");
  sponge(250);

  if(!bcc.empty()) {
    sc.writen("RCPT To:<"+ bcc +">\r\n");
    sponge(250);
  }
  
  sc.writen("DATA\r\n");
  sponge(354);
  sc.writen("From: "+from+"\r\n");
  sc.writen("To: "+to+"\r\n");

  bool needb64 = false;
  for(const auto& c : subject) {
    if(c < 32 || (unsigned char)c > 127) {
      needb64 = true;
      break;
    }
  }
  string esubject;
  if(needb64)
    esubject = "=?utf-8?B?"+base64::to_base64(subject)+"?=";
  else
    esubject = subject;

  
  sc.writen("Subject: "+esubject+"\r\n");

  for(const auto& h : headers) {
    sc.writen(h.first+": "+h.second+"\r\n");
  }

  sc.writen(fmt::format("Message-Id: <{}@opentk.hostname>\r\n", getRandom64()));
  
  //Date: Thu, 28 Dec 2023 14:31:37 +0100 (CET)
  sc.writen(fmt::format("Date: {:%a, %d %b %Y %H:%M:%S %z (%Z)}\r\n", fmt::localtime(time(0))));

  sc.writen("Auto-Submitted: auto-generated\r\nPrecedence: bulk\r\n");

  string sepa="_----------=_MCPart_"+getLargeId();
  if(htmlBody.empty()) {
    sc.writen("Content-Type: text/plain; charset=\"utf-8\"\r\n");
    sc.writen("Content-Transfer-Encoding: quoted-printable\r\n");
  }
  else {
    sc.writen("Content-Type: multipart/alternative; boundary=\""+sepa+"\"\r\n");
    sc.writen("MIME-Version: 1.0\r\n");
  }
  sc.writen("\r\n");


  if(!htmlBody.empty()) {
    sc.writen("This is a multi-part message in MIME format\r\n\r\n");

    sc.writen("--"+sepa+"\r\n");
    sc.writen("Content-Type: text/plain; charset=\"utf-8\"; format=\"fixed\"\r\n");
    sc.writen("Content-Transfer-Encoding: quoted-printable\r\n\r\n");
  }
  string qp = toQuotedPrintable(textBody);

  sc.writen(qp +"\r\n");
  
  if(htmlBody.empty()) {
    sc.writen("\r\n.\r\n");
    sponge(250);
    return;
  }
  sc.writen("--"+sepa+"\r\n");

  string sepa2 = "_"+getLargeId();
  sc.writen("Content-Type: multipart/related; boundary=\""+sepa2+"\"\r\n\r\n");

  sc.writen("--"+sepa2+"\r\n");
  
  sc.writen("Content-Type: text/html; charset=\"utf-8\"\r\n");
  sc.writen("Content-Transfer-Encoding: base64\r\n\r\n");
  int linelen = 76;
  string b64 = base64::to_base64(htmlBody);
  int pos = 0;
  for(pos = 0 ; pos < (int)b64.length() - linelen; pos += linelen) {
    sc.writen(b64.substr(pos, linelen)+"\r\n");
  }
  sc.writen(b64.substr(pos) +"\r\n");
  // perhaps another empty line?

  for(const auto& [id, fname] : att) {
    sc.writen("--"+sepa2+"\r\n");
    string type="jpeg";
    if(endsWith(fname, ".png"))
      type="png";
    else if(endsWith(fname, ".webp"))
      type="webp";
    
    sc.writen("Content-Type: image/"+type+"; name=\""+fname+"\"\r\n");
    sc.writen("Content-Disposition: inline; filename=\""+fname+"\"\r\n");
    sc.writen("Content-Id: <" + id+ ">\r\n");
    sc.writen("Content-Transfer-Encoding: base64\r\n\r\n");
    b64 = base64::to_base64(getContentsOfFile(fname));
    for(pos = 0 ; pos < (int)b64.length() - linelen; pos += linelen) {
      sc.writen(b64.substr(pos, linelen)+"\r\n");
    }
    sc.writen(b64.substr(pos) +"\r\n");
  }
  
  sc.writen("--"+sepa2+"--\r\n\r\n");
  sc.writen("--"+sepa+"--\r\n.\r\n");
  sponge(250);
  return;
}


std::string htmlEscape(const std::string& data)
{
  std::string buffer;
  buffer.reserve(1.1*data.size());
  for(size_t pos = 0; pos != data.size(); ++pos) {
    switch(data[pos]) {
    case '&':  buffer.append("&amp;");       break;
    case '\"': buffer.append("&quot;");      break;
    case '\'': buffer.append("&apos;");      break;
    case '<':  buffer.append("&lt;");        break;
    case '>':  buffer.append("&gt;");        break;
    default:   buffer.append(&data[pos], 1); break;
    }
  }
  return buffer;
}

std::string urlEscape(const std::string& data)
{
  std::string buffer;
  buffer.reserve(1.1*data.size());
  for(const auto& c : data) {
    if(!isalnum(c) && (c!= '-' && c != '.' && c !='_' && c != '~'))
      buffer += fmt::sprintf("%%%02x", (unsigned int) (unsigned char) c);
    else
      buffer.append(1, c); 
  }
  return buffer;
}
