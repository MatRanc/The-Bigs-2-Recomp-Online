// thebigs2 - temporary online/sign-in debugging helpers.
//
// Enabled only when TB2_ONLINE_DEBUG is set in the environment. Binds:
//   F11 - dump the online manager state (DAT_825e34f0) to the log
//   F12 - broadcast XN_SYS_SIGNINCHANGED (0x0A)
//   F6  - broadcast the Xbox Live connection-changed notification (0x02000001)
//
// The manager layout was recovered with Ghidra; offsets are relative to the
// pointer stored at guest address 0x825e34f0.

#include <atomic>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <mutex>
#include <map>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>
#include <SDL3/SDL.h>
#include <set>
#include <utility>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fmt/format.h>
#include "guest_memory.h"
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "online_debug.h"

// Per-player identity. The SDK's UserProfile (rexglue-sdk user_profile.cpp) still uses its own B13EBABE... XUID
// for XamUserGetXUID; nothing seen so far needs the two to match.
// The Game Room draws the account XUID's 8 bytes as the player's name (a real Live XUID starts with 0x00
// and draws nothing), so the synthetic XUID is the name itself: first 8 chars, NUL-padded.
// shortcut: names sharing their first 8 chars collide and longer names are cut to 8; fine for two friends.
static uint64_t TB2XuidFromName(const char* n) {
  uint64_t x = 0;
  for (int i = 0; i < 8 && n[i]; i++) x |= uint64_t(uint8_t(n[i])) << (8 * (7 - i));
  return x;
}


extern std::atomic<uint32_t> g_fn_epoch;
void Tb2FnTrace(uint32_t addr, uint32_t lr);
namespace {

uint32_t LoadGuestU32(uint8_t* base, uint32_t addr) {
  uint32_t v = 0;
  std::memcpy(&v, GuestPtr(base, addr), sizeof(v));
  return __builtin_bswap32(v);
}

// Rate-limits the high-frequency (per-frame / ~100 Hz) diagnostic lines so a
// run's log stays readable. Each call site gets its own `last` timestamp.
bool OncePerSecond(std::chrono::steady_clock::time_point& last) {
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::seconds(1)) return false;
  last = now;
  return true;
}

void StoreGuestU8(uint8_t* base, uint32_t addr, uint8_t v) { (*GuestPtr(base, addr)) = v; }

void StoreGuestU32(uint8_t* base, uint32_t addr, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(GuestPtr(base, addr), &v, sizeof(v));
}

}  // namespace

// --- roster sync -----------------------------------------------------------

// Function_822DD5A0 is the "is the local roster up to date?" check used by
// Function_8230EF38: nonzero -> run the search callback directly (skip the
// "download the latest rosters?" prompt + download). Forcing 1 makes Quick
// Match skip the dead roster server and use the built-in roster.
extern "C" REX_FUNC(__imp__sub_822DD5A0);
extern "C" REX_FUNC(sub_822DD5A0) {
  if (std::getenv("TB2_SKIP_ROSTER")) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_822DD5A0(ctx, base);
}

// Roster download start (roster object vtable+0x14). The real path sends a
// request through the Live stack and hangs. Instead, mark the download in
// progress and enqueue an empty download response (event 0x35); the roster
// object's update then enqueues 0x37 ("download complete") and Quick Match
// continues with the built-in roster (MLB16's "skip live roster download").
extern "C" REX_FUNC(__imp__sub_823EA090);
extern "C" REX_FUNC(__imp__sub_823F08F8);
extern "C" REX_FUNC(sub_823F08F8) {
  if (std::getenv("TB2_SKIP_ROSTER")) {
    const uint32_t obj = ctx.r3.u32;
    if (obj) StoreGuestU8(base, obj + 0xc, 1);
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = 0x35;
    __imp__sub_823EA090(frame.ctx, base);
    if (std::getenv("TB2_ONLINE_DEBUG")) {
      REXLOG_INFO("OnlineDebug: skipped roster download, enqueued 0x35 (obj=0x{:08X})", obj);
    }
    return;
  }
  __imp__sub_823F08F8(ctx, base);
}

// --- (a2) synthetic Live sign-in + (b) synthetic match result --------------
//
// The statically-linked Live stack never finishes sign-in because there is no
// Live service. The game itself creates the pending sign-in object in
// DAT_825f50ec (Function_823F5C68, 0x38 bytes); the Live stack is supposed to
// fill the gamertag (+0x08, 16 bytes) and the X_USER_SIGNIN_INFO prefix
// (+0x1c, 20 bytes), then enqueue event 0x3c. Function_823F5F88 turns that
// event into an account by calling Function_823E9F18 (the game's own account
// constructor) and adds it to DAT_825f50e8. That account list gates the
// roster, the match search and the connection.
//
// TB2_FAKE_ACCOUNT fabricates the pending object and enqueues 0x3c, so the
// account is built by the game (valid vtable) instead of the old zeroed-buffer
// hack that crashed.
//
// TB2_FAKE_SEARCH injects an XSESSION_SEARCHRESULT list into the search
// manager (DAT_825f5110) after Function_823F82D8 issues the (dead) Live
// search, then runs Function_823F81B0 to turn it into event 0xe. The result's
// XNADDR points at TB2_PEER_IP:TB2_PEER_PORT (default 127.0.0.1:3074).

extern "C" REX_FUNC(__imp__sub_8240A5C0);
extern "C" REX_FUNC(__imp__sub_823F81B0);
extern "C" REX_FUNC(__imp__sub_823F82D8);
extern "C" REX_FUNC(__imp__sub_823E9F18);
extern "C" REX_FUNC(__imp__sub_823F4AB0);
extern "C" REX_FUNC(__imp__sub_823F44B8);
extern "C" REX_FUNC(__imp__sub_823ECEE0);
extern "C" REX_FUNC(__imp__sub_82420F20);
extern "C" REX_FUNC(__imp__sub_82420CB8);
extern "C" REX_FUNC(__imp__sub_823FA360);
extern "C" REX_FUNC(__imp__sub_823F97E8);
extern "C" REX_FUNC(__imp__sub_823F47D0);
extern "C" REX_FUNC(__imp__sub_823ED1E0);
extern "C" REX_FUNC(__imp__sub_823F9960);
extern "C" REX_FUNC(__imp__sub_823F7308);
extern "C" REX_FUNC(__imp__sub_823F7AA0);
extern "C" REX_FUNC(__imp__sub_823F6CD8);
extern "C" REX_FUNC(__imp__sub_823F65D8);

namespace {

constexpr uint32_t kLiveMode = 0x825f50e0;       // DAT_825f50e0
constexpr uint32_t kAccountList = 0x825f50e8;    // DAT_825f50e8
constexpr uint32_t kPendingSignin = 0x825f50ec;  // DAT_825f50ec
constexpr uint32_t kEventQueue = 0x825f50fc;     // DAT_825f50fc
constexpr uint32_t kLiveModeOnline = 2;

uint32_t GuestAlloc(PPCContext& parent, uint8_t* base, uint32_t size, uint32_t flag) {
  rex::CallFrame frame(parent);
  frame.ctx.r3.u64 = size;
  frame.ctx.r4.u64 = 25;
  frame.ctx.r5.u64 = 0;
  frame.ctx.r6.u64 = flag;
  __imp__sub_8240A5C0(frame.ctx, base);
  return frame.ctx.r3.u32;
}

void StoreGuestU16(uint8_t* base, uint32_t addr, uint16_t v) {
  v = __builtin_bswap16(v);
  std::memcpy(GuestPtr(base, addr), &v, sizeof(v));
}

void StoreGuestU64(uint8_t* base, uint32_t addr, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(GuestPtr(base, addr), &v, sizeof(v));
}

void StoreGuestBytes(uint8_t* base, uint32_t addr, const void* src, size_t len) {
  std::memcpy(GuestPtr(base, addr), src, len);
}

void EnqueueLiveEvent(PPCContext& parent, uint8_t* base, uint32_t id) {
  rex::CallFrame frame(parent);
  frame.ctx.r3.u64 = id;
  __imp__sub_823EA090(frame.ctx, base);
}

// Build a synthetic XSESSION_SEARCHRESULT_HEADER + one XSESSION_SEARCHRESULT +
// eight X_ONLINE properties. Returns the header address, or 0 on failure.
uint32_t BuildFakeSearchResults(PPCContext& parent, uint8_t* base) {
  constexpr uint32_t kHeaderSize = 0x08;
  constexpr uint32_t kResultSize = 0x5C;
  constexpr uint32_t kPropCount = 8;
  constexpr uint32_t kPropSize = 0x18;
  constexpr uint32_t kTotal = kHeaderSize + kResultSize + kPropCount * kPropSize;

  const uint32_t block = GuestAlloc(parent, base, kTotal, 1);
  if (!block) return 0;
  std::memset(GuestPtr(base, block), 0, kTotal);

  const uint32_t result = block + kHeaderSize;
  const uint32_t props = result + kResultSize;

  // XSESSION_SEARCHRESULT_HEADER: one result.
  StoreGuestU32(base, block + 0x00, 1);
  StoreGuestU32(base, block + 0x04, result);

  uint8_t ip[4] = {127, 0, 0, 1};
  if (const char* peer = std::getenv("TB2_PEER_IP")) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(peer, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a < 256 && b < 256 &&
        c < 256 && d < 256) {
      ip[0] = (uint8_t)a;
      ip[1] = (uint8_t)b;
      ip[2] = (uint8_t)c;
      ip[3] = (uint8_t)d;
    }
  }
  uint32_t port = 1000;
  if (const char* p = std::getenv("TB2_PEER_PORT")) port = (uint32_t)std::strtoul(p, nullptr, 0);

  uint64_t session_id = 0x4249473200000001ull;
  if (const char* s = std::getenv("TB2_SESSION_ID")) session_id = std::strtoull(s, nullptr, 0);

  // XSESSION_SEARCHRESULT: XSESSION_INFO { XNKID, XNADDR, XNKEY }.
  StoreGuestU64(base, result + 0x00, session_id);  // XNKID
  StoreGuestBytes(base, result + 0x08, ip, 4);     // XNADDR.ina
  StoreGuestBytes(base, result + 0x0C, ip, 4);     // XNADDR.inaOnline
  StoreGuestU16(base, result + 0x10, (uint16_t)port);
  const uint8_t mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
  StoreGuestBytes(base, result + 0x12, mac, sizeof(mac));
  StoreGuestBytes(base, result + 0x18, ip, 4);  // SGADDR.ina
  StoreGuestU32(base, result + 0x1C, 0x12345678);  // SGADDR.security_parameter_index
  StoreGuestU64(base, result + 0x20, 0xB13EBABEBABEBABEull);  // SGADDR.xbox_id
  StoreGuestU8(base, result + 0x28, 1);  // SGADDR.platform_type = Xbox 360

  uint8_t key[16] = {0x42, 0x49, 0x47, 0x53, 0x32, 0x42, 0x49, 0x47,
                     0x53, 0x32, 0x42, 0x49, 0x47, 0x53, 0x32, 0x01};
  if (const char* k = std::getenv("TB2_SESSION_KEY")) {
    const size_t len = std::strlen(k);
    if (len >= 32) {
      for (int i = 0; i < 16; ++i) {
        unsigned byte = 0;
        if (std::sscanf(k + i * 2, "%2x", &byte) == 1) key[i] = (uint8_t)byte;
      }
    }
  }
  StoreGuestBytes(base, result + 0x2C, key, sizeof(key));  // XNKEY

  StoreGuestU32(base, result + 0x3C, 1);  // open public slots
  StoreGuestU32(base, result + 0x40, 0);  // open private slots
  StoreGuestU32(base, result + 0x44, 1);  // filled public slots
  StoreGuestU32(base, result + 0x48, 0);  // filled private slots
  StoreGuestU32(base, result + 0x4C, kPropCount);
  StoreGuestU32(base, result + 0x50, 0);
  StoreGuestU32(base, result + 0x54, props);
  StoreGuestU32(base, result + 0x58, 0);

  // The game requires at least eight properties. Use the ids the search filter
  // asks for (0x1000003e..0x10000044); those map into the result object without
  // hitting any special-case branch.
  for (uint32_t i = 0; i < kPropCount; ++i) {
    StoreGuestU32(base, props + i * kPropSize + 0x00, 0x1000003E + (i % 7));
    StoreGuestU32(base, props + i * kPropSize + 0x10, 0);
  }
  return block;
}

