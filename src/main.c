/* hp-supplies — HP printer consumables & usage monitor (LEDM over USB)
 *
 * Independent re-implementation (protocol reverse-engineered from HP's
 * Windows "HP ColorLaserJet M153-M154.exe" Printer Assistant, UDCDevicePage).
 *
 * Protocol summary (verified against real hardware):
 *   Modern HP printers expose a USB vendor interface ("HP LEDM",
 *   class 0xFF / subclass 0x04 / protocol 0x01) that is an HTTP tunnel:
 *   write a plain HTTP/1.1 GET to the bulk-OUT pipe, read the response from the
 *   bulk-IN pipe. The built-in LEDM web server serves device management XML:
 *     /DevMgmt/ConsumableConfigDyn.xml  — toner levels / state / cartridge info
 *     /DevMgmt/ProductUsageDyn.xml      — total printed pages, per-cartridge usage
 *     /DevMgmt/ProductStatusDyn.xml     — machine state + active alerts (jam,
 *                                         door open, printer error, toner…)
 *   A "Host: localhost" header is required; "Connection: close" prevents the
 *   firmware from queueing keep-alive responses across sessions. The firmware
 *   may replay stale queued responses after abrupt sessions, so every response
 *   is validated against its expected root element and retried.
 *
 * GUI: GTK3. Also provides --text mode and --host (network EWS) mode.
 *
 * Build:  gcc -O2 -o hp-supplies src/main.c \
 *           $(pkg-config --cflags --libs gtk+-3.0 libusb-1.0) -lpthread
 * License: MIT
 */

#define _GNU_SOURCE

#include <gtk/gtk.h>
#include <libusb-1.0/libusb.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <ctype.h>
#include <errno.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef _WIN32
/* ---- Windows compatibility shims (MinGW-w64 / UCRT) ---- */
#define usleep(us) Sleep((DWORD)((us) / 1000))
#define strncasecmp _strnicmp

/* msvcrt lacks strcasestr */
static char *strcasestr(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    for (; *hay; hay++)
        if (strncasecmp(hay, needle, nl) == 0) return (char *)hay;
    return NULL;
}

/* UTF-8 console output for the Chinese CLI text */
static void console_utf8(void)
{
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);
}
#else
static void console_utf8(void) {}
#endif

#ifndef APP_VERSION
#define APP_VERSION "1.0.0"
#endif
#define APP_NAME    "HP 打印机耗材监控"
#define DEFAULT_VID 0x03F0
#define DEFAULT_PID 0xAE2A

#define MAX_SUPPLIES 8
#define MAX_ALERTS   8
#define DRAIN_TIMEOUT_MS 250
#define RX_TIMEOUT_MS    4000
#define RX_DEADLINE_S    12

static const char CRLF4[] = "\r\n\r\n";   /* HTTP header terminator */

/* ---------------------------------------------------------------- data */

typedef struct {
    char label[8];        /* K / C / M / Y */
    char family[64];      /* "Black toner supply" */
    char product[32];     /* CF510A */
    char serial[32];
    char state[24];       /* ok / used / new */
    char qty[24];         /* ok / low / veryLow */
    char brand[16];       /* HP / clone */
    char installed[16];   /* 2026-03-09 */
    int  level;           /* percent remaining, -1 unknown */
    int  yield;           /* rated pages, -1 unknown */
    int  station;         /* -1 unknown */
    int  printed;         /* pages printed by this cartridge, -1 */
    int  est_remaining;   /* estimated pages remaining, -1 */
    int  raw_level;       /* raw percent, -1 */
} Supply;

typedef struct {
    char id[64];          /* ProductStatusAlertID, e.g. cartridgeVeryLow */
    char severity[24];    /* Info / Warning / StrictWarning / Error */
    char color[24];       /* AlertDetailsMarkerColor, may be empty */
    char loc[160];        /* firmware LocString (zh-CN) if present */
} Alert;

typedef struct {
    char model[80];
    char serial[40];
    long total, mono, color;
    int  jams, mispicks;
    int  nsupplies;
    Supply supplies[MAX_SUPPLIES];
    char status_cat[48];  /* pscat:StatusCategory, e.g. inPowerSave / DoorOpen */
    char status_loc[160]; /* firmware LocString (zh-CN) if present */
    int  nalerts;
    Alert alerts[MAX_ALERTS];
    char error[320];
} Snapshot;

/* ---------------------------------------------------------------- usb ledm */

typedef struct {
    libusb_device_handle *h;
    uint8_t ep_out, ep_in;
    uint8_t iface;
    libusb_context *ctx;
} Ledm;

static int iface_is_ledm(const struct libusb_interface_descriptor *d)
{
    return d->bInterfaceClass == 0xFF && d->bInterfaceSubClass == 0x04 &&
           d->bInterfaceProtocol == 0x01 && d->bNumEndpoints >= 2;
}

static int scan_ledm_ifaces(libusb_device *dev, uint8_t *ifnum,
                            uint8_t *epout, uint8_t *epin, int max)
{
    struct libusb_config_descriptor *cfg;
    if (libusb_get_active_config_descriptor(dev, &cfg) != 0)
        return 0;
    int n = 0;
    for (int i = 0; i < (int)cfg->bNumInterfaces && n < max; i++) {
        const struct libusb_interface *itf = &cfg->interface[i];
        for (int a = 0; a < itf->num_altsetting; a++) {
            const struct libusb_interface_descriptor *d = &itf->altsetting[a];
            if (!iface_is_ledm(d))
                continue;
            uint8_t eo = 0, ei = 0;
            for (int e = 0; e < (int)d->bNumEndpoints; e++) {
                const struct libusb_endpoint_descriptor *ep = &d->endpoint[e];
                if ((ep->bmAttributes & 3) != LIBUSB_TRANSFER_TYPE_BULK)
                    continue;
                if (ep->bEndpointAddress & 0x80) { if (!ei) ei = ep->bEndpointAddress; }
                else                             { if (!eo) eo = ep->bEndpointAddress; }
            }
            if (eo && ei) {
                ifnum[n] = d->bInterfaceNumber;
                epout[n] = eo;
                epin[n]  = ei;
                n++;
            }
            break;
        }
    }
    libusb_free_config_descriptor(cfg);
    return n;
}

