/****************************************************************************
 *  Genesis Plus EX -- Win32 GUI frontend
 *
 *  upnp.c -- port forwarding through UPnP (Internet Gateway Device).
 *
 *  The router is found with an SSDP search on the local network, its
 *  description is read over HTTP, and the port is opened (and later closed)
 *  with the SOAP actions AddPortMapping and DeletePortMapping. Plain Winsock,
 *  no COM. Everything runs on a worker thread with short time-outs; failing
 *  is harmless: the player is told to forward the port by hand.
 ****************************************************************************/

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "upnp.h"

extern void gui_log(const char *fmt, ...);

static struct
{
  volatile LONG state;
  volatile LONG busy;             /* a worker thread is running */
  HANDLE        thread;
  int           port;
  int           mapped;
  char          ext_ip[48];
  char          msg[400];
  char          host[48];         /* the router, from its LOCATION header */
  int           host_port;
  char          control[256];     /* path of the control URL */
  char          service[96];      /* the service type to put in SOAPAction */
  char          local_ip[48];
} up;

/* A connected TCP socket to host:port, or INVALID_SOCKET; gives up after about three seconds. */
static SOCKET tcp_connect(const char *host, int port)
{
  struct addrinfo hints, *res = NULL;
  char ps[16];
  SOCKET s;
  u_long nb = 1;
  fd_set wf, ef;
  struct timeval tv;
  int r, so = 0, sl = sizeof(so);

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  wsprintfA(ps, "%d", port);
  if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return INVALID_SOCKET;
  s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (s == INVALID_SOCKET) { freeaddrinfo(res); return INVALID_SOCKET; }
  ioctlsocket(s, FIONBIO, &nb);
  r = connect(s, res->ai_addr, (int)res->ai_addrlen);
  freeaddrinfo(res);
  if (r == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK)
  {
    FD_ZERO(&wf); FD_ZERO(&ef); FD_SET(s, &wf); FD_SET(s, &ef);
    tv.tv_sec = 3; tv.tv_usec = 0;
    r = select(0, NULL, &wf, &ef, &tv);
    getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&so, &sl);
    if (r <= 0 || so != 0 || FD_ISSET(s, &ef)) { closesocket(s); return INVALID_SOCKET; }
  }
  else if (r == SOCKET_ERROR) { closesocket(s); return INVALID_SOCKET; }
  nb = 0;
  ioctlsocket(s, FIONBIO, &nb);
  { DWORD t = 3000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t)); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof(t)); }
  return s;
}

/* Sends `req` and reads the whole answer (the connection is closed after it). Returns the length read. */
static int http_exchange(const char *host, int port, const char *req, char *out, int out_len, char *local_ip, int local_len)
{
  SOCKET s = tcp_connect(host, port);
  int total = 0, n;

  if (s == INVALID_SOCKET) return -1;
  if (local_ip)
  {
    struct sockaddr_in me;
    int ml = sizeof(me);
    if (getsockname(s, (struct sockaddr *)&me, &ml) == 0) lstrcpynA(local_ip, inet_ntoa(me.sin_addr), local_len);
  }
  if (send(s, req, (int)strlen(req), 0) <= 0) { closesocket(s); return -1; }
  while (total < out_len - 1 && (n = recv(s, out + total, out_len - 1 - total, 0)) > 0) total += n;
  closesocket(s);
  out[total] = '\0';
  return total;
}

/* The text between <tag> and </tag> after position `from`, or NULL. */
static const char *xml_value(const char *from, const char *tag, char *out, int out_len)
{
  char open[64], close[64];
  const char *a, *b;
  int n;

  wsprintfA(open, "<%s>", tag);
  wsprintfA(close, "</%s>", tag);
  a = strstr(from, open);
  if (!a) return NULL;
  a += strlen(open);
  b = strstr(a, close);
  if (!b) return NULL;
  n = (int)(b - a);
  if (n >= out_len) n = out_len - 1;
  memcpy(out, a, (size_t)n);
  out[n] = '\0';
  return b + strlen(close);
}

