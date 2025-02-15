// Copyright 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "Core/IOS/Network/IP/Top.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <netdb.h>
#include <poll.h>
#endif

#include <fmt/format.h>

#include "Common/Assert.h"
#include "Common/CommonTypes.h"
#include "Common/Logging/Log.h"
#include "Common/Network.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"

#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/Network/ICMP.h"
#include "Core/IOS/Network/MACUtils.h"
#include "Core/IOS/Network/Socket.h"

#ifdef _WIN32
#include <iphlpapi.h>
#include <ws2tcpip.h>

#define MALLOC(x) HeapAlloc(GetProcessHeap(), 0, (x))
#define FREE(x) HeapFree(GetProcessHeap(), 0, (x))

#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <resolv.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace IOS::HLE::Device
{
enum SOResultCode : s32
{
  SO_ERROR_INVALID_REQUEST = -51,
  SO_ERROR_HOST_NOT_FOUND = -305,
};

NetIPTop::NetIPTop(Kernel& ios, const std::string& device_name) : Device(ios, device_name)
{
#ifdef _WIN32
  const int ret = WSAStartup(MAKEWORD(2, 2), &InitData);
  INFO_LOG_FMT(IOS_NET, "WSAStartup: {}", ret);
#endif
}

NetIPTop::~NetIPTop()
{
#ifdef _WIN32
  WSACleanup();
#endif
}

void NetIPTop::DoState(PointerWrap& p)
{
  DoStateShared(p);
  WiiSockMan::GetInstance().DoState(p);
}

static int inet_pton(const char* src, unsigned char* dst)
{
  int saw_digit = 0;
  int octets = 0;
  unsigned char tmp[4]{};
  unsigned char* tp = tmp;
  char ch;

  while ((ch = *src++) != '\0')
  {
    if (ch >= '0' && ch <= '9')
    {
      unsigned int newt = (*tp * 10) + (ch - '0');

      if (newt > 255)
        return 0;
      *tp = newt;
      if (!saw_digit)
      {
        if (++octets > 4)
          return 0;
        saw_digit = 1;
      }
    }
    else if (ch == '.' && saw_digit)
    {
      if (octets == 4)
        return 0;
      *++tp = 0;
      saw_digit = 0;
    }
    else
    {
      return 0;
    }
  }
  if (octets < 4)
    return 0;
  memcpy(dst, tmp, 4);
  return 1;
}

// Maps SOCKOPT level from Wii to native
static s32 MapWiiSockOptLevelToNative(u32 level)
{
  if (level == 0xFFFF)
    return SOL_SOCKET;

  INFO_LOG_FMT(IOS_NET, "SO_SETSOCKOPT: unknown level {}", level);
  return level;
}

// Maps SOCKOPT optname from native to Wii
static s32 MapWiiSockOptNameToNative(u32 optname)
{
  switch (optname)
  {
  case 0x4:
    return SO_REUSEADDR;
  case 0x80:
    return SO_LINGER;
  case 0x100:
    return SO_OOBINLINE;
  case 0x1001:
    return SO_SNDBUF;
  case 0x1002:
    return SO_RCVBUF;
  case 0x1003:
    return SO_SNDLOWAT;
  case 0x1004:
    return SO_RCVLOWAT;
  case 0x1008:
    return SO_TYPE;
  case 0x1009:
    return SO_ERROR;
  }

  INFO_LOG_FMT(IOS_NET, "SO_SETSOCKOPT: unknown optname {}", optname);
  return optname;
}

// u32 values are in little endian (i.e. 0x0100007f means 127.0.0.1)
struct DefaultInterface
{
  u32 inet;       // IPv4 address
  u32 netmask;    // IPv4 subnet mask
  u32 broadcast;  // IPv4 broadcast address
};

static std::optional<DefaultInterface> GetSystemDefaultInterface()
{
#ifdef _WIN32
  std::unique_ptr<MIB_IPFORWARDTABLE> forward_table;
  DWORD forward_table_size = 0;
  if (GetIpForwardTable(nullptr, &forward_table_size, FALSE) == ERROR_INSUFFICIENT_BUFFER)
  {
    forward_table =
        std::unique_ptr<MIB_IPFORWARDTABLE>((PMIB_IPFORWARDTABLE) operator new(forward_table_size));
  }

  std::unique_ptr<MIB_IPADDRTABLE> ip_table;
  DWORD ip_table_size = 0;
  if (GetIpAddrTable(nullptr, &ip_table_size, FALSE) == ERROR_INSUFFICIENT_BUFFER)
  {
    ip_table = std::unique_ptr<MIB_IPADDRTABLE>((PMIB_IPADDRTABLE) operator new(ip_table_size));
  }

  // find the interface IP used for the default route and use that
  NET_IFINDEX ifIndex = NET_IFINDEX_UNSPECIFIED;
  DWORD result = GetIpForwardTable(forward_table.get(), &forward_table_size, FALSE);
  // can return ERROR_MORE_DATA on XP even after the first call
  while (result == NO_ERROR || result == ERROR_MORE_DATA)
  {
    for (DWORD i = 0; i < forward_table->dwNumEntries; ++i)
    {
      if (forward_table->table[i].dwForwardDest == 0)
      {
        ifIndex = forward_table->table[i].dwForwardIfIndex;
        break;
      }
    }

    if (result == NO_ERROR || ifIndex != NET_IFINDEX_UNSPECIFIED)
      break;

    result = GetIpForwardTable(forward_table.get(), &forward_table_size, FALSE);
  }

  if (ifIndex != NET_IFINDEX_UNSPECIFIED &&
      GetIpAddrTable(ip_table.get(), &ip_table_size, FALSE) == NO_ERROR)
  {
    for (DWORD i = 0; i < ip_table->dwNumEntries; ++i)
    {
      const auto& entry = ip_table->table[i];
      if (entry.dwIndex == ifIndex)
        return DefaultInterface{entry.dwAddr, entry.dwMask, entry.dwBCastAddr};
    }
  }
#elif !defined(__ANDROID__)
  // Assume that the address that is used to access the Internet corresponds
  // to the default interface.
  auto get_default_address = []() -> std::optional<in_addr> {
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    Common::ScopeGuard sock_guard{[sock] { close(sock); }};

    sockaddr_in addr{};
    socklen_t length = sizeof(addr);
    addr.sin_family = AF_INET;
    // The address and port are irrelevant -- no packet is actually sent. These just need to be set
    // to a valid IP and port.
    addr.sin_addr.s_addr = inet_addr("8.8.8.8");
    addr.sin_port = htons(53);
    if (connect(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == -1)
      return {};
    if (getsockname(sock, reinterpret_cast<sockaddr*>(&addr), &length) == -1)
      return {};
    return addr.sin_addr;
  };

  auto get_addr = [](const sockaddr* addr) {
    return reinterpret_cast<const sockaddr_in*>(addr)->sin_addr.s_addr;
  };

  const auto default_interface_address = get_default_address();
  if (!default_interface_address)
    return {};

  ifaddrs* iflist;
  if (getifaddrs(&iflist) != 0)
    return {};
  Common::ScopeGuard iflist_guard{[iflist] { freeifaddrs(iflist); }};

  for (const ifaddrs* iface = iflist; iface; iface = iface->ifa_next)
  {
    if (iface->ifa_addr && iface->ifa_addr->sa_family == AF_INET &&
        get_addr(iface->ifa_addr) == default_interface_address->s_addr)
    {
      return DefaultInterface{get_addr(iface->ifa_addr), get_addr(iface->ifa_netmask),
                              get_addr(iface->ifa_broadaddr)};
    }
  }
#endif
  return {};
}

static DefaultInterface GetSystemDefaultInterfaceOrFallback()
{
  static const u32 FALLBACK_IP = inet_addr("10.0.1.30");
  static const u32 FALLBACK_NETMASK = inet_addr("255.255.255.0");
  static const u32 FALLBACK_GATEWAY = inet_addr("10.0.255.255");
  static const DefaultInterface FALLBACK_VALUES{FALLBACK_IP, FALLBACK_NETMASK, FALLBACK_GATEWAY};
  return GetSystemDefaultInterface().value_or(FALLBACK_VALUES);
}

IPCCommandResult NetIPTop::IOCtl(const IOCtlRequest& request)
{
  if (Core::WantsDeterminism())
  {
    return GetDefaultReply(IPC_EACCES);
  }

  switch (request.request)
  {
  case IOCTL_SO_INITINTERFACE:
    return HandleInitInterfaceRequest(request);
  case IOCTL_SO_SOCKET:
    return HandleSocketRequest(request);
  case IOCTL_SO_ICMPSOCKET:
    return HandleICMPSocketRequest(request);
  case IOCTL_SO_CLOSE:
  case IOCTL_SO_ICMPCLOSE:
    return HandleCloseRequest(request);
  case IOCTL_SO_ACCEPT:
  case IOCTL_SO_BIND:
  case IOCTL_SO_CONNECT:
  case IOCTL_SO_FCNTL:
    return HandleDoSockRequest(request);
  case IOCTL_SO_SHUTDOWN:
    return HandleShutdownRequest(request);
  case IOCTL_SO_LISTEN:
    return HandleListenRequest(request);
  case IOCTL_SO_GETSOCKOPT:
    return HandleGetSockOptRequest(request);
  case IOCTL_SO_SETSOCKOPT:
    return HandleSetSockOptRequest(request);
  case IOCTL_SO_GETSOCKNAME:
    return HandleGetSockNameRequest(request);
  case IOCTL_SO_GETPEERNAME:
    return HandleGetPeerNameRequest(request);
  case IOCTL_SO_GETHOSTID:
    return HandleGetHostIDRequest(request);
  case IOCTL_SO_INETATON:
    return HandleInetAToNRequest(request);
  case IOCTL_SO_INETPTON:
    return HandleInetPToNRequest(request);
  case IOCTL_SO_INETNTOP:
    return HandleInetNToPRequest(request);
  case IOCTL_SO_POLL:
    return HandlePollRequest(request);
  case IOCTL_SO_GETHOSTBYNAME:
    return HandleGetHostByNameRequest(request);
  case IOCTL_SO_ICMPCANCEL:
    return HandleICMPCancelRequest(request);
  default:
    request.DumpUnknown(GetDeviceName(), Common::Log::IOS_NET);
    break;
  }

  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult NetIPTop::IOCtlV(const IOCtlVRequest& request)
{
  switch (request.request)
  {
  case IOCTLV_SO_GETINTERFACEOPT:
    return HandleGetInterfaceOptRequest(request);
  case IOCTLV_SO_SENDTO:
    return HandleSendToRequest(request);
  case IOCTLV_SO_RECVFROM:
    return HandleRecvFromRequest(request);
  case IOCTLV_SO_GETADDRINFO:
    return HandleGetAddressInfoRequest(request);
  case IOCTLV_SO_ICMPPING:
    return HandleICMPPingRequest(request);
  default:
    request.DumpUnknown(GetDeviceName(), Common::Log::IOS_NET);
    break;
  }

  return GetDefaultReply(IPC_SUCCESS);
}

void NetIPTop::Update()
{
  WiiSockMan::GetInstance().Update();
}

IPCCommandResult NetIPTop::HandleInitInterfaceRequest(const IOCtlRequest& request)
{
  request.Log(GetDeviceName(), Common::Log::IOS_WC24);
  return GetDefaultReply(IPC_SUCCESS);
}

IPCCommandResult NetIPTop::HandleSocketRequest(const IOCtlRequest& request)
{
  const u32 af = Memory::Read_U32(request.buffer_in);
  const u32 type = Memory::Read_U32(request.buffer_in + 4);
  const u32 prot = Memory::Read_U32(request.buffer_in + 8);

  WiiSockMan& sm = WiiSockMan::GetInstance();
  const s32 return_value = sm.NewSocket(af, type, prot);
  INFO_LOG_FMT(IOS_NET,
               "IOCTL_SO_SOCKET "
               "Socket: {:08x} ({},{},{}), BufferIn: ({:08x}, {}), BufferOut: ({:08x}, {})",
               return_value, af, type, prot, request.buffer_in, request.buffer_in_size,
               request.buffer_out, request.buffer_out_size);

  return GetDefaultReply(return_value);
}

IPCCommandResult NetIPTop::HandleICMPSocketRequest(const IOCtlRequest& request)
{
  const u32 pf = Memory::Read_U32(request.buffer_in);

  WiiSockMan& sm = WiiSockMan::GetInstance();
  const s32 return_value = sm.NewSocket(pf, SOCK_RAW, IPPROTO_ICMP);
  INFO_LOG_FMT(IOS_NET, "IOCTL_SO_ICMPSOCKET({:x}) {}", pf, return_value);
  return GetDefaultReply(return_value);
}

IPCCommandResult NetIPTop::HandleCloseRequest(const IOCtlRequest& request)
{
  const u32 fd = Memory::Read_U32(request.buffer_in);
  WiiSockMan& sm = WiiSockMan::GetInstance();
  const s32 return_value = sm.DeleteSocket(fd);
  const char* const close_fn =
      request.request == IOCTL_SO_ICMPCLOSE ? "IOCTL_SO_ICMPCLOSE" : "IOCTL_SO_CLOSE";

  INFO_LOG_FMT(IOS_NET, "{}({:x}) {:x}", close_fn, fd, return_value);

  return GetDefaultReply(return_value);
}

IPCCommandResult NetIPTop::HandleDoSockRequest(const IOCtlRequest& request)
{
  const u32 fd = Memory::Read_U32(request.buffer_in);
  WiiSockMan& sm = WiiSockMan::GetInstance();
  sm.DoSock(fd, request, static_cast<NET_IOCTL>(request.request));
  return GetNoReply();
}

IPCCommandResult NetIPTop::HandleShutdownRequest(const IOCtlRequest& request)
{
  if (request.buffer_in == 0 || request.buffer_in_size < 8)
  {
    ERROR_LOG_FMT(IOS_NET, "IOCTL_SO_SHUTDOWN = EINVAL, BufferIn: ({:08x}, {})", request.buffer_in,
                  request.buffer_in_size);
    return GetDefaultReply(-SO_EINVAL);
  }

  const u32 fd = Memory::Read_U32(request.buffer_in);
  const u32 how = Memory::Read_U32(request.buffer_in + 4);
  WiiSockMan& sm = WiiSockMan::GetInstance();
  const s32 return_value = sm.ShutdownSocket(fd, how);

  INFO_LOG_FMT(IOS_NET, "IOCTL_SO_SHUTDOWN(fd={}, how={}) = {}", fd, how, return_value);
  return GetDefaultReply(return_value);
}

IPCCommandResult NetIPTop::HandleListenRequest(const IOCtlRequest& request)
{
  u32 fd = Memory::Read_U32(request.buffer_in);
  u32 BACKLOG = Memory::Read_U32(request.buffer_in + 0x04);
  u32 ret = listen(WiiSockMan::GetInstance().GetHostSocket(fd), BACKLOG);

  request.Log(GetDeviceName(), Common::Log::IOS_WC24);
  return GetDefaultReply(WiiSockMan::GetNetErrorCode(ret, "SO_LISTEN", false));
}

IPCCommandResult NetIPTop::HandleGetSockOptRequest(const IOCtlRequest& request)
{
  u32 fd = Memory::Read_U32(request.buffer_out);
  u32 level = Memory::Read_U32(request.buffer_out + 4);
  u32 optname = Memory::Read_U32(request.buffer_out + 8);

  request.Log(GetDeviceName(), Common::Log::IOS_WC24);

  // Do the level/optname translation
  int nat_level = MapWiiSockOptLevelToNative(level);
  int nat_optname = MapWiiSockOptNameToNative(optname);

  u8 optval[20];
  u32 optlen = 4;

  int ret = getsockopt(WiiSockMan::GetInstance().GetHostSocket(fd), nat_level, nat_optname,
                       (char*)&optval, (socklen_t*)&optlen);
  const s32 return_value = WiiSockMan::GetNetErrorCode(ret, "SO_GETSOCKOPT", false);

  Memory::Write_U32(optlen, request.buffer_out + 0xC);
  Memory::CopyToEmu(request.buffer_out + 0x10, optval, optlen);

  if (optname == SO_ERROR)
  {
    s32 last_error = WiiSockMan::GetInstance().GetLastNetError();

    Memory::Write_U32(sizeof(s32), request.buffer_out + 0xC);
    Memory::Write_U32(last_error, request.buffer_out + 0x10);
  }

  return GetDefaultReply(return_value);
}

IPCCommandResult NetIPTop::HandleSetSockOptRequest(const IOCtlRequest& request)
{
  const u32 fd = Memory::Read_U32(request.buffer_in);
  const u32 level = Memory::Read_U32(request.buffer_in + 4);
  const u32 optname = Memory::Read_U32(request.buffer_in + 8);
  u32 optlen = Memory::Read_U32(request.buffer_in + 0xc);
  u8 optval[20];
  optlen = std::min(optlen, (u32)sizeof(optval));
  Memory::CopyFromEmu(optval, request.buffer_in + 0x10, optlen);

  INFO_LOG_FMT(IOS_NET,
               "IOCTL_SO_SETSOCKOPT({:08x}, {:08x}, {:08x}, {:08x}) "
               "BufferIn: ({:08x}, {}), BufferOut: ({:08x}, {})"
               "{:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} "
               "{:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
               fd, level, optname, optlen, request.buffer_in, request.buffer_in_size,
               request.buffer_out, request.buffer_out_size, optval[0], optval[1], optval[2],
               optval[3], optval[4], optval[5], optval[6], optval[7], optval[8], optval[9],
               optval[10], optval[11], optval[12], optval[13], optval[14], optval[15], optval[16],
               optval[17], optval[18], optval[19]);

  // TODO: bug booto about this, 0x2005 most likely timeout related, default value on Wii is ,
  // 0x2001 is most likely tcpnodelay
  if (level == 6 && (optname == 0x2005 || optname == 0x2001))
    return GetDefaultReply(0);

  // Do the level/optname translation
  const int nat_level = MapWiiSockOptLevelToNative(level);
  const int nat_optname = MapWiiSockOptNameToNative(optname);

  const int ret = setsockopt(WiiSockMan::GetInstance().GetHostSocket(fd), nat_level, nat_optname,
                             reinterpret_cast<char*>(optval), optlen);
  return GetDefaultReply(WiiSockMan::GetNetErrorCode(ret, "SO_SETSOCKOPT", false));
}

IPCCommandResult NetIPTop::HandleGetSockNameRequest(const IOCtlRequest& request)
{
  u32 fd = Memory::Read_U32(request.buffer_in);

  request.Log(GetDeviceName(), Common::Log::IOS_WC24);

  sockaddr sa;
  socklen_t sa_len = sizeof(sa);
  const int ret = getsockname(WiiSockMan::GetInstance().GetHostSocket(fd), &sa, &sa_len);

  if (request.buffer_out_size < 2 + sizeof(sa.sa_data))
    WARN_LOG_FMT(IOS_NET, "IOCTL_SO_GETSOCKNAME output buffer is too small. Truncating");

  if (request.buffer_out_size > 0)
    Memory::Write_U8(request.buffer_out_size, request.buffer_out);
  if (request.buffer_out_size > 1)
    Memory::Write_U8(sa.sa_family & 0xFF, request.buffer_out + 1);
  if (request.buffer_out_size > 2)
  {
    Memory::CopyToEmu(request.buffer_out + 2, &sa.sa_data,
                      std::min<size_t>(sizeof(sa.sa_data), request.buffer_out_size - 2));
  }

  return GetDefaultReply(ret);
}

IPCCommandResult NetIPTop::HandleGetPeerNameRequest(const IOCtlRequest& request)
{
  u32 fd = Memory::Read_U32(request.buffer_in);

  sockaddr sa;
  socklen_t sa_len = sizeof(sa);
  const int ret = getpeername(WiiSockMan::GetInstance().GetHostSocket(fd), &sa, &sa_len);

  if (request.buffer_out_size < 2 + sizeof(sa.sa_data))
    WARN_LOG_FMT(IOS_NET, "IOCTL_SO_GETPEERNAME output buffer is too small. Truncating");

  if (request.buffer_out_size > 0)
    Memory::Write_U8(request.buffer_out_size, request.buffer_out);
  if (request.buffer_out_size > 1)
    Memory::Write_U8(AF_INET, request.buffer_out + 1);
  if (request.buffer_out_size > 2)
  {
    Memory::CopyToEmu(request.buffer_out + 2, &sa.sa_data,
                      std::min<size_t>(sizeof(sa.sa_data), request.buffer_out_size - 2));
  }

  INFO_LOG_FMT(IOS_NET, "IOCTL_SO_GETPEERNAME({:x})", fd);
  return GetDefaultReply(ret);
}

IPCCommandResult NetIPTop::HandleGetHostIDRequest(const IOCtlRequest& request)
{
  const DefaultInterface interface = GetSystemDefaultInterfaceOrFallback();
  const u32 host_ip = Common::swap32(interface.inet);
  INFO_LOG_FMT(IOS_NET, "IOCTL_SO_GETHOSTID = {}.{}.{}.{}", host_ip >> 24, (host_ip >> 16) & 0xFF,
               (host_ip >> 8) & 0xFF, host_ip & 0xFF);
  return GetDefaultReply(host_ip);
}

IPCCommandResult NetIPTop::HandleInetAToNRequest(const IOCtlRequest& request)
{
  const std::string hostname = Memory::GetString(request.buffer_in);
  struct hostent* remoteHost = gethostbyname(hostname.c_str());

  if (remoteHost == nullptr || remoteHost->h_addr_list == nullptr ||
      remoteHost->h_addr_list[0] == nullptr)
  {
    INFO_LOG_FMT(IOS_NET,
                 "IOCTL_SO_INETATON = -1 "
                 "{}, BufferIn: ({:08x}, {}), BufferOut: ({:08x}, {}), IP Found: None",
                 hostname, request.buffer_in, request.buffer_in_size, request.buffer_out,
                 request.buffer_out_size);

    return GetDefaultReply(0);
  }

  const auto ip = Common::swap32(reinterpret_cast<u8*>(remoteHost->h_addr_list[0]));
  Memory::Write_U32(ip, request.buffer_out);

  INFO_LOG_FMT(IOS_NET,
               "IOCTL_SO_INETATON = 0 "
               "{}, BufferIn: ({:08x}, {}), BufferOut: ({:08x}, {}), IP Found: {:08X}",
               hostname, request.buffer_in, request.buffer_in_size, request.buffer_out,
               request.buffer_out_size, ip);

  return GetDefaultReply(1);
}

IPCCommandResult NetIPTop::HandleInetPToNRequest(const IOCtlRequest& request)
{
  const std::string address = Memory::GetString(request.buffer_in);
  INFO_LOG_FMT(IOS_NET, "IOCTL_SO_INETPTON (Translating: {})", address);
  return GetDefaultReply(inet_pton(address.c_str(), Memory::GetPointer(request.buffer_out + 4)));
}

IPCCommandResult NetIPTop::HandleInetNToPRequest(const IOCtlRequest& request)
{
  // u32 af = Memory::Read_U32(BufferIn);
  // u32 validAddress = Memory::Read_U32(request.buffer_in + 4);
  // u32 src = Memory::Read_U32(request.buffer_in + 8);

  char ip_s[16];
  sprintf(ip_s, "%i.%i.%i.%i", Memory::Read_U8(request.buffer_in + 8),
          Memory::Read_U8(request.buffer_in + 8 + 1), Memory::Read_U8(request.buffer_in + 8 + 2),
          Memory::Read_U8(request.buffer_in + 8 + 3));

  INFO_LOG_FMT(IOS_NET, "IOCTL_SO_INETNTOP {}", ip_s);
  Memory::CopyToEmu(request.buffer_out, reinterpret_cast<u8*>(ip_s), std::strlen(ip_s));
  return GetDefaultReply(0);
}

IPCCommandResult NetIPTop::HandlePollRequest(const IOCtlRequest& request)
{
  WiiSockMan& sm = WiiSockMan::GetInstance();

  if (!request.buffer_in || !request.buffer_out)
    return GetDefaultReply(-SO_EINVAL);

  // Negative timeout indicates wait forever
  const s64 timeout = static_cast<s64>(Memory::Read_U64(request.buffer_in));

  const u32 nfds = request.buffer_out_size / 0xc;
  if (nfds == 0 || nfds > WII_SOCKET_FD_MAX)
  {
    ERROR_LOG_FMT(IOS_NET, "IOCTL_SO_POLL failed: Invalid array size {}, ret={}", nfds, -SO_EINVAL);
    return GetDefaultReply(-SO_EINVAL);
  }

  std::vector<pollfd_t> ufds(nfds);

  for (u32 i = 0; i < nfds; ++i)
  {
    const s32 wii_fd = Memory::Read_U32(request.buffer_out + 0xc * i);
    ufds[i].fd = sm.GetHostSocket(wii_fd);                                  // fd
    const int events = Memory::Read_U32(request.buffer_out + 0xc * i + 4);  // events
    ufds[i].revents = 0;

    // Translate Wii to native events
    ufds[i].events = WiiSockMan::ConvertEvents(events, WiiSockMan::ConvertDirection::WiiToNative);
    DEBUG_LOG_FMT(IOS_NET,
                  "IOCTL_SO_POLL({}) "
                  "Sock: {:08x}, Events: {:08x}, "
                  "NativeEvents: {:08x}",
                  i, wii_fd, events, ufds[i].events);

    // Do not pass return-only events to the native poll
    ufds[i].events &= ~(POLLERR | POLLHUP | POLLNVAL | UNSUPPORTED_WSAPOLL);
  }

  // Prevents blocking emulation on a blocking poll
  sm.AddPollCommand({request.address, request.buffer_out, std::move(ufds), timeout});
  return GetNoReply();
}

IPCCommandResult NetIPTop::HandleGetHostByNameRequest(const IOCtlRequest& request)
{
  if (request.buffer_out_size != 0x460)
  {
    ERROR_LOG_FMT(IOS_NET, "Bad buffer size for IOCTL_SO_GETHOSTBYNAME");
    return GetDefaultReply(-1);
  }

  const std::string hostname = Memory::GetString(request.buffer_in);
  hostent* remoteHost = gethostbyname(hostname.c_str());

  INFO_LOG_FMT(IOS_NET,
               "IOCTL_SO_GETHOSTBYNAME "
               "Address: {}, BufferIn: ({:08x}, {}), BufferOut: ({:08x}, {})",
               hostname, request.buffer_in, request.buffer_in_size, request.buffer_out,
               request.buffer_out_size);

  if (remoteHost == nullptr)
    return GetDefaultReply(-1);

  for (int i = 0; remoteHost->h_aliases[i]; ++i)
  {
    DEBUG_LOG_FMT(IOS_NET, "alias{}:{}", i, remoteHost->h_aliases[i]);
  }

  for (int i = 0; remoteHost->h_addr_list[i]; ++i)
  {
    const u32 ip = Common::swap32(*(u32*)(remoteHost->h_addr_list[i]));
    const std::string ip_s =
        fmt::format("{}.{}.{}.{}", ip >> 24, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff);
    DEBUG_LOG_FMT(IOS_NET, "addr{}:{}", i, ip_s);
  }

  // Host name; located immediately after struct
  static constexpr u32 GETHOSTBYNAME_STRUCT_SIZE = 0x10;
  static constexpr u32 GETHOSTBYNAME_IP_LIST_OFFSET = 0x110;
  // Limit host name length to avoid buffer overflow.
  const auto name_length = static_cast<u32>(strlen(remoteHost->h_name)) + 1;
  if (name_length > (GETHOSTBYNAME_IP_LIST_OFFSET - GETHOSTBYNAME_STRUCT_SIZE))
  {
    ERROR_LOG_FMT(IOS_NET, "Hostname too long in IOCTL_SO_GETHOSTBYNAME");
    return GetDefaultReply(-1);
  }
  Memory::CopyToEmu(request.buffer_out + GETHOSTBYNAME_STRUCT_SIZE, remoteHost->h_name,
                    name_length);
  Memory::Write_U32(request.buffer_out + GETHOSTBYNAME_STRUCT_SIZE, request.buffer_out);

  // IP address list; located at offset 0x110.
  u32 num_ip_addr = 0;
  while (remoteHost->h_addr_list[num_ip_addr])
    num_ip_addr++;
  // Limit number of IP addresses to avoid buffer overflow.
  // (0x460 - 0x340) / sizeof(pointer) == 72
  static const u32 GETHOSTBYNAME_MAX_ADDRESSES = 71;
  num_ip_addr = std::min(num_ip_addr, GETHOSTBYNAME_MAX_ADDRESSES);
  for (u32 i = 0; i < num_ip_addr; ++i)
  {
    u32 addr = request.buffer_out + GETHOSTBYNAME_IP_LIST_OFFSET + i * 4;
    Memory::Write_U32_Swap(*(u32*)(remoteHost->h_addr_list[i]), addr);
  }

  // List of pointers to IP addresses; located at offset 0x340.
  // This must be exact: PPC code to convert the struct hardcodes
  // this offset.
  static const u32 GETHOSTBYNAME_IP_PTR_LIST_OFFSET = 0x340;
  Memory::Write_U32(request.buffer_out + GETHOSTBYNAME_IP_PTR_LIST_OFFSET, request.buffer_out + 12);
  for (u32 i = 0; i < num_ip_addr; ++i)
  {
    u32 addr = request.buffer_out + GETHOSTBYNAME_IP_PTR_LIST_OFFSET + i * 4;
    Memory::Write_U32(request.buffer_out + GETHOSTBYNAME_IP_LIST_OFFSET + i * 4, addr);
  }
  Memory::Write_U32(0, request.buffer_out + GETHOSTBYNAME_IP_PTR_LIST_OFFSET + num_ip_addr * 4);

  // Aliases - empty. (Hardware doesn't return anything.)
  Memory::Write_U32(request.buffer_out + GETHOSTBYNAME_IP_PTR_LIST_OFFSET + num_ip_addr * 4,
                    request.buffer_out + 4);

  // Returned struct must be ipv4.
  ASSERT_MSG(IOS_NET, remoteHost->h_addrtype == AF_INET && remoteHost->h_length == sizeof(u32),
             "returned host info is not IPv4");
  Memory::Write_U16(AF_INET, request.buffer_out + 8);
  Memory::Write_U16(sizeof(u32), request.buffer_out + 10);

  return GetDefaultReply(0);
}

IPCCommandResult NetIPTop::HandleICMPCancelRequest(const IOCtlRequest& request)
{
  ERROR_LOG_FMT(IOS_NET, "IOCTL_SO_ICMPCANCEL");
  return GetDefaultReply(0);
}

IPCCommandResult NetIPTop::HandleGetInterfaceOptRequest(const IOCtlVRequest& request)
{
  const u32 param = Memory::Read_U32(request.in_vectors[0].address);
  const u32 param2 = Memory::Read_U32(request.in_vectors[0].address + 4);
  const u32 param3 = Memory::Read_U32(request.io_vectors[0].address);
  const u32 param4 = Memory::Read_U32(request.io_vectors[1].address);
  u32 param5 = 0;

  if (param != 0xfffe)
  {
    WARN_LOG_FMT(IOS_NET, "GetInterfaceOpt: received invalid request with param0={:08x}", param);
    return GetDefaultReply(SO_ERROR_INVALID_REQUEST);
  }

  if (request.io_vectors[0].size >= 8)
  {
    param5 = Memory::Read_U32(request.io_vectors[0].address + 4);
  }

  INFO_LOG_FMT(IOS_NET,
               "IOCTLV_SO_GETINTERFACEOPT({:08X}, {:08X}, {:X}, {:X}, {:X}) "
               "BufferIn: ({:08x}, {}), BufferIn2: ({:08x}, {}) ",
               param, param2, param3, param4, param5, request.in_vectors[0].address,
               request.in_vectors[0].size,
               request.in_vectors.size() > 1 ? request.in_vectors[1].address : 0,
               request.in_vectors.size() > 1 ? request.in_vectors[1].size : 0);

  switch (param2)
  {
  case 0xb003:  // dns server table
  {
    const u32 default_main_dns_resolver = ntohl(::inet_addr("8.8.8.8"));
    const u32 default_backup_dns_resolver = ntohl(::inet_addr("8.8.4.4"));
    u32 address = 0;
#ifdef _WIN32
    if (!Core::WantsDeterminism())
    {
      PIP_ADAPTER_ADDRESSES AdapterAddresses = nullptr;
      ULONG OutBufferLength = 0;
      ULONG RetVal = 0, i;
      for (i = 0; i < 5; ++i)
      {
        RetVal = GetAdaptersAddresses(AF_INET, 0, nullptr, AdapterAddresses, &OutBufferLength);

        if (RetVal != ERROR_BUFFER_OVERFLOW)
        {
          break;
        }

        if (AdapterAddresses != nullptr)
        {
          FREE(AdapterAddresses);
        }

        AdapterAddresses = (PIP_ADAPTER_ADDRESSES)MALLOC(OutBufferLength);
        if (AdapterAddresses == nullptr)
        {
          RetVal = GetLastError();
          break;
        }
      }
      if (RetVal == NO_ERROR)
      {
        unsigned long dwBestIfIndex = 0;
        IPAddr dwDestAddr = static_cast<IPAddr>(default_main_dns_resolver);
        // If successful, output some information from the data we received
        PIP_ADAPTER_ADDRESSES AdapterList = AdapterAddresses;
        if (GetBestInterface(dwDestAddr, &dwBestIfIndex) == NO_ERROR)
        {
          while (AdapterList)
          {
            if (AdapterList->IfIndex == dwBestIfIndex && AdapterList->FirstDnsServerAddress &&
                AdapterList->OperStatus == IfOperStatusUp)
            {
              INFO_LOG_FMT(IOS_NET, "Name of valid interface: {}",
                           WStringToUTF8(AdapterList->FriendlyName));
              INFO_LOG_FMT(IOS_NET, "DNS: {}.{}.{}.{}",
                           u8(AdapterList->FirstDnsServerAddress->Address.lpSockaddr->sa_data[2]),
                           u8(AdapterList->FirstDnsServerAddress->Address.lpSockaddr->sa_data[3]),
                           u8(AdapterList->FirstDnsServerAddress->Address.lpSockaddr->sa_data[4]),
                           u8(AdapterList->FirstDnsServerAddress->Address.lpSockaddr->sa_data[5]));
              address = Common::swap32(
                  *(u32*)(&AdapterList->FirstDnsServerAddress->Address.lpSockaddr->sa_data[2]));
              break;
            }
            AdapterList = AdapterList->Next;
          }
        }
      }
      if (AdapterAddresses != nullptr)
      {
        FREE(AdapterAddresses);
      }
    }
#elif defined(__linux__) && !defined(__ANDROID__)
    if (!Core::WantsDeterminism())
    {
      if (res_init() == 0)
        address = ntohl(_res.nsaddr_list[0].sin_addr.s_addr);
      else
        WARN_LOG_FMT(IOS_NET, "Call to res_init failed");
    }
#endif
    if (address == 0)
      address = default_main_dns_resolver;

    INFO_LOG_FMT(IOS_NET, "Primary DNS: {:X}", address);
    INFO_LOG_FMT(IOS_NET, "Secondary DNS: {:X}", default_backup_dns_resolver);

    Memory::Write_U32(address, request.io_vectors[0].address);
    Memory::Write_U32(default_backup_dns_resolver, request.io_vectors[0].address + 4);
    break;
  }
  case 0x1003:  // error
    Memory::Write_U32(0, request.io_vectors[0].address);
    break;

  case 0x1004:  // mac address
  {
    const Common::MACAddress address = IOS::Net::GetMACAddress();
    Memory::CopyToEmu(request.io_vectors[0].address, address.data(), address.size());
    break;
  }

  case 0x1005:  // link state
    Memory::Write_U32(1, request.io_vectors[0].address);
    break;

  case 0x3001:  // hardcoded value
    Memory::Write_U32(0x10, request.io_vectors[0].address);
    break;

  case 0x4002:  // ip addr numberHandle
    Memory::Write_U32(1, request.io_vectors[0].address);
    break;

  case 0x4003:  // ip addr table
  {
    // XXX: this isn't exactly right; the buffer can be larger than 12 bytes, in which case
    // SO can write 12 more bytes.
    Memory::Write_U32(0xC, request.io_vectors[1].address);
    const DefaultInterface interface = GetSystemDefaultInterfaceOrFallback();
    Memory::Write_U32(Common::swap32(interface.inet), request.io_vectors[0].address);
    Memory::Write_U32(Common::swap32(interface.netmask), request.io_vectors[0].address + 4);
    Memory::Write_U32(Common::swap32(interface.broadcast), request.io_vectors[0].address + 8);
    break;
  }

  case 0x4005:  // hardcoded value
    Memory::Write_U32(0x20, request.io_vectors[0].address);
    break;

  case 0x6003:  // hardcoded value
    Memory::Write_U32(0x80, request.io_vectors[0].address);
    break;

  case 0x600a:  // hardcoded value
    Memory::Write_U32(0x80, request.io_vectors[0].address);
    break;

  case 0x600c:  // hardcoded value
    Memory::Write_U32(0x80, request.io_vectors[0].address);
    break;

  case 0xb002:  // hardcoded value
    Memory::Write_U32(2, request.io_vectors[0].address);
    break;

  default:
    ERROR_LOG_FMT(IOS_NET, "Unknown param2: {:08X}", param2);
    break;
  }

  return GetDefaultReply(0);
}

IPCCommandResult NetIPTop::HandleSendToRequest(const IOCtlVRequest& request)
{
  u32 fd = Memory::Read_U32(request.in_vectors[1].address);
  WiiSockMan& sm = WiiSockMan::GetInstance();
  sm.DoSock(fd, request, IOCTLV_SO_SENDTO);
  return GetNoReply();
}

IPCCommandResult NetIPTop::HandleRecvFromRequest(const IOCtlVRequest& request)
{
  u32 fd = Memory::Read_U32(request.in_vectors[0].address);
  WiiSockMan& sm = WiiSockMan::GetInstance();
  sm.DoSock(fd, request, IOCTLV_SO_RECVFROM);
  return GetNoReply();
}

IPCCommandResult NetIPTop::HandleGetAddressInfoRequest(const IOCtlVRequest& request)
{
  addrinfo hints;
  const bool hints_valid = request.in_vectors.size() > 2 && request.in_vectors[2].size;

  if (hints_valid)
  {
    hints.ai_flags = Memory::Read_U32(request.in_vectors[2].address);
    hints.ai_family = Memory::Read_U32(request.in_vectors[2].address + 0x4);
    hints.ai_socktype = Memory::Read_U32(request.in_vectors[2].address + 0x8);
    hints.ai_protocol = Memory::Read_U32(request.in_vectors[2].address + 0xC);
    hints.ai_addrlen = Memory::Read_U32(request.in_vectors[2].address + 0x10);
    hints.ai_canonname = nullptr;
    hints.ai_addr = nullptr;
    hints.ai_next = nullptr;
  }

  // getaddrinfo allows a null pointer for the nodeName or serviceName strings
  // So we have to do a bit of juggling here.
  std::string nodeNameStr;
  const char* pNodeName = nullptr;
  if (!request.in_vectors.empty() && request.in_vectors[0].size > 0)
  {
    nodeNameStr = Memory::GetString(request.in_vectors[0].address, request.in_vectors[0].size);
    pNodeName = nodeNameStr.c_str();
  }

  std::string serviceNameStr;
  const char* pServiceName = nullptr;
  if (request.in_vectors.size() > 1 && request.in_vectors[1].size > 0)
  {
    serviceNameStr = Memory::GetString(request.in_vectors[1].address, request.in_vectors[1].size);
    pServiceName = serviceNameStr.c_str();
  }

  addrinfo* result = nullptr;
  int ret = getaddrinfo(pNodeName, pServiceName, hints_valid ? &hints : nullptr, &result);
  u32 addr = request.io_vectors[0].address;
  u32 sockoffset = addr + 0x460;
  if (ret == 0)
  {
    constexpr size_t WII_ADDR_INFO_SIZE = 0x20;
    for (addrinfo* result_iter = result; result_iter != nullptr; result_iter = result_iter->ai_next)
    {
      Memory::Write_U32(result_iter->ai_flags, addr);
      Memory::Write_U32(result_iter->ai_family, addr + 0x04);
      Memory::Write_U32(result_iter->ai_socktype, addr + 0x08);
      Memory::Write_U32(result_iter->ai_protocol, addr + 0x0C);
      Memory::Write_U32((u32)result_iter->ai_addrlen, addr + 0x10);
      // what to do? where to put? the buffer of 0x834 doesn't allow space for this
      Memory::Write_U32(/*result->ai_cannonname*/ 0, addr + 0x14);

      if (result_iter->ai_addr)
      {
        Memory::Write_U32(sockoffset, addr + 0x18);
        Memory::Write_U8(result_iter->ai_addrlen & 0xFF, sockoffset);
        Memory::Write_U8(result_iter->ai_addr->sa_family & 0xFF, sockoffset + 0x01);
        Memory::CopyToEmu(sockoffset + 0x2, result_iter->ai_addr->sa_data,
                          sizeof(result_iter->ai_addr->sa_data));
        sockoffset += 0x1C;
      }
      else
      {
        Memory::Write_U32(0, addr + 0x18);
      }

      if (result_iter->ai_next)
      {
        Memory::Write_U32(addr + WII_ADDR_INFO_SIZE, addr + 0x1C);
      }
      else
      {
        Memory::Write_U32(0, addr + 0x1C);
      }

      addr += WII_ADDR_INFO_SIZE;
    }

    freeaddrinfo(result);
  }
  else
  {
    ret = SO_ERROR_HOST_NOT_FOUND;
  }

  request.Dump(GetDeviceName(), Common::Log::IOS_NET, Common::Log::LINFO);
  return GetDefaultReply(ret);
}

IPCCommandResult NetIPTop::HandleICMPPingRequest(const IOCtlVRequest& request)
{
  struct
  {
    u8 length;
    u8 addr_family;
    u16 icmp_id;
    u32 ip;
  } ip_info;

  const u32 fd = Memory::Read_U32(request.in_vectors[0].address);
  const u32 num_ip = Memory::Read_U32(request.in_vectors[0].address + 4);
  const u64 timeout = Memory::Read_U64(request.in_vectors[0].address + 8);

  if (num_ip != 1)
  {
    INFO_LOG_FMT(IOS_NET, "IOCTLV_SO_ICMPPING {} IPs", num_ip);
  }

  ip_info.length = Memory::Read_U8(request.in_vectors[0].address + 16);
  ip_info.addr_family = Memory::Read_U8(request.in_vectors[0].address + 17);
  ip_info.icmp_id = Memory::Read_U16(request.in_vectors[0].address + 18);
  ip_info.ip = Memory::Read_U32(request.in_vectors[0].address + 20);

  if (ip_info.length != 8 || ip_info.addr_family != AF_INET)
  {
    INFO_LOG_FMT(IOS_NET,
                 "IOCTLV_SO_ICMPPING strange IPInfo:\n"
                 "length {:x} addr_family {:x}",
                 ip_info.length, ip_info.addr_family);
  }

  INFO_LOG_FMT(IOS_NET, "IOCTLV_SO_ICMPPING {:x}", ip_info.ip);

  sockaddr_in addr;
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = Common::swap32(ip_info.ip);
  memset(addr.sin_zero, 0, 8);

  u8 data[0x20];
  memset(data, 0, sizeof(data));
  s32 icmp_length = sizeof(data);

  if (request.in_vectors.size() > 1 && request.in_vectors[1].size == sizeof(data))
  {
    Memory::CopyFromEmu(data, request.in_vectors[1].address, request.in_vectors[1].size);
  }
  else
  {
    // TODO sequence number is incremented either statically, by
    // port, or by socket. Doesn't seem to matter, so we just leave
    // it 0
    ((u16*)data)[0] = Common::swap16(ip_info.icmp_id);
    icmp_length = 22;
  }

  int ret = icmp_echo_req(WiiSockMan::GetInstance().GetHostSocket(fd), &addr, data, icmp_length);
  if (ret == icmp_length)
  {
    ret = icmp_echo_rep(WiiSockMan::GetInstance().GetHostSocket(fd), &addr,
                        static_cast<u32>(timeout), icmp_length);
  }

  // TODO proper error codes
  return GetDefaultReply(0);
}
}  // namespace IOS::HLE::Device