/* returns 0 ok, -2 device absent (retryable), -1 other error */
static int leedm_open_once(Ledm *L, uint16_t vid, uint16_t pid, char *err, size_t errn)
{
    memset(L, 0, sizeof *L);
    libusb_device **list = NULL;
    ssize_t cnt = libusb_get_device_list(L->ctx, &list);
    if (cnt < 0) {
        snprintf(err, errn, "libusb_get_device_list 失败 (%d)", (int)cnt);
        return -1;
    }

    /* pass 0: exact VID/PID; pass 1: any device with an LEDM iface */
    libusb_device *chosen = NULL;
    uint8_t pick_iface = 0, pick_out = 0, pick_in = 0;
    for (int pass = 0; pass < 2 && !chosen; pass++) {
        for (ssize_t i = 0; i < cnt && !chosen; i++) {
            struct libusb_device_descriptor dd;
            if (libusb_get_device_descriptor(list[i], &dd) != 0)
                continue;
            if (dd.idVendor != vid)
                continue;
            if (pass == 0 && dd.idProduct != pid)
                continue;
            uint8_t fnum[8], fout[8], fin[8];
            int k = scan_ledm_ifaces(list[i], fnum, fout, fin, 8);
            if (k <= 0)
                continue;
            int pick = 0;
            for (int j = 0; j < k; j++)
                if (fnum[j] == 1) { pick = j; break; }
            chosen = list[i];
            libusb_ref_device(chosen);
            pick_iface = fnum[pick];
            pick_out = fout[pick];
            pick_in = fin[pick];
        }
    }
    libusb_free_device_list(list, 1);
    if (!chosen) {
        snprintf(err, errn, "未找到带 LEDM 接口的 USB 打印机 (VID %04X)\n"
                           "提示: 大部分较新的 HP 打印机可直接支持; "
                           "其它型号用 --vid/--pid 指定", vid);
        return -2;
    }

    int rc = libusb_open(chosen, &L->h);
    libusb_unref_device(chosen);
    if (rc != 0) {
        if (rc == LIBUSB_ERROR_NO_DEVICE || rc == LIBUSB_ERROR_NOT_FOUND)
            return -2;                      /* mid re-enumeration */
        snprintf(err, errn, "libusb_open 失败: %s（权限不足? 安装 udev 规则后重新插拔）",
                 libusb_error_name(rc));
        return -1;
    }
    L->iface = pick_iface;
    L->ep_out = pick_out;
    L->ep_in = pick_in;

    if (libusb_kernel_driver_active(L->h, L->iface) == 1)
        libusb_detach_kernel_driver(L->h, L->iface);
    rc = libusb_claim_interface(L->h, L->iface);
    if (rc != 0) {
        snprintf(err, errn, "claim_interface(%d) 失败: %s", L->iface, libusb_error_name(rc));
        libusb_close(L->h);
        L->h = NULL;
        return -1;
    }
    return 0;
}

static int leedm_open(Ledm *L, uint16_t vid, uint16_t pid, char *err, size_t errn)
{
    /* 打印机进出睡眠模式时固件会复位 USB 栈: 设备从总线消失几秒后以
     * 新设备号重新枚举 (实测 M154)。扫描不到时持续重试约 12 秒。 */
    time_t t0 = time(NULL);
    char first_err[256] = "";
    for (;;) {
        int rc = leedm_open_once(L, vid, pid, err, errn);
        if (rc == 0)
            return 0;
        if (rc != -2)
            return -1;                      /* 权限/驱动等错误不重试 */
        if (!first_err[0])
            snprintf(first_err, sizeof first_err, "%s", err);
        if (difftime(time(NULL), t0) > 12) {
            snprintf(err, errn,
                     "%s\n"
                     "已重试 12 秒仍找不到设备。常见原因:\n"
                     "  - 打印机正在睡眠切换/打印启动中复位 USB (稍等再试)\n"
                     "  - USB 线松动或打印机断电\n"
                     "  - 复位后打印通道未恢复: 重新插拔 USB 线",
                     first_err);
            return -1;
        }
        usleep(800 * 1000);
    }
}

static void leedm_close(Ledm *L)
{
    if (L->h) {
        libusb_release_interface(L->h, L->iface);
        libusb_attach_kernel_driver(L->h, L->iface); /* best effort */
        libusb_close(L->h);
        L->h = NULL;
    }
}

/* discard stale responses the firmware buffered after earlier sessions */
static void leedm_drain(Ledm *L)
{
    unsigned char buf[4096];
    int quiet = 0;
    while (quiet < 2) {
        int n = 0;
        libusb_bulk_transfer(L->h, L->ep_in, buf, sizeof buf, &n, DRAIN_TIMEOUT_MS);
        if (n > 0) quiet = 0; else quiet++;
    }
}

static int buf_append(char **buf, size_t *len, size_t *cap, const void *data, size_t n)
{
    if (*len + n + 1 > *cap) {
        size_t nc = *cap ? *cap * 2 : 65536;
        while (nc < *len + n + 1) nc *= 2;
        char *nb = realloc(*buf, nc);
        if (!nb) return -1;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = 0;
    return 0;
}

/* read one response; returns 0 with *resp (malloc'd NUL-terminated, may be NULL) */
static int read_response(Ledm *L, char **resp, char *err, size_t errn)
{
    char *buf = NULL;
    size_t len = 0, cap = 0;
    time_t t0 = time(NULL);
    for (;;) {
        unsigned char chunk[16384];
        int got = 0;
        int rc = libusb_bulk_transfer(L->h, L->ep_in, chunk, sizeof chunk, &got, RX_TIMEOUT_MS);
        if (rc == 0 && got > 0) {
            if (buf_append(&buf, &len, &cap, chunk, (size_t)got) != 0) {
                snprintf(err, errn, "内存不足");
                free(buf);
                return -1;
            }
        } else if (rc == 0 && got == 0) {
            /* zero-length packet: the firmware may emit stray ZLPs mid-stream;
             * only treat as end-of-stream once content-length is satisfied
             * (checked below) — otherwise keep reading until quiet/deadline */
            if (difftime(time(NULL), t0) > RX_DEADLINE_S) break;
            usleep(5000);
            continue;
        } else if (len > 0) {
            break;                       /* quiet after data: response done */
        } else if (rc == LIBUSB_ERROR_TIMEOUT) {
            if (difftime(time(NULL), t0) > RX_DEADLINE_S) break;
            continue;
        } else {
            snprintf(err, errn, "USB 读失败: %s", libusb_error_name(rc));
            free(buf);
            return -1;
        }

        char *sep = buf ? strstr(buf, CRLF4) : NULL;
        if (sep) {
            const char *cl = strcasestr(buf, "content-length:");
            if (cl) {
                long v = strtol(cl + 15, NULL, 10);
                size_t body_off = (size_t)(sep - buf) + 4;
                if (v >= 0 && len - body_off >= (size_t)v)
                    break;
            }
        }
        if (difftime(time(NULL), t0) > RX_DEADLINE_S) break;
    }
    *resp = buf;
    return 0;
}

/* one HTTP GET attempt with stale-queue resync; returns 0 and fills body */
static int leedm_get_once(Ledm *L, const char *path, char **out, char *err, size_t errn)
{
    char req[512];
    int rl = snprintf(req, sizeof req,
                      "GET %s HTTP/1.1\r\n"
                      "Host: localhost\r\n"
                      "Connection: close\r\n"
                      "\r\n", path);
    if (rl <= 0 || (size_t)rl >= sizeof req) {
        snprintf(err, errn, "请求过长");
        return -1;
    }

    for (int inner = 0; inner < 6; inner++) {
        leedm_drain(L);

        int n = 0;
        int rc = libusb_bulk_transfer(L->h, L->ep_out, (unsigned char *)req, rl, &n, RX_TIMEOUT_MS);
        if (rc != 0) {
            snprintf(err, errn, "USB 写失败: %s", libusb_error_name(rc));
            return -1;
        }

        char *buf = NULL;
        if (read_response(L, &buf, err, errn) != 0)
            return -1;
        if (!buf || strncmp(buf, "HTTP/1.", 7) != 0 || !strstr(buf, " 200 ")) {
            char first[32] = {0};
            if (buf) strncpy(first, buf, sizeof first - 1);
            free(buf);
            leedm_drain(L);                  /* stale/partial data: resync */
            continue;
        }
        char *sep = strstr(buf, CRLF4);
        char *body = strdup(sep ? sep + 4 : buf);
        free(buf);
        if (!body) return -1;
        *out = body;
        return 0;
    }
    snprintf(err, errn, "多次重发后仍未获得有效响应（其它程序占用中?）");
    return -1;
}

/* GET with root-element validation + retry (firmware may replay stale data) */
static int leedm_get(Ledm *L, const char *path, const char *root, char **out,
                     char *err, size_t errn)
{
    char probe[80];
    snprintf(probe, sizeof probe, ":%s>", root);
    for (int attempt = 0; attempt < 5; attempt++) {
        char *body = NULL;
        if (leedm_get_once(L, path, &body, err, errn) == 0) {
            if (strstr(body, probe) || strstr(body, root)) {
                *out = body;
                return 0;
            }
            free(body);
            snprintf(err, errn, "响应内容不匹配（固件残留队列），重试中");
        }
        usleep(400 * 1000);
    }
    return -1;
}

/* ---------------------------------------------------------------- net mode */

#ifdef _WIN32
typedef SOCKET net_socket_t;
#define NET_SOCK_INVALID INVALID_SOCKET
static int netsock_init(void)
{
    static int done;
    WSADATA wsa;
    if (!done) {
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return -1;
        done = 1;
    }
    return 0;
}
static ssize_t net_recv(net_socket_t fd, void *buf, size_t n)
{
    return recv(fd, (char *)buf, (int)n, 0);
}
static ssize_t net_send(net_socket_t fd, const void *buf, size_t n)
{
    return send(fd, (const char *)buf, (int)n, 0);
}
static void net_close(net_socket_t fd) { closesocket(fd); }
#else
typedef int net_socket_t;
#define NET_SOCK_INVALID (-1)
static ssize_t net_recv(net_socket_t fd, void *buf, size_t n) { return read(fd, buf, n); }
static ssize_t net_send(net_socket_t fd, const void *buf, size_t n) { return (ssize_t)write(fd, buf, n); }
static void net_close(net_socket_t fd) { close(fd); }
#endif

static const char *g_nethost;

static int net_get(const char *host, const char *path, char **out, char *err, size_t errn)
{
#ifdef _WIN32
    if (netsock_init() != 0) {
        snprintf(err, errn, "WSA 初始化失败");
        return -1;
    }
#endif
    struct addrinfo hints = {0}, *res = NULL;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, "80", &hints, &res) != 0 || !res) {
        snprintf(err, errn, "无法解析主机 %s", host);
        return -1;
    }
    net_socket_t fd = NET_SOCK_INVALID;
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd == NET_SOCK_INVALID) continue;
#ifdef _WIN32
        DWORD tv = 5000;
