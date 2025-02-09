#include "nonblocker.hh"
#include "peglib.h"
#include <openssl/ssl.h>
#include <fmt/format.h>
#include <fmt/chrono.h>
#include <fmt/ranges.h>
#include <openssl/x509v3.h>
#include "support.hh"

// https://www.atmail.com/blog/imap-101-manual-imap-sessions/
// https://nickb.dev/blog/introduction-to-imap/
// https://donsutherland.org/crib/imap

#include <mutex>

using namespace std;
// empty string == EOF
string sslGetLine(SSL* ssl)
{
  string resp;
  for(;;) {
    char c;
    int rc = SSL_read(ssl, &c, 1);
    if(rc == 1) {
      resp.append(1,c);
      if(c=='\n')
        break;
    }
    if(rc <= 0)
      break;
  }
  return resp;
}

struct SSLHelper
{
  SSLHelper()
  {
    // Initialize OpenSSL - can you do this all the time?
    std::lock_guard<std::mutex> l(d_lock);

    SSL_load_error_strings();
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    // Initialize SSL
    ssl_ctx = SSL_CTX_new(SSLv23_client_method());
    if (ssl_ctx == NULL) {
      throw std::runtime_error("Error: SSL context creation failed");
    }

    // Load trusted CA certificates
    if (SSL_CTX_set_default_verify_paths(ssl_ctx) != 1) {
      SSL_CTX_free(ssl_ctx);
      throw std::runtime_error("Error loading CA certificates\n");
    }
    
    // Create SSL connection
    ssl = SSL_new(ssl_ctx);
    if (ssl == NULL) {
      SSL_CTX_free(ssl_ctx);
      throw std::runtime_error("Creating SSL struct");
    }
  }

  ~SSLHelper()
  {
    SSL_free(ssl);
    SSL_CTX_free(ssl_ctx);
  }
  
  
  void attachFD(int fd)
  {
    if (SSL_set_fd(ssl, fd) != 1) 
      throw std::runtime_error("Error: Failed to attach socket descriptor to SSL");
  }

  void initTLS()
  {
    if (SSL_connect(ssl) != 1) {
      throw std::runtime_error("Error: SSL handshake failed\n");
    }
  }
  void checkConnection(const std::string& host, int days = -1);
  void printDetails();
  SSL_CTX *ssl_ctx;
  SSL *ssl;

  static std::mutex d_lock;
};

std::mutex SSLHelper::d_lock;

void SSLHelper::printDetails()
{
  shared_ptr<X509> cert(SSL_get_peer_certificate(ssl), X509_free);
  
  X509_NAME_print_ex_fp(stdout, X509_get_subject_name(cert.get()), 0,XN_FLAG_RFC2253);
  fmt::print("\n");
  
  X509_NAME_print_ex_fp(stdout, X509_get_issuer_name(cert.get()), 0,XN_FLAG_RFC2253);
  fmt::print("\n");
  
  
  ASN1_INTEGER *serial = X509_get_serialNumber(cert.get());
  if (serial != NULL) {
    BIGNUM *bn_serial = ASN1_INTEGER_to_BN(serial, NULL);
    char *serial_string = BN_bn2hex(bn_serial);
    if (serial_string != NULL) {
      fmt::print("Serial Number: {}\n", serial_string);
      OPENSSL_free(serial_string);
    }
    BN_free(bn_serial);
  }
  
  auto t=X509_get_notAfter(cert.get());
  struct tm notafter, notbefore;
  ASN1_TIME_to_tm(t, &notafter);

  t=X509_get_notBefore(cert.get());
  ASN1_TIME_to_tm(t, &notbefore);
  fmt::print("{:%Y-%m-%d %H:%M} - {:%Y-%m-%d %H:%M}\n", notbefore, notafter);
}

void SSLHelper::checkConnection(const std::string& host, int minCertDays)
{
  long verify_result = SSL_get_verify_result(ssl);
  if (verify_result != X509_V_OK) {
    throw std::runtime_error(fmt::format("Certificate verification error: {}\n", X509_verify_cert_error_string(verify_result)));
  }
  
  shared_ptr<X509> cert(SSL_get_peer_certificate(ssl), X509_free);
  // this is sensitive to trailing dots
  if (X509_check_host(cert.get(), host.c_str(), host.size(), 0, NULL) != 1) {
    throw std::runtime_error(fmt::format("Cert does not match host {}", host));
  }

  if(minCertDays > 0) {
    auto t=X509_get_notAfter(cert.get());
    struct tm notafter;
    ASN1_TIME_to_tm(t, &notafter);
    time_t expire = mktime(&notafter);
    double days = (expire - time(nullptr))/86400.0;
    if(days < minCertDays)
      throw std::runtime_error(
                               fmt::format("Certificate for {} set to expire in {:.0f} days",
                                           host, days));
  }
}


