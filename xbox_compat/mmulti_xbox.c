/*
 * mmulti_xbox.c — Xbox UDP LAN multiplayer for jfduke3d-xbox
 *
 * Uses lwIP BSD socket API (nxdk) for packet transport.
 * Discovery: UDP broadcast on port 23513, same subnet.
 *
 * Lobby protocol phases (driven by xbox_mp_tick() each menu frame):
 *   HOST broadcasts PKT_DISCOVER every ~500ms on 255.255.255.255:23513.
 *   JOINER receives DISCOVER, sends PKT_DISCO_REPLY back to host.
 *   HOST assigns player slot, sends PKT_WELCOME(slot).
 *   HOST calls xbox_mp_host_start() when ready → sends PKT_START with all IPs.
 *   Joiners receive PKT_START → rebuild peer table → enter game phase.
 *
 * Game phase: sendpacket / getpacket use direct UDP unicast.
 *
 * Insignia (Xbox Live emulator) tunnels system-link broadcast traffic,
 * so this broadcast-based discovery works transparently over Insignia.
 */

#include <nxdk/net.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <string.h>
#include <stdio.h>
#include "mmulti.h"

extern void xbox_log(const char *fmt, ...);
extern unsigned long totalclock;

/* ─── Constants ─────────────────────────────────────────────────────────── */

#define NET_PORT          23513
#define BCAST_ADDR        "255.255.255.255"
#define ANNOUNCE_INTERVAL 13   /* ~500ms at 26 tics/sec */
#define MAX_NET_PLAYERS   4

/* Lobby packet type tags (first byte of each discovery packet).
 * Range 0xD0-0xDF is never used by Duke3D game packets. */
#define PKT_DISCOVER     0xD0  /* host → broadcast: "I am hosting" */
#define PKT_DISCO_REPLY  0xD1  /* joiner → host:    "I want to join" */
#define PKT_WELCOME      0xD2  /* host → joiner:    "you are player N" */
#define PKT_WELCOME_ACK  0xD3  /* joiner → host:    "got WELCOME" */
#define PKT_START        0xD4  /* host → all:       start + full peer IP list */

/* Roles */
#define NET_ROLE_NONE  0
#define NET_ROLE_HOST  1
#define NET_ROLE_JOIN  2

/* Lobby state machine */
#define NET_STATE_IDLE      0
#define NET_STATE_HOSTING   1  /* host: broadcasting, collecting joiners */
#define NET_STATE_JOINING   2  /* joiner: searching for host */
#define NET_STATE_WELCOMED  3  /* joiner: received slot, waiting for START */
#define NET_STATE_GAME      4  /* all: in-game packet phase */
#define NET_STATE_ERROR     5

/* ─── mmulti.h globals ──────────────────────────────────────────────────── */

int myconnectindex = 0;
int numplayers     = 1;
int networkmode    = -1;
int connecthead    = 0;
int connectpoint2[MAXMULTIPLAYERS];
unsigned char syncstate = 0;

/* ─── Internal state ────────────────────────────────────────────────────── */

static int net_sock        = -1;
static int net_role        = NET_ROLE_NONE;
static int net_max_players = 2;
static int net_state       = NET_STATE_IDLE;
static int net_initialized = 0;
static unsigned long my_ip = 0;
static unsigned long last_announce_clock = 0;

/* peer_addr[i] = IPv4 address of player i (port = NET_PORT) */
static struct sockaddr_in peer_addr[MAXMULTIPLAYERS];

static char net_status[96] = "";

/* ─── Status / state accessors (used by menues.c) ──────────────────────── */

void  xbox_mp_get_status(char *buf, int maxlen)
{
    strncpy(buf, net_status, maxlen - 1);
    buf[maxlen - 1] = '\0';
}
int xbox_mp_get_state(void)      { return net_state; }
int xbox_mp_get_numplayers(void) { return numplayers; }

/* ─── Helpers ───────────────────────────────────────────────────────────── */