#else
        struct timeval tv = {5, 0};
#endif
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof tv);
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        net_close(fd);
        fd = NET_SOCK_INVALID;
    }
    freeaddrinfo(res);
    if (fd == NET_SOCK_INVALID) {
        snprintf(err, errn, "无法连接 %s:80", host);
        return -1;
    }
    char req[512];
    int rl = snprintf(req, sizeof req,
                      "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                      path, host);
    if (net_send(fd, req, (size_t)rl) != rl) {
        snprintf(err, errn, "发送失败");
        net_close(fd);
        return -1;
    }
    char *buf = NULL;
    size_t len = 0, cap = 0;
    char chunk[16384];
    ssize_t r;
    while ((r = net_recv(fd, chunk, sizeof chunk)) > 0)
        if (buf_append(&buf, &len, &cap, chunk, (size_t)r) != 0) break;
    net_close(fd);
    if (!buf || len == 0) {
        snprintf(err, errn, "无响应");
        free(buf);
        return -1;
    }
    if (strncmp(buf, "HTTP/1.", 7) != 0 || !strstr(buf, " 200 ")) {
        char first[32] = {0};
        strncpy(first, buf, sizeof first - 1);
        snprintf(err, errn, "HTTP 状态异常: %s", first);
        free(buf);
        return -1;
    }
    char *sep = strstr(buf, CRLF4);
    char *body = strdup(sep ? sep + 4 : buf);
    free(buf);
    *out = body;
    return 0;
}

static int net_fetch_wrap(const char *path, char **out, char *err, size_t errn)
{
    return net_get(g_nethost, path, out, err, errn);
}

/* ---------------------------------------------------------------- xml mini parser
 * LEDM XML is machine generated and stable; tags are matched by local name
 * (the "dd:"/"ccdyn:"/"pudyn:" prefixes are skipped). */

/* p points just after '<'; returns pointer past '>' of this tag if its
 * local name equals `name` */
static const char *tag_match(const char *p, const char *name)
{
    if (*p == '/' || *p == '!' || *p == '?')
        return NULL;
    const char *q = p;
    while (isalnum((unsigned char)*q) || *q == '_' || *q == '-') q++;
    if (*q == ':') {                    /* namespace prefix */
        p = q + 1;
        q = p;
        while (isalnum((unsigned char)*q) || *q == '_' || *q == '-') q++;
    }
    size_t nl = strlen(name);
    if ((size_t)(q - p) != nl || strncmp(p, name, nl) != 0)
        return NULL;
    while (*q && *q != '>') q++;        /* skip attributes */
    if (*q != '>') return NULL;
    return q + 1;
}

static const char *find_open(const char *from, const char *name)
{
    const char *p = from;
    while ((p = strchr(p, '<')) != NULL) {
        const char *after = tag_match(p + 1, name);
        if (after) return after;
        p++;
    }
    return NULL;
}

static const char *find_close(const char *from, const char *name)
{
    const char *p = from;
    while ((p = strstr(p, "</")) != NULL) {
        if (tag_match(p + 2, name)) return p;
        p++;
    }
    return NULL;
}

static int find_block(const char *from, const char *name,
                      const char **bstart, const char **bend)
{
    const char *s = find_open(from, name);
    if (!s) return 0;
    const char *e = find_close(s, name);
    if (!e) return 0;
    *bstart = s;
    *bend = e;
    return 1;
}

/* find <prefix:name>value</...>, searching from `from`; returns pointer
 * past the value, or NULL. */
static const char *find_value_from(const char *from, const char *name,
                                   char *out, size_t outn)
{
    const char *v = find_open(from, name);
    if (!v) return NULL;
    const char *e = strchr(v, '<');
    if (!e) return NULL;
    size_t vn = (size_t)(e - v);
    if (vn >= outn) vn = outn - 1;
    memcpy(out, v, vn);
    out[vn] = 0;
    /* trim whitespace */
    char *s = out;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *t = out + vn;
    while (t > s && (t[-1] == ' ' || t[-1] == '\t' || t[-1] == '\r' || t[-1] == '\n')) t--;
    *t = 0;
    if (s != out) memmove(out, s, strlen(s) + 1);
    return e;
}

static int value_int(const char *from, const char *name, int def)
{
    char tmp[32];
    if (!find_value_from(from, name, tmp, sizeof tmp)) return def;
    return (int)strtol(tmp, NULL, 10);
}

/* ---------------------------------------------------------------- snapshot */

