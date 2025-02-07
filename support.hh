#pragma once
#include <string>
#include <variant>
#include "nonblocker.hh"
void sendEmail(const std::string& server, const std::string& from, const std::string& to, const std::string& subject, const std::string& textBody, const std::string& htmlBody, const std::string& bcc="", const std::string& envelopeFrom="", const std::vector<std::pair<std::string, std::string>>& att={},
	       const std::vector<std::pair<std::string, std::string>>& headers={});
uint64_t getRandom64();
std::string getLargeId();
std::string imapGetMessages(const ComboAddress& server, const std::string& user, const std::string& password);
std::string getContentsOfFile(const std::string& fname);
std::string htmlEscape(const std::string& data);
std::string urlEscape(const std::string& data);


void replaceSubstring(std::string &originalString, const std::string &searchString, const std::string &replaceString);
bool endsWith(const std::string& str, const std::string& suffix);
template<typename T, typename R>
R genget(const T& cont, const std::string& fname)
{
  R ret{};
  auto iter = cont.find(fname);
  if(iter == cont.end() || !std::get_if<R>(&iter->second))
    return ret;

  return std::get<R>(iter->second);  
}

template<typename T>
std::string eget(const T& cont, const std::string& fname)
{
  return genget<T, std::string>(cont, fname);
}

template<typename T>
int64_t iget(const T& cont, const std::string& fname)
{
  return genget<T, int64_t>(cont, fname);
}