/* Looks for a router on the network and reads where its port-forwarding service is. */
static int find_router(void)
{
  static const char *const targets[] =
  {
    "urn:schemas-upnp-org:device:InternetGatewayDevice:1",
    "urn:schemas-upnp-org:service:WANIPConnection:1",
    "urn:schemas-upnp-org:service:WANPPPConnection:1"
  };
  SOCKET s;
  struct sockaddr_in to;
  char query[400], reply[1400], location[300] = "";
  int t;
  DWORD tmo = 1200;

  s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == INVALID_SOCKET) return 0;
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof(tmo));
  memset(&to, 0, sizeof(to));
  to.sin_family = AF_INET;
  to.sin_port = htons(1900);
  to.sin_addr.s_addr = inet_addr("239.255.255.250");

  for (t = 0; t < 3 && !location[0]; t++)
  {
    int n, tries;
    wsprintfA(query, "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: %s\r\n\r\n", targets[t]);
    sendto(s, query, (int)strlen(query), 0, (struct sockaddr *)&to, sizeof(to));
    for (tries = 0; tries < 3 && !location[0]; tries++)
    {
      char *p;
      n = recv(s, reply, sizeof(reply) - 1, 0);
      if (n <= 0) break;
      reply[n] = '\0';
      p = strstr(reply, "LOCATION:");
      if (!p) p = strstr(reply, "Location:");
      if (!p) p = strstr(reply, "location:");
      if (p)
      {
        int k = 0;
        p += 9;
        while (*p == ' ') p++;
        while (*p && *p != '\r' && *p != '\n' && k < (int)sizeof(location) - 1) location[k++] = *p++;
        location[k] = '\0';
      }
    }
  }
  closesocket(s);
  if (strncmp(location, "http://", 7) != 0) return 0;

  /* http://host:port/path */
  {
    char hostport[300], path[300], *colon, *slash;
    char *page, root[700];
    const char *svc, *after;
    static const char *const types[] =
    {
      "urn:schemas-upnp-org:service:WANIPConnection:1", "urn:schemas-upnp-org:service:WANIPConnection:2",
      "urn:schemas-upnp-org:service:WANPPPConnection:1"
    };
    int i, got;

    lstrcpynA(hostport, location + 7, sizeof(hostport));      /* host:port/path -- the path can be long */
    slash = strchr(hostport, '/');
    lstrcpynA(path, slash ? slash : "/", sizeof(path));
    if (slash) *slash = '\0';
    colon = strchr(hostport, ':');
    up.host_port = 80;
    if (colon) { up.host_port = atoi(colon + 1); *colon = '\0'; }
    lstrcpynA(up.host, hostport, sizeof(up.host));

    page = (char *)malloc(65536);
    if (!page) return 0;
    wsprintfA(root, "GET %s HTTP/1.0\r\nHOST: %s:%d\r\nConnection: close\r\n\r\n", path, up.host, up.host_port);
    got = http_exchange(up.host, up.host_port, root, page, 65536, up.local_ip, sizeof(up.local_ip));
    if (got <= 0) { free(page); return 0; }

    svc = NULL;
    for (i = 0; i < 3 && !svc; i++)
    {
      char *p = strstr(page, types[i]);
      if (p) { svc = p; lstrcpynA(up.service, types[i], sizeof(up.service)); }
    }
    if (!svc) { free(page); return 0; }
    after = svc;
    if (!xml_value(after, "controlURL", up.control, sizeof(up.control))) { free(page); return 0; }
    if (strncmp(up.control, "http://", 7) == 0)         /* an absolute address: keep only its path */
    {
      char *p = strchr(up.control + 7, '/'), keep[256];
      lstrcpynA(keep, p ? p : "/", sizeof(keep));          /* a copy: source and destination share the buffer */
      lstrcpyA(up.control, keep);
    }
    else if (up.control[0] != '/')
    {
      char tmp[256];
      wsprintfA(tmp, "/%s", up.control);
      lstrcpynA(up.control, tmp, sizeof(up.control));
    }
    free(page);
  }
  return 1;
}

/* One SOAP action. Returns the HTTP status (200 = done), or 0 when the router could not be reached. `answer` may be NULL. */
static int soap(const char *action, const char *args, char *answer, int answer_len)
{
  char body[900], head[700], *reply;
  int n, code = 0;

  wsprintfA(body, "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                  "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>",
            action, up.service, args, action);
  wsprintfA(head, "POST %s HTTP/1.0\r\nHOST: %s:%d\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nContent-Length: %d\r\n"
                  "SOAPAction: \"%s#%s\"\r\nConnection: close\r\n\r\n",
            up.control, up.host, up.host_port, (int)strlen(body), up.service, action);

  reply = (char *)malloc(8192);
  if (!reply) return 0;
  {
    char *req = (char *)malloc(strlen(head) + strlen(body) + 1);
    if (!req) { free(reply); return 0; }
    lstrcpyA(req, head);
    lstrcatA(req, body);
    n = http_exchange(up.host, up.host_port, req, reply, 8192, NULL, 0);
    free(req);
  }
  if (n > 12 && strncmp(reply, "HTTP/", 5) == 0) code = atoi(reply + 9);
  if (answer && answer_len > 0) { lstrcpynA(answer, n > 0 ? reply : "", answer_len); }
  free(reply);
  return code;
}