static void parse_consumables(const char *xml, Snapshot *snap)
{
    const char *pos = xml;
    const char *bs, *be;
    while (snap->nsupplies < MAX_SUPPLIES &&
           find_block(pos, "ConsumableInfo", &bs, &be)) {
        size_t blen = (size_t)(be - bs);
        char *blk = malloc(blen + 1);
        if (!blk) break;
        memcpy(blk, bs, blen);
        blk[blen] = 0;

        Supply *s = &snap->supplies[snap->nsupplies];
        memset(s, 0, sizeof *s);
        s->level = s->yield = s->station = s->printed = s->est_remaining = s->raw_level = -1;

        char v[128];
        if (find_value_from(blk, "ConsumableLabelCode", v, sizeof v))
            snprintf(s->label, sizeof s->label, "%s", v);
        if (find_value_from(blk, "ConsumableFamilyName", v, sizeof v))
            snprintf(s->family, sizeof s->family, "%s", v);
        if (find_value_from(blk, "ProductNumber", v, sizeof v))
            snprintf(s->product, sizeof s->product, "%s", v);
        if (find_value_from(blk, "SerialNumber", v, sizeof v))
            snprintf(s->serial, sizeof s->serial, "%s", v);
        s->station = value_int(blk, "ConsumableStation", -1);
        /* first ConsumableState is the one inside ConsumableLifeState */
        if (find_value_from(blk, "ConsumableState", v, sizeof v))
            snprintf(s->state, sizeof s->state, "%s", v);
        if (find_value_from(blk, "MeasuredQuantityState", v, sizeof v))
            snprintf(s->qty, sizeof s->qty, "%s", v);
        if (find_value_from(blk, "Brand", v, sizeof v))
            snprintf(s->brand, sizeof s->brand, "%s", v);

        const char *is, *ie;
        if (find_block(blk, "Installation", &is, &ie)) {
            char inst[256];
            size_t n = (size_t)(ie - is);
            if (n < sizeof inst) {
                memcpy(inst, is, n);
                inst[n] = 0;
                find_value_from(inst, "Date", s->installed, sizeof s->installed);
            }
        }
        const char *es, *ee;
        if (find_block(blk, "ConsumableEstimatedPages", &es, &ee)) {
            char eb[128];
            size_t n = (size_t)(ee - es);
            if (n < sizeof eb) {
                memcpy(eb, es, n);
                eb[n] = 0;
                s->yield = value_int(eb, "ValueInt", -1);
            }
        }
        {
            char lv[16];
            if (find_value_from(blk, "ConsumablePercentageLevelRemaining", lv, sizeof lv))
                s->level = (int)strtol(lv, NULL, 10);
        }
        free(blk);
        snap->nsupplies++;
        pos = be;
    }
}

static void parse_usage(const char *xml, Snapshot *snap)
{
    /* printer-level stats: slice PrinterSubunit before ConsumableSubunit */
    const char *ps, *pe;
    if (find_block(xml, "PrinterSubunit", &ps, &pe)) {
        const char *cut = strstr(ps, "ConsumableSubunit");
        size_t n = (size_t)((cut && cut < pe ? cut : pe) - ps);
        char *blk = malloc(n + 1);
        if (blk) {
            memcpy(blk, ps, n);
            blk[n] = 0;
            char v[32];
            if (find_value_from(blk, "TotalImpressions", v, sizeof v))
                snap->total = strtol(v, NULL, 10);
            if (find_value_from(blk, "MonochromeImpressions", v, sizeof v))
                snap->mono = strtol(v, NULL, 10);
            if (find_value_from(blk, "ColorImpressions", v, sizeof v))
                snap->color = strtol(v, NULL, 10);
            if (find_value_from(blk, "JamEvents", v, sizeof v))
                snap->jams = (int)strtol(v, NULL, 10);
            if (find_value_from(blk, "MispickEvents", v, sizeof v))
                snap->mispicks = (int)strtol(v, NULL, 10);
            free(blk);
        }
    }
    /* per-cartridge usage */
    const char *pos = xml;
    const char *cs, *ce;
    while (find_block(pos, "Consumable", &cs, &ce)) {
        size_t n = (size_t)(ce - cs);
        char *blk = malloc(n + 1);
        if (!blk) break;
        memcpy(blk, cs, n);
        blk[n] = 0;
        int station = value_int(blk, "ConsumableStation", -1);
        for (int i = 0; i < snap->nsupplies; i++) {
            if (snap->supplies[i].station == station) {
                snap->supplies[i].printed = value_int(blk, "TotalImpressions", -1);
                snap->supplies[i].est_remaining = value_int(blk, "EstimatedPagesRemaining", -1);
                snap->supplies[i].raw_level = value_int(blk, "ConsumableRawPercentageLevelRemaining", -1);
                break;
            }
        }
        free(blk);
        pos = ce;
    }
}

/* ---------------------------------------------------------------- product status */

/* /DevMgmt/ProductStatusDyn.xml — pscat:StatusCategory + psdyn:AlertTable */
static void parse_status(const char *xml, Snapshot *snap)
{
    const char *ss, *se;
    if (find_block(xml, "Status", &ss, &se)) {
        char blk[1024];
        size_t n = (size_t)(se - ss);
        if (n < sizeof blk) {
            memcpy(blk, ss, n);
            blk[n] = 0;
            find_value_from(blk, "StatusCategory", snap->status_cat, sizeof snap->status_cat);
            find_value_from(blk, "LocString", snap->status_loc, sizeof snap->status_loc);
        }
    }
    const char *pos = xml;
    const char *as, *ae;
    while (snap->nalerts < MAX_ALERTS && find_block(pos, "Alert", &as, &ae)) {
        size_t n = (size_t)(ae - as);
        char *blk = malloc(n + 1);
        if (!blk) break;
        memcpy(blk, as, n);
        blk[n] = 0;
        Alert *a = &snap->alerts[snap->nalerts];
        find_value_from(blk, "ProductStatusAlertID", a->id, sizeof a->id);
        find_value_from(blk, "Severity", a->severity, sizeof a->severity);
        find_value_from(blk, "AlertDetailsMarkerColor", a->color, sizeof a->color);
        find_value_from(blk, "LocString", a->loc, sizeof a->loc);
        if (a->id[0])
            snap->nalerts++;
        free(blk);
        pos = ae;
    }
}

typedef int (*fetch_fn)(const char *path, char **out, char *err, size_t errn);

static int get_validated(fetch_fn get, const char *path, const char *root,
                         char **out, char *err, size_t errn)
{
    char probe[80], closer[96];
    snprintf(probe, sizeof probe, ":%s>", root);
    snprintf(closer, sizeof closer, "</%s>", root);
    for (int attempt = 0; attempt < 5; attempt++) {
        char *body = NULL;
        if (get(path, &body, err, errn) == 0) {
            /* require the full document: root element AND its closing tag,
             * so truncated/stale partial responses are rejected */
            if ((strstr(body, probe) || strstr(body, root)) &&
                strstr(body, closer)) {
                *out = body;
                return 0;
            }
            free(body);
            snprintf(err, errn, "响应不完整或不匹配，重试中");
        }
        usleep(400 * 1000);
    }
    return -1;
}

/* USB fetch context (one device session for the whole snapshot) */
static Ledm g_ledm;
static int  g_ledm_ready;
static libusb_context *g_usbctx;

static int usb_fetch(const char *path, char **out, char *err, size_t errn)
{
    const char *root = strstr(path, "Consumable") ? "ccdyn:ConsumableConfigDyn"
                     : strstr(path, "ProductStatus") ? "psdyn:ProductStatusDyn"
                                                  : "pudyn:ProductUsageDyn";
    return leedm_get(&g_ledm, path, root, out, err, errn);
}

/* open the shared transport (network EWS or USB LEDM); on success *get is set
 * and, for USB, g_ledm is left open (close with snapshot_shutdown) */