static void net_sendto(struct sockaddr_in *dst,
                       const unsigned char *buf, int len)
{
    if (net_sock < 0) return;
    sendto(net_sock, (const char *)buf, len, 0,
           (struct sockaddr *)dst, sizeof(*dst));
}

static void net_broadcast(const unsigned char *buf, int len)
{
    struct sockaddr_in bcast;
    memset(&bcast, 0, sizeof(bcast));
    bcast.sin_family      = AF_INET;
    bcast.sin_port        = htons(NET_PORT);
    bcast.sin_addr.s_addr = inet_addr(BCAST_ADDR);
    sendto(net_sock, (const char *)buf, len, 0,
           (struct sockaddr *)&bcast, sizeof(bcast));
}

/* Discover our own IPv4 address by peeking at a connected UDP socket */
static unsigned long probe_my_ip(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) return 0;
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family      = AF_INET;
    dst.sin_port        = htons(53);
    dst.sin_addr.s_addr = inet_addr("8.8.8.8");
    connect(s, (struct sockaddr *)&dst, sizeof(dst));
    struct sockaddr_in me;
    socklen_t melen = sizeof(me);
    memset(&me, 0, sizeof(me));
    getsockname(s, (struct sockaddr *)&me, &melen);
    close(s);
    return me.sin_addr.s_addr;
}

/* ─── Network initialisation ────────────────────────────────────────────── */

static int init_net_stack(void)
{
    if (net_initialized) {
        xbox_log("mmulti_xbox: init_net_stack already initialized\n");
        return 1;
    }
    snprintf(net_status, sizeof(net_status), "CONNECTING TO NETWORK...");
    xbox_log("mmulti_xbox: init_net_stack enter, calling nxNetInit DHCP (may block up to 10s)\n");
    nx_net_parameters_t p;
    memset(&p, 0, sizeof(p));
    p.ipv4_mode = NX_NET_DHCP;
    int r = nxNetInit(&p);
    xbox_log("mmulti_xbox: nxNetInit returned %d\n", r);
    if (r != 0) {
        /* -1 = netif_add failure (no NIC?). -2 = DHCP timeout (no cable/no DHCP server). */
        const char *why = (r == -2) ? "DHCP TIMEOUT (CHECK CABLE)"
                        : (r == -1) ? "NIC INIT FAILED"
                                    : "UNKNOWN";
        snprintf(net_status, sizeof(net_status), "NET INIT FAILED: %s", why);
        return 0;
    }
    xbox_log("mmulti_xbox: nxNetInit ok, probing own IP...\n");
    my_ip = probe_my_ip();
    if (my_ip == 0) {
        xbox_log("mmulti_xbox: probe_my_ip returned 0 (no route)\n");
        snprintf(net_status, sizeof(net_status), "NO IP ASSIGNED");
        return 0;
    }
    xbox_log("mmulti_xbox: network up, IP=%lu.%lu.%lu.%lu\n",
             (my_ip      ) & 0xFF, (my_ip >>  8) & 0xFF,
             (my_ip >> 16) & 0xFF, (my_ip >> 24) & 0xFF);
    net_initialized = 1;
    return 1;
}