// Address of the account our synthetic sign-in created (set in the
// Function_823E9F18 hook), so the sign-out/reset path can leave it alone.
uint32_t g_fake_account = 0;

// Log the XLive account list (vector: +4 capacity, +8 size, +0xc data ptr of
// 4-byte element pointers) whenever its size changes.
void LogAccountList(uint8_t* base, const char* where) {
  const uint32_t list = LoadGuestU32(base, kAccountList);
  if (!list) return;
  const uint32_t cap = LoadGuestU32(base, list + 4);
  const uint32_t size = LoadGuestU32(base, list + 8);
  const uint32_t data = LoadGuestU32(base, list + 0xC);
  static uint32_t last_size = 0xFFFFFFFF;
  if (size == last_size) return;
  last_size = size;
  REXLOG_INFO("OnlineDebug: account list {}: cap={} size={} data=0x{:08X}", where, cap, size, data);
  for (uint32_t i = 0; i < size && i < 8; ++i) {
    const uint32_t acct = LoadGuestU32(base, data + i * 4);
    REXLOG_INFO("OnlineDebug:   acct[{}]=0x{:08X} user={} vtable=0x{:08X}", i, acct,
                acct ? LoadGuestU32(base, acct + 4) : 0xFFFFFFFF,
                acct ? LoadGuestU32(base, acct) : 0);
  }
}

void MaybeInjectFakeAccount(PPCContext& parent, uint8_t* base) {
  if (!std::getenv("TB2_FAKE_ACCOUNT")) return;
  static bool injected = false;
  if (LoadGuestU32(base, kLiveMode) != kLiveModeOnline) {
    injected = false;  // left Live mode; allow a fresh injection later
    return;
  }
  if (injected) return;
  if (LoadGuestU32(base, kAccountList) == 0) return;
  if (LoadGuestU32(base, kEventQueue) == 0) return;

  const uint32_t pending = GuestAlloc(parent, base, 0x38, 1);
  if (!pending) {
    REXLOG_WARN("OnlineDebug: TB2_FAKE_ACCOUNT: pending allocation failed");
    return;
  }
  std::memset(GuestPtr(base, pending), 0, 0x38);

  StoreGuestU32(base, pending + 0x00, 0);  // user index
  char name[16] = {};
  if (const char* n = std::getenv("TB2_ACCOUNT_NAME")) {
    std::strncpy(name, n, sizeof(name) - 1);
  } else {
    // Match the SDK's TB2_FAKE_LIVE profile ("User" /
    // 0xB13EBABEBABEBABE) so the XLive account and the XamUser profile agree.
    std::strncpy(name, "User", sizeof(name) - 1);
  }
  // pending+0x08 is the account's 16-byte player-id record (account vtable[0]). It travels in the
  // participant table and the Game Room draws it as the gamertag; the game also uses its first
  // 8 bytes as an id (stats lookups, slot matching). So it holds the NUL-padded name.
  // shortcut: names sharing their first 8 chars get the same id; fine for two friends, revisit for lobbies.
  std::memcpy(GuestPtr(base, pending + 0x08), name, 16);
  StoreGuestU32(base, pending + 0x18, 0);  // account type (0 = Live account)
  StoreGuestU64(base, pending + 0x1C, TB2XuidFromName(name));  // XUID (drawn as the name)
  StoreGuestU32(base, pending + 0x24, 0);  // user flags
  StoreGuestU32(base, pending + 0x28, 2);  // sign-in state (signed in to Live)
  StoreGuestU32(base, pending + 0x2C, 0);  // guest number
  StoreGuestU32(base, pending + 0x30, 4);
  StoreGuestU8(base, pending + 0x34, 1);

  // Replace the never-completing Live pending object. The old one is
  // intentionally leaked so the Live stack cannot double-free it.
  StoreGuestU32(base, kPendingSignin, pending);
  EnqueueLiveEvent(parent, base, 0x3c);
  injected = true;
  REXLOG_INFO("OnlineDebug: synthetic Live sign-in pending=0x{:08X} name='{}'", pending, name);
}

}  // namespace

// Function_823F82D8 is the end of the search-start path (Function_823FA440):
// it issues the XLive search and, if that reports success synchronously, runs
// Function_823F81B0. Since the Live service is dead it normally fails, so
// install a synthetic result list and process it ourselves.
extern "C" REX_FUNC(sub_823F82D8) {
  const uint32_t manager = ctx.r3.u32;
  __imp__sub_823F82D8(ctx, base);
  if (!manager) return;

  if (std::getenv("TB2_FAKE_HOST")) {
    uint32_t session = LoadGuestU32(base, 0x825f5144);
    if (!session) {
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = manager;
      frame.ctx.r4.u64 = 0;
      frame.ctx.r5.u64 = 0;
      __imp__sub_823F4AB0(frame.ctx, base);
      session = frame.ctx.r3.u32;
      if (session) {
        StoreGuestU32(base, 0x825f5144, session);
      }
    }
    // Clear search status
    StoreGuestU32(base, manager + 0xA8, 0);

    if (session) {
      if (std::getenv("TB2_ONLINE_DEBUG")) {
        REXLOG_INFO("OnlineDebug: TB2_FAKE_HOST: triggering host session create (session=0x{:08X}, mgr=0x{:08X})",
                    session, manager);
      }
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = session;
      frame.ctx.r4.u64 = manager + 0x44;
      frame.ctx.r5.u64 = 0;
      __imp__sub_823F44B8(frame.ctx, base);
      if (std::getenv("TB2_ONLINE_DEBUG")) {
        REXLOG_INFO("OnlineDebug: TB2_FAKE_HOST: sub_823F44B8 returned 0x{:08X}", frame.ctx.r3.u32);
      }
    }
    return;
  }

  if (!std::getenv("TB2_FAKE_SEARCH")) return;

  const uint32_t block = BuildFakeSearchResults(ctx, base);
  if (!block) {
    REXLOG_WARN("OnlineDebug: TB2_FAKE_SEARCH: result allocation failed");
    return;
  }
  StoreGuestU32(base, manager + 0xE0, block);  // result list header
  StoreGuestU32(base, manager + 0xDC, 0x08 + 0x5C + 8 * 0x18);
  StoreGuestU32(base, manager + 0xA8, 0);  // clear the search status
  rex::CallFrame frame(ctx);
  frame.ctx.r3.u64 = manager;
  __imp__sub_823F81B0(frame.ctx, base);
  const uint32_t result = block + 0x08;
  REXLOG_INFO("OnlineDebug: injected synthetic search result (mgr=0x{:08X}, block=0x{:08X}, "
              "peer={}.{}.{}.{}:{}, xnkid=0x{:016X})",
              manager, block, (*GuestPtr(base, result + 0x08)), (*GuestPtr(base, result + 0x09)), (*GuestPtr(base, result + 0x0A)),
              (*GuestPtr(base, result + 0x0B)), (uint32_t)(((*GuestPtr(base, result + 0x10)) << 8) | (*GuestPtr(base, result + 0x11))),
              ((uint64_t)LoadGuestU32(base, result) << 32) | LoadGuestU32(base, result + 4));
}

// Function_822A5328 is the only code path that shows the "Signing In..."
// dialog (via the online manager). Log every call with its user index.
extern "C" REX_FUNC(__imp__sub_822A5328);
extern "C" REX_FUNC(sub_822A5328) {
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    REXLOG_INFO("OnlineDebug: Function_822A5328(manager=0x{:08X}, user={}, lr=0x{:08X})", ctx.r3.u32,
                ctx.r4.u32, (uint32_t)ctx.lr);
  }
  // Boot race: a sign-in message can arrive before the UI manager exists
  // (*(DAT_82633548+4) is null); 822A5328 then reads its vtable and faults.
  {
    uint32_t mgr_ptr, ui;
    std::memcpy(&mgr_ptr, GuestPtr(base, 0x82633548), 4);
    mgr_ptr = __builtin_bswap32(mgr_ptr);
    ui = 0;
    if (mgr_ptr) {
      std::memcpy(&ui, GuestPtr(base, mgr_ptr + 4), 4);
      ui = __builtin_bswap32(ui);
    }
    if (!ui) {
      REXLOG_WARN("OnlineDebug: 822A5328 skipped - UI manager not ready yet");
      return;
    }
  }
  __imp__sub_822A5328(ctx, base);
}

// Function_822A54F0 starts the profile sign-in and posts the busy dialog.
extern "C" REX_FUNC(__imp__sub_822A54F0);
extern "C" REX_FUNC(sub_822A54F0) {
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    REXLOG_INFO("OnlineDebug: Function_822A54F0(manager=0x{:08X}, lr=0x{:08X})", ctx.r3.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_822A54F0(ctx, base);
}

// Function_82287700 posts a UI message (type, manager, string pointer).
// TB2_SKIP_JOIN_DIALOG suppresses the modal "Attempting to join Game Room"
// message (type 0xC52601EE, posted by Function_822DCEE8 right before the P2P
// connect) so the connect events can drive the screen transition instead of
// waiting forever on the Live game-room join.
extern "C" REX_FUNC(__imp__sub_82287700);
extern "C" REX_FUNC(sub_82287700) {
  const uint32_t type = ctx.r3.u32;
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    REXLOG_INFO("OnlineDebug: Function_82287700(type=0x{:08X}, mgr=0x{:08X}, str=0x{:08X}, lr=0x{:08X})",
                type, ctx.r4.u32, ctx.r5.u32, (uint32_t)ctx.lr);
  }
  if (std::getenv("TB2_SKIP_JOIN_DIALOG")) {
    bool suppress = (type == 0xC52601EE);
    if (!suppress && type == 0xDFC52393 && ctx.r5.u32) {
      // The dialog text is passed as a guest string pointer; only drop the
      // game-room join one (0xDFC52393 is a generic "show text" message).
      const char* text = reinterpret_cast<const char*>(GuestPtr(base, ctx.r5.u32));
      suppress = std::strncmp(text, "Attempting to join", 18) == 0;
    }
    if (suppress) {
      REXLOG_INFO("OnlineDebug: suppressed 'Attempting to join Game Room' dialog (type=0x{:08X})",
                  type);
      return;
    }
  }
  __imp__sub_82287700(ctx, base);
}

// Trace the UI message flow after MATCH_START: what is posted to the UI queue
// (820EA310), what the active screen's handler receives (823346E8), and which
// dialog types get dismissed (824B0128). Logging only, gated on TB2_UI_TRACE.
static bool g_return_to_menu_pending = false;
static uint32_t g_stats_mgr = 0;  // stats manager with a pending request; ticked from the per-frame hook
static inline uint32_t gbe32(uint8_t* base, uint32_t ea) {
  uint32_t v;
  std::memcpy(&v, GuestPtr(base, ea), 4);
  return __builtin_bswap32(v);
}