static int transport_open(uint16_t vid, uint16_t pid, const char *host,
                          char *model, size_t modeln,
                          char *serial, size_t serialn,
                          char *err, size_t errn, fetch_fn *get)
{
    if (host && *host) {
        g_nethost = host;
        *get = net_fetch_wrap;
        if (model && modeln) snprintf(model, modeln, "%s (网络)", host);
        return 0;
    }
    if (g_ledm_ready) {
        *get = usb_fetch;
        return 0;
    }
    if (libusb_init(&g_usbctx) != 0) {
        snprintf(err, errn, "libusb 初始化失败");
        return -1;
    }
    g_ledm.ctx = g_usbctx;
    if (leedm_open(&g_ledm, vid, pid, err, errn) != 0) {
        libusb_exit(g_usbctx);
        g_usbctx = NULL;
        return -1;
    }
    g_ledm_ready = 1;
    char desc[128];
    int r = libusb_get_string_descriptor_ascii(
        g_ledm.h, 2, (unsigned char *)desc, sizeof desc);  /* iProduct */
    if (r > 0 && model && modeln) snprintf(model, modeln, "%.*s", r, desc);
    r = libusb_get_string_descriptor_ascii(
        g_ledm.h, 3, (unsigned char *)desc, sizeof desc);  /* iSerialNumber */
    if (r > 0 && serial && serialn) snprintf(serial, serialn, "%.*s", r, desc);
    *get = usb_fetch;
    return 0;
}

static int snapshot_build(uint16_t vid, uint16_t pid, const char *host, Snapshot *snap)
{
    memset(snap, 0, sizeof *snap);
    char err[256] = "";
    fetch_fn get;

    if (transport_open(vid, pid, host, snap->model, sizeof snap->model,
                       snap->serial, sizeof snap->serial,
                       err, sizeof err, &get) != 0) {
        snprintf(snap->error, sizeof snap->error, "%s", err);
        return -1;
    }

    char *xml = NULL;
    if (get_validated(get, "/DevMgmt/ConsumableConfigDyn.xml",
                      "ccdyn:ConsumableConfigDyn", &xml, err, sizeof err) != 0) {
        snprintf(snap->error, sizeof snap->error, "耗材数据获取失败: %s", err);
        return -1;
    }
    parse_consumables(xml, snap);
    free(xml);

    xml = NULL;
    if (get_validated(get, "/DevMgmt/ProductUsageDyn.xml",
                      "pudyn:ProductUsageDyn", &xml, err, sizeof err) != 0) {
        snprintf(snap->error, sizeof snap->error, "用量数据获取失败: %s", err);
        return -1;
    }
    parse_usage(xml, snap);
    free(xml);

    /* printer state/alerts — optional endpoint, absence on older firmware is
     * not fatal */
    xml = NULL;
    if (get_validated(get, "/DevMgmt/ProductStatusDyn.xml",
                      "psdyn:ProductStatusDyn", &xml, err, sizeof err) == 0) {
        parse_status(xml, snap);
        free(xml);
    }
    return 0;
}

static void snapshot_shutdown(void)
{
    if (g_ledm_ready) {
        leedm_close(&g_ledm);
        g_ledm_ready = 0;
    }
    if (g_usbctx) {
        libusb_exit(g_usbctx);
        g_usbctx = NULL;
    }
}

/* ---------------------------------------------------------------- debug get */

/* --get PATH: fetch an arbitrary LEDM path and dump the raw XML (debugging /
 * reverse-engineering aid; bypasses root-element validation) */
static int debug_get_mode(uint16_t vid, uint16_t pid, const char *host, const char *path)
{
    setlocale(LC_ALL, "");
    char model[80] = "", serial[40] = "", err[256] = "";
    fetch_fn get;
    if (transport_open(vid, pid, host, model, sizeof model, serial, sizeof serial,
                       err, sizeof err, &get) != 0) {
        fprintf(stderr, "错误: %s\n", err);
        snapshot_shutdown();
        return 1;
    }
    char *body = NULL;
    int rc;
    if (host && *host)
        rc = net_get(host, path, &body, err, sizeof err);
    else
        rc = leedm_get_once(&g_ledm, path, &body, err, sizeof err);
    if (rc != 0 || !body) {
        fprintf(stderr, "错误: %s\n", err);
        snapshot_shutdown();
        return 1;
    }
    fputs(body, stdout);
    fputc('\n', stdout);
    free(body);
    snapshot_shutdown();
    return 0;
}

/* ---------------------------------------------------------------- text mode */

static const char *color_cn(const char *label, const char *family)
{
    if (!strcmp(label, "K")) return "黑色";
    if (!strcmp(label, "C")) return "青色";
    if (!strcmp(label, "M")) return "品红";
    if (!strcmp(label, "Y")) return "黄色";
    return (family && *family) ? family : ((label && *label) ? label : "?");
}

/* ---- status & alert vocabulary (LEDM pscat enum + bidi-LEDM.js mapping) ---- */

static const char *status_cat_cn(const char *cat)
{
    if (!cat || !*cat) return "";
    if (!strcmp(cat, "ready"))                return "就绪";
    if (!strcmp(cat, "inPowerSave"))          return "省电模式";
    if (!strcmp(cat, "processing"))           return "正在处理";
    if (!strcmp(cat, "printing"))             return "正在打印";
    if (!strcmp(cat, "calibrating"))          return "正在校准";
    if (!strcmp(cat, "canceling"))            return "正在取消";
    if (!strcmp(cat, "paused"))               return "已暂停";
    if (!strcmp(cat, "DoorOpen"))             return "仓门未关";
    if (!strcmp(cat, "closeDoorOrCover"))     return "请关上仓门/顶盖";
    if (!strcmp(cat, "MediaJam"))             return "卡纸";
    if (!strcmp(cat, "jamInPrinter"))         return "卡纸";
    if (!strcmp(cat, "MediaEmpty"))           return "纸盒缺纸";
    if (!strcmp(cat, "trayEmptyOrOpen"))      return "纸盒为空或未关好";
    if (!strcmp(cat, "sizeMismatchInTray"))   return "纸张尺寸不匹配";
    if (!strcmp(cat, "output-area-full"))     return "出纸区已满";
    if (!strcmp(cat, "outputBinOpened"))      return "出纸盒被打开";
    if (!strcmp(cat, "toner-empty-error"))    return "硒鼓已用尽";
    if (!strcmp(cat, "toner-empty-warning"))  return "硒鼓即将用尽";
    if (!strcmp(cat, "toner-low-report"))     return "硒鼓余量不足";
    if (!strcmp(cat, "printFailure"))         return "打印失败";
    if (!strcmp(cat, "printerError"))         return "打印机错误";
    if (!strcmp(cat, "startupRoutineFailed")) return "开机自检失败";
    if (!strcmp(cat, "inReserveMode"))        return "储备模式";
    return cat;
}

