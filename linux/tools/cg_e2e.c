/*
 * cg_e2e — a throwaway Carrier member for end-to-end tests of the group service.
 *
 * One identity per data dir. It can join a group (friend request to the group
 * address), send files to the group byte for byte as ONE friend message each,
 * and record every message it receives to <outdir>/NNN.bin, so a test can
 * compare what arrived with what was sent (sha256sum / cmp).
 *
 *   cg_e2e -d DIR -a                          print this identity's address, exit
 *   cg_e2e -d DIR -n NAME -j GROUPADDR -t 60  befriend the group, wait connected
 *   cg_e2e -d DIR -g GROUPID -s f1 -s f2 -t 60 -o OUT
 *                                             send f1, f2 (2 s apart) once the
 *                                             group is online; record replies
 *   cg_e2e -d DIR -g GROUPID -t 60 -o OUT     just listen
 *
 * Build (against the SDK the service uses):
 *   cc -O2 -o cg_e2e linux/tools/cg_e2e.c \
 *      -I$SDK/src/carrier -L$SDK/dist/lib -lcarrier -lcrystal -Wl,-rpath,$SDK/dist/lib
 *
 * Never point it at a real group: it is for throwaway identities and a
 * throwaway test group only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <carrier.h>

#define MAX_SENDS 32

static const char *g_dir, *g_name, *g_join, *g_group, *g_out;
static const char *g_sends[MAX_SENDS];
static int g_nsends, g_sent, g_recv_count;
static bool g_print_addr, g_ready, g_group_online, g_killed;
static time_t g_deadline, g_next_send;
static char g_group_id[CARRIER_MAX_ID_LEN + 1];

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static unsigned char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(n > 0 ? n : 1);
    if (fread(buf, 1, n, f) != (size_t)n) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

static void on_receipt(uint32_t msgid, CarrierReceiptState state, void *ctx)
{
    printf("%.3f RECEIPT msgid=%u state=%d (%s)\n", now_s(), msgid, (int)state,
           state == CarrierReceipt_ByFriend ? "byfriend" :
           state == CarrierReceipt_Offline ? "offline" : "error");
    fflush(stdout);
}

static void on_idle(Carrier *w, void *ctx)
{
    if (g_killed) return;
    time_t t = time(NULL);
    if (t >= g_deadline) {
        g_killed = true;
        printf("%.3f DONE sent=%d received=%d\n", now_s(), g_sent, g_recv_count);
        fflush(stdout);
        carrier_kill(w);
        return;
    }
    if (g_ready && g_group_online && g_sent < g_nsends && t >= g_next_send) {
        size_t len = 0;
        unsigned char *buf = read_file(g_sends[g_sent], &len);
        if (!buf || !len) {
            printf("SEND-ERR cannot read %s\n", g_sends[g_sent]);
        } else {
            uint32_t msgid = 0;
            int rc = carrier_send_friend_message(w, g_group_id, buf, len, &msgid, on_receipt, NULL);
            printf("%.3f SENT file=%s len=%zu rc=%d err=0x%x msgid=%u\n", now_s(),
                   g_sends[g_sent], len, rc, rc ? carrier_get_error() : 0, msgid);
            free(buf);
        }
        fflush(stdout);
        g_sent++;
        g_next_send = t + 2;
    }
}

static void on_ready(Carrier *w, void *ctx)
{
    char addr[CARRIER_MAX_ADDRESS_LEN + 1];
    g_ready = true;
    carrier_get_address(w, addr, sizeof(addr));
    if (g_print_addr) {
        char uid[CARRIER_MAX_ID_LEN + 1];
        carrier_get_userid(w, uid, sizeof(uid));
        printf("ADDRESS %s\nUSERID %s\n", addr, uid);
        fflush(stdout);
        g_deadline = 0;
        return;
    }
    printf("%.3f READY\n", now_s());
    if (g_name) {
        CarrierUserInfo ui;
        carrier_get_self_info(w, &ui);
        if (strcmp(ui.name, g_name) != 0) {
            snprintf(ui.name, sizeof(ui.name), "%s", g_name);
            carrier_set_self_info(w, &ui);
        }
    }
    if (g_join) {
        int rc = carrier_add_friend(w, g_join, "cg_e2e test member");
        printf("ADD_FRIEND rc=%d err=0x%x\n", rc, rc ? carrier_get_error() : 0);
    }
    fflush(stdout);
}

static void on_friend_connection(Carrier *w, const char *friendid,
                                 CarrierConnectionStatus status, void *ctx)
{
    printf("%.3f FRIEND %s %s\n", now_s(), friendid,
           status == CarrierConnectionStatus_Connected ? "online" : "offline");
    fflush(stdout);
    if (g_group_id[0] && strcmp(friendid, g_group_id) == 0) {
        g_group_online = (status == CarrierConnectionStatus_Connected);
        if (g_group_online && g_next_send < time(NULL) + 3)
            g_next_send = time(NULL) + 3;  /* let the bot settle */
    }
}

