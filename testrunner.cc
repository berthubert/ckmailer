#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm> // std::move() and friends
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h> //unlink(), usleep()
#include <unordered_map>
#include "doctest.h"
#include <chrono>
#include <fmt/chrono.h>
#include <fmt/printf.h>
#include "nlohmann/json.hpp"

#include "support.hh"

using namespace std;

TEST_CASE("concat test") {
  CHECK(concatUrl("https://berthub.eu", "index.html") == "https://berthub.eu/index.html");
  CHECK(concatUrl("https://berthub.eu/", "index.html") == "https://berthub.eu/index.html");
  CHECK(concatUrl("https://berthub.eu/", "/index.html") == "https://berthub.eu/index.html");
  CHECK(concatUrl("https://berthub.eu", "/index.html") == "https://berthub.eu/index.html");
  CHECK(concatUrl("https://berthub.eu", "") == "https://berthub.eu");
  CHECK(concatUrl("https://berthub.eu/", "") == "https://berthub.eu/");
}