static const char *alert_id_cn(const char *id)
{
    if (!id || !*id) return "";
    if (!strcmp(id, "cartridgeVeryLow"))             return "硒鼓余量极低";
    if (!strcmp(id, "cartridgeLow"))                 return "硒鼓余量低";
    if (!strcmp(id, "usedConsumable"))               return "硒鼓已用尽";
    if (!strcmp(id, "replaceCartridgeOut"))          return "请更换硒鼓";
    if (!strcmp(id, "cartridgeMissing"))             return "未检测到硒鼓";
    if (!strcmp(id, "cartridgeFailure"))             return "硒鼓故障";
    if (!strcmp(id, "incompatibleConsumable"))       return "耗材不兼容";
    if (!strcmp(id, "nonHPSupplyDetected"))          return "检测到非 HP 耗材";
    if (!strcmp(id, "cartridgeCounterfeitQuestion")) return "疑似非原装耗材";
    if (!strcmp(id, "insertSETUPCartridge"))         return "请插入随机安装硒鼓";
    if (!strcmp(id, "genuineHP"))                    return "原装 HP 硒鼓";
    if (!strcmp(id, "upgradableSupply"))             return "耗材固件可升级";
    if (!strcmp(id, "jamInPrinter"))                 return "卡纸";
    if (!strcmp(id, "MediaJam"))                     return "卡纸";
    if (!strcmp(id, "DoorOpen"))                     return "仓门未关";
    if (!strcmp(id, "closeDoorOrCover"))             return "请关上仓门/顶盖";
    if (!strcmp(id, "MediaEmpty"))                   return "纸盒缺纸";
    if (!strcmp(id, "trayEmptyOrOpen"))              return "纸盒为空或未关好";
    if (!strcmp(id, "sizeMismatchInTray"))           return "纸张尺寸不匹配";
    if (!strcmp(id, "output-area-full"))             return "出纸区已满";
    if (!strcmp(id, "paused"))                       return "已暂停";
    if (!strcmp(id, "printFailure"))                 return "打印失败";
    if (!strcmp(id, "printerError"))                 return "打印机错误";
    return id;
}

static const char *severity_cn(const char *sev)
{
    if (!strcmp(sev, "Error"))         return "错误";
    if (!strcmp(sev, "StrictWarning")) return "严重警告";
    if (!strcmp(sev, "Warning"))       return "警告";
    if (!strcmp(sev, "Info"))          return "提示";
    return (sev && *sev) ? sev : "提示";
}

static const char *marker_color_cn(const char *c)
{
    if (!strcmp(c, "Black"))   return "黑色";
    if (!strcmp(c, "Cyan"))    return "青色";
    if (!strcmp(c, "Magenta")) return "品红";
    if (!strcmp(c, "Yellow"))  return "黄色";
    return "";
}

/* overall level: 2 = error, 1 = warnings, 0 = normal/busy */
static int snapshot_alert_level(const Snapshot *snap)
{
    static const char *err_cats[] = {
        "DoorOpen", "closeDoorOrCover", "MediaJam", "jamInPrinter",
        "printerError", "printFailure", "startupRoutineFailed",
        "toner-empty-error", NULL
    };
    int level = 0;
    for (int i = 0; err_cats[i]; i++)
        if (!strcmp(snap->status_cat, err_cats[i])) return 2;
    for (int i = 0; i < snap->nalerts; i++) {
        if (!strcmp(snap->alerts[i].severity, "Error")) return 2;
        if (!strcmp(snap->alerts[i].severity, "StrictWarning") ||
            !strcmp(snap->alerts[i].severity, "Warning")) level = 1;
    }
    return level;
}

static int text_mode(uint16_t vid, uint16_t pid, const char *host)
{
    setlocale(LC_ALL, "");
    Snapshot snap;
    if (snapshot_build(vid, pid, host, &snap) != 0) {
        fprintf(stderr, "错误: %s\n", snap.error);
        snapshot_shutdown();
        return 1;
    }
    printf("打印机 : %s  序列号 %s\n", snap.model, snap.serial);
    {
        const char *st = snap.status_loc[0] ? snap.status_loc
                       : status_cat_cn(snap.status_cat);
        printf("状态   : %s\n", st[0] ? st : "未知");
        if (snap.nalerts) {
            printf("== 活动提醒 ==\n");
            for (int i = 0; i < snap.nalerts; i++) {
                Alert *a = &snap.alerts[i];
                const char *mc = marker_color_cn(a->color);
                printf("  [%s] %s%s%s\n", severity_cn(a->severity),
                       mc, mc[0] ? " " : "",
                       a->loc[0] ? a->loc : alert_id_cn(a->id));
            }
        }
    }
    printf("== 用量统计 ==\n");
    printf("总打印页数 : %ld  (黑白 %ld / 彩色 %ld)\n", snap.total, snap.mono, snap.color);
    printf("卡纸 %d 次  进纸失败 %d 次\n", snap.jams, snap.mispicks);
    printf("== 耗材 ==\n");
    for (int i = 0; i < snap.nsupplies; i++) {
        Supply *s = &snap.supplies[i];
        char lvl[16];
        if (s->level >= 0) snprintf(lvl, sizeof lvl, "%d%%", s->level);
        else snprintf(lvl, sizeof lvl, "—");
        printf("%-4s [%s] 余量 %-5s 状态 %-8s 本支已打印 %d 页  预计剩余 %d 页  标称 %d 页  品牌 %s  装机 %s  S/N %s\n",
               color_cn(s->label, s->family), s->product, lvl,
               *s->qty ? s->qty : (*s->state ? s->state : "?"),
               s->printed, s->est_remaining, s->yield,
               *s->brand ? s->brand : "?",
               *s->installed ? s->installed : "?",
               *s->serial ? s->serial : "?");
    }
    snapshot_shutdown();
    return 0;
}

/* ---------------------------------------------------------------- GUI */

typedef struct {
    GtkWidget *win;
    GtkWidget *lbl_product;
    GtkWidget *lbl_state;
    GtkWidget *lbl_usage_big;
    GtkWidget *lbl_usage_sub;
    GtkWidget *lbl_status;
    GtkWidget *btn_refresh;
    GtkWidget *chk_auto;
    guint   auto_src;
    struct {
        GtkWidget *frame;
        GtkWidget *name;
        GtkWidget *brand;
        GtkWidget *bar;
        GtkWidget *detail;
        GtkWidget *meta;
    } card[MAX_SUPPLIES];
    gint    ncards;
    volatile gint busy;
    guint16 vid, pid;
    gchar  *host;
} Ui;

static Ui ui;