extern "C" REX_FUNC(__imp__sub_820EA310);
extern "C" REX_FUNC(sub_820EA310) {
  if (std::getenv("TB2_UI_TRACE") && ctx.r4.u32) {
    REXLOG_INFO("UITrace: post q=0x{:08X} msg={{{:08X} {:08X} {:08X} {:08X}}} lr=0x{:08X}",
                ctx.r3.u32, gbe32(base, ctx.r4.u32), gbe32(base, ctx.r4.u32 + 4),
                gbe32(base, ctx.r4.u32 + 8), gbe32(base, ctx.r4.u32 + 12), (uint32_t)ctx.lr);
  }
  __imp__sub_820EA310(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_823346E8);
extern "C" REX_FUNC(sub_823346E8) {
  if (std::getenv("TB2_UI_TRACE") && ctx.r4.u32) {
    REXLOG_INFO("UITrace: screen-handler type=0x{:08X} sub=0x{:08X} lr=0x{:08X}",
                gbe32(base, ctx.r4.u32 + 4), gbe32(base, ctx.r4.u32 + 8), (uint32_t)ctx.lr);
  }
  __imp__sub_823346E8(ctx, base);
}

// Screen-change requests (screen-name hash in r4) and the online-flow state
// (DAT_825df688 -> +0xC8C), sampled at most once per second from the online
// manager's per-frame update (82289560).
extern "C" REX_FUNC(__imp__sub_824A2C68);
extern "C" REX_FUNC(__imp__sub_823FB6A0);
extern "C" REX_FUNC(__imp__sub_824AF288);
extern "C" REX_FUNC(sub_824AF288) {
  if (std::getenv("TB2_UI_TRACE")) {
    REXLOG_INFO("UITrace: pop-screen mgr=0x{:08X} lr=0x{:08X}", ctx.r3.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_824AF288(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_8235BAA8);
extern "C" REX_FUNC(sub_8235BAA8) {
  if (std::getenv("TB2_UI_TRACE")) {
    REXLOG_INFO("UITrace: transition-kick r3=0x{:08X} r4=0x{:08X} lr=0x{:08X}", ctx.r3.u32,
                ctx.r4.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_8235BAA8(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_82289278);
extern "C" REX_FUNC(sub_82289278) {
  if (std::getenv("TB2_UI_TRACE")) {
    REXLOG_INFO("UITrace: return-to-menu (82289278) lr=0x{:08X}", (uint32_t)ctx.lr);
  }
  __imp__sub_82289278(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_824AF148);
extern "C" REX_FUNC(sub_824AF148) {
  if (std::getenv("TB2_UI_TRACE")) {
    REXLOG_INFO("UITrace: screen-change mgr=0x{:08X} hash=0x{:08X} arg=0x{:08X} lr=0x{:08X}",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_824AF148(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_82289560);
extern "C" REX_FUNC(sub_82289560) {
  // The Game Room's per-player lookups use the local Live account (FUN_823e9e90), which is
  // null while DAT_825d4bf8 is 4 = "no user". The real sign-in sets it to the user index via
  // 823E9EB0; the synthetic account never does. Once a session exists, mark user 0 signed in.
  if (std::getenv("TB2_FAKE_ACCOUNT") && LoadGuestU32(base, 0x825f5144) != 0 &&
      LoadGuestU32(base, 0x825d4bf8) == 4) {
    StoreGuestU32(base, 0x825d4bf8, 0);
    REXLOG_INFO("OnlineDebug: session exists, set Live user index (DAT_825d4bf8) 4 -> 0");
  }
  if (std::getenv("TB2_UI_TRACE")) {
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds(2);
    auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::seconds(1)) {
      last = now;
      const uint32_t flow = gbe32(base, 0x825df688);
      REXLOG_INFO("UITrace: flow=0x{:08X} state(+C8C)=0x{:08X} next(+C90)=0x{:08X}", flow,
                  flow ? gbe32(base, flow + 0xC8C) : 0, flow ? gbe32(base, flow + 0xC90) : 0);
      // Which known modal dialogs does the UI think are active? (hash read from each DAT)
      static const uint32_t kDlg[] = {0x825e62c8, 0x825e3944, 0x825e4e40, 0x825e6200, 0x825e613c,
                                      0x825e61d4, 0x825e6080, 0x825e614c, 0x825e5ff8, 0x825e4f3c,
                                      0x825e62e4, 0x825e6320, 0x825e6108, 0x825e6168};
      std::string act;
      for (uint32_t d : kDlg) {
        rex::CallFrame frame(ctx);
        frame.ctx.r3.u64 = gbe32(base, d);
        __imp__sub_824A2C68(frame.ctx, base);
        if (frame.ctx.r3.u32 & 0xFF) act += fmt::format(" {:08X}", d);
      }
      REXLOG_INFO("UITrace: active dialogs:{}", act.empty() ? " none" : act);
    }
  }
  __imp__sub_82289560(ctx, base);
  // The Game Room never pumps the stats manager's update (vtable+0x28 = 823FB6A0), so the
  // overlapped stats result is never collected. Tick it while any slot is pending.
  if (g_stats_mgr) {
    const uint32_t m = g_stats_mgr;
    if ((*GuestPtr(base, m + 0x104)) || (*GuestPtr(base, m + 0x105)) || (*GuestPtr(base, m + 0x106))) {
      static std::chrono::steady_clock::time_point last_tick_log{};
      if (std::getenv("TB2_ONLINE_DEBUG") && OncePerSecond(last_tick_log)) {
        REXLOG_INFO("OnlineDebug: ticking stats mgr 0x{:08X} flags={}{}{} ov0=0x{:08X} ov1=0x{:08X} buf0=0x{:08X} buf1=0x{:08X}",
                    m, (*GuestPtr(base, m + 0x104)), (*GuestPtr(base, m + 0x105)), (*GuestPtr(base, m + 0x106)),
                    LoadGuestU32(base, m + 0xB0), LoadGuestU32(base, m + 0xB0 + 0x1C),
                    LoadGuestU32(base, m + 0x108), LoadGuestU32(base, m + 0x10C));
      }
      rex::CallFrame frame(ctx);
      frame.ctx.r3.u64 = m;
      __imp__sub_823FB6A0(frame.ctx, base);
    } else {
      g_stats_mgr = 0;
    }
  }
  if (g_return_to_menu_pending) {
    g_return_to_menu_pending = false;
    REXLOG_INFO("OnlineDebug: leave finished, calling return-to-menu (82289278)");
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = ctx.r3.u64;
    __imp__sub_82289278(frame.ctx, base);
  }
}

extern "C" REX_FUNC(__imp__sub_824B0128);
extern "C" REX_FUNC(sub_824B0128) {
  if (std::getenv("TB2_UI_TRACE")) {
    REXLOG_INFO("UITrace: dismiss type=0x{:08X} lr=0x{:08X}", ctx.r3.u32, (uint32_t)ctx.lr);
  }
  // End of the leave handling (event 0x26): with the fake account the game never
  // pops the Game Room by itself, so queue the game's own return-to-menu call.
  if (std::getenv("TB2_FAKE_ACCOUNT") && ctx.r3.u32 == 0xDFC52393 && (uint32_t)ctx.lr == 0x822A4924) {
    g_return_to_menu_pending = true;
  }
  __imp__sub_824B0128(ctx, base);
}

// Select (X) in the Game Room opens a "view player" popup. It needs the Live
// player-info record for the focused slot, which does not exist offline, so
// sub_822DCB20 dereferences null (guest read of 0x4/0xC, endless fault loop).
// With the synthetic Live account the action is a no-op.
extern "C" REX_FUNC(__imp__sub_8230E458);
extern "C" REX_FUNC(sub_8230E458) {
  if (std::getenv("TB2_FAKE_ACCOUNT")) {
    REXLOG_INFO("OnlineDebug: Game Room Select (view player) blocked - no Live player data");
    return;
  }
  __imp__sub_8230E458(ctx, base);
}

// Right stick in the Game Room = "open gamer card": same missing player record
// (sub_822DCB20 null deref) and a Live-only XamShowGamerCardUIForXUID afterwards.
// Report the input as handled and do nothing.
extern "C" REX_FUNC(__imp__sub_8230E0F8);
extern "C" REX_FUNC(sub_8230E0F8) {
  if (std::getenv("TB2_FAKE_ACCOUNT")) {
    REXLOG_INFO("OnlineDebug: Game Room gamer card blocked - no Live player data");
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_8230E0F8(ctx, base);
}

// LB/RB in the Game Room switch to "player list" mode (sub_822DC738), which waits for an
// online-player enumeration that never arrives; in that mode most buttons are ignored.
// Report the input as handled and stay in the normal view.
extern "C" REX_FUNC(__imp__sub_8230E0A0);
extern "C" REX_FUNC(sub_8230E0A0) {
  if (std::getenv("TB2_FAKE_ACCOUNT")) {
    REXLOG_INFO("OnlineDebug: Game Room player list blocked - no Live player list");
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_8230E0A0(ctx, base);
}

// Function_82297018 posts the "Signing In..." busy dialog and then runs the
// XLive sign-in check (Function_822897C8). TB2_SKIP_SIGNIN_DIALOG skips the
// dialog post so we can see what the game does next.
extern "C" REX_FUNC(__imp__sub_82297018);
extern "C" REX_FUNC(sub_82297018) {
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    REXLOG_INFO("OnlineDebug: Function_82297018(manager=0x{:08X}) skip={})", ctx.r3.u32,
                std::getenv("TB2_SKIP_SIGNIN_DIALOG") != nullptr);
  }
  if (std::getenv("TB2_SKIP_SIGNIN_DIALOG")) return;
  __imp__sub_82297018(ctx, base);
}

// Function_822897C8 decides whether the "Signing In..." dialog can be dismissed
// (FUN_823e32d8(user) == 0 && Function_823E9E38(user) == 1).
extern "C" REX_FUNC(__imp__sub_822897C8);
extern "C" REX_FUNC(sub_822897C8) {
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    const uint32_t c = LoadGuestU32(base, 0x825e2574);
    const uint32_t m = c ? LoadGuestU32(base, c + 0x1c) : 0;
    const uint32_t mode = m ? LoadGuestU32(base, m + 0x24) : 0;
    const uint32_t arr = m ? LoadGuestU32(base, m + mode * 4) : 0;
    const uint32_t user = arr ? LoadGuestU32(base, arr + 4) : 0xFFFFFFFF;
    const uint32_t sess = LoadGuestU32(base, 0x825f50ec);
    const uint32_t sess_user = sess ? LoadGuestU32(base, sess) : 0xFFFFFFFF;
    const uint32_t cur = LoadGuestU32(base, 0x825f50e8);
    REXLOG_INFO(
        "OnlineDebug: Function_822897C8(user={}, DAT_825f50ec=0x{:08X}, sess_user={}, "
        "DAT_825f50e8=0x{:08X})",
        user, sess, sess_user, cur);
  }
  __imp__sub_822897C8(ctx, base);
}

// Log Function_823E9E38 only when called from Function_822897C8.
extern "C" REX_FUNC(__imp__sub_823E9E38);
extern "C" REX_FUNC(sub_823E9E38) {
  const uint32_t arg = ctx.r3.u32;
  const uint32_t lr = (uint32_t)ctx.lr;
  __imp__sub_823E9E38(ctx, base);
  if (std::getenv("TB2_ONLINE_DEBUG") && lr >= 0x822897C8 && lr < 0x82289A80) {
    REXLOG_INFO("OnlineDebug: Function_823E9E38({}) -> {}", arg, ctx.r3.u32);
  }
}

// --- XLive stack tracing (statistically-linked Live library) ---------------

// Sign-in processor: only advances queue entries of type 0x3c. TB2_FAKE_ACCOUNT
// installs a synthetic pending object and enqueues 0x3c just before the scan.
extern "C" REX_FUNC(__imp__sub_823F5F88);
extern "C" REX_FUNC(sub_823F5F88) {
  MaybeInjectFakeAccount(ctx, base);
  static std::chrono::steady_clock::time_point last_log{};
  if (std::getenv("TB2_ONLINE_DEBUG") && OncePerSecond(last_log)) {
    REXLOG_INFO("OnlineDebug: Function_823F5F88 (sign-in processor)");
  }
  __imp__sub_823F5F88(ctx, base);
  if (std::getenv("TB2_FAKE_ACCOUNT")) LogAccountList(base, "post-signin");
}

// Account constructor: log its inputs/output so a failed sign-in is visible.
extern "C" REX_FUNC(__imp__sub_823E9F18);
extern "C" REX_FUNC(sub_823E9F18) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t type = ctx.r4.u32;
  const uint32_t pending = LoadGuestU32(base, kPendingSignin);
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    REXLOG_INFO("OnlineDebug: Function_823E9F18 enter user={} type={} pending=0x{:08X}", user, type,
                pending);
  }
  __imp__sub_823E9F18(ctx, base);
  const uint32_t acct = ctx.r3.u32;
  if (acct) g_fake_account = acct;
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    REXLOG_INFO("OnlineDebug: Function_823E9F18 -> 0x{:08X}{}", acct,
                acct ? fmt::format(" (user={} vtable=0x{:08X})", LoadGuestU32(base, acct + 4),
                                   LoadGuestU32(base, acct))
                     : std::string());
  }
}

// Function_823F5DA0(user, reason) is the sign-out/removal handler: it removes
// the user's account from DAT_825f50e8 and deletes it. The online manager calls
// it as a "Live disconnected" reset (Function_822B3B78), which would otherwise
// erase the account our synthetic sign-in just created. Keep that account.
extern "C" REX_FUNC(__imp__sub_823E3348);
extern "C" REX_FUNC(__imp__sub_823F5DA0);
extern "C" REX_FUNC(sub_823F5DA0) {
  const uint32_t user = ctx.r3.u32;
  const uint32_t reason = ctx.r4.u32;
  uint32_t acct = 0;
  {
    rex::CallFrame frame(ctx);
    frame.ctx.r3.u64 = user;
    __imp__sub_823E3348(frame.ctx, base);
    acct = frame.ctx.r3.u32;
  }
  static uint32_t log_count = 0;
  if (std::getenv("TB2_ONLINE_DEBUG") && log_count < 5) {
    ++log_count;
    REXLOG_INFO("OnlineDebug: Function_823F5DA0(user={}, reason={}, acct=0x{:08X}, lr=0x{:08X})",
                user, reason, acct, (uint32_t)ctx.lr);
  }
  if (std::getenv("TB2_FAKE_ACCOUNT") && acct && acct == g_fake_account) {
    static uint32_t keep_logged = 0;
    if (std::getenv("TB2_ONLINE_DEBUG") && keep_logged < 5) {
      ++keep_logged;
      REXLOG_INFO("OnlineDebug: keeping synthetic account 0x{:08X} (skip sign-out)", acct);
    }
    return;
  }
  __imp__sub_823F5DA0(ctx, base);
}

// OP Session connect (vtable+0x58): the game hands the XSESSION_INFO to the
// Live session layer here. Dump the info the game will actually use.
extern "C" REX_FUNC(__imp__sub_823F46C0);
extern "C" REX_FUNC(sub_823F46C0) {
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    const uint32_t info = ctx.r4.u32;  // manager+0x44 (session-info object)
    const uint32_t arg3 = ctx.r5.u32;  // screen[0x298]
    if (info) {
      const uint32_t si = info + 0x10;  // XSESSION_INFO
      REXLOG_INFO(
          "OnlineDebug: 823F46C0 info=0x{:08X} arg3={} xnkid=0x{:016X} "
          "ina={}.{}.{}.{} online={}.{}.{}.{} port={} key0=0x{:08X}",
          info, arg3, ((uint64_t)LoadGuestU32(base, si) << 32) | LoadGuestU32(base, si + 4),
          (*GuestPtr(base, si + 8)), (*GuestPtr(base, si + 9)), (*GuestPtr(base, si + 10)), (*GuestPtr(base, si + 11)), (*GuestPtr(base, si + 12)), (*GuestPtr(base, si + 13)),
          (*GuestPtr(base, si + 14)), (*GuestPtr(base, si + 15)), (uint32_t)(((*GuestPtr(base, si + 0x10)) << 8) | (*GuestPtr(base, si + 0x11))),
          LoadGuestU32(base, si + 0x2C));
    } else {
      REXLOG_INFO("OnlineDebug: 823F46C0 info=0 arg3={}", arg3);
    }
  }
  __imp__sub_823F46C0(ctx, base);
}

// Host session create (session vtable+0x5c / slot 23).
extern "C" REX_FUNC(sub_823F44B8) {
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    REXLOG_INFO("OnlineDebug: 823F44B8 host session create: session=0x{:08X} info=0x{:08X} arg3={} lr=0x{:08X}",
                ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_823F44B8(ctx, base);
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    REXLOG_INFO("OnlineDebug: 823F44B8 returned 0x{:08X}", ctx.r3.u32);
  }
}

// Host state-1 transition handler (enqueues event 0x42).
extern "C" REX_FUNC(sub_823ECEE0) {
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    REXLOG_INFO("OnlineDebug: 823ECEE0 host state 1 enter: session=0x{:08X} lr=0x{:08X}",
                ctx.r3.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_823ECEE0(ctx, base);
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    REXLOG_INFO("OnlineDebug: 823ECEE0 enqueued event 0x42 (session+152={})",
                ctx.r3.u32 ? (uint32_t)(*GuestPtr(base, ctx.r3.u32 + 152)) : 0);
  }
}

// Low-level connect (sockaddr builder): log the address/port the transport uses.
extern "C" REX_FUNC(__imp__sub_82420E38);
extern "C" REX_FUNC(sub_82420E38) {
  if (const char* p = std::getenv("TB2_PEER_PORT")) {
    ctx.r5.u64 = (uint16_t)std::strtoul(p, nullptr, 0);
  }
  if (const char* peer = std::getenv("TB2_PEER_IP")) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(peer, "%u.%u.%u.%u", &a, &b, &c, &d) == 4 && a < 256 && b < 256 &&
        c < 256 && d < 256) {
      ctx.r4.u64 = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8) | (uint32_t)d;
    }
  }
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    const uint32_t packed = ctx.r4.u32;
    REXLOG_INFO("OnlineDebug: 82420E38 obj=0x{:08X} addr={}.{}.{}.{} port={} lr=0x{:08X}",
                ctx.r3.u32, (packed >> 24) & 0xFF, (packed >> 16) & 0xFF, (packed >> 8) & 0xFF,
                packed & 0xFF, ctx.r5.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_82420E38(ctx, base);
}

// Server bind & listen: binds 0.0.0.0:port with SO_REUSEADDR and listen(1).
extern "C" REX_FUNC(sub_82420F20) {
  const uint32_t obj = ctx.r3.u32;
  uint16_t port = ctx.r4.u16;
  // Only override the match transport's listener. The statically-linked Live
  // session layer also calls this (its 4 listeners on 5007); rewriting those to
  // TB2_HOST_PORT would make the peer simulator accidentally connect to one of
  // them instead of the P2P host listener.
  if (port == 1000) {
    if (const char* p = std::getenv("TB2_HOST_PORT")) {
      port = (uint16_t)std::strtoul(p, nullptr, 0);
      ctx.r4.u64 = port;
    }
  }
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    REXLOG_INFO("OnlineDebug: 82420F20 server bind & listen: obj=0x{:08X} port={} lr=0x{:08X}",
                obj, port, (uint32_t)ctx.lr);
  }
  __imp__sub_82420F20(ctx, base);
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    REXLOG_INFO("OnlineDebug: 82420F20 returned {} (socket_fd={})", ctx.r3.u32,
                obj ? LoadGuestU32(base, obj + 24) : 0);
  }
}

// cXblSession::SessionDelete (vtable[25]) and the state-4 transition. The fake
// Live layer declares the freshly created host session dead, which tears the
// transport down (and bounces the UI back to the main menu). TB2_HOST_KEEPALIVE
// suppresses that so the P2P transport stays up for private-server play.
extern "C" REX_FUNC(sub_823F47D0) {
  if (std::getenv("TB2_HOST_KEEPALIVE")) {
    REXLOG_INFO("OnlineDebug: SessionDelete suppressed (session=0x{:08X}, lr=0x{:08X})",
                ctx.r3.u32, (uint32_t)ctx.lr);
    return;
  }
  __imp__sub_823F47D0(ctx, base);
}

extern "C" REX_FUNC(sub_823ED1E0) {
  if (std::getenv("TB2_HOST_KEEPALIVE")) {
    REXLOG_INFO("OnlineDebug: 823ED1E0 teardown suppressed (session=0x{:08X}, lr=0x{:08X})",
                ctx.r3.u32, (uint32_t)ctx.lr);
    return;
  }
  __imp__sub_823ED1E0(ctx, base);
}

// Session health check (sub_823F9960). It validates that DAT_825d4bf8 has a
// Live account; if the lookup fails it marks the session dead (session+84=3)
// and calls SessionDelete every frame. Our synthetic account is user 0, but
// the session's user index is not, so force the check to user 0 for the call.
extern "C" REX_FUNC(sub_823F9960) {
  const uint32_t saved_user = LoadGuestU32(base, 0x825d4bf8);
  const bool tick = std::getenv("TB2_SESSION_TICK") != nullptr;
  if (tick && saved_user != 0) StoreGuestU32(base, 0x825d4bf8, 0);
  static std::chrono::steady_clock::time_point last_log{};
  if (std::getenv("TB2_ONLINE_DEBUG") && OncePerSecond(last_log)) {
    const uint32_t s = ctx.r3.u32;
    REXLOG_INFO("OnlineDebug: 823F9960 health check session=0x{:08X} user_global={} mode={} state={}",
                s, saved_user, s ? LoadGuestU32(base, s + 0x5C) : 0,
                s ? LoadGuestU32(base, s + 0xC0) : 0);
  }
  const uint32_t sess = ctx.r3.u32, st0 = sess ? LoadGuestU32(base, sess + 0x54) : 0;
  __imp__sub_823F9960(ctx, base);
  { const uint32_t st1 = sess ? LoadGuestU32(base, sess + 0x54) : 0;
    static uint32_t prev = 0xFFFF; if (st1 != prev) { REXLOG_INFO("SessState +0x54: {} -> {} (health check: before={} user_global={} acct={:#x})", prev, st1, st0, saved_user, 0); prev = st1; } }
  if (tick && saved_user != 0) StoreGuestU32(base, 0x825d4bf8, saved_user);
}

// Per-player stats request (sub_823EE498, the Game Room slot lookup). It bails out while
// DAT_825d4bf8 (the Live user index) is 4 = no user, so no XamUserCreateStatsEnumerator is
// issued and the slot name stays blank. Use user 0 (the synthetic account) for the call.
extern "C" REX_FUNC(__imp__sub_823EE498);
extern "C" REX_FUNC(sub_823EE498) {
  const uint32_t this_mgr = ctx.r3.u32;
  const uint32_t saved_user = LoadGuestU32(base, 0x825d4bf8);
  const bool fix = std::getenv("TB2_FAKE_ACCOUNT") && saved_user == 4;
  if (fix) StoreGuestU32(base, 0x825d4bf8, 0);
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    REXLOG_INFO("OnlineDebug: 823EE498 player stats request (user_global was {}, forced {})",
                saved_user, fix);
    if (ctx.r4.u32) {
      char hex[64] = {};
      for (int i = 0; i < 16; i++) std::snprintf(hex + i * 2, 3, "%02X", (*GuestPtr(base, ctx.r4.u32 + i)));
      REXLOG_INFO("OnlineDebug: 823EE498 xuid record @0x{:08X} = {}", ctx.r4.u32, hex);
    }
  }
  __imp__sub_823EE498(ctx, base);
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    REXLOG_INFO("OnlineDebug: 823EE498 returned {} (1 = request pending)", ctx.r3.u32 & 0xFF);
  }
  if (std::getenv("TB2_FAKE_ACCOUNT") && (ctx.r3.u32 & 0xFF) == 1) g_stats_mgr = this_mgr;
  if (fix) StoreGuestU32(base, 0x825d4bf8, saved_user);
}

