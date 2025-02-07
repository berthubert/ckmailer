#include "nonblocker.hh"
#include "peglib.h"
#include <openssl/ssl.h>
#include <fmt/format.h>
#include <fmt/chrono.h>
#include <fmt/ranges.h>
#include <openssl/x509v3.h>

// https://www.atmail.com/blog/imap-101-manual-imap-sessions/
// https://nickb.dev/blog/introduction-to-imap/
// https://donsutherland.org/crib/imap

#include <mutex>

using namespace std;

bool getLine(FILE* fp, std::string& line)
{
  char buf[256]={};
  if(fgets(buf, sizeof(buf)-1, fp) == nullptr)
    return false;
  line = buf;
  return true;
}

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


string imapGetMessages(const ComboAddress& server, const std::string& user, const std::string& password)
{
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

  int counter=0;
  vector<string> lines;
  auto scommand = [&](const std::string& cmd) {

    string line="A"+to_string(counter++)+" " + cmd+"\r\n";
    //    fmt::print("Sending {}", line);
    SSL_write(sh.ssl, line.c_str(), line.size());
    lines.clear();
    do {
      resp = sslGetLine(sh.ssl);
      //fmt::print("Response is {}", resp);
      if(lines.empty()) {
        auto pos = resp.rfind('{');
        if(pos != string::npos) {
          int bytes = atoi(&resp.at(pos+1));
          vector<char> c(bytes);
          SSL_read(sh.ssl, &c.at(0), bytes);
          lines.push_back(string(&c.at(0), bytes));
          SSL_read(sh.ssl, &c.at(0), 3); // ")\r\n"
          continue;
        }
      }
      lines.push_back(resp);
    }while(!resp.empty() && resp[0]=='*');
  };

  
  scommand("login "+user+" "+password);
  cout<<"Logged in!"<<endl;
  scommand("namespace");
  scommand(R"(select "INBOX")");

  scommand("UID FETCH 1:* (FLAGS)");
  fmt::print("{}\n", lines);

  scommand("FETCH 9 body[header]");
  fmt::print("{}\n", lines);
  
  scommand(R"(uid search subject "Simplomon test message")");
  /*
  * SEARCH 171430 171431 171432 171433 171434 171435
a9 OK Search completed (0.045 + 0.000 + 0.044 secs).
  */

  peg::parser p(R"(
LINE <- '* SEARCH' (' ' UID)*'\r\n'?
UID <- (![ \r\n] .)+
)");

  if(!(bool)p)
    throw runtime_error("Error in grammar");
  
  p["UID"] = [](const peg::SemanticValues& vs) {
    return vs.token_to_string();
  };

  p["LINE"] = [](const peg::SemanticValues& vs) {
    return vs.transform<string>();
  };
  vector<string> uids;
  p.parse(lines[0], uids);

  fmt::print("Got {} uids: {}\n", uids.size(), uids);

  /*
n uid fetch 171446 BODY[TEXT]
* 13508 FETCH (UID 171446 FLAGS (\Seen) BODY[TEXT] {58}
hallo
)
en gaat het nu door dan?
zou wel leuk zijn
)
)
n OK Fetch completed (0.026 + 0.000 + 0.025 secs).
  */
  vector<string> todel;
  time_t freshest = 0;
  for(const auto& uid : uids) {
    scommand("uid fetch "+uid+" BODY.PEEK[TEXT]");
    //    fmt::print("lines: {}\n", lines);
    if(!lines.empty()) {
      time_t then = atoi(lines[0].c_str());
      if(freshest < then)
        freshest = then;
      time_t age = time(nullptr) - then;
      //      fmt::print("Age is {} seconds\n", age);
      if(age > 300)
        todel.push_back(uid);
    }
  }

  for(const auto& del : todel) {
    scommand("uid store "+del+" +FLAGS (\\Deleted)");
  }
  if(!todel.empty())
    scommand("expunge");
  
  if(time(nullptr) - freshest > 300) {
    return "No recent sentinel message found";
  }
  return "";
}