static void apply_snapshot_to_ui(const Snapshot *snap)
{
    if (snap->error[0]) {
        gtk_label_set_text(GTK_LABEL(ui.lbl_status), snap->error);
        return;
    }
    gchar *t = g_strdup_printf("%s  ·  S/N %s", snap->model, snap->serial);
    gtk_label_set_text(GTK_LABEL(ui.lbl_product), t);
    g_free(t);

    /* machine state line: firmware LocString or mapped category, plus alerts */
    {
        GtkStyleContext *sc = gtk_widget_get_style_context(ui.lbl_state);
        gtk_style_context_remove_class(sc, "state-ok");
        gtk_style_context_remove_class(sc, "state-warn");
        gtk_style_context_remove_class(sc, "state-err");
        const char *base = snap->status_loc[0] ? snap->status_loc
                         : status_cat_cn(snap->status_cat);
        int level = snapshot_alert_level(snap);
        gboolean has_warn = FALSE;
        GString *g = g_string_new("");
        if (level == 2)
            g_string_append_printf(g, "⚠ 打印机错误: %s", base[0] ? base : "请查看打印机面板");
        else {
            if (base[0]) g_string_append(g, base);
            for (int i = 0; i < snap->nalerts; i++) {
                Alert *a = &snap->alerts[i];
                if (!strcmp(a->severity, "Info")) continue;
                const char *mc = marker_color_cn(a->color);
                g_string_append_printf(g, "%s%s%s%s", g->len ? "    " : "",
                                       mc, mc[0] ? " " : "",
                                       a->loc[0] ? a->loc : alert_id_cn(a->id));
                has_warn = TRUE;
            }
            if (!g->len) g_string_append(g, "正常");
        }
        gtk_style_context_add_class(sc,
            level == 2 ? "state-err" : (has_warn ? "state-warn" : "state-ok"));
        gtk_label_set_text(GTK_LABEL(ui.lbl_state), g->str);
        g_string_free(g, TRUE);
    }

    t = g_strdup_printf("%'ld", snap->total);
    gtk_label_set_text(GTK_LABEL(ui.lbl_usage_big), t);
    g_free(t);

    t = g_strdup_printf("黑白 %'ld 页    彩色 %'ld 页    卡纸 %d 次    进纸失败 %d 次",
                        snap->mono, snap->color, snap->jams, snap->mispicks);
    gtk_label_set_text(GTK_LABEL(ui.lbl_usage_sub), t);
    g_free(t);

    for (gint i = 0; i < ui.ncards; i++) {
        if (i >= snap->nsupplies) {
            gtk_widget_set_visible(ui.card[i].frame, FALSE);
            continue;
        }
        Supply s = snap->supplies[i];
        gtk_widget_set_visible(ui.card[i].frame, TRUE);

        gchar *name = g_strdup_printf("%s  %s", color_cn(s.label, s.family), s.product);
        gtk_label_set_text(GTK_LABEL(ui.card[i].name), name);
        g_free(name);

        gtk_label_set_text(GTK_LABEL(ui.card[i].brand),
                           g_str_has_prefix(s.brand, "clone") ? "兼容耗材"
                           : (g_strcmp0(s.brand, "HP") == 0 ? "原装 HP" : ""));

        gchar lvl[16];
        if (s.level >= 0) snprintf(lvl, sizeof lvl, "%d%%", s.level);
        else snprintf(lvl, sizeof lvl, "—");
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(ui.card[i].bar), lvl);
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(ui.card[i].bar), TRUE);
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui.card[i].bar),
                                      s.level >= 0 ? CLAMP(s.level, 0, 100) / 100.0 : 0.0);

        const char *statecn =
            !strcmp(s.qty, "veryLow") ? "严重不足" :
            !strcmp(s.qty, "low")     ? "不足"     :
            !strcmp(s.qty, "ok")      ? "正常"     :
            !strcmp(s.state, "used")  ? "已用尽"   :
            !strcmp(s.state, "new")   ? "全新"     :
                                        (s.qty[0] ? s.qty : s.state);
        gchar *detail = g_strdup_printf("状态: %s    本支已打印 %'d 页    预计还能打 %'d 页",
                                        statecn, s.printed, s.est_remaining);
        gtk_label_set_text(GTK_LABEL(ui.card[i].detail), detail);
        g_free(detail);

        gchar *meta = g_strdup_printf("装机 %s    标称 %d 页    S/N %s",
                                      s.installed[0] ? s.installed : "?",
                                      s.yield, s.serial);
        gtk_label_set_text(GTK_LABEL(ui.card[i].meta), meta);
        g_free(meta);
    }
    gtk_label_set_text(GTK_LABEL(ui.lbl_status), "已连接（USB LEDM）");
}

static gboolean job_done(gpointer data)
{
    Snapshot *snap = data;
    apply_snapshot_to_ui(snap);
    gtk_widget_set_sensitive(ui.btn_refresh, TRUE);
    g_atomic_int_set(&ui.busy, 0);
    g_free(snap);
    return G_SOURCE_REMOVE;
}

static void *job_worker(void *arg)
{
    (void)arg;
    Snapshot *snap = g_new0(Snapshot, 1);
    snapshot_build(ui.vid, ui.pid, ui.host, snap);
    gdk_threads_add_idle(job_done, snap);
    return NULL;
}

static void start_refresh(void)
{
    if (g_atomic_int_get(&ui.busy)) return;
    g_atomic_int_set(&ui.busy, 1);
    gtk_widget_set_sensitive(ui.btn_refresh, FALSE);
    gtk_label_set_text(GTK_LABEL(ui.lbl_status), "查询中…");
    gtk_label_set_text(GTK_LABEL(ui.lbl_state), "查询中…");
    GThread *t = g_thread_new("refresh", job_worker, NULL);
    g_thread_unref(t);
}

static void on_refresh(GtkButton *b, gpointer u) { (void)b; (void)u; start_refresh(); }

static gboolean on_auto(gpointer u) { (void)u; start_refresh(); return G_SOURCE_CONTINUE; }

static void on_auto_toggled(GtkToggleButton *b, gpointer u)
{
    (void)u;
    if (gtk_toggle_button_get_active(b)) {
        if (!ui.auto_src) ui.auto_src = g_timeout_add_seconds(10, on_auto, NULL);
    } else if (ui.auto_src) {
        g_source_remove(ui.auto_src);
        ui.auto_src = 0;
    }
}

static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer u)
{
    (void)w; (void)e; (void)u;
    snapshot_shutdown();
    gtk_main_quit();
    return FALSE;
}

static void css_apply(void)
{
    const char *css =
        " .title-big { font-size: 15px; font-weight: bold; }"
        " .usage-big  { font-size: 34px; font-weight: bold; color: #1a73e8; }"
        " .muted { color: alpha(@theme_fg_color, 0.72); font-size: 12px; }"
        " .state-ok   { color: #2e8b57; font-weight: bold; }"
        " .state-warn { color: #d18f00; font-weight: bold; }"
        " .state-err  { color: #d93025; font-weight: bold; }"
        " .dot { font-size: 20px; }"
        " .dot-k { color: #9aa0a6; } .dot-c { color: #00aeef; }"
        " .dot-m { color: #ec008c; } .dot-y { color: #f4b400; }"
        " .chip { background: alpha(#1a73e8, 0.18); color: #8ab4f8; border-radius: 8px;"
        "         padding: 1px 8px; font-size: 11px; }"
        " progressbar.toner-k > trough > progress { background-color: #9aa0a6; }"
        " progressbar.toner-c > trough > progress { background-color: #00aeef; }"
        " progressbar.toner-m > trough > progress { background-color: #ec008c; }"
        " progressbar.toner-y > trough > progress { background-color: #f4b400; }"
        " progressbar > trough { min-height: 14px; }";
    GtkCssProvider *p = gtk_css_provider_new();
    gtk_css_provider_load_from_data(p, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(), GTK_STYLE_PROVIDER(p),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(p);
}

static GtkWidget *make_card(gint idx, const char *dotcls, const char *tonercls)
{
    GtkWidget *frame = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_OUT);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(box), 10);
    gtk_container_add(GTK_CONTAINER(frame), box);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *dot = gtk_label_new("●");
    gtk_style_context_add_class(gtk_widget_get_style_context(dot), "dot");
    gtk_style_context_add_class(gtk_widget_get_style_context(dot), dotcls);
    GtkWidget *name = gtk_label_new(NULL);
    gtk_widget_set_halign(name, GTK_ALIGN_START);
    gtk_widget_set_hexpand(name, TRUE);
    GtkWidget *brand = gtk_label_new(NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(brand), "chip");
    gtk_box_pack_start(GTK_BOX(top), dot, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(top), name, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(top), brand, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), top, FALSE, FALSE, 0);

    GtkWidget *bar = gtk_progress_bar_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(bar), tonercls);
    gtk_box_pack_start(GTK_BOX(box), bar, FALSE, FALSE, 2);

    GtkWidget *detail = gtk_label_new(NULL);
    gtk_widget_set_halign(detail, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), detail, FALSE, FALSE, 0);

    GtkWidget *meta = gtk_label_new(NULL);
    gtk_widget_set_halign(meta, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(meta), "muted");
    gtk_box_pack_start(GTK_BOX(box), meta, FALSE, FALSE, 0);

    ui.card[idx].frame = frame;
    ui.card[idx].name = name;
    ui.card[idx].brand = brand;
    ui.card[idx].bar = bar;
    ui.card[idx].detail = detail;
    ui.card[idx].meta = meta;
    return frame;
}