static int open_socket(void)
{
    if (net_sock >= 0) { close(net_sock); net_sock = -1; }
    xbox_log("mmulti_xbox: open_socket: creating UDP socket\n");
    net_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (net_sock < 0) {
        xbox_log("mmulti_xbox: socket() failed\n");
        return 0;
    }
    int on = 1;
    setsockopt(net_sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    struct sockaddr_in me;
    memset(&me, 0, sizeof(me));
    me.sin_family      = AF_INET;
    me.sin_port        = htons(NET_PORT);
    me.sin_addr.s_addr = INADDR_ANY;
    if (bind(net_sock, (struct sockaddr *)&me, sizeof(me)) != 0) {
        xbox_log("mmulti_xbox: bind() port %d failed\n", NET_PORT);
        close(net_sock); net_sock = -1;
        return 0;
    }
    xbox_log("mmulti_xbox: open_socket: bound port %d ok\n", NET_PORT);
    return 1;
}

/* ─── Internal: finalise multiplayer state after handshake ──────────────── */

static void finalize_connection(void)
{
    /* Build Duke3D connect chain: 0 → 1 → 2 → ... → (numplayers-1) → -1 */
    networkmode = MMULTI_MODE_P2P;
    connecthead = 0;
    for (int i = 0; i < numplayers - 1; i++)
        connectpoint2[i] = i + 1;
    connectpoint2[numplayers - 1] = -1;
    net_state = NET_STATE_GAME;
    snprintf(net_status, sizeof(net_status),
             "CONNECTED! %d PLAYERS", numplayers);
    xbox_log("mmulti_xbox: game phase numplayers=%d myidx=%d\n",
             numplayers, myconnectindex);
}

/* ─── Public: set role and start lobby ──────────────────────────────────── */

void xbox_mp_set_role(int role, int max_pl)
{
    xbox_log("mmulti_xbox: xbox_mp_set_role enter role=%d max=%d\n", role, max_pl);
    net_role        = role;
    net_max_players = max_pl < 2 ? 2
                    : max_pl > MAX_NET_PLAYERS ? MAX_NET_PLAYERS : max_pl;
    net_state       = NET_STATE_IDLE;
    numplayers      = 1;
    myconnectindex  = 0;
    memset(peer_addr, 0, sizeof(peer_addr));

    if (!init_net_stack()) {
        xbox_log("mmulti_xbox: init_net_stack failed, entering ERROR state\n");
        net_state = NET_STATE_ERROR;
        return;
    }
    if (!open_socket()) {
        xbox_log("mmulti_xbox: open_socket failed, entering ERROR state\n");
        net_state = NET_STATE_ERROR;
        return;
    }
    xbox_log("mmulti_xbox: socket open, lobby ready\n");

    /* Store our own IP as player 0 (needed for the START packet payload) */
    peer_addr[0].sin_family      = AF_INET;
    peer_addr[0].sin_port        = htons(NET_PORT);
    peer_addr[0].sin_addr.s_addr = my_ip;

    if (role == NET_ROLE_HOST) {
        net_state = NET_STATE_HOSTING;
        last_announce_clock = 0;
        snprintf(net_status, sizeof(net_status),
                 "HOSTING...  PLAYERS: 1/%d", net_max_players);
        xbox_log("mmulti_xbox: hosting max=%d\n", net_max_players);
    } else {
        net_state = NET_STATE_JOINING;
        snprintf(net_status, sizeof(net_status), "SEARCHING FOR HOST...");
        xbox_log("mmulti_xbox: joining\n");
    }
}

/* Called by host lobby when the host presses Start */
void xbox_mp_host_start(void)
{
    if (net_role != NET_ROLE_HOST || net_state != NET_STATE_HOSTING) return;
    if (numplayers < 2) return;

    /* Build START packet: [PKT_START][n][ip0.0..ip0.3]...[ipN-1.0..ipN-1.3] */
    unsigned char pkt[2 + MAX_NET_PLAYERS * 4];
    int plen = 0;
    pkt[plen++] = PKT_START;
    pkt[plen++] = (unsigned char)numplayers;
    for (int i = 0; i < numplayers; i++) {
        unsigned long ip = peer_addr[i].sin_addr.s_addr;
        pkt[plen++] = (unsigned char)(ip & 0xFF);
        pkt[plen++] = (unsigned char)((ip >> 8)  & 0xFF);
        pkt[plen++] = (unsigned char)((ip >> 16) & 0xFF);
        pkt[plen++] = (unsigned char)((ip >> 24) & 0xFF);
    }
    for (int i = 1; i < numplayers; i++)
        net_sendto(&peer_addr[i], pkt, plen);

    xbox_log("mmulti_xbox: sent START to %d players\n", numplayers);
    finalize_connection();
}

/* ─── Lobby tick: call once per menu frame ──────────────────────────────── */

/* Returns 1 when lobby is complete (transition to game), 0 otherwise. */
int xbox_mp_tick(void)
{
    unsigned char buf[512];
    struct sockaddr_in src;
    socklen_t srclen;
    int n;

    if (net_state == NET_STATE_GAME)  return 1;
    if (net_state == NET_STATE_ERROR) return 0;
    if (net_state == NET_STATE_IDLE)  return 0;

    /* ── HOST: broadcast presence, collect joiners ──────────────────── */
    if (net_state == NET_STATE_HOSTING) {
        if (totalclock - last_announce_clock >= ANNOUNCE_INTERVAL) {
            unsigned char ann[3] = {
                PKT_DISCOVER,
                (unsigned char)net_max_players,
                (unsigned char)numplayers
            };
            net_broadcast(ann, 3);
            last_announce_clock = totalclock;
        }

        srclen = sizeof(src);
        while ((n = recvfrom(net_sock, (char *)buf, sizeof(buf),
                             MSG_DONTWAIT,
                             (struct sockaddr *)&src, &srclen)) > 0) {
            if (buf[0] == PKT_DISCO_REPLY && numplayers < net_max_players) {
                /* Ignore if IP already registered */
                int already = 0;
                for (int i = 1; i < numplayers; i++) {
                    if (peer_addr[i].sin_addr.s_addr == src.sin_addr.s_addr) {
                        already = 1; break;
                    }
                }
                if (!already) {
                    int slot = numplayers++;
                    peer_addr[slot] = src;
                    peer_addr[slot].sin_port = htons(NET_PORT);
                    unsigned char wp[2] = { PKT_WELCOME, (unsigned char)slot };
                    net_sendto(&peer_addr[slot], wp, 2);
                    xbox_log("mmulti_xbox: player %d joined from %08lx\n",
                             slot, (unsigned long)src.sin_addr.s_addr);
                    snprintf(net_status, sizeof(net_status),
                             "HOSTING  PLAYERS: %d/%d   A=START",
                             numplayers, net_max_players);
                }
            }
            srclen = sizeof(src);
        }
    }

    /* ── JOINER: look for host announce, reply ──────────────────────── */
    else if (net_state == NET_STATE_JOINING
          || net_state == NET_STATE_WELCOMED) {

        srclen = sizeof(src);
        while ((n = recvfrom(net_sock, (char *)buf, sizeof(buf),
                             MSG_DONTWAIT,
                             (struct sockaddr *)&src, &srclen)) > 0) {
            if (buf[0] == PKT_DISCOVER && net_state == NET_STATE_JOINING) {
                unsigned char rep[1] = { PKT_DISCO_REPLY };
                src.sin_port = htons(NET_PORT);
                net_sendto(&src, rep, 1);
                peer_addr[0] = src;  /* record host as player 0 */
                snprintf(net_status, sizeof(net_status),
                         "FOUND HOST  WAITING...");
            }
            else if (buf[0] == PKT_WELCOME && n >= 2) {
                myconnectindex = buf[1];
                unsigned char ack[1] = { PKT_WELCOME_ACK };
                net_sendto(&peer_addr[0], ack, 1);
                net_state = NET_STATE_WELCOMED;
                snprintf(net_status, sizeof(net_status),
                         "JOINED AS PLAYER %d  WAITING FOR HOST...",
                         myconnectindex + 1);
            }
            else if (buf[0] == PKT_START && n >= 2) {
                int np = buf[1];
                if (np >= 2 && np <= MAX_NET_PLAYERS) {
                    numplayers = np;
                    for (int i = 0; i < np; i++) {
                        unsigned long ip =
                            (unsigned long)buf[2 + i*4 + 0]         |
                            ((unsigned long)buf[2 + i*4 + 1] << 8)  |
                            ((unsigned long)buf[2 + i*4 + 2] << 16) |
                            ((unsigned long)buf[2 + i*4 + 3] << 24);
                        peer_addr[i].sin_family      = AF_INET;
                        peer_addr[i].sin_port        = htons(NET_PORT);
                        peer_addr[i].sin_addr.s_addr = ip;
                    }
                    finalize_connection();
                    return 1;
                }
            }
            srclen = sizeof(src);
        }
    }

    return 0;
}

/* ─── mmulti.h interface ────────────────────────────────────────────────── */

int isvalidipaddress(char *st) { (void)st; return 0; }

int initmultiplayersparms(int argc, char const * const argv[])
{
    (void)argc; (void)argv;
    return 0;
}

int initmultiplayerscycle(void)
{
    /* Lobby is handled in the menu. By the time the game calls this,
     * the connection is already established or we're single-player. */
    return 0;
}

void initmultiplayers(int argc, char const * const argv[])
{
    (void)argc; (void)argv;
    numplayers = 1; myconnectindex = 0;
    connecthead = 0; connectpoint2[0] = -1;
    networkmode = -1;
}

void setpackettimeout(int a, int b) { (void)a; (void)b; }

void uninitmultiplayers(void)
{
    if (net_sock >= 0) { close(net_sock); net_sock = -1; }
    net_state = NET_STATE_IDLE;
}

void initsingleplayers(void)
{
    /* Only reset to single-player if we haven't set up a multiplayer game
     * (either networked or local splitscreen). */
    if (net_state != NET_STATE_GAME) {
        numplayers = 1; myconnectindex = 0;
        connecthead = 0; connectpoint2[0] = -1;
    }
}

/* Called by menues.c when the user picks SPLIT SCREEN.
 * Sets up 2-player state without any network initialisation. */
void xbox_splitscreen_setup(void)
{
    extern unsigned char ready2send;
    networkmode    = 0;   /* local — no packet I/O */
    numplayers     = 2;
    myconnectindex = 0;
    connecthead    = 0;
    connectpoint2[0] = 1;
    connectpoint2[1] = -1;
    net_state  = NET_STATE_GAME; /* prevents initsingleplayers() from resetting us */
    ready2send = 1;              /* allows faketimerhandler() to push input FIFOs */
    xbox_log("mmulti_xbox: splitscreen 2-player local setup\n");
}

/* Called by menues.c when the user returns to single-player (NEW GAME / LOAD).
 * Tears down splitscreen/LAN state so initsingleplayers() can reset properly. */
void xbox_mp_teardown(void)
{
    net_state = NET_STATE_IDLE;
    numplayers = 1; myconnectindex = 0;
    connecthead = 0; connectpoint2[0] = -1;
    networkmode = -1;
}

void sendlogon(void)  {}
void sendlogoff(void) {}

int getoutputcirclesize(void) { return 0; }

void setsocket(int s) { (void)s; }

void sendpacket(int other, unsigned char *buf, int len)
{
    if (net_state != NET_STATE_GAME) return;
    if (other < 0 || other >= numplayers) return;
    if (other == myconnectindex) return;
    net_sendto(&peer_addr[other], buf, len);
}

int getpacket(int *other, unsigned char *buf)
{
    if (net_state != NET_STATE_GAME) return 0;
    struct sockaddr_in src;
    socklen_t srclen = sizeof(src);
    int n = recvfrom(net_sock, (char *)buf, 576, MSG_DONTWAIT,
                     (struct sockaddr *)&src, &srclen);
    if (n <= 0) return 0;
    /* Map source IP to player index */
    for (int i = 0; i < numplayers; i++) {
        if (peer_addr[i].sin_addr.s_addr == src.sin_addr.s_addr) {
            *other = i;
            return n;
        }
    }
    return 0;  /* unknown sender, discard */
}

void flushpackets(void) {}

void genericmultifunction(int other, unsigned char *buf,
                          int len, int cmd)
{
    (void)other; (void)buf; (void)len; (void)cmd;
}
