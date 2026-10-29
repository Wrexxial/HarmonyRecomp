// harmonyrecomp netplay - Xbox Live sessions (XGI app 0xFB) without a server.
//
// A session the local player hosts is advertised through the lobby service;
// searches ask the lobby service of every reachable machine. Everything else
// (members, state, details) is tracked locally, like XAM does on a console.

#include "sessions.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <rex/system/kernel_state.h>
#include <rex/system/xio.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>

#include "guest.h"
#include "lobby.h"
#include "netplay.h"
#include "udp.h"

// The SDK's X_ERROR_* / X_E_* macros expand to these types.
using rex::X_HRESULT;
using rex::X_RESULT;
using rex::X_STATUS;

namespace harmony::netplay::sessions {
namespace {

using namespace guest;

constexpr uint32_t kUserIndexNone = 0xFE;

struct Member {
  uint64_t xuid = 0;
  uint32_t user_index = kUserIndexNone;  // local user index, or none for remote
  bool is_private = false;
};

struct Session {
  uint32_t obj = 0;
  uint32_t flags = 0;
  uint32_t max_public = 0;
  uint32_t max_private = 0;
  uint64_t id = 0;
  XNKEY key{};
  XNADDR host{};
  bool is_host = false;
  uint64_t nonce = 0;
  SessionState state = SessionState::kLobby;
  std::vector<Member> members;
};

std::mutex g_mutex;
std::map<uint32_t, Session> g_sessions;  // by guest object pointer
std::map<uint32_t, uint32_t> g_handle_objects;
uint32_t g_next_handle = 0xCAFE0001;
std::map<uint32_t, uint32_t> g_contexts;                  // local user contexts
std::map<uint32_t, std::vector<uint8_t>> g_properties;    // local user properties
std::atomic<uint64_t> g_current_session{0};

uint64_t Random64() {
  static std::mutex m;
  static std::mt19937_64 rng(std::random_device{}() ^
                             static_cast<uint64_t>(
                                 std::chrono::steady_clock::now().time_since_epoch().count()));
  std::lock_guard lock(m);
  return rng();
}

rex::memory::Memory* Mem() { return REX_KERNEL_MEMORY(); }

template <typename T>
T* G(uint32_t guest_address) {
  return guest_address ? Mem()->TranslateVirtual<T*>(guest_address) : nullptr;
}

uint32_t PropertyType(uint32_t id) { return (id >> 28) & 0xF; }
bool IsPointerType(uint32_t type) { return type == 4 || type == 6; }  // wstring, binary

std::vector<uint8_t> Utf16Be(const std::string& text) {
  std::vector<uint8_t> out;
  for (unsigned char c : text) {
    out.push_back(0);
    out.push_back(c < 0x80 ? c : '?');
  }
  out.push_back(0);
  out.push_back(0);
  return out;
}

bool Advertisable(const Session& s) {
  return s.is_host && (s.flags & (kFlagMatchmaking | kFlagPeerNetwork)) &&
         s.state != SessionState::kDeleted;
}

void Counts(const Session& s, uint32_t* filled_pub, uint32_t* filled_priv) {
  *filled_pub = 0;
  *filled_priv = 0;
  for (const auto& m : s.members) (m.is_private ? *filled_priv : *filled_pub) += 1;
}

lobby::SessionAd MakeAd(const Session& s) {
  lobby::SessionAd ad;
  ad.id = s.id;
  std::memcpy(ad.key, s.key.ab, sizeof(ad.key));
  ad.flags = s.flags;
  ad.max_public = s.max_public;
  ad.max_private = s.max_private;
  Counts(s, &ad.filled_public, &ad.filled_private);
  ad.state = static_cast<uint32_t>(s.state);
  ad.host_name = Gamertag();
  for (const auto& [id, value] : g_contexts) ad.contexts.push_back({id, value});
  bool has_hostname = false;
  for (const auto& [id, value] : g_properties) {
    ad.properties.push_back({id, value});
    has_hostname |= id == kPropertyGamerHostname;
  }
  if (!has_hostname) ad.properties.push_back({kPropertyGamerHostname, Utf16Be(ad.host_name)});
  return ad;
}

// Caller holds g_mutex.
void Publish(const Session& s) {
  if (Advertisable(s)) {
    lobby::Advertise(MakeAd(s));
  } else if (s.is_host && s.id) {
    lobby::Withdraw(s.id);
  }
}

void PublishAllHosted() {
  for (const auto& [obj, s] : g_sessions) {
    if (s.is_host) Publish(s);
  }
}

Session& SessionFor(uint32_t obj) {
  auto& s = g_sessions[obj];
  s.obj = obj;
  return s;
}

// ---- Search results ------------------------------------------------------------

// Bump allocator over the caller's result buffer; spills to the system heap
// when the buffer is full (only for property payloads).
class ResultSpace {
 public:
  ResultSpace(uint32_t begin, uint32_t end) : cursor_(begin), end_(end) {}
  uint32_t Alloc(uint32_t size) {
    if (!size) return 0;
    uint32_t aligned = (cursor_ + 7) & ~7u;
    if (aligned + size <= end_) {
      cursor_ = aligned + size;
      std::memset(G<uint8_t>(aligned), 0, size);
      return aligned;
    }
    uint32_t heap = Mem()->SystemHeapAlloc(size);
    if (heap) std::memset(G<uint8_t>(heap), 0, size);
    return heap;
  }