/* 0 for an address that is not on the internet: private ranges, the shared 100.64.0.0/10 of providers, 0.0.0.0. */
static int up_public(const char *ip)
{
  unsigned a = 0, b = 0, c = 0, d = 0;

  if (sscanf(ip, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return 1;
  if (a == 0 || a == 10 || a == 127) return 0;
  if (a == 172 && b >= 16 && b <= 31) return 0;
  if (a == 192 && b == 168) return 0;
  if (a == 169 && b == 254) return 0;
  if (a == 100 && b >= 64 && b <= 127) return 0;
  return 1;
}

static DWORD WINAPI open_thread(LPVOID unused)
{
  WSADATA d;
  char args[600], ans[2048];
  int code;
  (void)unused;

  WSAStartup(MAKEWORD(2, 2), &d);
  lstrcpyA(up.msg, "Looking for the router...");

  if (!find_router())
  {
    lstrcpyA(up.msg, "No router answered (UPnP may be switched off on it). Forward the port by hand if friends cannot join.");
    gui_log("UPnP: %s", up.msg);
    InterlockedExchange(&up.state, UPNP_FAILED);
    goto done;
  }
  gui_log("UPnP: router at %s:%d, control %s, this PC %s", up.host, up.host_port, up.control, up.local_ip);

  wsprintfA(args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>TCP</NewProtocol>"
                  "<NewInternalPort>%d</NewInternalPort><NewInternalClient>%s</NewInternalClient><NewEnabled>1</NewEnabled>"
                  "<NewPortMappingDescription>Genesis Plus EX netplay</NewPortMappingDescription><NewLeaseDuration>0</NewLeaseDuration>",
            up.port, up.port, up.local_ip);
  code = soap("AddPortMapping", args, ans, sizeof(ans));
  if (code != 200)                       /* some routers only take timed leases */
  {
    char *p = strstr(args, "<NewLeaseDuration>0</NewLeaseDuration>");
    if (p) { memcpy(p, "<NewLeaseDuration>7200", 22); lstrcpyA(p + 22, "</NewLeaseDuration>"); }
    code = soap("AddPortMapping", args, ans, sizeof(ans));
  }
  if (code != 200)                       /* an old entry for this port (left by a crash, or pointing at another PC) can be in the way */
  {
    char del[200];
    wsprintfA(del, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>TCP</NewProtocol>", up.port);
    soap("DeletePortMapping", del, NULL, 0);
    code = soap("AddPortMapping", args, ans, sizeof(ans));
  }
  if (code != 200)
  {
    wsprintfA(up.msg, "The router refused to open port %d (answer %d). Forward it by hand if friends cannot join.", up.port, code);
    gui_log("UPnP: %s", up.msg);
    InterlockedExchange(&up.state, UPNP_FAILED);
    goto done;
  }
  up.mapped = 1;

  up.ext_ip[0] = '\0';
  if (soap("GetExternalIPAddress", "", ans, sizeof(ans)) == 200)
    xml_value(ans, "NewExternalIPAddress", up.ext_ip, sizeof(up.ext_ip));
  if (up.ext_ip[0] && !up_public(up.ext_ip))
    wsprintfA(up.msg, "Router port %d opened, but the router's own address (%s) is not a public one: friends outside your network cannot reach it "
                      "(the internet provider shares one address between customers, or another router sits in front). Friends on your own network can join.", up.port, up.ext_ip);
  else if (up.ext_ip[0]) wsprintfA(up.msg, "Router port %d opened. Friends join %s, port %d.", up.port, up.ext_ip, up.port);
  else              wsprintfA(up.msg, "Router port %d opened.", up.port);
  gui_log("UPnP: %s", up.msg);
  InterlockedExchange(&up.state, UPNP_OPEN);

done:
  WSACleanup();
  InterlockedExchange(&up.busy, 0);
  return 0;
}

static DWORD WINAPI close_thread(LPVOID unused)
{
  WSADATA d;
  char args[200];
  (void)unused;

  WSAStartup(MAKEWORD(2, 2), &d);
  wsprintfA(args, "<NewRemoteHost></NewRemoteHost><NewExternalPort>%d</NewExternalPort><NewProtocol>TCP</NewProtocol>", up.port);
  soap("DeletePortMapping", args, NULL, 0);
  gui_log("UPnP: router port %d closed again", up.port);
  up.mapped = 0;
  WSACleanup();
  InterlockedExchange(&up.busy, 0);
  return 0;
}

void upnp_open(int port)
{
  if (up.busy) return;
  memset(up.ext_ip, 0, sizeof(up.ext_ip));
  up.port = port;
  up.mapped = 0;
  InterlockedExchange(&up.state, UPNP_WORKING);
  InterlockedExchange(&up.busy, 1);
  lstrcpyA(up.msg, "Opening the router's port...");
  if (up.thread) { CloseHandle(up.thread); up.thread = NULL; }
  up.thread = CreateThread(NULL, 0, open_thread, NULL, 0, NULL);
  if (!up.thread) { InterlockedExchange(&up.busy, 0); InterlockedExchange(&up.state, UPNP_FAILED); lstrcpyA(up.msg, "Could not start the router search."); }
}

/* Takes the forwarding away (on a thread of its own; waits for it for a moment when the program is closing). */
void upnp_close(void)
{
  if (up.thread && up.busy) WaitForSingleObject(up.thread, 4000);   /* let the opening finish first */
  if (up.thread) { CloseHandle(up.thread); up.thread = NULL; }
  if (up.mapped && !up.busy)
  {
    InterlockedExchange(&up.busy, 1);
    up.thread = CreateThread(NULL, 0, close_thread, NULL, 0, NULL);
    if (!up.thread) InterlockedExchange(&up.busy, 0);
  }
  InterlockedExchange(&up.state, UPNP_OFF);
  up.ext_ip[0] = '\0';
  up.msg[0] = '\0';
}

int upnp_state(void) { return (int)up.state; }
const char *upnp_external_ip(void) { return up.ext_ip; }
const char *upnp_message(void) { return up.msg; }
