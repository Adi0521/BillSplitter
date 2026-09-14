// Proves the unit-test binary builds and links against billsplitter-core.
// If this fails, no other unit test's result means anything.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "auth/auth.h"

TEST_CASE("the test binary links the core library") {
    // generate_token is a pure function in billsplitter-core with no database
    // dependency, so it is a safe canary for "did linking actually work".
    const std::string a = auth::generate_token(16);
    const std::string b = auth::generate_token(16);
    CHECK(a.size() == 32);          // 16 bytes, hex-encoded
    CHECK(a != b);                  // and actually random
}

TEST_CASE("pbkdf2 is deterministic for the same salt") {
    const std::string salt = "00112233445566778899aabbccddeeff";
    CHECK(auth::pbkdf2_hash("hunter2hunter2", salt) ==
          auth::pbkdf2_hash("hunter2hunter2", salt));
    CHECK(auth::pbkdf2_hash("hunter2hunter2", salt) !=
          auth::pbkdf2_hash("hunter2hunter3", salt));
}