static void on_friend_added(Carrier *w, const CarrierFriendInfo *info, void *ctx)
{
    printf("%.3f FRIEND_ADDED %s\n", now_s(), info->user_info.userid);
    fflush(stdout);
}

static void on_friend_message(Carrier *w, const char *from, const void *msg, size_t len,
                              int64_t timestamp, bool offline, void *ctx)
{
    int n = ++g_recv_count;
    int nuls = 0;
    for (size_t i = 0; i < len; i++) if (((const unsigned char *)msg)[i] == 0) nuls++;
    printf("%.3f RECV #%d from=%s len=%zu nuls=%d offline=%d head=", now_s(), n, from, len,
           nuls, offline);
    for (size_t i = 0; i < len && i < 48; i++) {
        unsigned char c = ((const unsigned char *)msg)[i];
        putchar(c >= 0x20 && c < 0x7f ? c : '.');
    }
    putchar('\n');
    fflush(stdout);
    if (g_out) {
        char path[4096];
        snprintf(path, sizeof(path), "%s/%03d.bin", g_out, n);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(msg, 1, len, f); fclose(f); }
    }
}

int main(int argc, char **argv)
{
    int opt, seconds = 60;
    while ((opt = getopt(argc, argv, "d:n:j:g:s:t:o:a")) != -1) {
        switch (opt) {
        case 'd': g_dir = optarg; break;
        case 'n': g_name = optarg; break;
        case 'j': g_join = optarg; break;
        case 'g': g_group = optarg; break;
        case 's': if (g_nsends < MAX_SENDS) g_sends[g_nsends++] = optarg; break;
        case 't': seconds = atoi(optarg); break;
        case 'o': g_out = optarg; break;
        case 'a': g_print_addr = true; break;
        default: fprintf(stderr, "bad args\n"); return 2;
        }
    }
    if (!g_dir) { fprintf(stderr, "-d DIR required\n"); return 2; }
    mkdir(g_dir, 0700);
    if (g_out) mkdir(g_out, 0700);
    if (g_group) snprintf(g_group_id, sizeof(g_group_id), "%s", g_group);
    if (g_join && !g_group_id[0])
        carrier_get_id_by_address(g_join, g_group_id, sizeof(g_group_id));
    g_deadline = time(NULL) + seconds;

    /* Same bootstrap list as the group service (CarrierConfig.cpp). */
    BootstrapNode nodes[] = {
        {"13.58.208.50", NULL, "33445", "89vny8MrKdDKs7Uta9RdVmspPjnRMdwMmaiEW27pZ7gh"},
        {"18.216.6.197", NULL, "33445", "H8sqhRrQuJZ6iLtP2wanxt4LzdNrN2NNFnpPdq1uJ9n2"},
        {"52.83.127.85", NULL, "33445", "CDkze7mJpSuFAUq6byoLmteyGYMeJ6taXxWoVvDMexWC"},
        {"52.83.127.216", NULL, "33445", "4sL3ZEriqW7pdoqHSoYXfkc1NMNpiMz7irHMMrMjp9CM"},
        {"52.83.171.135", NULL, "33445", "5tuHgK1Q4CYf4K5PutsEPK5E3Z7cbtEBdx7LwmdzqXHL"},
        {"52.83.191.228", NULL, "33445", "3khtxZo89SBScAMaHhTvD68pPHiKxgZT6hTCSZZVgNEm"},
    };
    CarrierOptions opts;
    memset(&opts, 0, sizeof(opts));
    opts.persistent_location = g_dir;
    opts.udp_enabled = true;
    opts.log_level = CarrierLogLevel_Warning;
    opts.bootstraps = nodes;
    opts.bootstraps_size = sizeof(nodes) / sizeof(nodes[0]);

    CarrierCallbacks cbs;
    memset(&cbs, 0, sizeof(cbs));
    cbs.idle = on_idle;
    cbs.ready = on_ready;
    cbs.friend_connection = on_friend_connection;
    cbs.friend_added = on_friend_added;
    cbs.friend_message = on_friend_message;

    Carrier *w = carrier_new(&opts, &cbs, NULL);
    if (!w) { fprintf(stderr, "carrier_new failed 0x%x\n", carrier_get_error()); return 1; }
    if (g_print_addr) {
        /* The address is known before the node is online. */
        char addr[CARRIER_MAX_ADDRESS_LEN + 1], uid[CARRIER_MAX_ID_LEN + 1];
        carrier_get_address(w, addr, sizeof(addr));
        carrier_get_userid(w, uid, sizeof(uid));
        printf("ADDRESS %s\nUSERID %s\n", addr, uid);
        carrier_kill(w);
        return 0;
    }
    carrier_run(w, 100);
    return 0;
}