// Per-slot poll of the stats request (sub_823FA9F8): slot flag + overlapped status, 1 Hz.
extern "C" REX_FUNC(__imp__sub_823FA9F8);
extern "C" REX_FUNC(sub_823FA9F8) {
  static std::chrono::steady_clock::time_point last{};
  if (std::getenv("TB2_ONLINE_DEBUG") && ctx.r3.u32 && ctx.r4.u32 < 3 && OncePerSecond(last)) {
    const uint32_t m = ctx.r3.u32, slot = ctx.r4.u32;
    const uint32_t flag = (*GuestPtr(base, m + 0x104 + slot));
    if (flag) {
      REXLOG_INFO("OnlineDebug: 823FA9F8 poll slot {} flag={} overlapped[0]=0x{:08X} buf=0x{:08X}",
                  slot, flag, LoadGuestU32(base, m + 0xB0 + slot * 0x1C),
                  LoadGuestU32(base, m + 0x108 + slot * 4));
    }
  }
  __imp__sub_823FA9F8(ctx, base);
}

// Game Room enter handler and its stats-request helper: log whether they run and the flag
// (game-state +0xE0 = settings byte 12) that gates the per-player stats requests.
extern "C" REX_FUNC(__imp__sub_8234E1F0);
extern "C" REX_FUNC(sub_8234E1F0) {
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    const uint32_t g = LoadGuestU32(base, 0x825e2574);
    const uint32_t x = g ? LoadGuestU32(base, g + 0x1c) : 0;
    const uint32_t p = x ? LoadGuestU32(base, x + 8) : 0;
    REXLOG_INFO("OnlineDebug: 8234E1F0 Game Room enter, game-state flag +0xE0 = {}",
                p ? int((*GuestPtr(base, p + 0xE0))) : -1);
  }
  __imp__sub_8234E1F0(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_8230DA00);
extern "C" REX_FUNC(sub_8230DA00) {
  if (std::getenv("TB2_ONLINE_DEBUG")) REXLOG_INFO("OnlineDebug: 8230DA00 requesting player stats");
  __imp__sub_8230DA00(ctx, base);
}