static auto scommand(unsigned int &counter, SSLHelper& sh, const std::string& cmd)
{
  vector<string> lines;
  string line="A"+to_string(counter++)+" " + cmd+"\r\n";
  //    fmt::print("Sending {}", line);
  SSL_write(sh.ssl, line.c_str(), line.size());
  string resp;
  do {
    resp = sslGetLine(sh.ssl);
    //      fmt::print("Response is {}", resp);
      if(lines.empty()) {
        auto pos = resp.rfind('{');
        if(pos != string::npos) {
          int bytes = atoi(&resp.at(pos+1));
	  //	  fmt::print("Going to read {} bytes\n", bytes);
          vector<char> c(bytes);
	  int left = bytes;
	  string line;
	  do {
	    int received = SSL_read(sh.ssl, &c.at(0), left);
	    if(received < 0)
	      throw std::runtime_error("Error from SSL_read");
	    if(!received)
	      throw std::runtime_error("EOF from SSL_read");
	    //	    fmt::print("Got {} bytes..\n", received);
	    line+=(string(&c.at(0), received));
	    left -= received;
	  }while(left);
          lines.push_back(line);
          SSL_read(sh.ssl, &c.at(0), 3); // ")\r\n"
          continue;
        }
      }
      lines.push_back(resp);
  }while(!resp.empty() && resp[0]=='*');
  return lines;
}

std::vector<pair<uint32_t, std::unordered_map<string,string>>> imapGetMessages(const ComboAddress& server, const std::string& user, const std::string& password)
{
  std::vector<pair<uint32_t, std::unordered_map<string,string>>> ret;
  
  string servername;
  NonBlocker nb(server, 10);

  SSLHelper sh;
  sh.attachFD(nb);

  sh.initTLS();
  //  sh.printDetails();
  string checkname = servername.empty() ? "" : servername;
  if(!checkname.empty())
    checkname.resize(checkname.size()-1);
  if(!checkname.empty())
    sh.checkConnection(checkname, 0); // does 0 work??

  string resp = sslGetLine(sh.ssl);
  cout<<resp<<endl;
  unsigned int counter=0;
  vector<string> lines;

  
  scommand(counter, sh, "login "+user+" "+password);
  cout<<"Logged in!"<<endl;
  scommand(counter,sh,"namespace");
  scommand(counter, sh, R"(select "INBOX")");

  // what does the 1: do??
  lines = scommand(counter, sh, "UID FETCH 1:* (FLAGS)");
  fmt::print("{}\n", lines);

  set<unsigned int> uids;
  for(const auto& l : lines) {
    if(l.empty()) continue;
    if(auto pos = l.find("(UID "); pos != string::npos)
      uids.insert(atol(&l.at(pos+4)));
  }
  fmt::print("Had the following uids: {}\n", uids);
  for(auto i : uids) {
    
    lines = scommand(counter, sh, "UID FETCH "+to_string(i)+" body[header]");
    if(lines.empty())
      continue;
    auto split = splitString(lines[0], "\r\n");

    std::unordered_map<string,string> hdrs;
    for(const auto& l : split) {
      if(l.find("Subject: ") == 0) {
	hdrs["Subject"] = l.substr(9);
      }
    }
    ret.push_back({i, hdrs});
  }
  
  return ret;
}


void imapMove(const ComboAddress& server, const std::string& user, const std::string& password, const std::set<uint32_t>& uids)
{
  if(uids.empty())
    return;
  std::vector<pair<uint32_t, std::unordered_map<string,string>>> ret;
  
  string servername;
  NonBlocker nb(server, 10);

  SSLHelper sh;
  sh.attachFD(nb);

  sh.initTLS();
  //  sh.printDetails();
  string checkname = servername.empty() ? "" : servername;
  if(!checkname.empty())
    checkname.resize(checkname.size()-1);
  if(!checkname.empty())
    sh.checkConnection(checkname, 0); // does 0 work??

  string resp = sslGetLine(sh.ssl);
  cout<<resp<<endl;
  
  unsigned int counter=0;

  auto lines = scommand(counter, sh, "login "+user+" "+password);
  fmt::print("Logged in: {}\n", lines);

  scommand(counter,sh,"namespace");
  scommand(counter, sh, R"(select "INBOX")");
  lines = scommand(counter, sh, "CREATE INBOX.ProcessedByCKMailer");
  fmt::print("Got creating folder: {}\n", lines);

  string str;
  for(const auto& uid : uids) {
    if(!str.empty()) str.append(",");
    str += to_string(uid);
  }
  
  lines =scommand(counter, sh, "UID MOVE " + str+ " INBOX.ProcessedByCKMailer");
  fmt::print("Got moving message: {}\n", lines);
}