static int gui_run(uint16_t vid, uint16_t pid, const char *host)
{
    ui.vid = vid;
    ui.pid = pid;
    ui.host = g_strdup(host ? host : "");

#ifndef _WIN32
    if (!getenv("DISPLAY") && !getenv("WAYLAND_DISPLAY")) {
        fprintf(stderr,
                "错误: 当前终端没有图形显示环境 (DISPLAY/WAYLAND_DISPLAY 未设置)\n"
                "  - 请在桌面环境的终端里运行: hp-supplies\n"
                "  - 或从应用菜单启动 \"HP 打印机耗材监控\"\n"
                "  - 无显示环境(如 SSH)请用命令行模式: hp-supplies --text\n");
        return 1;
    }
#endif

    gtk_init(NULL, NULL);
    css_apply();

    ui.win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(ui.win), APP_NAME);
    gtk_window_set_default_size(GTK_WINDOW(ui.win), 780, 640);
    g_signal_connect(ui.win, "delete-event", G_CALLBACK(on_delete), NULL);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 14);
    gtk_container_add(GTK_CONTAINER(ui.win), vbox);

    ui.lbl_product = gtk_label_new("未连接");
    gtk_style_context_add_class(gtk_widget_get_style_context(ui.lbl_product), "title-big");
    gtk_widget_set_halign(ui.lbl_product, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), ui.lbl_product, FALSE, FALSE, 0);

    ui.lbl_state = gtk_label_new("—");
    gtk_widget_set_halign(ui.lbl_state, GTK_ALIGN_START);
    gtk_widget_set_hexpand(ui.lbl_state, TRUE);
    gtk_label_set_line_wrap(GTK_LABEL(ui.lbl_state), TRUE);
    gtk_label_set_xalign(GTK_LABEL(ui.lbl_state), 0.0);
    gtk_box_pack_start(GTK_BOX(vbox), ui.lbl_state, FALSE, FALSE, 0);

    GtkWidget *uframe = gtk_frame_new(NULL);
    gtk_frame_set_shadow_type(GTK_FRAME(uframe), GTK_SHADOW_OUT);
    GtkWidget *ubox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_container_set_border_width(GTK_CONTAINER(ubox), 12);
    gtk_container_add(GTK_CONTAINER(uframe), ubox);
    GtkWidget *utitle = gtk_label_new("总打印页数");
    gtk_widget_set_halign(utitle, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(utitle), "muted");
    ui.lbl_usage_big = gtk_label_new("—");
    gtk_widget_set_halign(ui.lbl_usage_big, GTK_ALIGN_START);
    gtk_style_context_add_class(gtk_widget_get_style_context(ui.lbl_usage_big), "usage-big");
    ui.lbl_usage_sub = gtk_label_new("—");
    gtk_widget_set_halign(ui.lbl_usage_sub, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(ubox), utitle, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ubox), ui.lbl_usage_big, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ubox), ui.lbl_usage_sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), uframe, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    const char *dotcls[4]   = { "dot-k", "dot-c", "dot-m", "dot-y" };
    const char *tonercls[4] = { "toner-k", "toner-c", "toner-m", "toner-y" };
    for (gint i = 0; i < 4; i++) {
        GtkWidget *card = make_card(i, dotcls[i], tonercls[i]);
        gtk_grid_attach(GTK_GRID(grid), card, i % 2, i / 2, 1, 1);
        gtk_widget_set_hexpand(card, TRUE);
        gtk_widget_set_vexpand(card, TRUE);
    }
    ui.ncards = 4;
    gtk_box_pack_start(GTK_BOX(vbox), grid, TRUE, TRUE, 0);

    GtkWidget *foot = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    ui.chk_auto = gtk_check_button_new_with_label("每10秒自动刷新");
    g_signal_connect(ui.chk_auto, "toggled", G_CALLBACK(on_auto_toggled), NULL);
    ui.btn_refresh = gtk_button_new_with_label("刷新");
    g_signal_connect(ui.btn_refresh, "clicked", G_CALLBACK(on_refresh), NULL);
    ui.lbl_status = gtk_label_new("准备…");
    gtk_widget_set_halign(ui.lbl_status, GTK_ALIGN_START);
    gtk_widget_set_hexpand(ui.lbl_status, TRUE);
    gtk_box_pack_start(GTK_BOX(foot), ui.chk_auto, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(foot), ui.btn_refresh, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(foot), ui.lbl_status, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), foot, FALSE, FALSE, 0);

    gtk_widget_show_all(ui.win);
    start_refresh();
    gtk_main();
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    console_utf8();
    uint16_t vid = DEFAULT_VID, pid = DEFAULT_PID;
    const char *host = NULL;
    const char *getpath = NULL;
    int text = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--text")) text = 1;
        else if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
        else if (!strcmp(argv[i], "--get") && i + 1 < argc) getpath = argv[++i];
        else if (!strcmp(argv[i], "--vid") && i + 1 < argc) vid = (uint16_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--pid") && i + 1 < argc) pid = (uint16_t)strtoul(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("hp-supplies %s — %s（LEDM over USB）\n"
                   "\n"
                   "用法:\n"
                   "  hp-supplies                  GUI 模式（自动扫描 USB 打印机）\n"
                   "  hp-supplies --text           终端输出\n"
                   "  hp-supplies --host IP        走网络 EWS 查询（打印机联网时）\n"
                   "\n"
                   "选项:\n"
                   "  --text            纯文本输出, 不启动 GUI\n"
                   "  --host IP         通过网络访问打印机内置 EWS（80 端口）\n"
                   "  --get PATH        调试: 获取指定 LEDM 路径并输出原文\n"
                   "  --vid XXXX        USB 厂商 ID（十六进制, 默认 03f0）\n"
                   "  --pid XXXX        USB 产品 ID（十六进制, 默认 ae2a）\n"
                   "  -h, --help        显示本帮助并退出\n"
                   "  --version         显示版本并退出\n"
                   "\n"
                   "退出码: 0 查询成功, 1 查询失败, 2 参数错误\n"
                   "支持的打印机: 带 \"HP LEDM\" USB 接口的 HP 打印机\n"
                   "（较新的 InkJet / LaserJet / Smart Tank 系列）\n",
                   APP_VERSION, APP_NAME);
            return 0;
        }
        else if (!strcmp(argv[i], "--version")) { printf("%s %s\n", APP_NAME, APP_VERSION); return 0; }
        else {
            fprintf(stderr,
                    "hp-supplies: 无法识别的参数 '%s'\n"
                    "用法: hp-supplies [--text] [--host IP] [--vid XXXX] [--pid XXXX]\n"
                    "详情: hp-supplies --help\n", argv[i]);
            return 2;
        }
    }
    if (getpath) return debug_get_mode(vid, pid, host, getpath);
    if (text) return text_mode(vid, pid, host);
    return gui_run(vid, pid, host);
}