 private:
  uint32_t cursor_;
  uint32_t end_;
};

void WriteProperty(XUSER_PROPERTY* out, const lobby::Property& p, ResultSpace& space) {
  std::memset(out, 0, sizeof(*out));
  out->id = p.id;
  uint32_t type = PropertyType(p.id);
  out->type = static_cast<uint8_t>(type);
  if (IsPointerType(type)) {
    uint32_t size = static_cast<uint32_t>(p.value.size());
    uint32_t ptr = space.Alloc(size);
    if (ptr && size) std::memcpy(G<uint8_t>(ptr), p.value.data(), size);
    rex::be<uint32_t> be_size = ptr ? size : 0, be_ptr = ptr;
    std::memcpy(out->data, &be_size, 4);
    std::memcpy(out->data + 4, &be_ptr, 4);
  } else {
    // Scalars are stored big-endian as the game wrote them; 4-byte values
    // (int32/float) sit in the first half of the union.
    std::memcpy(out->data, p.value.data(), std::min<size_t>(p.value.size(), 8));
  }
}

uint32_t WriteSearchResults(const std::vector<lobby::Found>& found, uint32_t max_results,
                            uint32_t buffer_ptr, uint32_t buffer_size) {
  if (!buffer_ptr || buffer_size < sizeof(XSESSION_SEARCHRESULT_HEADER)) {
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  uint32_t fit = (buffer_size - sizeof(XSESSION_SEARCHRESULT_HEADER)) / sizeof(XSESSION_SEARCHRESULT);
  uint32_t count = std::min<uint32_t>({static_cast<uint32_t>(found.size()), max_results, fit});
  uint32_t results_ptr = buffer_ptr + sizeof(XSESSION_SEARCHRESULT_HEADER);
  std::memset(G<uint8_t>(buffer_ptr), 0,
              sizeof(XSESSION_SEARCHRESULT_HEADER) + count * sizeof(XSESSION_SEARCHRESULT));
  auto* header = G<XSESSION_SEARCHRESULT_HEADER>(buffer_ptr);
  header->count = count;
  header->results_ptr = count ? results_ptr : 0;
  ResultSpace space(results_ptr + count * sizeof(XSESSION_SEARCHRESULT), buffer_ptr + buffer_size);

  for (uint32_t i = 0; i < count; ++i) {
    const auto& f = found[i];
    auto* r = G<XSESSION_SEARCHRESULT>(results_ptr + i * sizeof(XSESSION_SEARCHRESULT));
    WriteXnkid(f.ad.id, &r->info.session_id);
    FillXnAddr(f.from.ip, &r->info.host_address);
    std::memcpy(r->info.key_exchange_key.ab, f.ad.key, sizeof(f.ad.key));
    r->filled_public_slots = f.ad.filled_public;
    r->filled_private_slots = f.ad.filled_private;
    r->open_public_slots = f.ad.max_public > f.ad.filled_public ? f.ad.max_public - f.ad.filled_public : 0;
    r->open_private_slots =
        f.ad.max_private > f.ad.filled_private ? f.ad.max_private - f.ad.filled_private : 0;

    uint32_t ctx_count = static_cast<uint32_t>(f.ad.contexts.size());
    uint32_t ctx_ptr = space.Alloc(ctx_count * sizeof(XUSER_CONTEXT));
    if (ctx_ptr) {
      auto* ctx = G<XUSER_CONTEXT>(ctx_ptr);
      for (uint32_t c = 0; c < ctx_count; ++c) {
        ctx[c].id = f.ad.contexts[c].id;
        ctx[c].value = f.ad.contexts[c].value;
      }
    }
    r->contexts_count = ctx_ptr ? ctx_count : 0;
    r->contexts_ptr = ctx_ptr;

    uint32_t prop_count = static_cast<uint32_t>(f.ad.properties.size());
    uint32_t prop_ptr = space.Alloc(prop_count * sizeof(XUSER_PROPERTY));
    if (prop_ptr) {
      for (uint32_t p = 0; p < prop_count; ++p) {
        WriteProperty(G<XUSER_PROPERTY>(prop_ptr + p * sizeof(XUSER_PROPERTY)), f.ad.properties[p],
                      space);
      }
    }
    r->properties_count = prop_ptr ? prop_count : 0;
    r->properties_ptr = prop_ptr;
  }
  return X_ERROR_SUCCESS;
}

std::vector<lobby::Found> FilterJoinable(std::vector<lobby::Found> found) {
  std::vector<uint64_t> own;
  {
    std::lock_guard lock(g_mutex);
    for (const auto& [obj, s] : g_sessions) {
      if (s.is_host) own.push_back(s.id);
    }
  }
  std::vector<lobby::Found> out;
  for (auto& f : found) {
    if (std::find(own.begin(), own.end(), f.ad.id) != own.end()) continue;
    bool full = f.ad.filled_public >= f.ad.max_public;
    bool started = f.ad.state == static_cast<uint32_t>(SessionState::kInGame);
    bool no_late_join = f.ad.flags & kFlagJoinInProgressDisabled;
    if (full || (started && no_late_join)) {
      NETPLAY_LOG("skipping lobby {:016X} of '{}': {}", f.ad.id, f.ad.host_name,
                  full ? "full" : "already started");
      continue;
    }
    out.push_back(std::move(f));
  }
  return out;
}

void LogFound(const char* what, const std::vector<lobby::Found>& found) {
  NETPLAY_LOG("{}: {} lobby(s)", what, found.size());
  for (const auto& f : found) {
    NETPLAY_LOG("  {:016X} host '{}' at {} players {}/{} state {} rtt {} ms", f.ad.id,
                f.ad.host_name, udp::ToString(f.from), f.ad.filled_public + f.ad.filled_private,
                f.ad.max_public + f.ad.max_private, f.ad.state, f.rtt_ms);
  }
}

// ---- Message handlers ------------------------------------------------------------

uint32_t Create(const XGI_SESSION_CREATE& req) {
  std::lock_guard lock(g_mutex);
  Session& s = SessionFor(req.obj_ptr);
  s.flags = req.flags;
  s.max_public = req.num_slots_public;
  s.max_private = req.num_slots_private;
  s.members.clear();
  s.state = SessionState::kLobby;
  s.nonce = Random64();
  s.is_host = (s.flags & kFlagHost) != 0;

  auto* info = G<XSESSION_INFO>(req.session_info_ptr);
  if (s.is_host) {
    s.id = (Random64() & 0x0FFFFFFFFFFFFFFFull) | (uint64_t(kXnkidOnlinePeer) << 56);
    uint64_t k1 = Random64(), k2 = Random64();
    std::memcpy(s.key.ab, &k1, 8);
    std::memcpy(s.key.ab + 8, &k2, 8);
    FillXnAddr(LocalIp(), &s.host);
    if (info) {
      WriteXnkid(s.id, &info->session_id);
      info->host_address = s.host;
      info->key_exchange_key = s.key;
    }
  } else if (info) {
    s.id = ReadXnkid(info->session_id);
    s.host = info->host_address;
    s.key = info->key_exchange_key;
  }
  if (auto* nonce = G<rex::be<uint64_t>>(req.nonce_ptr)) *nonce = s.nonce;
  if (s.flags & (kFlagMatchmaking | kFlagPeerNetwork)) g_current_session = s.id;

  NETPLAY_LOG("XSessionCreate obj {:08X} flags {:08X} slots {}+{} {} session {:016X} host {}",
              s.obj, s.flags, s.max_public, s.max_private, s.is_host ? "HOST" : "join",
              s.id, udp::ToString(IpFromXnAddr(s.host)));
  Publish(s);
  return X_ERROR_SUCCESS;
}

uint32_t Delete(const XGI_SESSION_STATE& req) {
  std::lock_guard lock(g_mutex);
  auto it = g_sessions.find(req.obj_ptr);
  if (it == g_sessions.end()) return X_ERROR_SUCCESS;
  NETPLAY_LOG("XSessionDelete obj {:08X} session {:016X}", it->second.obj, it->second.id);
  if (it->second.is_host && it->second.id) lobby::Withdraw(it->second.id);
  if (g_current_session == it->second.id) g_current_session = 0;
  g_sessions.erase(it);
  return X_ERROR_SUCCESS;
}

uint32_t Manage(const XGI_SESSION_MANAGE& req, bool join) {
  std::lock_guard lock(g_mutex);
  Session& s = SessionFor(req.obj_ptr);
  auto* xuids = G<rex::be<uint64_t>>(req.xuids_ptr);
  auto* indices = G<rex::be<uint32_t>>(req.indices_ptr);
  auto* privates = G<rex::be<uint32_t>>(req.private_slots_ptr);
  for (uint32_t i = 0; i < req.count; ++i) {
    Member m;
    if (xuids) {
      m.xuid = xuids[i];
    } else if (indices) {
      m.user_index = indices[i];
      m.xuid = m.user_index == 0 ? OnlineXuid() : 0;
    }
    m.is_private = privates && privates[i] != 0u;
    auto same = [&](const Member& x) { return x.xuid == m.xuid; };
    s.members.erase(std::remove_if(s.members.begin(), s.members.end(), same), s.members.end());
    if (join) s.members.push_back(m);
    NETPLAY_LOG("XSession{} obj {:08X} {} {:016X}{}", join ? "Join" : "Leave", s.obj,
                xuids ? "remote" : "local", m.xuid, m.is_private ? " (private)" : "");
  }
  Publish(s);
  return X_ERROR_SUCCESS;
}

uint32_t SetState(const XGI_SESSION_STATE& req, SessionState state) {
  std::lock_guard lock(g_mutex);
  Session& s = SessionFor(req.obj_ptr);
  s.state = state;
  NETPLAY_LOG("XSession{} obj {:08X}", state == SessionState::kInGame ? "Start" : "End", s.obj);
  Publish(s);
  return X_ERROR_SUCCESS;
}

uint32_t Modify(const XGI_SESSION_MODIFY& req) {
  std::lock_guard lock(g_mutex);
  Session& s = SessionFor(req.obj_ptr);
  s.flags = (s.flags & kFlagHost) | (req.flags & ~kFlagHost);
  s.max_public = req.max_public_slots;
  s.max_private = req.max_private_slots;
  NETPLAY_LOG("XSessionModify obj {:08X} flags {:08X} slots {}+{}", s.obj, s.flags, s.max_public,
              s.max_private);
  Publish(s);
  return X_ERROR_SUCCESS;
}

uint32_t Details(const XGI_SESSION_DETAILS& req) {
  auto* size_ptr = G<rex::be<uint32_t>>(req.buffer_size_ptr);
  auto* details = G<XSESSION_LOCAL_DETAILS>(req.details_ptr);
  if (!size_ptr || !details) return X_E_INVALIDARG;
  uint32_t size = *size_ptr;
  if (size < sizeof(XSESSION_LOCAL_DETAILS)) {
    *size_ptr = static_cast<uint32_t>(sizeof(XSESSION_LOCAL_DETAILS));
    return X_ERROR_INSUFFICIENT_BUFFER;
  }
  std::lock_guard lock(g_mutex);
  Session& s = SessionFor(req.obj_ptr);
  uint32_t filled_pub, filled_priv;
  Counts(s, &filled_pub, &filled_priv);
  std::memset(details, 0, sizeof(*details));
  details->user_index_host = s.is_host ? 0 : kUserIndexNone;
  details->game_type = g_contexts.count(kContextGameType) ? g_contexts[kContextGameType] : 1;
  details->game_mode = g_contexts.count(kContextGameMode) ? g_contexts[kContextGameMode] : 0;
  details->flags = s.flags;
  details->max_public_slots = s.max_public;
  details->max_private_slots = s.max_private;
  details->available_public_slots = s.max_public > filled_pub ? s.max_public - filled_pub : 0;
  details->available_private_slots = s.max_private > filled_priv ? s.max_private - filled_priv : 0;
  details->state = static_cast<uint32_t>(s.state);
  details->nonce = s.nonce;
  WriteXnkid(s.id, &details->session_info.session_id);
  details->session_info.host_address = s.host;
  details->session_info.key_exchange_key = s.key;

  uint32_t room = (size - sizeof(XSESSION_LOCAL_DETAILS)) / sizeof(XSESSION_MEMBER);
  uint32_t returned = std::min<uint32_t>(room, static_cast<uint32_t>(s.members.size()));
  uint32_t members_ptr = req.details_ptr + sizeof(XSESSION_LOCAL_DETAILS);
  for (uint32_t i = 0; i < returned; ++i) {
    auto* m = G<XSESSION_MEMBER>(members_ptr + i * sizeof(XSESSION_MEMBER));
    m->online_xuid = s.members[i].xuid;
    m->user_index = s.members[i].user_index;
    m->flags = s.members[i].is_private ? 1 : 0;
  }
  details->actual_member_count = static_cast<uint32_t>(s.members.size());
  details->returned_member_count = returned;
  details->members_ptr = returned ? members_ptr : 0;
  return X_ERROR_SUCCESS;
}

uint32_t Migrate(const XGI_SESSION_MIGRATE& req) {
  std::lock_guard lock(g_mutex);
  Session& s = SessionFor(req.obj_ptr);
  auto* info = G<XSESSION_INFO>(req.session_info_ptr);
  if (req.user_index != kUserIndexNone) {
    // We become the host.
    s.is_host = true;
    s.flags |= kFlagHost;
    FillXnAddr(LocalIp(), &s.host);
    if (info) {
      WriteXnkid(s.id, &info->session_id);
      info->host_address = s.host;
      info->key_exchange_key = s.key;
    }
  } else if (info) {
    if (s.is_host && s.id) lobby::Withdraw(s.id);
    s.is_host = false;
    s.flags &= ~kFlagHost;
    s.host = info->host_address;
  }
  NETPLAY_LOG("XSessionMigrateHost obj {:08X} new host {}", s.obj,
              udp::ToString(IpFromXnAddr(s.host)));
  Publish(s);
  return X_ERROR_SUCCESS;
}

uint32_t Arbitration(const XGI_SESSION_ARBITRATION& req) {
  std::lock_guard lock(g_mutex);
  Session& s = SessionFor(req.obj_ptr);
  if (!req.results_ptr) return X_ERROR_SUCCESS;
  uint32_t end = req.results_ptr + req.results_buffer_size;
  uint32_t n = static_cast<uint32_t>(std::max<size_t>(s.members.size(), 1));
  uint32_t registrants = req.results_ptr + sizeof(XSESSION_REGISTRATION_RESULTS);
  uint32_t xuids = registrants + n * sizeof(XSESSION_REGISTRANT);
  if (xuids + n * 8 > end) return X_ERROR_INSUFFICIENT_BUFFER;
  auto* results = G<XSESSION_REGISTRATION_RESULTS>(req.results_ptr);
  results->count = n;
  results->registrants_ptr = registrants;
  for (uint32_t i = 0; i < n; ++i) {
    uint64_t xuid = i < s.members.size() ? s.members[i].xuid : OnlineXuid();
    bool local = i >= s.members.size() || s.members[i].user_index != kUserIndexNone;
    auto* r = G<XSESSION_REGISTRANT>(registrants + i * sizeof(XSESSION_REGISTRANT));
    r->machine_id = local ? MachineId(LocalIp()) : (0xFB00000000000000ull | (xuid & 0xFFFFFFFFFFFFull));
    r->trustworthiness = 1;
    r->num_users = 1;
    r->users_ptr = xuids + i * 8;
    *G<rex::be<uint64_t>>(xuids + i * 8) = xuid;
  }
  return X_ERROR_SUCCESS;
}

}  // namespace

uint32_t CreateHandle() {
  std::lock_guard lock(g_mutex);
  return g_next_handle++;
}

uint32_t ObjectForHandle(uint32_t handle) {
  std::lock_guard lock(g_mutex);
  auto it = g_handle_objects.find(handle);
  if (it != g_handle_objects.end()) return it->second;
  // A small guest block gives every session a distinct object pointer.
  uint32_t obj = Mem()->SystemHeapAlloc(16);
  if (obj) std::memset(G<uint8_t>(obj), 0, 16);
  g_handle_objects[handle] = obj;
  return obj;
}

uint64_t CurrentSessionId() { return g_current_session; }

uint32_t RunRequest(uint32_t overlapped_ptr, bool background, std::function<uint32_t()> work) {
  if (!overlapped_ptr) return work();
  auto* ks = REX_KERNEL_STATE();
  if (!background) {
    ks->CompleteOverlappedImmediate(overlapped_ptr, work());
    return X_ERROR_IO_PENDING;
  }
  auto* ptr = ks->memory()->TranslateVirtual(overlapped_ptr);
  rex::system::XOverlappedSetResult(ptr, X_ERROR_IO_PENDING);
  rex::system::XOverlappedSetContext(ptr, rex::system::XThread::GetCurrentThreadHandle());
  std::thread([ks, overlapped_ptr, work = std::move(work)] {
    uint32_t result = work();
    uint32_t length = result == X_ERROR_SUCCESS ? 0 : 0xFFFFFFFF;
    ks->CompleteOverlappedEx(overlapped_ptr, result, result, length);
  }).detach();
  return X_ERROR_IO_PENDING;
}

bool HandleXgi(uint32_t message, uint32_t buffer_ptr, uint32_t buffer_length,
               uint32_t overlapped_ptr, uint32_t* result) {
  (void)buffer_length;
  auto* buffer = G<uint8_t>(buffer_ptr);
  if (!buffer) return false;

  switch (message) {
    case 0x000B0006: {  // XUserSetContextEx: record, then let the SDK handle it too
      uint32_t id = rex::memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t value = rex::memory::load_and_swap<uint32_t>(buffer + 20);
      std::lock_guard lock(g_mutex);
      g_contexts[id] = value;
      PublishAllHosted();
      return false;
    }
    case 0x000B0007: {  // XUserSetPropertyEx
      uint32_t id = rex::memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t size = rex::memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t ptr = rex::memory::load_and_swap<uint32_t>(buffer + 24);
      std::vector<uint8_t> value;
      if (auto* src = G<uint8_t>(ptr); src && size <= 4096) value.assign(src, src + size);
      std::lock_guard lock(g_mutex);
      g_properties[id] = std::move(value);
      PublishAllHosted();
      return false;
    }
    case 0x000B0010: {
      auto req = *reinterpret_cast<XGI_SESSION_CREATE*>(buffer);
      *result = RunRequest(overlapped_ptr, false, [req] { return Create(req); });
      return true;
    }
    case 0x000B0011: {
      auto req = *reinterpret_cast<XGI_SESSION_STATE*>(buffer);
      *result = RunRequest(overlapped_ptr, false, [req] { return Delete(req); });
      return true;
    }
    case 0x000B0012:
    case 0x000B0013: {
      auto req = *reinterpret_cast<XGI_SESSION_MANAGE*>(buffer);
      bool join = message == 0x000B0012;
      *result = RunRequest(overlapped_ptr, false, [req, join] { return Manage(req, join); });
      return true;
    }
    case 0x000B0014:
    case 0x000B0015: {
      auto req = *reinterpret_cast<XGI_SESSION_STATE*>(buffer);
      auto state = message == 0x000B0014 ? SessionState::kInGame : SessionState::kReporting;
      *result = RunRequest(overlapped_ptr, false, [req, state] { return SetState(req, state); });
      return true;
    }
    case 0x000B0018: {
      auto req = *reinterpret_cast<XGI_SESSION_MODIFY*>(buffer);
      *result = RunRequest(overlapped_ptr, false, [req] { return Modify(req); });
      return true;
    }
    case 0x000B001A: {
      auto req = *reinterpret_cast<XGI_SESSION_ARBITRATION*>(buffer);
      *result = RunRequest(overlapped_ptr, false, [req] { return Arbitration(req); });
      return true;
    }
    case 0x000B0016:
    case 0x000B001C: {  // XSessionSearch / XSessionSearchEx
      auto req = *reinterpret_cast<XGI_SESSION_SEARCH*>(buffer);
      NETPLAY_LOG("XSessionSearch proc {} max {} results, {} props, {} contexts",
                  uint32_t(req.proc_index), uint32_t(req.num_results), uint16_t(req.num_props),
                  uint16_t(req.num_ctx));
      *result = RunRequest(overlapped_ptr, true, [req] {
        auto found = FilterJoinable(lobby::Search(static_cast<int>(SearchTimeoutMs())));
        LogFound("XSessionSearch", found);
        if (!found.empty()) NoteRemoteHost(found.front().from.ip);
        return WriteSearchResults(found, req.num_results, req.results_ptr,
                                  req.results_buffer_size);
      });
      return true;
    }
    case 0x000B001B: {  // XSessionSearchByID
      auto req = *reinterpret_cast<XGI_SESSION_SEARCH_BYID*>(buffer);
      uint64_t id = ReadXnkid(req.session_id);
      *result = RunRequest(overlapped_ptr, true, [req, id] {
        auto found = lobby::Search(static_cast<int>(SearchTimeoutMs()), id);
        LogFound("XSessionSearchByID", found);
        if (!found.empty()) NoteRemoteHost(found.front().from.ip);
        return WriteSearchResults(found, 1, req.results_ptr, req.results_buffer_size);
      });
      return true;
    }
    case 0x000B0060: {  // XSessionSearchByIDs
      auto req = *reinterpret_cast<XGI_SESSION_SEARCH_BYIDS*>(buffer);
      std::vector<uint64_t> ids;
      if (auto* p = G<XNKID>(req.ids_ptr)) {
        for (uint32_t i = 0; i < req.num_ids; ++i) ids.push_back(ReadXnkid(p[i]));
      }
      *result = RunRequest(overlapped_ptr, true, [req, ids] {
        std::vector<lobby::Found> found;
        for (auto& f : lobby::Search(static_cast<int>(SearchTimeoutMs()))) {
          if (std::find(ids.begin(), ids.end(), f.ad.id) != ids.end()) found.push_back(f);
        }
        LogFound("XSessionSearchByIDs", found);
        return WriteSearchResults(found, static_cast<uint32_t>(ids.size()), req.results_ptr,
                                  req.results_buffer_size);
      });
      return true;
    }
    case 0x000B001D: {
      auto req = *reinterpret_cast<XGI_SESSION_DETAILS*>(buffer);
      *result = RunRequest(overlapped_ptr, false, [req] { return Details(req); });
      return true;
    }
    case 0x000B001E: {
      auto req = *reinterpret_cast<XGI_SESSION_MIGRATE*>(buffer);
      *result = RunRequest(overlapped_ptr, false, [req] { return Migrate(req); });
      return true;
    }
    case 0x000B0021: {  // XUserReadStats (leaderboards)
      // No leaderboard server: answer every requested view with zero rows.
      // The SDK returned success without writing the results, so the game
      // parsed an uninitialized buffer (its own malloc) as view and row
      // pointers and crashed on the results screen after an online stage
      // (GitHub issue #14).
      uint32_t specs_count = rex::memory::load_and_swap<uint32_t>(buffer + 12);
      uint32_t specs_ptr = rex::memory::load_and_swap<uint32_t>(buffer + 16);
      uint32_t results_size = rex::memory::load_and_swap<uint32_t>(buffer + 20);
      uint32_t results_ptr = rex::memory::load_and_swap<uint32_t>(buffer + 24);
      *result = RunRequest(overlapped_ptr, false, [=] {
        auto* results = G<uint8_t>(results_ptr);
        if (!results || results_size < 8) return uint32_t(X_ERROR_INSUFFICIENT_BUFFER);
        // XUSER_STATS_READ_RESULTS { dwNumViews, pViews }, then the views:
        // XUSER_STATS_VIEW { dwViewId, dwTotalViewRows, dwNumRows, pRows }.
        uint32_t views = specs_count;
        if (results_size < 8 + 16ull * views) views = 0;
        std::memset(results, 0, std::min<uint32_t>(results_size, 8 + 16 * views));
        rex::memory::store_and_swap<uint32_t>(results + 0, views);
        rex::memory::store_and_swap<uint32_t>(results + 4, views ? results_ptr + 8 : 0);
        auto* specs = G<uint8_t>(specs_ptr);
        for (uint32_t i = 0; i < views; ++i) {
          // XUSER_STATS_SPEC { dwViewId, dwNumColumnIds, rgwColumnIds[64] } = 136 bytes.
          uint32_t view_id = specs ? rex::memory::load_and_swap<uint32_t>(specs + 136 * i) : 0;
          rex::memory::store_and_swap<uint32_t>(results + 8 + 16 * i, view_id);
        }
        REXLOG_INFO("[netplay] XUserReadStats: {} view(s), no rows (no leaderboard server)", views);
        return uint32_t(X_ERROR_SUCCESS);
      });
      return true;
    }
    case 0x000B001F:    // XSessionModifySkill
    case 0x000B0025:    // XSessionWriteStats
    case 0x000B0026: {  // XSessionFlushStats
      *result = RunRequest(overlapped_ptr, false, [] { return uint32_t(X_ERROR_SUCCESS); });
      return true;
    }
    default:
      return false;
  }
}

}  // namespace harmony::netplay::sessions