// Game Room Ready logic (log only): sub_8230DC58 = A/Ready handler. It starts the match when
// both per-slot ready states (screen+8, screen+0xC) are 2, else shows "waiting for opponent".
extern "C" REX_FUNC(__imp__sub_8230DC58);
extern "C" REX_FUNC(sub_8230DC58) {
  if (std::getenv("TB2_ONLINE_DEBUG") && ctx.r3.u32) {
    const uint32_t scr = ctx.r3.u32;
    REXLOG_INFO("OnlineDebug: Game Room A/Ready handler: mode={} slot-state[0]={} slot-state[1]={} cmd=0x{:X}",
                LoadGuestU32(base, scr + 0x60), LoadGuestU32(base, scr + 8), LoadGuestU32(base, scr + 12),
                ctx.r4.u32);
  }
  __imp__sub_8230DC58(ctx, base);
  if (std::getenv("TB2_ONLINE_DEBUG") && ctx.r3.u32 <= 1) {
    REXLOG_INFO("OnlineDebug: Game Room A/Ready handler returned {}", ctx.r3.u32 & 0xFF);
  }
}
extern "C" REX_FUNC(__imp__sub_822DC4D0);
extern "C" REX_FUNC(sub_822DC4D0) {
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    REXLOG_INFO("OnlineDebug: ready-state update (822DC4D0) row={} slot={} lr=0x{:08X}", ctx.r4.u32,
                ctx.r5.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_822DC4D0(ctx, base);
}

// Player-info event (0x46) producers and the Game Room update that consumes it.
extern "C" REX_FUNC(__imp__sub_823F8480);
extern "C" REX_FUNC(sub_823F8480) {
  if (std::getenv("TB2_ONLINE_DEBUG")) REXLOG_INFO("OnlineDebug: 823F8480 building 0x46 (XUID-only fallback) slot={}", ctx.r4.u32);
  __imp__sub_823F8480(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_823F8598);
extern "C" REX_FUNC(sub_823F8598) {
  if (std::getenv("TB2_ONLINE_DEBUG")) REXLOG_INFO("OnlineDebug: 823F8598 building 0x46 (from stats result)");
  __imp__sub_823F8598(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_82334068);
extern "C" REX_FUNC(sub_82334068) {
  static std::chrono::steady_clock::time_point last{};
  if (std::getenv("TB2_ONLINE_DEBUG") && OncePerSecond(last)) {
    REXLOG_INFO("OnlineDebug: Game Room Update (82334068) running");
  }
  __imp__sub_82334068(ctx, base);
  static std::chrono::steady_clock::time_point last_ret{};
  if (std::getenv("TB2_ONLINE_DEBUG") && OncePerSecond(last_ret)) {
    REXLOG_INFO("OnlineDebug: Game Room Update returned {}", ctx.r3.u32 & 0xFF);
  }
}

// Session state machine / packet pump. The Game Room screen does not tick the
// OP session in this build, so while TB2_FAKE_HOST/TB2_FAKE_SEARCH is set we
// drive it from the socket pump below (which does run every frame).
extern "C" REX_FUNC(sub_823FA360) {
  static std::chrono::steady_clock::time_point last_log{};
  if (std::getenv("TB2_ONLINE_DEBUG") && OncePerSecond(last_log)) {
    const uint32_t s = ctx.r3.u32;
    REXLOG_INFO("OnlineDebug: 823FA360 session tick session=0x{:08X} state={}", s,
                s ? LoadGuestU32(base, s + 0xC0) : 0);
  }
  __imp__sub_823FA360(ctx, base);
}

// Per-tick socket pump: handles accept, select, send, and recv.
extern "C" REX_FUNC(sub_82420CB8) {
  const uint32_t obj = ctx.r3.u32;
  const uint32_t old_state = obj ? LoadGuestU32(base, obj + 16) : 0;
  __imp__sub_82420CB8(ctx, base);

  // TB2_P2P_HOST: the Game Room screen does not pump the OP session, so
  // inbound packets sit in the transport ring buffer forever. Drive just the
  // packet pump (sub_823F97E8) here - NOT the state machine, which tears the
  // session down if ticked outside the game's own transition points.
  if (std::getenv("TB2_P2P_HOST")) {
    static bool in_tick = false;
    static std::chrono::steady_clock::time_point last_tick{};
    const auto now = std::chrono::steady_clock::now();
    if (!in_tick && now - last_tick >= std::chrono::milliseconds(10)) {
      last_tick = now;
      const uint32_t session = LoadGuestU32(base, 0x825f5144);
      const uint32_t transport = session ? LoadGuestU32(base, session + 0x60) : 0;
      const uint32_t sockwrap = transport ? LoadGuestU32(base, transport + 48) : 0;
      const uint32_t sockstate = sockwrap ? LoadGuestU32(base, sockwrap + 16) : 0;
      // Only touch the session once a peer is actually connected; changing the
      // mode on an idle host session makes the participant monitor dereference
      // a half-built list.
      if (session && transport && sockstate == 3) {
        // The Live layer normally sets the session mode to 2 once the P2P
        // session is joined; without it sub_823F97E8 routes packets to the
        // Live-layer parser (sub_823F7780), which ignores wire opcodes 9/10.
        // Force the P2P dispatcher and mark our handshake as sent so the host
        // only answers the peer's opcode 9 instead of sending one too.
        StoreGuestU32(base, session + 0x5C, 2);
        StoreGuestU32(base, session + 0x58, 1);
        in_tick = true;
        rex::CallFrame frame(ctx);
        frame.ctx.r3.u64 = session;
        __imp__sub_823F97E8(frame.ctx, base);
        in_tick = false;
      }
    }
  }

  // TB2_SESSION_TICK (client/joiner side): the normal flow arms +0x58=2 by
  // receiving opcode 5, but that opcode's handler also joins the real Xbox
  // Live XSession (XamSessionRefObjByHandle) and crashes on the SDK's fake
  // object. Set the flag directly once we have sent our participant table
  // (+0x58==1), so a subsequent opcode 10 queues MATCH_START.
  if (std::getenv("TB2_SESSION_TICK")) {
    const uint32_t session = LoadGuestU32(base, 0x825f5144);
    const uint32_t transport = session ? LoadGuestU32(base, session + 0x60) : 0;
    if (session && transport && LoadGuestU32(base, session + 0x5C) == 2 &&
        LoadGuestU32(base, session + 0x58) == 1) {
      StoreGuestU32(base, session + 0x58, 2);
    }
  }

  if (!std::getenv("TB2_ONLINE_DEBUG") || !obj) return;
  const uint32_t new_state = LoadGuestU32(base, obj + 16);
  if (old_state != new_state) {
    const uint32_t fd = LoadGuestU32(base, obj + 24);
    if (old_state == 1 && new_state == 3) {
      const uint32_t peer_ip = LoadGuestU32(base, obj + 8);
      REXLOG_INFO("OnlineDebug: 82420CB8 [HOST] ACCEPTED PEER connection! socket_fd={} "
                  "peer_ip={}.{}.{}.{}",
                  fd, (peer_ip >> 24) & 0xFF, (peer_ip >> 16) & 0xFF, (peer_ip >> 8) & 0xFF,
                  peer_ip & 0xFF);
    } else if (old_state == 2 && new_state == 3) {
      REXLOG_INFO("OnlineDebug: 82420CB8 [CLIENT] CONNECTED to host! socket_fd={}", fd);
    } else if (new_state == 0) {
      REXLOG_INFO("OnlineDebug: 82420CB8 socket state transitioned {} -> 0 (CLOSED / EOF / ERROR)",
                  old_state);
    } else {
      REXLOG_INFO("OnlineDebug: 82420CB8 socket state {} -> {}", old_state, new_state);
    }
  }
}

// Handshake packet builder/sender (sends wire opcode 9).
extern "C" REX_FUNC(sub_823F7308) {
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    REXLOG_INFO("OnlineDebug: 823F7308 sending handshake (wire opcode 9) session=0x{:08X}", ctx.r3.u32);
  }
  __imp__sub_823F7308(ctx, base);
}

// Wire packet dispatcher (handles opcodes 1, 2, 6, 7, 9, 10, 11, etc.).
// An empty type-10 (host accept, payload_len 0) makes the joiner parse a participant table
// out of uninitialised stack (count = garbage, then it walks off the buffer: AV, endless
// XamSessionRefObjByHandle loop). Real hosts send it empty; skip the table parse for it.
static bool g_skip_empty_table = false;
extern "C" REX_FUNC(__imp__sub_823F3878);
extern "C" REX_FUNC(sub_823F3878) {
  if (g_skip_empty_table) {
    g_skip_empty_table = false;
    if (std::getenv("TB2_ONLINE_DEBUG")) {
      REXLOG_INFO("OnlineDebug: 823F3878 skipped participant-table parse of an empty type-10");
    }
    return;
  }
  __imp__sub_823F3878(ctx, base);
}

extern "C" REX_FUNC(sub_823F7AA0) {
  {
    const uint32_t obj = ctx.r4.u32;
    const uint32_t data = obj ? LoadGuestU32(base, obj) : 0;
    g_skip_empty_table = data && LoadGuestU32(base, data) == 10 && (*GuestPtr(base, data + 4)) == 0;
  }
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    const uint32_t session = ctx.r3.u32;
    const uint32_t pkt_obj = ctx.r4.u32;
    uint32_t pkt_data = pkt_obj ? LoadGuestU32(base, pkt_obj) : 0;
    uint32_t opcode = pkt_data ? LoadGuestU32(base, pkt_data) : 0;
    REXLOG_INFO("OnlineDebug: 823F7AA0 packet dispatch: session=0x{:08X} opcode={} (data=0x{:08X})",
                session, opcode, pkt_data);
  }
  __imp__sub_823F7AA0(ctx, base);
  g_skip_empty_table = false;
}

// HandleParticipantsLeft (called when participant status +12 becomes 1 -> LOST_P2P).
extern "C" REX_FUNC(sub_823F6CD8) {
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    REXLOG_INFO("OnlineDebug: 823F6CD8 HandleParticipantsLeft(session=0x{:08X}, reason={}) lr=0x{:08X} - LOST_P2P",
                ctx.r3.u32, ctx.r4.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_823F6CD8(ctx, base);
}

// Participant monitor / timer check (sets participant+12 = 1 on timeout or socket closed).
extern "C" REX_FUNC(sub_823F65D8) {
  const uint32_t part = ctx.r3.u32;
  const uint8_t old_status = part ? (*GuestPtr(base, part + 12)) : 0;
  __imp__sub_823F65D8(ctx, base);
  const uint8_t new_status = part ? (*GuestPtr(base, part + 12)) : 0;
  if (std::getenv("TB2_ONLINE_DEBUG") && old_status != new_status) {
    REXLOG_INFO("OnlineDebug: 823F65D8 participant 0x{:08X} status changed {} -> {} (transport=0x{:08X})",
                part, (uint32_t)old_status, (uint32_t)new_status, part ? LoadGuestU32(base, part + 8) : 0);
  }
}

// Connect helper (caller of the connect vtable): logs the object's address
// (+42) and port (+46) fields so we can see which writer sets the port.
extern "C" REX_FUNC(__imp__sub_8241F158);
extern "C" REX_FUNC(sub_8241F158) {
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    const uint32_t obj = ctx.r3.u32;
    if (obj) {
      REXLOG_INFO("OnlineDebug: 8241F158 obj=0x{:08X} addr32=0x{:08X} port16={} lr=0x{:08X}",
                  obj, LoadGuestU32(base, obj + 42), (uint32_t)(((*GuestPtr(base, obj + 46)) << 8) | (*GuestPtr(base, obj + 47))),
                  (uint32_t)ctx.lr);
    }
  }
  __imp__sub_8241F158(ctx, base);
}

// Port-source chain (see docs/ONLINE_P2P_HANDOFF.md): log the 16-byte record
// the connect path copies and its port halfword (+14), with the caller lr.
static uint32_t PortAt(uint8_t* base, uint32_t addr) {
  return (uint32_t)(((*GuestPtr(base, addr)) << 8) | (*GuestPtr(base, addr + 1)));
}
extern "C" REX_FUNC(__imp__sub_823E5208);
extern "C" REX_FUNC(sub_823E5208) {
  if (std::getenv("TB2_ONLINE_DEBUG"))
    REXLOG_INFO("OnlineDebug: 823E5208 rec=0x{:08X} port14={} lr=0x{:08X}", ctx.r5.u32,
                PortAt(base, ctx.r5.u32 + 14), (uint32_t)ctx.lr);
  __imp__sub_823E5208(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_823EBB40);
extern "C" REX_FUNC(sub_823EBB40) {
  if (std::getenv("TB2_ONLINE_DEBUG"))
    REXLOG_INFO("OnlineDebug: 823EBB40 obj=0x{:08X} src100port14={} lr=0x{:08X}", ctx.r3.u32,
                PortAt(base, ctx.r3.u32 + 114), (uint32_t)ctx.lr);
  __imp__sub_823EBB40(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_823EB6B8);
extern "C" REX_FUNC(sub_823EB6B8) {
  if (std::getenv("TB2_ONLINE_DEBUG"))
    REXLOG_INFO("OnlineDebug: 823EB6B8 obj=0x{:08X} src68port14={} lr=0x{:08X}", ctx.r3.u32,
                PortAt(base, ctx.r3.u32 + 82), (uint32_t)ctx.lr);
  __imp__sub_823EB6B8(ctx, base);
}
// State-4 handler: queues 0x44 when obj+0x90 is nonzero (docs/ONLINE_P2P_HANDOFF.md, session 4).
extern "C" REX_FUNC(__imp__sub_823ED638);
extern "C" REX_FUNC(sub_823ED638) {
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    const uint32_t obj = ctx.r3.u32;
    REXLOG_INFO("OnlineDebug: 823ED638 obj=0x{:08X} +90={} +C0={} lr=0x{:08X}", obj,
                (uint32_t)(*GuestPtr(base, obj + 0x90)), LoadGuestU32(base, obj + 0xC0), (uint32_t)ctx.lr);
  }
  __imp__sub_823ED638(ctx, base);
}
// Virtual callback that bumps obj+0x90 (see docs/ONLINE_P2P_HANDOFF.md, session 4).
extern "C" REX_FUNC(__imp__sub_823FE970);
extern "C" REX_FUNC(sub_823FE970) {
  if (std::getenv("TB2_ONLINE_DEBUG"))
    REXLOG_INFO("OnlineDebug: 823FE970 this=0x{:08X} arg=0x{:08X} lr=0x{:08X}", ctx.r3.u32,
                ctx.r4.u32, (uint32_t)ctx.lr);
  __imp__sub_823FE970(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_823F68E0);
extern "C" REX_FUNC(sub_823F68E0) {
  if (std::getenv("TB2_ONLINE_DEBUG"))
    REXLOG_INFO("OnlineDebug: 823F68E0 obj=0x{:08X} lr=0x{:08X}", ctx.r3.u32, (uint32_t)ctx.lr);
  __imp__sub_823F68E0(ctx, base);
}

// XLive event enqueue: Function_823EA090(event_id) pushes onto DAT_825f50fc.
extern "C" REX_FUNC(__imp__sub_823EA090);
extern "C" REX_FUNC(sub_823EA090) {
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    REXLOG_INFO("OnlineDebug: XLive event enqueue id=0x{:08X}", ctx.r3.u32);
    if (ctx.r3.u32 == 0x17 || ctx.r3.u32 == 0x27) {  // guest backtrace: back chain [sp], saved LR at [callerSp-8]
      std::string bt = fmt::format("{:08X}", (uint32_t)ctx.lr);
      uint32_t sp = ctx.r1.u32;
      for (int i = 0; i < 10 && sp; i++) { const uint32_t up = LoadGuestU32(base, sp); if (!up) break; bt += fmt::format(" <- {:08X}", LoadGuestU32(base, up - 8)); sp = up; }
      REXLOG_INFO("OnlineDebug: enqueue 0x{:X} backtrace {} tickepoch={}", ctx.r3.u32, bt, (uint32_t)(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 400));
    }
  }
  __imp__sub_823EA090(ctx, base);
}

// sub_823EBC38(session, id) builds and enqueues a session event (the state
// machine's 0x1C/0x14/etc. all go through here, not sub_823EA090). Log ids to
// see which transition events actually fire.
extern "C" REX_FUNC(__imp__sub_823EBC38);
extern "C" REX_FUNC(sub_823EBC38) {
  const uint32_t id = ctx.r4.u32;
  __imp__sub_823EBC38(ctx, base);
  if (std::getenv("TB2_ONLINE_DEBUG") && id < 0x80) {
    static std::chrono::steady_clock::time_point last_log[0x80]{};
    if (OncePerSecond(last_log[id])) {
      REXLOG_INFO("OnlineDebug: 823EBC38 event enqueue id=0x{:08X}", id);
    }
  }
}

// XLive event poll: Function_823EA110(id, flag) pops a matching event. Only log
// calls that actually returned an event (the dispatcher polls every id each
// frame, so logging all of them floods the log).
extern "C" REX_FUNC(__imp__sub_823EA110);
extern "C" REX_FUNC(sub_823EA110) {
  {
    static std::chrono::steady_clock::time_point lastDump{};
    const auto now = std::chrono::steady_clock::now();
    if (std::getenv("TB2_DUMP_MATCH") && now - lastDump > std::chrono::seconds(5)) {  // host/guest M differences (stadium etc.)
      lastDump = now;
      const uint32_t M = LoadGuestU32(base, LoadGuestU32(base, 0x825E0000 + 9588) + 20);
      if (M) { std::string d; for (uint32_t o = 0; o < 0x80; o += 4) { char b[16]; snprintf(b, sizeof b, "%x ", LoadGuestU32(base, M + o)); d += b; } REXLOG_INFO("MatchDump M={:#x}: {}", M, d); }
    }
  }
  const uint32_t id = ctx.r3.u32;
  __imp__sub_823EA110(ctx, base);
  if (ctx.r3.u32 != 0 && std::getenv("TB2_ONLINE_DEBUG") && id < 0x100) {
    static std::chrono::steady_clock::time_point last_log[0x100]{};
    if (OncePerSecond(last_log[id])) {
      REXLOG_INFO("OnlineDebug: XLive event delivered id=0x{:08X}", id);
    }
  }
}

// Live session request wrapper (XSessionCreate / join / delete). Called from
// the game-room join path; repeated calls mean the join is retrying.
extern "C" REX_FUNC(__imp__sub_823CDE88);
extern "C" REX_FUNC(sub_823CDE88) {
  static std::chrono::steady_clock::time_point last_log{};
  if (std::getenv("TB2_ONLINE_DEBUG") && OncePerSecond(last_log)) {
    REXLOG_INFO("OnlineDebug: 823CDE88 live session request arg1=0x{:08X} arg2=0x{:08X} lr=0x{:08X}",
                ctx.r4.u32, ctx.r5.u32, (uint32_t)ctx.lr);
  }
  __imp__sub_823CDE88(ctx, base);
}

// XLive worker tick (called when the worker event is signalled).
extern "C" REX_FUNC(__imp__sub_8250AD30);
extern "C" REX_FUNC(sub_8250AD30) {
  static std::chrono::steady_clock::time_point last_log{};
  if (std::getenv("TB2_ONLINE_DEBUG") && OncePerSecond(last_log)) {
    REXLOG_INFO("OnlineDebug: XLive worker tick (rate-limited)");
  }
  __imp__sub_8250AD30(ctx, base);
}

// XLive state machine, called from the worker tick.
extern "C" REX_FUNC(__imp__sub_8250A6E8);
extern "C" REX_FUNC(sub_8250A6E8) {
  static std::chrono::steady_clock::time_point last_log{};
  if (std::getenv("TB2_ONLINE_DEBUG") && OncePerSecond(last_log)) {
    REXLOG_INFO("OnlineDebug: XLive state machine (8250A6E8, rate-limited)");
  }
  __imp__sub_8250A6E8(ctx, base);
}

// Function_822BF488 is the online manager's UI-message handler.
extern "C" REX_FUNC(__imp__sub_822BF488);
extern "C" REX_FUNC(sub_822BF488) {
  if (std::getenv("TB2_ONLINE_DEBUG")) {
    const uint32_t task = ctx.r3.u32;
    const uint32_t msg = ctx.r4.u32;
    uint32_t msg_id = 0;
    uint32_t task_user = 0;
    if (msg) msg_id = LoadGuestU32(base, msg + 4);
    if (task) task_user = LoadGuestU32(base, task + 0x94);
    REXLOG_INFO("OnlineDebug: Function_822BF488(task=0x{:08X}, msg=0x{:08X}, msg_id=0x{:08X}, "
                "task_user={}, lr=0x{:08X})",
                task, msg, msg_id, task_user, (uint32_t)ctx.lr);
  }
  __imp__sub_822BF488(ctx, base);
}

namespace tb2 {
namespace {

constexpr uint32_t kOnlineManager = 0x825e34f0;

uint32_t ReadU32(rex::system::KernelState* ks, uint32_t guest) {
  auto* p = ks->memory()->TranslateVirtual<const uint8_t*>(guest);
  if (!p) return 0;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint8_t ReadU8(rex::system::KernelState* ks, uint32_t guest) {
  auto* p = ks->memory()->TranslateVirtual<const uint8_t*>(guest);
  return p ? *p : 0;
}

}  // namespace

void DumpOnlineManager() {
  auto* ks = rex::system::kernel_state();
  if (!ks) {
    REXLOG_WARN("OnlineDebug: no kernel state");
    return;
  }
  const uint32_t mgr = ReadU32(ks, kOnlineManager);
  REXLOG_INFO("OnlineDebug: manager=0x{:08X}", mgr);
  if (!mgr) return;
  REXLOG_INFO(
      "OnlineDebug: +11f4={} +11f8={} | 11fc={} 11fd={} 11fe={} 11ff={} 1200={} 1201={} 1202={}",
      ReadU32(ks, mgr + 0x11f4), ReadU32(ks, mgr + 0x11f8), ReadU8(ks, mgr + 0x11fc),
      ReadU8(ks, mgr + 0x11fd), ReadU8(ks, mgr + 0x11fe), ReadU8(ks, mgr + 0x11ff),
      ReadU8(ks, mgr + 0x1200), ReadU8(ks, mgr + 0x1201), ReadU8(ks, mgr + 0x1202));
  REXLOG_INFO(
      "OnlineDebug: user(+1204)={} ctx(+1208)=0x{:08X} | online(+1310)={} count(+1318)={} "
      "listener(+131c)=0x{:08X}",
      ReadU32(ks, mgr + 0x1204), ReadU32(ks, mgr + 0x1208), ReadU8(ks, mgr + 0x1310),
      ReadU32(ks, mgr + 0x1318), ReadU32(ks, mgr + 0x131c));
  REXLOG_INFO("OnlineDebug: xuid(+1320)=0x{:08X}{:08X} 1328={} 1329={}",
              ReadU32(ks, mgr + 0x1320), ReadU32(ks, mgr + 0x1324), ReadU8(ks, mgr + 0x1328),
              ReadU8(ks, mgr + 0x1329));
}

void NudgeSigninChanged() {
  auto* ks = rex::system::kernel_state();
  if (!ks) return;
  REXLOG_INFO("OnlineDebug: broadcast XN_SYS_SIGNINCHANGED (0x0A, 1)");
  ks->BroadcastNotification(0x0000000A, 1);
}

void NudgeLiveConnection() {
  auto* ks = rex::system::kernel_state();
  if (!ks) return;
  REXLOG_INFO("OnlineDebug: broadcast XN_LIVE_CONNECTIONCHANGED (0x02000001, 0x001510F0)");
  ks->BroadcastNotification(0x02000001, 0x001510F0u);
}

}  // namespace tb2

// Ready-handler gate probes (session 10): log each gate's result only while
// the A/Ready handler sub_8230DDD8 is running, to see which one fails.
namespace { bool g_in_ready = false; }
extern "C" REX_FUNC(__imp__sub_8230DDD8);
extern "C" REX_FUNC(sub_8230DDD8) {
  g_in_ready = true;
  REXLOG_INFO("OnlineDebug: READY handler enter screen=0x{:08X} mode(+96)={}", ctx.r3.u32,
              LoadGuestU32(base, ctx.r3.u32 + 96));
  __imp__sub_8230DDD8(ctx, base);
  g_in_ready = false;
  REXLOG_INFO("OnlineDebug: READY handler returned {}", ctx.r3.u32 & 0xFF);
}
#define TB2_GATE_PROBE(addr, what)                                                \
  extern "C" REX_FUNC(__imp__sub_##addr);                                         \
  extern "C" REX_FUNC(sub_##addr) {                                               \
    const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32;                              \
    __imp__sub_##addr(ctx, base);                                                 \
    if (g_in_ready) REXLOG_INFO("OnlineDebug: READY gate " what " r3=0x{:08X} r4=0x{:08X} -> 0x{:08X}", a3, a4, ctx.r3.u32); \
  }
TB2_GATE_PROBE(822DC608, "822DC608")
TB2_GATE_PROBE(82375BE0, "82375BE0")
TB2_GATE_PROBE(823E9DF8, "823E9DF8")
TB2_GATE_PROBE(82375A20, "82375A20")
extern "C" REX_FUNC(__imp__sub_823E3B18);
extern "C" REX_FUNC(sub_823E3B18) {
  __imp__sub_823E3B18(ctx, base);
  if (g_in_ready) {
    const uint32_t o = ctx.r3.u32;
    static bool dumped = false;
    if (!dumped && o) {
      dumped = true;
      std::string w;
      for (uint32_t i = 0; i < 0x60; i += 4) w += fmt::format(" {:08X}", LoadGuestU32(base, o + i));
      REXLOG_INFO("OnlineDebug: READY obj=0x{:08X} dump:{}", o, w);
    }
  }
}
extern "C" REX_FUNC(__imp__sub_823E41B8);
extern "C" REX_FUNC(sub_823E41B8) {
  const uint32_t rec = ctx.r3.u32, want = ctx.r4.u32;
  const uint32_t have = rec ? LoadGuestU32(base, rec) : 0xFFFFFFFF;
  __imp__sub_823E41B8(ctx, base);
  if (g_in_ready) REXLOG_INFO("OnlineDebug: READY gate 823E41B8 rec=0x{:08X} id={} want={} -> {}", rec, have, want, ctx.r3.u32 & 0xFF);
}

extern "C" REX_FUNC(__imp__sub_823E3D40);
extern "C" REX_FUNC(sub_823E3D40) {
  if (std::getenv("TB2_ONLINE_DEBUG")) REXLOG_INFO("OnlineDebug: participant create 823E3D40 id={}", ctx.r4.u32);
  __imp__sub_823E3D40(ctx, base);
}

// Is the StartSession UI message (0x13AB92BE) ever seen by the Game Room handler,
// and does the start-match function run? (session 10)
extern "C" REX_FUNC(__imp__sub_8234E3A0);
extern "C" REX_FUNC(sub_8234E3A0) {
  if (std::getenv("TB2_ONLINE_DEBUG") && ctx.r4.u32)
    REXLOG_INFO("OnlineDebug: GameRoom msg handler 8234E3A0 type=0x{:08X} id=0x{:08X}",
                LoadGuestU32(base, ctx.r4.u32 + 4), LoadGuestU32(base, ctx.r4.u32 + 8));
  __imp__sub_8234E3A0(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_822DC1D8);
extern "C" REX_FUNC(sub_822DC1D8) {
  REXLOG_INFO("OnlineDebug: START-MATCH sub_822DC1D8 called screen=0x{:08X}", ctx.r3.u32);
  __imp__sub_822DC1D8(ctx, base);
}

// Game Room Update's push of OnlineMatchSetup needs three things (session 10):
// timer armed (82230390), timer expired (8241F758), screen-17 request queued (82379C98).
extern "C" REX_FUNC(__imp__sub_82230390);
extern "C" REX_FUNC(sub_82230390) {
  const uint32_t t = ctx.r3.u32;
  __imp__sub_82230390(ctx, base);
  static std::chrono::steady_clock::time_point last{};
  if (std::getenv("TB2_ONLINE_DEBUG") && OncePerSecond(last))
    REXLOG_INFO("OnlineDebug: GATE timer-armed 82230390 timer=0x{:08X} -> {}", t, ctx.r3.u32 & 0xFF);
}
extern "C" REX_FUNC(__imp__sub_8241F758);
extern "C" REX_FUNC(sub_8241F758) {
  const uint32_t t = ctx.r3.u32;
  __imp__sub_8241F758(ctx, base);
  static std::chrono::steady_clock::time_point last{};
  if (std::getenv("TB2_ONLINE_DEBUG") && OncePerSecond(last))
    REXLOG_INFO("OnlineDebug: GATE timer-expired 8241F758 timer=0x{:08X} -> {}", t, ctx.r3.u32 & 0xFF);
}
extern "C" REX_FUNC(__imp__sub_82379C98);
extern "C" REX_FUNC(sub_82379C98) {
  const uint32_t o = ctx.r3.u32, id = ctx.r4.u32;
  const uint32_t busy = o ? (LoadGuestU32(base, o + 52) >> 24) : 0xFF, cnt = o ? LoadGuestU32(base, o + 24) : 0;
  __imp__sub_82379C98(ctx, base);
  static std::chrono::steady_clock::time_point last{};
  if (std::getenv("TB2_ONLINE_DEBUG") && OncePerSecond(last))
    REXLOG_INFO("OnlineDebug: GATE request-queued 82379C98 obj=0x{:08X} id={} busy(+52)={} count(+24)={} -> 0x{:08X}", o, id, busy, cnt, ctx.r3.u32);
}

// 0x46 player-info matching in the Game Room (session 10): event id vs slot ids.
static std::string HexWords(uint8_t* base, uint32_t a, int n) {
  std::string w;
  for (int i = 0; i < n; ++i) w += fmt::format(" {:08X}", LoadGuestU32(base, a + 4 * i));
  return w;
}
extern "C" REX_FUNC(__imp__sub_82333FA0);
extern "C" REX_FUNC(sub_82333FA0) {
  const uint32_t scr = ctx.r3.u32, ev = ctx.r4.u32;
  if (std::getenv("TB2_ONLINE_DEBUG") && ev)
    REXLOG_INFO("OnlineDebug: 0x46 handler 82333FA0 screen=0x{:08X} id obj r4: {}", scr, HexWords(base, ev, 14));
  __imp__sub_82333FA0(ctx, base);
}
#define TB2_SLOTID_PROBE(addr)                                                              \
  extern "C" REX_FUNC(__imp__sub_##addr);                                                   \
  extern "C" REX_FUNC(sub_##addr) {                                                         \
    const uint32_t out = ctx.r3.u32;                                                        \
    __imp__sub_##addr(ctx, base);                                                           \
    if (std::getenv("TB2_ONLINE_DEBUG") && out)                                             \
      REXLOG_INFO("OnlineDebug: slot-id probe " #addr " out:{}", HexWords(base, out, 4));   \
  }
TB2_SLOTID_PROBE(822DBB80)
TB2_SLOTID_PROBE(822DBBD8)

// TB2_CTRL_TRACE=1: log each distinct (caller, value) of the UI widget-state setter, to diff
// the offline Team Setup (controller icon lit) against the online one (session 10).
extern "C" REX_FUNC(__imp__sub_824AD228);
extern "C" REX_FUNC(sub_824AD228) {
  if (std::getenv("TB2_CTRL_TRACE")) {
    static std::set<std::pair<uint32_t, uint32_t>> seen;
    const auto key = std::make_pair(static_cast<uint32_t>(ctx.lr), ctx.r4.u32);
    if (seen.insert(key).second)
      REXLOG_INFO("CtrlTrace: setter widget=0x{:08X} value=0x{:08X} lr=0x{:08X}", ctx.r3.u32, ctx.r4.u32, key.first);
  }
  __imp__sub_824AD228(ctx, base);
}

// Team Setup adds a controller icon only if the pad is "active" (session 10): pad table 0x82633BF0,
// record +0x2FA4 connected byte, +0x2FA8 == 1 active. Log per pad, once per second.
extern "C" REX_FUNC(__imp__sub_822B8460);
extern "C" REX_FUNC(sub_822B8460) {
  const uint32_t i = ctx.r3.u32;
  __imp__sub_822B8460(ctx, base);
  static std::chrono::steady_clock::time_point last[16]{};
  if (std::getenv("TB2_CTRL_TRACE") && i < 16 && OncePerSecond(last[i])) {
    const uint32_t rec = LoadGuestU32(base, 0x82633BF0 + 4 * i);
    REXLOG_INFO("CtrlTrace: pad {} active={} rec=0x{:08X} connected(+2FA4)={} state(+2FA8)={}", i, ctx.r3.u32 & 0xFF, rec,
                rec ? (*GuestPtr(base, rec + 0x2FA4)) : 0xFF, rec ? LoadGuestU32(base, rec + 0x2FA8) : 0xFFFFFFFF);
  }
}

// Who enables/disables a pad (sets the +0x2FA4 connected byte)? (session 10)
extern "C" REX_FUNC(__imp__sub_824B7D58);
extern "C" REX_FUNC(sub_824B7D58) {
  if (std::getenv("TB2_CTRL_TRACE"))
    REXLOG_INFO("CtrlTrace: SetPadEnabled pad={} value={} lr=0x{:08X}", ctx.r3.u32, ctx.r4.u32 & 0xFF, (uint32_t)ctx.lr);
  __imp__sub_824B7D58(ctx, base);
}

// TB2_AUTOKEY=<file>: debug-only key injector (session 10). Wayland blocks xdotool, so a thread in the
// game pushes SDL key events into its window. Commands, one per line, appended to the file:
//   tap [shift+]<SDL key name>   |   hold <key> <ms>   |   wait <ms>
// The file is truncated after it is read.
namespace {
void AutoKeyKey(SDL_Scancode sc, SDL_Keymod mod, bool down, SDL_WindowID wid) {
  SDL_Event e{};
  e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
  e.key.windowID = wid;
  e.key.scancode = sc;
  e.key.key = SDL_GetKeyFromScancode(sc, mod, false);
  e.key.mod = mod;
  e.key.down = down;
  SDL_PushEvent(&e);
}
void AutoKeyLoop(std::string path) {
  for (;;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::string content;
    {
      std::ifstream in(path);
      std::stringstream ss;
      ss << in.rdbuf();
      content = ss.str();
    }
    if (content.empty()) continue;
    { std::ofstream(path, std::ios::trunc); }
    int n = 0;
    SDL_Window** wins = SDL_GetWindows(&n);
    const SDL_WindowID wid = (wins && n > 0) ? SDL_GetWindowID(wins[0]) : 0;
    if (wins) SDL_free(wins);
    std::istringstream lines(content);
    std::string line;
    while (std::getline(lines, line)) {
      std::istringstream ls(line);
      std::string cmd, key;
      ls >> cmd;
      if (cmd == "wait") { int ms = 0; ls >> ms; std::this_thread::sleep_for(std::chrono::milliseconds(ms)); continue; }
      if (cmd != "tap" && cmd != "hold") continue;
      ls >> key;
      int ms = cmd == "hold" ? 400 : 120;
      ls >> ms;
      SDL_Keymod mod = SDL_KMOD_NONE;
      if (key.rfind("shift+", 0) == 0) { mod = SDL_KMOD_LSHIFT; key = key.substr(6); }
      const SDL_Scancode sc = SDL_GetScancodeFromName(key.c_str());
      if (sc == SDL_SCANCODE_UNKNOWN) { REXLOG_INFO("AutoKey: unknown key '{}'", key); continue; }
      {  // the game ignores keys while its window is unfocused; two instances share one desktop
        SDL_Event f{};
        f.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
        f.window.windowID = wid;
        SDL_PushEvent(&f);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
      }
      if (mod) AutoKeyKey(SDL_SCANCODE_LSHIFT, SDL_KMOD_LSHIFT, true, wid);
      AutoKeyKey(sc, mod, true, wid);
      std::this_thread::sleep_for(std::chrono::milliseconds(ms));
      AutoKeyKey(sc, mod, false, wid);
      if (mod) AutoKeyKey(SDL_SCANCODE_LSHIFT, SDL_KMOD_NONE, false, wid);
      ++g_fn_epoch; REXLOG_INFO("AutoKey: {} {}", cmd, key);
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
  }
}
struct AutoKeyStarter {
  AutoKeyStarter() {
    if (const char* p = std::getenv("TB2_AUTOKEY")) std::thread(AutoKeyLoop, std::string(p)).detach();
  }
} g_autokey_starter;
}  // namespace

// Team Setup "opponent re-connecting controller" (session 11): 82335278 compares
// 823E3E90(participant 823F2738(i,0)) with 8230FAE8(screen,i). Log them, once per second per call site.
#define TB2_CMP_PROBE(ADDR)                                                                              \
  extern "C" REX_FUNC(__imp__sub_##ADDR);                                                                \
  extern "C" REX_FUNC(sub_##ADDR) {                                                                      \
    const uint32_t a3 = ctx.r3.u32, a4 = ctx.r4.u32;                                                     \
    __imp__sub_##ADDR(ctx, base);                                                                        \
    static std::map<uint64_t, std::chrono::steady_clock::time_point> last;\
    if (std::getenv("TB2_CTRL_TRACE") && OncePerSecond(last[(uint64_t(a3) << 32) | a4]))                                    \
      REXLOG_INFO("CmpProbe " #ADDR "({:#x},{:#x}) -> {:#x} lr={:#x}", a3, a4, ctx.r3.u32, (uint32_t)ctx.lr); \
  }
TB2_CMP_PROBE(823F2738)
TB2_CMP_PROBE(823E3E90)


// Team Setup join/leave probes (session 11): log args and the per-pad joined bytes at screen+0x50..0x60.
#define TB2_JOIN_PROBE(ADDR)                                                                              \
  extern "C" REX_FUNC(__imp__sub_##ADDR);                                                                 \
  extern "C" REX_FUNC(sub_##ADDR) {                                                                       \
    const uint32_t s = ctx.r3.u32, a4 = ctx.r4.u32, a5 = ctx.r5.u32, a6 = ctx.r6.u32, lr = (uint32_t)ctx.lr; \
    __imp__sub_##ADDR(ctx, base);                                                                         \
    if (std::getenv("TB2_CTRL_TRACE")) {                                                                  \
      std::string b; for (int i = 0; i < 16; i++) b += fmt::format("{:02x}", (*GuestPtr(base, s + 80 + i)));          \
      std::string w, t; for (int i = 0; i < 4; i++) w += fmt::format("{:x} ", LoadGuestU32(base, s + 132 + 4 * i)); \
      for (int i = 0; i < 16; i++) t += fmt::format("{:02x}", (*GuestPtr(base, s + 116 + i)));                         \
      REXLOG_INFO("JoinProbe " #ADDR "(scr={:#x} pad={} a5={} a6={:#x}) lr={:#x} bytes80={} words132={} tok116={} mode={}", s, (int)a4, a5, a6, lr, b, w, t, LoadGuestU32(base, LoadGuestU32(base, LoadGuestU32(base, 0x825E0000 + 9588) + 28) + 36)); \
    }                                                                                                     \
  }
TB2_JOIN_PROBE(822F8240)
TB2_JOIN_PROBE(822F8320)

TB2_CMP_PROBE(82236708)
TB2_CMP_PROBE(82236998)
TB2_CMP_PROBE(82236778)
TB2_CMP_PROBE(82236730)
TB2_CMP_PROBE(822D7B58)

TB2_CMP_PROBE(8231F1A0)
TB2_CMP_PROBE(8230F888)
TB2_CMP_PROBE(822DE418)

// Game-message sender (wire opcode = subtype + 0xF): log every call (session 11).
extern "C" REX_FUNC(__imp__sub_823F7018);
extern "C" REX_FUNC(sub_823F7018) {
  if (std::getenv("TB2_CTRL_TRACE"))
    REXLOG_INFO("MsgSend 823F7018 r3={:#x} r4={:#x} r5={:#x} r6={:#x} lr={:#x}", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, (uint32_t)ctx.lr);
  __imp__sub_823F7018(ctx, base);
}

// TB2_FN_TRACE build: log each traced function once per AutoKey epoch (epoch bumps on every key tap).
std::atomic<uint32_t> g_fn_epoch{0};
void Tb2FnTrace(uint32_t addr, uint32_t lr) {
  static const bool on = std::getenv("TB2_FN_TRACE") != nullptr;
  if (!on) return;
  static std::mutex m; static std::map<uint32_t, uint32_t> seen;
  std::lock_guard<std::mutex> g(m);
  static const bool tick = std::getenv("TB2_FN_TRACE_TICK") != nullptr;
  const uint32_t e = tick ? (uint32_t)(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count() / 400) : g_fn_epoch.load();
  auto it = seen.find(addr);
  if (it != seen.end() && it->second == e) return;
  seen[addr] = e;
  REXLOG_INFO("FnTrace e={} {:08X} lr={:08X}", e, addr, lr);
}

// Team Setup probes (session 11): ready flag and team pick per side.
extern "C" REX_FUNC(__imp__sub_822F98E8);
extern "C" REX_FUNC(sub_822F98E8) {
  if (std::getenv("TB2_CTRL_TRACE")) REXLOG_INFO("ReadyProbe 822F98E8 scr={:#x} side={} ready={} lr={:#x}", ctx.r3.u32, (int)ctx.r4.u32, ctx.r5.u32 & 0xFF, (uint32_t)ctx.lr);
  __imp__sub_822F98E8(ctx, base);
}
extern "C" REX_FUNC(__imp__sub_8223FFB0);
extern "C" REX_FUNC(sub_8223FFB0) {
  if (std::getenv("TB2_CTRL_TRACE")) REXLOG_INFO("TeamSet 8223FFB0 side={} id={} lr={:#x}", ctx.r4.u32, ctx.r5.u32, (uint32_t)ctx.lr);
  __imp__sub_8223FFB0(ctx, base);
}

// Roster-exchange poll (82378C68): event 0x12 is only consumed when its type byte (+12) == 2.
extern "C" REX_FUNC(__imp__sub_820E3E18);
extern "C" REX_FUNC(sub_820E3E18) {
  const uint32_t obj = ctx.r3.u32, lr = (uint32_t)ctx.lr;
  __imp__sub_820E3E18(ctx, base);
  static std::chrono::steady_clock::time_point last{};
  if (lr == 0x82378CA4 && std::getenv("TB2_CTRL_TRACE") && OncePerSecond(last))
    REXLOG_INFO("RosterEvt 0x12 obj={:#x} +4={:#x} +8={:#x} type(+12)={}", obj, LoadGuestU32(base, obj + 4), LoadGuestU32(base, obj + 8), ctx.r3.u32 & 0xFF);
}

// Probe (session 11): who sends the type-7 session message that makes 823795E0 raise event 0x17?
extern "C" REX_FUNC(__imp__sub_823795E0);
extern "C" REX_FUNC(sub_823795E0) {
  const uint32_t m = ctx.r4.u32;
  if (std::getenv("TB2_CTRL_TRACE") && m)
    REXLOG_INFO("Msg823795E0 type={} +8={:#x} +c={:#x} +10={:#x} +14={:#x} +18={:#x} lr={:#x}", LoadGuestU32(base, m + 4),
                LoadGuestU32(base, m + 8), LoadGuestU32(base, m + 12), LoadGuestU32(base, m + 16),
                LoadGuestU32(base, m + 20), LoadGuestU32(base, m + 24), (uint32_t)ctx.lr);
  __imp__sub_823795E0(ctx, base);
}

// Probe: who tears the session down (82378B00 / 82376CB0(reason in r4))? Logs a guest backtrace.
static std::string GuestBt(PPCContext& ctx, uint8_t* base) {
  std::string bt = fmt::format("{:08X}", (uint32_t)ctx.lr);
  uint32_t sp = ctx.r1.u32;
  for (int i = 0; i < 10 && sp; i++) { const uint32_t up = LoadGuestU32(base, sp); if (!up) break; bt += fmt::format(" <- {:08X}", LoadGuestU32(base, up - 8)); sp = up; }
  return bt;
}
extern "C" REX_FUNC(__imp__sub_82378B00);
extern "C" REX_FUNC(sub_82378B00) { REXLOG_INFO("Teardown 82378B00 bt {}", GuestBt(ctx, base)); __imp__sub_82378B00(ctx, base); }
extern "C" REX_FUNC(__imp__sub_82376CB0);
extern "C" REX_FUNC(sub_82376CB0) { REXLOG_INFO("Teardown 82376CB0 reason={} bt {}", ctx.r4.u32, GuestBt(ctx, base)); __imp__sub_82376CB0(ctx, base); }


// Session 12: the lockstep link ("GameConn", UDP) binds 1795 (host) / 1796 (guest) when both ends share one IP, but
// connects to its OWN port, so on one machine each side talks to itself. When the target IP is our own (a UDP socket
// can bind to it), point the connect at the other port. Two machines (different IPs) both use 1795
// and are left alone. 823CDD10 = game's connect() wrapper (r4 = sockaddr_in, guest memory = network byte order).
// True if ip (host order) is one of this machine's addresses: binding a UDP socket to it only works for local IPs.
static bool IsOwnIp(uint32_t ip) {
  if ((ip >> 24) == 127) return true;
  const auto fd = socket(AF_INET, SOCK_DGRAM, 0);
  sockaddr_in a{}; a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(ip); a.sin_port = 0;
  const bool own = bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
#ifdef _WIN32
  closesocket(fd);
#else
  close(fd);
#endif
  return own;
}
extern "C" REX_FUNC(__imp__sub_823CDD10);
extern "C" REX_FUNC(sub_823CDD10) {
  if (ctx.r4.u32) {
    uint8_t* p = GuestPtr(base, ctx.r4.u32 + 2);  // sin_port
    const uint16_t port = uint16_t(p[0] << 8 | p[1]);
    const uint32_t ip = LoadGuestU32(base, ctx.r4.u32 + 4);
    if ((port == 1795 || port == 1796) && (port == 1796 || IsOwnIp(ip))) {  // 1796 is only used on one machine
      const uint16_t to = port == 1795 ? 1796 : 1795;
      p[0] = uint8_t(to >> 8); p[1] = uint8_t(to);
      REXLOG_INFO("OnlineDebug: GameConn same-machine connect {:08X}:{} -> {}", ip, port, to);
    } else if (port == 1795 || port == 1796) {
      REXLOG_INFO("OnlineDebug: GameConn connect {:08X}:{} (other machine)", ip, port);
    }
  }
  __imp__sub_823CDD10(ctx, base);
}


// Game Room slot picture (8230E510, "cFEONGameroom::GetPicture"): a fake account has no gamer picture and the
// fetch leaves the remote slot as a white square. Skip it so every slot keeps the "?" placeholder.
extern "C" REX_FUNC(__imp__sub_8230E510);
extern "C" REX_FUNC(sub_8230E510) {
  if (std::getenv("TB2_FAKE_ACCOUNT")) { ctx.r3.u64 = 0; return; }
  __imp__sub_8230E510(ctx, base);
}

// TB2_NET_LOG=1: the lockstep link (GameConn) uses overlapped WSASend/WSARecv, which the SDK doesn't log. Log the
// first calls in full and then a once-per-second summary, to see on each machine whether data goes out and comes in.
namespace {
struct WsaStats { uint32_t calls = 0, ok = 0, logged = 0; };
WsaStats g_wsa[3];  // 0 send, 1 recv, 2 overlapped result
std::chrono::steady_clock::time_point g_wsa_last{};
void WsaTrack(int kind, const char* name, PPCContext& ctx, const uint32_t (&in)[6]) {
  static const bool on = std::getenv("TB2_NET_LOG") != nullptr;
  if (!on) return;
  WsaStats& s = g_wsa[kind];
  ++s.calls;
  const uint32_t ret = ctx.r3.u32;
  if (kind == 2 ? ret != 0 : ret == 0) ++s.ok;  // WSASend/WSARecv: 0 = done now; GetOverlappedResult: TRUE = done
  if (s.logged < 6) {
    ++s.logged;
    REXLOG_INFO("NetTrace {} r3={:#x} r4={:#x} r5={:#x} r6={:#x} r7={:#x} r8={:#x} -> {:#x}", name, in[0], in[1], in[2],
                in[3], in[4], in[5], ret);
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - g_wsa_last >= std::chrono::seconds(1)) {
    g_wsa_last = now;
    REXLOG_INFO("NetTrace per second: WSASend {}/{} ok, WSARecv {}/{} ok, results {}/{} completed",
                g_wsa[0].ok, g_wsa[0].calls, g_wsa[1].ok, g_wsa[1].calls, g_wsa[2].ok, g_wsa[2].calls);
    for (auto& w : g_wsa) w.calls = w.ok = 0;
  }
}
}  // namespace
#define TB2_WSA_TRACE(ADDR, KIND, NAME)                                                                    \
  extern "C" REX_FUNC(__imp__sub_##ADDR);                                                                \
  extern "C" REX_FUNC(sub_##ADDR) {                                                                      \
    const uint32_t in[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};    \
    __imp__sub_##ADDR(ctx, base);                                                                        \
    WsaTrack(KIND, NAME, ctx, in);                                                                       \
  }
TB2_WSA_TRACE(823CDDF8, 0, "WSASend")
TB2_WSA_TRACE(823CDDB8, 1, "WSARecv")
TB2_WSA_TRACE(823CDD70, 2, "WSAGetOverlappedResult")
