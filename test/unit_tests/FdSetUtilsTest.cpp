#include <cstdint>
#include <cstring>

#include "Headers.hpp"
#include "TestHeaders.hpp"

using namespace et;

TEST_CASE("fdSetChecked adds descriptors that fit", "[FdSetUtils]") {
  fd_set fds;
  FD_ZERO(&fds);

  REQUIRE(fdSetChecked(0, &fds));
  REQUIRE(fdSetChecked(FD_SETSIZE - 1, &fds));

  REQUIRE(fdIsSetChecked(0, &fds));
  REQUIRE(fdIsSetChecked(FD_SETSIZE - 1, &fds));
  REQUIRE_FALSE(fdIsSetChecked(1, &fds));
}

#ifndef WIN32
namespace {
// An fd_set with live memory immediately behind it, mirroring the stack layout
// where the out-of-range FD_SET landed on a caller's local variable.
struct GuardedFdSet {
  fd_set fds;
  uint64_t guard[8];
};
}  // namespace

TEST_CASE("fdSetChecked rejects descriptors that do not fit", "[FdSetUtils]") {
  fd_set fds;
  FD_ZERO(&fds);

  REQUIRE_FALSE(fdSetChecked(-1, &fds));
  REQUIRE_FALSE(fdSetChecked(FD_SETSIZE, &fds));
  REQUIRE_FALSE(fdSetChecked(FD_SETSIZE + 320, &fds));

  REQUIRE_FALSE(fdIsSetChecked(-1, &fds));
  REQUIRE_FALSE(fdIsSetChecked(FD_SETSIZE, &fds));
  REQUIRE_FALSE(fdIsSetChecked(FD_SETSIZE + 320, &fds));
}

TEST_CASE("fdSetChecked leaves memory behind the fd_set untouched",
          "[FdSetUtils]") {
  GuardedFdSet guarded;
  FD_ZERO(&guarded.fds);
  memset(guarded.guard, 0, sizeof(guarded.guard));

  // Multiples of 64 past the end are the dangerous ones: each sets bit 0 of a
  // whole word beyond the bitmap. FD_SETSIZE + 320 is fd 1344, the descriptor
  // that overwrote a live pointer in the reported etserver crash.
  for (int fd = FD_SETSIZE; fd <= FD_SETSIZE + 384; fd += 64) {
    REQUIRE_FALSE(fdSetChecked(fd, &guarded.fds));
  }

  for (size_t i = 0; i < sizeof(guarded.guard) / sizeof(guarded.guard[0]);
       i++) {
    REQUIRE(guarded.guard[i] == 0);
  }
}
#endif
