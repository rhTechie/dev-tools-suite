#include <SylixOS.h>
#include <arpa/inet.h>
#include <errno.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <netpacket/packet.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define APP_NAME                    "eth_dual_selftest"

#define TEST_ETHERTYPE              0x88B5u
#define TEST_MAGIC                  0x45544852u
#define TEST_VERSION                0x0001u

#define DIRECTION_A_TO_B            1u
#define DIRECTION_B_TO_A            2u

#define DEFAULT_IFACE_A             "en1"
#define DEFAULT_IFACE_B             "en2"
#define DEFAULT_MODE                "bench"

#define DEFAULT_SMALL_COUNT         4000u
#define DEFAULT_SMALL_PAYLOAD       1472u
#define DEFAULT_SMALL_GAP_US        50u

#define DEFAULT_BIDIR_COUNT         3000u
#define DEFAULT_BIDIR_PAYLOAD       1472u
#define DEFAULT_BIDIR_GAP_US        20u

#define DEFAULT_TIMEOUT_MS          2000u
#define DEFAULT_DURATION_SEC        0u

#define MAX_PAYLOAD_LEN             9000u
#define MIN_PAYLOAD_LEN             64u
#define SEND_RETRY_MAX              8
#define PACKET_POLL_US              100000
#define EXTRA_WIRE_BYTES            24u
#define SOCKET_BUF_SIZE             (32 * 1024 * 1024)

typedef enum test_mode {
    TEST_MODE_VERIFY = 0,
    TEST_MODE_BENCH = 1,
} test_mode_t;

typedef struct iface_stats {
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
} iface_stats_t;

typedef struct iface_ctx {
    char ifname[IFNAMSIZ];
    int ifindex;
    int ctrl_sock;
    int rx_sock;
    int tx_sock;
    int mtu;
    short ifflags;
    unsigned char mac[ETHER_ADDR_LEN];
    unsigned int link_speed_mbps;
} iface_ctx_t;

typedef struct phase_spec {
    const char *name;
    uint16_t phase_id;
    uint32_t packet_limit;
    uint32_t payload_len;
    uint32_t gap_us;
    uint32_t timeout_ms;
    uint32_t duration_sec;
    test_mode_t mode;
    int bidirectional;
} phase_spec_t;

typedef struct phase_control {
    uint64_t stop_ms;
    volatile uint32_t sent_packets;
    volatile int sender_done;
    volatile uint64_t sender_done_ms;
} phase_control_t;

typedef struct recv_task {
    iface_ctx_t *iface;
    const phase_spec_t *phase;
    phase_control_t *control;
    const unsigned char *expect_src_mac;
    const unsigned char *expect_dst_mac;
    uint16_t direction;

    unsigned char *seen_bitmap;
    uint32_t bitmap_bits;
    uint32_t unique_packets;
    uint32_t duplicate_packets;
    uint32_t out_of_order_packets;
    uint32_t bad_packets;
    uint32_t short_packets;
    uint32_t recv_errors;

    uint64_t valid_bytes;
    uint64_t first_valid_ms;
    uint64_t last_valid_ms;
    uint64_t elapsed_ms;
    uint64_t max_inter_gap_ms;
    uint32_t max_seq_seen;
    int has_seq_seen;
} recv_task_t;

typedef struct send_task {
    iface_ctx_t *iface;
    const phase_spec_t *phase;
    phase_control_t *control;
    const unsigned char *dst_mac;
    uint16_t direction;

    uint32_t packets_sent;
    uint32_t send_errors;
    uint64_t valid_bytes;
    uint64_t elapsed_ms;
} send_task_t;

typedef struct cli_options {
    char iface_a[IFNAMSIZ];
    char iface_b[IFNAMSIZ];
    uint32_t small_count;
    uint32_t small_payload;
    uint32_t small_gap_us;
    uint32_t timeout_ms;
    uint32_t duration_sec;
    test_mode_t mode;
} cli_options_t;

typedef struct phase_dir_result {
    uint32_t packets_sent;
    uint32_t unique_packets;
    uint32_t duplicate_packets;
    uint32_t out_of_order_packets;
    uint32_t bad_packets;
    uint32_t short_packets;
    uint32_t recv_errors;
    uint32_t send_errors;
    uint64_t tx_bytes;
    uint64_t tx_elapsed_ms;
    uint64_t rx_bytes;
    uint64_t rx_elapsed_ms;
    uint64_t max_inter_gap_ms;
    iface_stats_t src_delta;
    iface_stats_t dst_delta;
    iface_stats_t lo_delta;
    int status;
} phase_dir_result_t;

typedef struct iface_summary {
    const char *ifname;
    uint64_t tx_bytes;
    uint64_t tx_elapsed_ms;
    uint64_t rx_bytes;
    uint64_t rx_elapsed_ms;
} iface_summary_t;

struct selftest_frame_hdr {
    uint32_t magic;
    uint16_t version;
    uint16_t phase_id;
    uint16_t direction;
    uint32_t seq;
    uint32_t total_packets;
    uint32_t payload_len;
    uint32_t pattern_seed;
} __attribute__((packed));

static void init_send_task(send_task_t *task, iface_ctx_t *iface, const phase_spec_t *phase,
                           phase_control_t *control, const unsigned char *dst_mac,
                           uint16_t direction);
static void print_phase_theory(const phase_spec_t *phase, const char *tag,
                               unsigned int link_speed_mbps);

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static void apply_thread_perf_hint(int cpu_index, int priority)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu_index, &set);
    pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    pthread_setschedprio(pthread_self(), priority);
}

static const char *mode_name(test_mode_t mode)
{
    return (mode == TEST_MODE_BENCH) ? "bench" : "verify";
}

static void format_ifflags(short flags, char *buf, size_t buf_len)
{
    int first = 1;

    buf[0] = '\0';

    if (flags & IFF_UP) {
        snprintf(buf + strlen(buf), buf_len - strlen(buf), "%sUP", first ? "" : "|");
        first = 0;
    }
    if (flags & IFF_BROADCAST) {
        snprintf(buf + strlen(buf), buf_len - strlen(buf), "%sBCAST", first ? "" : "|");
        first = 0;
    }
    if (flags & IFF_RUNNING) {
        snprintf(buf + strlen(buf), buf_len - strlen(buf), "%sRUNNING", first ? "" : "|");
        first = 0;
    }
    if (flags & IFF_MULTICAST) {
        snprintf(buf + strlen(buf), buf_len - strlen(buf), "%sMCAST", first ? "" : "|");
        first = 0;
    }
    if (flags & IFF_LOOPBACK) {
        snprintf(buf + strlen(buf), buf_len - strlen(buf), "%sLOOP", first ? "" : "|");
        first = 0;
    }
    if (flags & IFF_PROMISC) {
        snprintf(buf + strlen(buf), buf_len - strlen(buf), "%sPROMISC", first ? "" : "|");
        first = 0;
    }

    if (first) {
        snprintf(buf, buf_len, "none");
    }
}

static int parse_mode(const char *text, test_mode_t *mode)
{
    if (!strcmp(text, "verify")) {
        *mode = TEST_MODE_VERIFY;
        return 0;
    }
    if (!strcmp(text, "bench")) {
        *mode = TEST_MODE_BENCH;
        return 0;
    }

    return -1;
}

static void format_mac(const unsigned char *mac, char *buf, size_t buf_len)
{
    snprintf(buf, buf_len, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static int bitmap_test(const unsigned char *bitmap, uint32_t index)
{
    return !!(bitmap[index >> 3] & (1u << (index & 7u)));
}

static void bitmap_set(unsigned char *bitmap, uint32_t index)
{
    bitmap[index >> 3] |= (unsigned char)(1u << (index & 7u));
}

static int ensure_bitmap_capacity(recv_task_t *task, uint32_t index)
{
    uint32_t need_bits;
    uint32_t new_bits;
    size_t old_bytes;
    size_t new_bytes;
    unsigned char *new_bitmap;

    if (index < task->bitmap_bits) {
        return 0;
    }

    need_bits = index + 1u;
    new_bits = (task->bitmap_bits == 0u) ? 1024u : task->bitmap_bits;
    while (new_bits < need_bits) {
        if (new_bits > 0x7fffffffu) {
            return -1;
        }
        new_bits <<= 1;
    }

    old_bytes = (size_t)((task->bitmap_bits + 7u) / 8u);
    new_bytes = (size_t)((new_bits + 7u) / 8u);

    new_bitmap = (unsigned char *)realloc(task->seen_bitmap, new_bytes);
    if (new_bitmap == NULL) {
        return -1;
    }

    memset(new_bitmap + old_bytes, 0, new_bytes - old_bytes);
    task->seen_bitmap = new_bitmap;
    task->bitmap_bits = new_bits;
    return 0;
}

static void fill_payload_normal(unsigned char *payload, uint32_t len, uint32_t seed)
{
    uint32_t i;

    for (i = 0; i < len; ++i) {
        payload[i] = (unsigned char)((seed + (i * 29u) + (i >> 1)) & 0xffu);
    }
}

static void fill_payload_stress(unsigned char *payload, uint32_t len)
{
    memset(payload, 0x5a, len);
}

static int verify_payload(const phase_spec_t *phase, const unsigned char *payload,
                          uint32_t len, uint32_t seed)
{
    uint32_t i;

    if (phase->mode == TEST_MODE_BENCH) {
        return 0;
    }

    for (i = 0; i < len; ++i) {
        if (payload[i] != (unsigned char)((seed + (i * 29u) + (i >> 1)) & 0xffu)) {
            return -1;
        }
    }

    return 0;
}

static double bytes_to_mbytes_per_sec(uint64_t bytes, uint64_t elapsed_ms)
{
    if (bytes == 0u || elapsed_ms == 0u) {
        return 0.0;
    }

    return ((double)bytes * 1000.0) / (double)elapsed_ms / 1024.0 / 1024.0;
}

static double bytes_to_mbits_per_sec(uint64_t bytes, uint64_t elapsed_ms)
{
    if (bytes == 0u || elapsed_ms == 0u) {
        return 0.0;
    }

    return ((double)bytes * 8.0 * 1000.0) / (double)elapsed_ms / 1000000.0;
}

static uint32_t frame_bytes(const phase_spec_t *phase)
{
    return (uint32_t)sizeof(struct ether_header) +
           (uint32_t)sizeof(struct selftest_frame_hdr) +
           phase->payload_len;
}

static double theory_normal_mbits_per_sec(const phase_spec_t *phase)
{
    if (phase->gap_us == 0u) {
        return 0.0;
    }

    return ((double)frame_bytes(phase) * 8.0) / (double)phase->gap_us;
}

static double theory_stress_mbits_per_sec(const phase_spec_t *phase, unsigned int link_speed_mbps)
{
    double app_bytes;
    double wire_bytes;

    if (link_speed_mbps == 0u) {
        return 0.0;
    }

    app_bytes = (double)frame_bytes(phase);
    wire_bytes = app_bytes + EXTRA_WIRE_BYTES;

    return (double)link_speed_mbps * (app_bytes / wire_bytes);
}

static unsigned int get_eth_speed(const char *ifname)
{
    FILE *fp;
    char cmd[128];
    char line[256];
    char *pos;
    unsigned int speed;

    speed = 0u;

    snprintf(cmd, sizeof(cmd), "ifconfig %s", ifname);
    fp = popen(cmd, "r");
    if (fp == NULL) {
        return 0u;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        pos = strstr(line, "Spd:");
        if (pos != NULL) {
            speed = (unsigned int)strtoul(pos + 4, NULL, 10);
            break;
        }
    }

    pclose(fp);
    return speed;
}

static int get_ifconfig_stats(const char *ifname, iface_stats_t *stats)
{
    FILE *fp;
    char cmd[128];
    char line[256];
    uint64_t ucast;
    uint64_t nucast;

    memset(stats, 0, sizeof(*stats));

    snprintf(cmd, sizeof(cmd), "ifconfig %s", ifname);
    fp = popen(cmd, "r");
    if (fp == NULL) {
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        if (sscanf(line, " RX ucast packets:%llu nucast packets:%llu",
                   (unsigned long long *)&ucast,
                   (unsigned long long *)&nucast) == 2) {
            stats->rx_packets = ucast + nucast;
        } else if (sscanf(line, " TX ucast packets:%llu nucast packets:%llu",
                          (unsigned long long *)&ucast,
                          (unsigned long long *)&nucast) == 2) {
            stats->tx_packets = ucast + nucast;
        } else if (sscanf(line, " RX bytes:%llu",
                          (unsigned long long *)&stats->rx_bytes) == 1) {
            ;
        } else if (sscanf(line, " TX bytes:%llu",
                          (unsigned long long *)&stats->tx_bytes) == 1) {
            ;
        }
    }

    pclose(fp);
    return 0;
}

static void diff_stats(iface_stats_t *delta, const iface_stats_t *after, const iface_stats_t *before)
{
    delta->rx_packets = (after->rx_packets >= before->rx_packets) ? (after->rx_packets - before->rx_packets) : 0u;
    delta->tx_packets = (after->tx_packets >= before->tx_packets) ? (after->tx_packets - before->tx_packets) : 0u;
    delta->rx_bytes = (after->rx_bytes >= before->rx_bytes) ? (after->rx_bytes - before->rx_bytes) : 0u;
    delta->tx_bytes = (after->tx_bytes >= before->tx_bytes) ? (after->tx_bytes - before->tx_bytes) : 0u;
}

static void print_usage(void)
{
    printf("Usage: %s [options]\n", APP_NAME);
    printf("Options:\n");
    printf("  -a <ifname>        source/destination port A, default %s\n", DEFAULT_IFACE_A);
    printf("  -b <ifname>        source/destination port B, default %s\n", DEFAULT_IFACE_B);
    printf("  -m <mode>          test mode: verify|bench, default %s\n", DEFAULT_MODE);
    printf("  -sc <count>        single-direction packet count in count mode, default %u\n", DEFAULT_SMALL_COUNT);
    printf("  -sl <bytes>        single-direction payload bytes, default %u, max %u\n",
           DEFAULT_SMALL_PAYLOAD, MAX_PAYLOAD_LEN);
    printf("  -sg <usec>         single-direction send gap in verify mode, default %u\n", DEFAULT_SMALL_GAP_US);
    printf("  -t  <msec>         receiver quiet timeout after sender stops, default %u\n", DEFAULT_TIMEOUT_MS);
    printf("  -d  <sec>          phase duration seconds, 0 means count mode, default %u\n", DEFAULT_DURATION_SEC);
    printf("  -h                 show this help\n");
}

static int parse_u32(const char *text, uint32_t *value)
{
    char *endp;
    unsigned long parsed;

    errno = 0;
    parsed = strtoul(text, &endp, 0);
    if ((errno != 0) || (endp == text) || (*endp != '\0')) {
        return -1;
    }
    if (parsed > 0xffffffffUL) {
        return -1;
    }

    *value = (uint32_t)parsed;
    return 0;
}

static void set_default_options(cli_options_t *opt)
{
    memset(opt, 0, sizeof(*opt));
    strncpy(opt->iface_a, DEFAULT_IFACE_A, sizeof(opt->iface_a) - 1);
    strncpy(opt->iface_b, DEFAULT_IFACE_B, sizeof(opt->iface_b) - 1);
    opt->small_count = DEFAULT_SMALL_COUNT;
    opt->small_payload = DEFAULT_SMALL_PAYLOAD;
    opt->small_gap_us = DEFAULT_SMALL_GAP_US;
    opt->timeout_ms = DEFAULT_TIMEOUT_MS;
    opt->duration_sec = DEFAULT_DURATION_SEC;
    opt->mode = TEST_MODE_BENCH;
}

static int parse_args(int argc, char **argv, cli_options_t *opt)
{
    int i;

    set_default_options(opt);

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "-a") && (i + 1 < argc)) {
            strncpy(opt->iface_a, argv[++i], sizeof(opt->iface_a) - 1);
        } else if (!strcmp(argv[i], "-b") && (i + 1 < argc)) {
            strncpy(opt->iface_b, argv[++i], sizeof(opt->iface_b) - 1);
        } else if (!strcmp(argv[i], "-m") && (i + 1 < argc)) {
            if (parse_mode(argv[++i], &opt->mode) < 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "-sc") && (i + 1 < argc)) {
            if (parse_u32(argv[++i], &opt->small_count) < 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "-sl") && (i + 1 < argc)) {
            if (parse_u32(argv[++i], &opt->small_payload) < 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "-sg") && (i + 1 < argc)) {
            if (parse_u32(argv[++i], &opt->small_gap_us) < 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "-t") && (i + 1 < argc)) {
            if (parse_u32(argv[++i], &opt->timeout_ms) < 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "-d") && (i + 1 < argc)) {
            if (parse_u32(argv[++i], &opt->duration_sec) < 0) {
                return -1;
            }
        } else if (!strcmp(argv[i], "-h")) {
            print_usage();
            return 1;
        } else {
            return -1;
        }
    }

    if (!strcmp(opt->iface_a, opt->iface_b)) {
        fprintf(stderr, "interface A and B must be different.\n");
        return -1;
    }
    if ((opt->small_payload < MIN_PAYLOAD_LEN) || (opt->small_payload > MAX_PAYLOAD_LEN) ||
        0) {
        fprintf(stderr, "payload length must be between %u and %u bytes.\n",
                MIN_PAYLOAD_LEN, MAX_PAYLOAD_LEN);
        return -1;
    }
    if ((opt->small_count == 0u) || (opt->timeout_ms == 0u)) {
        fprintf(stderr, "packet count and timeout must be non-zero.\n");
        return -1;
    }
    if ((opt->mode == TEST_MODE_VERIFY) && (opt->small_gap_us == 0u)) {
        fprintf(stderr, "verify mode requires non-zero send gaps.\n");
        return -1;
    }

    return 0;
}

static int get_ifflags(int ctrl_sock, const char *ifname, short *flags)
{
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);

    if (ioctl(ctrl_sock, SIOCGIFFLAGS, &ifr) < 0) {
        return -1;
    }

    *flags = ifr.ifr_flags;
    return 0;
}

static int get_ifhwaddr(int ctrl_sock, const char *ifname, unsigned char *mac)
{
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);

    if (ioctl(ctrl_sock, SIOCGIFHWADDR, &ifr) < 0) {
        return -1;
    }

    memcpy(mac, ifr.ifr_hwaddr.sa_data, ETHER_ADDR_LEN);
    return 0;
}

static int get_ifmtu(int ctrl_sock, const char *ifname, int *mtu)
{
    struct ifreq ifr;

    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);

    if (ioctl(ctrl_sock, SIOCGIFMTU, &ifr) < 0) {
        return -1;
    }

    *mtu = ifr.ifr_mtu;
    return 0;
}

static int packet_socket_bind(int sockfd, int ifindex)
{
    struct sockaddr_ll sll;

    memset(&sll, 0, sizeof(sll));
    sll.sll_len = sizeof(sll);
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(TEST_ETHERTYPE);
    sll.sll_ifindex = ifindex;

    if (bind(sockfd, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        return -1;
    }

    return 0;
}

static int open_interface(iface_ctx_t *iface, const char *ifname, int need_rx)
{
    int buf_size;
    struct timeval timeout;

    memset(iface, 0, sizeof(*iface));
    iface->ctrl_sock = -1;
    iface->rx_sock = -1;
    iface->tx_sock = -1;

    strncpy(iface->ifname, ifname, sizeof(iface->ifname) - 1);

    iface->ifindex = (int)if_nametoindex(ifname);
    if (iface->ifindex <= 0) {
        fprintf(stderr, "%s: if_nametoindex failed for %s.\n", APP_NAME, ifname);
        return -1;
    }

    iface->ctrl_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (iface->ctrl_sock < 0) {
        fprintf(stderr, "%s: create control socket for %s failed: %s\n",
                APP_NAME, ifname, strerror(errno));
        return -1;
    }

    if (get_ifflags(iface->ctrl_sock, ifname, &iface->ifflags) < 0) {
        fprintf(stderr, "%s: get flags for %s failed: %s\n",
                APP_NAME, ifname, strerror(errno));
        return -1;
    }
    if (!(iface->ifflags & IFF_UP) || !(iface->ifflags & IFF_RUNNING)) {
        fprintf(stderr, "%s: interface %s is not up/running.\n", APP_NAME, ifname);
        return -1;
    }

    if (get_ifhwaddr(iface->ctrl_sock, ifname, iface->mac) < 0) {
        fprintf(stderr, "%s: get MAC for %s failed: %s\n",
                APP_NAME, ifname, strerror(errno));
        return -1;
    }

    if (get_ifmtu(iface->ctrl_sock, ifname, &iface->mtu) < 0) {
        fprintf(stderr, "%s: get MTU for %s failed: %s\n",
                APP_NAME, ifname, strerror(errno));
        return -1;
    }

    iface->link_speed_mbps = get_eth_speed(ifname);

    if (need_rx) {
        iface->rx_sock = socket(AF_PACKET, SOCK_DGRAM, htons(TEST_ETHERTYPE));
        if (iface->rx_sock < 0) {
            fprintf(stderr, "%s: create RX raw socket for %s failed: %s\n",
                    APP_NAME, ifname, strerror(errno));
            return -1;
        }

        if (packet_socket_bind(iface->rx_sock, iface->ifindex) < 0) {
            fprintf(stderr, "%s: bind RX raw socket for %s failed: %s\n",
                    APP_NAME, ifname, strerror(errno));
            return -1;
        }

        buf_size = SOCKET_BUF_SIZE;
        setsockopt(iface->rx_sock, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
        timeout.tv_sec = 0;
        timeout.tv_usec = PACKET_POLL_US;
        setsockopt(iface->rx_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    }

    iface->tx_sock = socket(AF_PACKET, SOCK_DGRAM, htons(TEST_ETHERTYPE));
    if (iface->tx_sock < 0) {
        fprintf(stderr, "%s: create TX raw socket for %s failed: %s\n",
                APP_NAME, ifname, strerror(errno));
        return -1;
    }

    if (packet_socket_bind(iface->tx_sock, iface->ifindex) < 0) {
        fprintf(stderr, "%s: bind TX raw socket for %s failed: %s\n",
                APP_NAME, ifname, strerror(errno));
        return -1;
    }

    setsockopt(iface->tx_sock, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    return 0;
}

static void close_interface(iface_ctx_t *iface)
{
    if (iface->tx_sock >= 0) {
        close(iface->tx_sock);
        iface->tx_sock = -1;
    }
    if (iface->rx_sock >= 0) {
        close(iface->rx_sock);
        iface->rx_sock = -1;
    }
    if (iface->ctrl_sock >= 0) {
        close(iface->ctrl_sock);
        iface->ctrl_sock = -1;
    }
}

static void drain_packet_socket(int sockfd)
{
    if (sockfd < 0) {
        return;
    }
    for (;;) {
        fd_set rfds;
        struct timeval timeout;
        unsigned char buf[2048];
        int ready;

        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);
        timeout.tv_sec = 0;
        timeout.tv_usec = 0;

        ready = select(sockfd + 1, &rfds, NULL, NULL, &timeout);
        if (ready <= 0) {
            break;
        }

        if (recv(sockfd, buf, sizeof(buf), 0) <= 0) {
            break;
        }
    }
}

static void *recv_worker(void *arg)
{
    recv_task_t *task;
    unsigned char packet_buf[sizeof(struct selftest_frame_hdr) + MAX_PAYLOAD_LEN];

    task = (recv_task_t *)arg;
    if (task->direction == DIRECTION_A_TO_B) {
        apply_thread_perf_hint(3, 250);
    } else {
        apply_thread_perf_hint(2, 250);
    }

    while (1) {
        struct sockaddr_ll from;
        socklen_t from_len;
        ssize_t rv;
        uint64_t now;
        uint64_t packet_time_ms;
        uint32_t sender_sent_packets;
        struct selftest_frame_hdr *hdr;
        uint16_t phase_id;
        uint16_t direction;
        uint32_t seq;
        uint32_t total_packets;
        uint32_t payload_len;
        uint32_t pattern_seed;
        unsigned char *payload;

        now = now_ms();
        if (task->control->sender_done) {
            sender_sent_packets = task->control->sent_packets;
            if ((sender_sent_packets > 0u) && (task->unique_packets >= sender_sent_packets)) {
                break;
            }
            if (now >= task->control->sender_done_ms + task->phase->timeout_ms) {
                break;
            }
        }

        if (task->phase->mode == TEST_MODE_BENCH) {
            rv = recv(task->iface->rx_sock, packet_buf, sizeof(packet_buf), 0);
        } else {
            memset(&from, 0, sizeof(from));
            from_len = sizeof(from);
            rv = recvfrom(task->iface->rx_sock, packet_buf, sizeof(packet_buf), 0,
                          (struct sockaddr *)&from, &from_len);
        }
        if (rv < 0) {
            if ((errno == EINTR) || (errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                continue;
            }
            task->recv_errors++;
            continue;
        }

        if ((size_t)rv < sizeof(struct selftest_frame_hdr)) {
            task->short_packets++;
            continue;
        }

        if (task->phase->mode == TEST_MODE_VERIFY) {
            if ((from.sll_hatype != ARPHRD_ETHER) ||
                (from.sll_halen != ETHER_ADDR_LEN) ||
                (from.sll_protocol != htons(TEST_ETHERTYPE)) ||
                (memcmp(from.sll_addr, task->expect_src_mac, ETHER_ADDR_LEN) != 0)) {
                task->bad_packets++;
                continue;
            }
        }

        hdr = (struct selftest_frame_hdr *)packet_buf;
        if (ntohl(hdr->magic) != TEST_MAGIC) {
            task->bad_packets++;
            continue;
        }
        if (ntohs(hdr->version) != TEST_VERSION) {
            task->bad_packets++;
            continue;
        }

        phase_id = ntohs(hdr->phase_id);
        direction = ntohs(hdr->direction);
        if ((phase_id != task->phase->phase_id) || (direction != task->direction)) {
            continue;
        }

        seq = ntohl(hdr->seq);
        total_packets = ntohl(hdr->total_packets);
        payload_len = ntohl(hdr->payload_len);
        pattern_seed = ntohl(hdr->pattern_seed);

        if ((payload_len != task->phase->payload_len) ||
            ((uint32_t)rv != sizeof(struct selftest_frame_hdr) + payload_len)) {
            task->bad_packets++;
            continue;
        }
        if ((task->phase->duration_sec == 0u) &&
            (total_packets != task->phase->packet_limit)) {
            task->bad_packets++;
            continue;
        }

        payload = packet_buf + sizeof(struct selftest_frame_hdr);
        if (verify_payload(task->phase, payload, payload_len, pattern_seed) < 0) {
            task->bad_packets++;
            continue;
        }

        if (task->phase->mode == TEST_MODE_VERIFY) {
            if (ensure_bitmap_capacity(task, seq) < 0) {
                task->bad_packets++;
                continue;
            }

            if (bitmap_test(task->seen_bitmap, seq)) {
                task->duplicate_packets++;
                continue;
            }
        }

        packet_time_ms = now_ms();
        if (task->unique_packets == 0u) {
            task->first_valid_ms = packet_time_ms;
        } else if (packet_time_ms >= task->last_valid_ms) {
            uint64_t gap_ms = packet_time_ms - task->last_valid_ms;
            if (gap_ms > task->max_inter_gap_ms) {
                task->max_inter_gap_ms = gap_ms;
            }
        }

        task->last_valid_ms = packet_time_ms;
        task->valid_bytes += (uint64_t)(rv + sizeof(struct ether_header));
        task->unique_packets++;
        if (task->phase->mode == TEST_MODE_VERIFY) {
            bitmap_set(task->seen_bitmap, seq);

            if (task->has_seq_seen && (seq < task->max_seq_seen)) {
                task->out_of_order_packets++;
            }
            if ((!task->has_seq_seen) || (seq > task->max_seq_seen)) {
                task->max_seq_seen = seq;
                task->has_seq_seen = 1;
            }
        }
    }

    if (task->unique_packets > 0u) {
        if (task->last_valid_ms > task->first_valid_ms) {
            task->elapsed_ms = task->last_valid_ms - task->first_valid_ms;
        } else {
            task->elapsed_ms = 1u;
        }
    }

    return NULL;
}

static int send_one_packet(send_task_t *task, uint32_t seq)
{
    unsigned char packet_buf[sizeof(struct selftest_frame_hdr) + MAX_PAYLOAD_LEN];
    struct sockaddr_ll to;
    struct selftest_frame_hdr *hdr;
    unsigned char *payload;
    uint32_t seed;
    uint32_t packet_len;
    uint32_t total_packets;
    int retry;
    ssize_t rv;

    seed = (task->phase->phase_id << 20) ^ (task->direction << 16) ^ seq;
    total_packets = (task->phase->duration_sec == 0u) ? task->phase->packet_limit : 0u;

    hdr = (struct selftest_frame_hdr *)packet_buf;
    hdr->magic = htonl(TEST_MAGIC);
    hdr->version = htons(TEST_VERSION);
    hdr->phase_id = htons(task->phase->phase_id);
    hdr->direction = htons(task->direction);
    hdr->seq = htonl(seq);
    hdr->total_packets = htonl(total_packets);
    hdr->payload_len = htonl(task->phase->payload_len);
    hdr->pattern_seed = htonl(seed);

    payload = packet_buf + sizeof(struct selftest_frame_hdr);
    if (task->phase->mode == TEST_MODE_BENCH) {
        fill_payload_stress(payload, task->phase->payload_len);
    } else {
        fill_payload_normal(payload, task->phase->payload_len, seed);
    }

    packet_len = sizeof(struct selftest_frame_hdr) + task->phase->payload_len;

    memset(&to, 0, sizeof(to));
    to.sll_len = sizeof(to);
    to.sll_family = AF_PACKET;
    to.sll_protocol = htons(TEST_ETHERTYPE);
    to.sll_ifindex = task->iface->ifindex;
    to.sll_hatype = ARPHRD_ETHER;
    to.sll_halen = ETHER_ADDR_LEN;
    memcpy(to.sll_addr, task->dst_mac, ETHER_ADDR_LEN);

    retry = 0;
    while (retry < SEND_RETRY_MAX) {
        rv = sendto(task->iface->tx_sock, packet_buf, packet_len, 0,
                    (struct sockaddr *)&to, sizeof(to));
        if (rv == (ssize_t)packet_len) {
            task->valid_bytes += (uint64_t)(rv + sizeof(struct ether_header));
            return 0;
        }
        if ((errno == EAGAIN) || (errno == ENOBUFS) || (errno == EINTR)) {
            usleep(1000);
            retry++;
            continue;
        }
        break;
    }

    return -1;
}

static void *send_worker(void *arg)
{
    send_task_t *task;
    uint32_t seq;
    uint64_t start_ms;
    uint64_t stop_ms;

    task = (send_task_t *)arg;
    if (task->direction == DIRECTION_A_TO_B) {
        apply_thread_perf_hint(0, 250);
    } else {
        apply_thread_perf_hint(1, 250);
    }
    seq = 0u;
    start_ms = now_ms();
    stop_ms = task->control->stop_ms;

    if (task->phase->duration_sec > 0u) {
        while (now_ms() < stop_ms) {
            if (send_one_packet(task, seq) < 0) {
                task->send_errors++;
                task->elapsed_ms = now_ms() - start_ms;
                task->control->sent_packets = task->packets_sent;
                task->control->sender_done_ms = now_ms();
                task->control->sender_done = 1;
                return NULL;
            }

            task->packets_sent++;
            seq++;

            if ((task->phase->mode == TEST_MODE_VERIFY) && (task->phase->gap_us != 0u)) {
                usleep(task->phase->gap_us);
            }
        }
    } else {
        while (seq < task->phase->packet_limit) {
            if (send_one_packet(task, seq) < 0) {
                task->send_errors++;
                task->elapsed_ms = now_ms() - start_ms;
                task->control->sent_packets = task->packets_sent;
                task->control->sender_done_ms = now_ms();
                task->control->sender_done = 1;
                return NULL;
            }

            task->packets_sent++;
            seq++;

            if ((task->phase->mode == TEST_MODE_VERIFY) && (task->phase->gap_us != 0u)) {
                usleep(task->phase->gap_us);
            }
        }
    }

    task->elapsed_ms = now_ms() - start_ms;
    if ((task->elapsed_ms == 0u) && (task->packets_sent > 0u)) {
        task->elapsed_ms = 1u;
    }

    task->control->sent_packets = task->packets_sent;
    task->control->sender_done_ms = now_ms();
    task->control->sender_done = 1;
    return NULL;
}

static void fill_phase_result(phase_dir_result_t *result,
                              const send_task_t *sender,
                              const recv_task_t *receiver,
                              const iface_stats_t *src_before,
                              const iface_stats_t *src_after,
                              const iface_stats_t *dst_before,
                              const iface_stats_t *dst_after,
                              const iface_stats_t *lo_before,
                              const iface_stats_t *lo_after)
{
    memset(result, 0, sizeof(*result));
    result->packets_sent = sender->packets_sent;
    result->unique_packets = receiver->unique_packets;
    result->duplicate_packets = receiver->duplicate_packets;
    result->out_of_order_packets = receiver->out_of_order_packets;
    result->bad_packets = receiver->bad_packets;
    result->short_packets = receiver->short_packets;
    result->recv_errors = receiver->recv_errors;
    result->send_errors = sender->send_errors;
    result->tx_bytes = sender->valid_bytes;
    result->tx_elapsed_ms = sender->elapsed_ms;
    result->rx_bytes = receiver->valid_bytes;
    result->rx_elapsed_ms = receiver->elapsed_ms;
    result->max_inter_gap_ms = receiver->max_inter_gap_ms;

    diff_stats(&result->src_delta, src_after, src_before);
    diff_stats(&result->dst_delta, dst_after, dst_before);
    diff_stats(&result->lo_delta, lo_after, lo_before);

    if (sender->phase->mode == TEST_MODE_BENCH) {
        result->status = 0;
    } else {
        result->status = ((sender->send_errors == 0u) &&
                          (receiver->unique_packets == sender->packets_sent) &&
                          (receiver->duplicate_packets == 0u) &&
                          (receiver->bad_packets == 0u) &&
                          (receiver->recv_errors == 0u) &&
                          (result->src_delta.tx_packets > 0u) &&
                          (result->dst_delta.rx_packets > 0u) &&
                          (result->lo_delta.tx_packets == 0u) &&
                          (result->lo_delta.rx_packets == 0u)) ? 0 : -1;
    }
}

static int run_bench_phase(const phase_spec_t *phase,
                           iface_ctx_t *src, iface_ctx_t *dst,
                           uint16_t direction, const char *tag,
                           phase_dir_result_t *result)
{
    send_task_t sender;
    phase_control_t control;
    iface_stats_t src_before;
    iface_stats_t src_after;
    iface_stats_t dst_before;
    iface_stats_t dst_after;
    iface_stats_t lo_before;
    iface_stats_t lo_after;
    pthread_t tx_tid;
    int ret;

    memset(&control, 0, sizeof(control));
    memset(&src_before, 0, sizeof(src_before));
    memset(&src_after, 0, sizeof(src_after));
    memset(&dst_before, 0, sizeof(dst_before));
    memset(&dst_after, 0, sizeof(dst_after));
    memset(&lo_before, 0, sizeof(lo_before));
    memset(&lo_after, 0, sizeof(lo_after));

    if (get_ifconfig_stats(src->ifname, &src_before) < 0 ||
        get_ifconfig_stats(dst->ifname, &dst_before) < 0 ||
        get_ifconfig_stats("lo0", &lo_before) < 0) {
        fprintf(stderr, "%s: failed to read interface stats before bench phase.\n", APP_NAME);
        return -1;
    }

    init_send_task(&sender, src, phase, &control, dst->mac, direction);

    print_phase_theory(phase, tag,
                       (src->link_speed_mbps && dst->link_speed_mbps) ?
                       ((src->link_speed_mbps < dst->link_speed_mbps) ? src->link_speed_mbps : dst->link_speed_mbps) :
                       (src->link_speed_mbps ? src->link_speed_mbps : dst->link_speed_mbps));

    if (phase->duration_sec > 0u) {
        control.stop_ms = now_ms() + ((uint64_t)phase->duration_sec * 1000ULL);
    }

    ret = pthread_create(&tx_tid, NULL, send_worker, &sender);
    if (ret != 0) {
        fprintf(stderr, "%s: create TX thread failed: %d\n", APP_NAME, ret);
        return -1;
    }

    pthread_join(tx_tid, NULL);
    usleep(100000);

    if (get_ifconfig_stats(src->ifname, &src_after) < 0 ||
        get_ifconfig_stats(dst->ifname, &dst_after) < 0 ||
        get_ifconfig_stats("lo0", &lo_after) < 0) {
        fprintf(stderr, "%s: failed to read interface stats after bench phase.\n", APP_NAME);
        return -1;
    }

    memset(result, 0, sizeof(*result));
    result->packets_sent = sender.packets_sent;
    result->send_errors = sender.send_errors;
    result->tx_bytes = sender.valid_bytes;
    result->tx_elapsed_ms = sender.elapsed_ms;
    diff_stats(&result->src_delta, &src_after, &src_before);
    diff_stats(&result->dst_delta, &dst_after, &dst_before);
    diff_stats(&result->lo_delta, &lo_after, &lo_before);
    result->unique_packets = (uint32_t)result->dst_delta.rx_packets;
    result->rx_bytes = result->dst_delta.rx_bytes ? result->dst_delta.rx_bytes :
                       (uint64_t)result->unique_packets * (uint64_t)frame_bytes(phase);
    result->rx_elapsed_ms = sender.elapsed_ms;
    result->status = 0;
    return 0;
}

static void print_phase_theory(const phase_spec_t *phase, const char *tag,
                               unsigned int link_speed_mbps)
{
    double theo_mbps;
    double theo_mbs;

    if (phase->mode == TEST_MODE_VERIFY) {
        theo_mbps = theory_normal_mbits_per_sec(phase);
        theo_mbs = theo_mbps / 8.0 / 1024.0 / 1024.0 * 1000000.0;
        printf("[%s] %s theory: verify paced %.2fMB/s(%.2fMb/s), frame=%uB, gap=%uus\n",
               phase->name, tag, theo_mbs, theo_mbps, frame_bytes(phase), phase->gap_us);
    } else {
        theo_mbps = theory_stress_mbits_per_sec(phase, link_speed_mbps);
        theo_mbs = theo_mbps * 1000000.0 / 8.0 / 1024.0 / 1024.0;
        if (link_speed_mbps > 0u) {
            printf("[%s] %s theory: bench line-limit about %.2fMB/s(%.2fMb/s), link=%uMb/s, frame=%uB\n",
                   phase->name, tag, theo_mbs, theo_mbps, link_speed_mbps, frame_bytes(phase));
        } else {
            printf("[%s] %s theory: bench mode tries to saturate the link, frame=%uB, link speed unknown\n",
                   phase->name, tag, frame_bytes(phase));
        }
    }
}

static void print_phase_report(const char *tag, const phase_dir_result_t *result)
{
    uint32_t lost_packets;
    double tx_mbps;
    double rx_mbps;

    lost_packets = result->packets_sent - result->unique_packets;
    tx_mbps = bytes_to_mbits_per_sec(result->tx_bytes, result->tx_elapsed_ms);
    rx_mbps = bytes_to_mbits_per_sec(result->rx_bytes, result->rx_elapsed_ms);

    printf("%s", tag);
    if (result->status == 0) {
        printf(" PASS");
    } else {
        printf(" FAIL");
    }
    printf(" | tx %.2fMb/s rx %.2fMb/s | sent %u recv %u lost %u | gap %llums | src tx+%llu dst rx+%llu lo0+%llu\n",
           tx_mbps, rx_mbps,
           result->packets_sent, result->unique_packets, lost_packets,
           (unsigned long long)result->max_inter_gap_ms,
           (unsigned long long)result->src_delta.tx_packets,
           (unsigned long long)result->dst_delta.rx_packets,
           (unsigned long long)(result->lo_delta.tx_packets + result->lo_delta.rx_packets));
    if ((result->status != 0) &&
        ((result->duplicate_packets != 0u) || (result->out_of_order_packets != 0u) ||
         (result->bad_packets != 0u) || (result->short_packets != 0u) ||
         (result->recv_errors != 0u) || (result->send_errors != 0u))) {
        printf("  detail: tx_err=%u dup=%u oo=%u bad=%u short=%u recv_err=%u\n",
               result->send_errors, result->duplicate_packets,
               result->out_of_order_packets, result->bad_packets,
               result->short_packets, result->recv_errors);
    }
}

static int init_recv_task(recv_task_t *task, iface_ctx_t *iface, const phase_spec_t *phase,
                          phase_control_t *control, const unsigned char *src_mac,
                          const unsigned char *dst_mac, uint16_t direction)
{
    memset(task, 0, sizeof(*task));
    task->iface = iface;
    task->phase = phase;
    task->control = control;
    task->expect_src_mac = src_mac;
    task->expect_dst_mac = dst_mac;
    task->direction = direction;
    task->bitmap_bits = 1024u;
    task->seen_bitmap = (unsigned char *)calloc(1, (size_t)(task->bitmap_bits / 8u));
    if (task->seen_bitmap == NULL) {
        fprintf(stderr, "%s: alloc bitmap failed for phase %s.\n", APP_NAME, phase->name);
        return -1;
    }

    return 0;
}

static void deinit_recv_task(recv_task_t *task)
{
    if (task->seen_bitmap != NULL) {
        free(task->seen_bitmap);
        task->seen_bitmap = NULL;
    }
}

static void init_send_task(send_task_t *task, iface_ctx_t *iface, const phase_spec_t *phase,
                           phase_control_t *control, const unsigned char *dst_mac,
                           uint16_t direction)
{
    memset(task, 0, sizeof(*task));
    task->iface = iface;
    task->phase = phase;
    task->control = control;
    task->dst_mac = dst_mac;
    task->direction = direction;
}

static int run_unidirectional_phase(const phase_spec_t *phase,
                                    iface_ctx_t *src, iface_ctx_t *dst,
                                    uint16_t direction, const char *tag,
                                    phase_dir_result_t *result)
{
    recv_task_t receiver;
    send_task_t sender;
    phase_control_t control;
    iface_stats_t src_before;
    iface_stats_t src_after;
    iface_stats_t dst_before;
    iface_stats_t dst_after;
    iface_stats_t lo_before;
    iface_stats_t lo_after;
    pthread_t rx_tid;
    pthread_t tx_tid;
    int ret;

    memset(&control, 0, sizeof(control));
    drain_packet_socket(dst->rx_sock);

    if (get_ifconfig_stats(src->ifname, &src_before) < 0 ||
        get_ifconfig_stats(dst->ifname, &dst_before) < 0 ||
        get_ifconfig_stats("lo0", &lo_before) < 0) {
        fprintf(stderr, "%s: failed to read interface stats before phase.\n", APP_NAME);
        return -1;
    }

    if (init_recv_task(&receiver, dst, phase, &control, src->mac, dst->mac, direction) < 0) {
        return -1;
    }
    init_send_task(&sender, src, phase, &control, dst->mac, direction);

    print_phase_theory(phase, tag,
                       (src->link_speed_mbps && dst->link_speed_mbps) ?
                       ((src->link_speed_mbps < dst->link_speed_mbps) ? src->link_speed_mbps : dst->link_speed_mbps) :
                       (src->link_speed_mbps ? src->link_speed_mbps : dst->link_speed_mbps));

    ret = pthread_create(&rx_tid, NULL, recv_worker, &receiver);
    if (ret != 0) {
        fprintf(stderr, "%s: create RX thread failed: %d\n", APP_NAME, ret);
        deinit_recv_task(&receiver);
        return -1;
    }

    usleep(100000);
    if (phase->duration_sec > 0u) {
        control.stop_ms = now_ms() + ((uint64_t)phase->duration_sec * 1000ULL);
    }

    ret = pthread_create(&tx_tid, NULL, send_worker, &sender);
    if (ret != 0) {
        fprintf(stderr, "%s: create TX thread failed: %d\n", APP_NAME, ret);
        control.sender_done_ms = now_ms();
        control.sender_done = 1;
        pthread_join(rx_tid, NULL);
        deinit_recv_task(&receiver);
        return -1;
    }

    pthread_join(tx_tid, NULL);
    pthread_join(rx_tid, NULL);

    if (get_ifconfig_stats(src->ifname, &src_after) < 0 ||
        get_ifconfig_stats(dst->ifname, &dst_after) < 0 ||
        get_ifconfig_stats("lo0", &lo_after) < 0) {
        fprintf(stderr, "%s: failed to read interface stats after phase.\n", APP_NAME);
        deinit_recv_task(&receiver);
        return -1;
    }

    fill_phase_result(result, &sender, &receiver,
                      &src_before, &src_after,
                      &dst_before, &dst_after,
                      &lo_before, &lo_after);
    ret = result->status;

    deinit_recv_task(&receiver);
    return ret;
}

static void accumulate_summary(iface_summary_t *summary, uint64_t tx_bytes, uint64_t tx_elapsed_ms,
                               uint64_t rx_bytes, uint64_t rx_elapsed_ms)
{
    summary->tx_bytes += tx_bytes;
    summary->tx_elapsed_ms += tx_elapsed_ms;
    summary->rx_bytes += rx_bytes;
    summary->rx_elapsed_ms += rx_elapsed_ms;
}

static void print_iface_summary(const iface_summary_t *summary)
{
    printf("[%s] tx_bytes=%llu tx_time=%llums tx_speed=%.2fMB/s(%.2fMb/s)\n",
           summary->ifname,
           (unsigned long long)summary->tx_bytes,
           (unsigned long long)summary->tx_elapsed_ms,
           bytes_to_mbytes_per_sec(summary->tx_bytes, summary->tx_elapsed_ms),
           bytes_to_mbits_per_sec(summary->tx_bytes, summary->tx_elapsed_ms));
    printf("[%s] rx_bytes=%llu rx_time=%llums rx_speed=%.2fMB/s(%.2fMb/s)\n",
           summary->ifname,
           (unsigned long long)summary->rx_bytes,
           (unsigned long long)summary->rx_elapsed_ms,
           bytes_to_mbytes_per_sec(summary->rx_bytes, summary->rx_elapsed_ms),
           bytes_to_mbits_per_sec(summary->rx_bytes, summary->rx_elapsed_ms));
}

int main(int argc, char **argv)
{
    cli_options_t opt;
    iface_ctx_t iface_a;
    iface_ctx_t iface_b;
    phase_spec_t phase_small_a2b;
    phase_spec_t phase_small_b2a;
    phase_dir_result_t result_small_a2b;
    phase_dir_result_t result_small_b2a;
    iface_summary_t summary_a;
    iface_summary_t summary_b;
    char mac_a[32];
    char mac_b[32];
    char flags_a[96];
    char flags_b[96];
    int ret;

    ret = parse_args(argc, argv, &opt);
    if (ret != 0) {
        if (ret < 0) {
            print_usage();
        }
        return (ret < 0) ? 2 : 0;
    }

    if (open_interface(&iface_a, opt.iface_a, opt.mode == TEST_MODE_VERIFY) < 0) {
        return 1;
    }
    if (open_interface(&iface_b, opt.iface_b, opt.mode == TEST_MODE_VERIFY) < 0) {
        close_interface(&iface_a);
        return 1;
    }

    format_mac(iface_a.mac, mac_a, sizeof(mac_a));
    format_mac(iface_b.mac, mac_b, sizeof(mac_b));
    format_ifflags(iface_a.ifflags, flags_a, sizeof(flags_a));
    format_ifflags(iface_b.ifflags, flags_b, sizeof(flags_b));

    printf("%s: single-board dual-port raw Ethernet self-test\n", APP_NAME);
    printf("  mode=%s\n", mode_name(opt.mode));
    printf("  iface_a=%s mac=%s ifindex=%d mtu=%d flags=0x%04x(%s) link=%uMb/s\n",
           iface_a.ifname, mac_a, iface_a.ifindex, iface_a.mtu,
           (unsigned)iface_a.ifflags, flags_a, iface_a.link_speed_mbps);
    printf("  iface_b=%s mac=%s ifindex=%d mtu=%d flags=0x%04x(%s) link=%uMb/s\n",
           iface_b.ifname, mac_b, iface_b.ifindex, iface_b.mtu,
           (unsigned)iface_b.ifflags, flags_b, iface_b.link_speed_mbps);
    printf("  note: connect %s and %s directly with a cable; this mode uses raw L2 frames and validates interface counters instead of host-route delivery.\n",
           iface_a.ifname, iface_b.ifname);

    {
        int common_mtu;
        uint32_t max_raw_payload;

        common_mtu = (iface_a.mtu < iface_b.mtu) ? iface_a.mtu : iface_b.mtu;
        if (common_mtu <= (int)sizeof(struct selftest_frame_hdr)) {
            fprintf(stderr, "%s: invalid common MTU %d.\n", APP_NAME, common_mtu);
            close_interface(&iface_b);
            close_interface(&iface_a);
            return 1;
        }

        max_raw_payload = (uint32_t)common_mtu - (uint32_t)sizeof(struct selftest_frame_hdr);
        printf("  raw payload limit from MTU: %u bytes (common MTU %d - header %zu)\n",
               max_raw_payload, common_mtu, sizeof(struct selftest_frame_hdr));

        if (opt.small_payload > max_raw_payload) {
            fprintf(stderr,
                    "%s: requested payload exceeds raw L2 MTU limit. current max=%u, requested=%u. "
                    "For raw mode, payload 8192 requires MTU at least %u.\n",
                    APP_NAME, max_raw_payload, opt.small_payload,
                    8192u + (uint32_t)sizeof(struct selftest_frame_hdr));
            close_interface(&iface_b);
            close_interface(&iface_a);
            return 1;
        }
    }

    memset(&phase_small_a2b, 0, sizeof(phase_small_a2b));
    phase_small_a2b.name = "phase1_small_a2b";
    phase_small_a2b.phase_id = 1u;
    phase_small_a2b.packet_limit = opt.small_count;
    phase_small_a2b.payload_len = opt.small_payload;
    phase_small_a2b.gap_us = opt.small_gap_us;
    phase_small_a2b.timeout_ms = opt.timeout_ms;
    phase_small_a2b.duration_sec = opt.duration_sec;
    phase_small_a2b.mode = opt.mode;

    memset(&phase_small_b2a, 0, sizeof(phase_small_b2a));
    phase_small_b2a.name = "phase2_small_b2a";
    phase_small_b2a.phase_id = 2u;
    phase_small_b2a.packet_limit = opt.small_count;
    phase_small_b2a.payload_len = opt.small_payload;
    phase_small_b2a.gap_us = opt.small_gap_us;
    phase_small_b2a.timeout_ms = opt.timeout_ms;
    phase_small_b2a.duration_sec = opt.duration_sec;
    phase_small_b2a.mode = opt.mode;

    memset(&summary_a, 0, sizeof(summary_a));
    memset(&summary_b, 0, sizeof(summary_b));
    summary_a.ifname = iface_a.ifname;
    summary_b.ifname = iface_b.ifname;

    if (opt.duration_sec > 0u) {
    printf("  duration mode: each enabled phase runs about %us\n", opt.duration_sec);
    } else {
        printf("  count mode: packets=%u\n", phase_small_a2b.packet_limit);
    }

    ret = 0;

    printf("\nRunning %s...\n", phase_small_a2b.name);
    if (opt.mode == TEST_MODE_BENCH) {
        if (run_bench_phase(&phase_small_a2b, &iface_a, &iface_b,
                            DIRECTION_A_TO_B, "A -> B", &result_small_a2b) < 0) {
            ret = -1;
        }
    } else {
        if (run_unidirectional_phase(&phase_small_a2b, &iface_a, &iface_b,
                                     DIRECTION_A_TO_B, "A -> B", &result_small_a2b) < 0) {
            ret = -1;
        }
    }
    print_phase_report("A -> B", &result_small_a2b);
    accumulate_summary(&summary_a, result_small_a2b.tx_bytes, result_small_a2b.tx_elapsed_ms, 0u, 0u);
    accumulate_summary(&summary_b, 0u, 0u, result_small_a2b.rx_bytes, result_small_a2b.rx_elapsed_ms);

    printf("\nRunning %s...\n", phase_small_b2a.name);
    if (opt.mode == TEST_MODE_BENCH) {
        if (run_bench_phase(&phase_small_b2a, &iface_b, &iface_a,
                            DIRECTION_B_TO_A, "B -> A", &result_small_b2a) < 0) {
            ret = -1;
        }
    } else {
        if (run_unidirectional_phase(&phase_small_b2a, &iface_b, &iface_a,
                                     DIRECTION_B_TO_A, "B -> A", &result_small_b2a) < 0) {
            ret = -1;
        }
    }
    print_phase_report("B -> A", &result_small_b2a);
    accumulate_summary(&summary_b, result_small_b2a.tx_bytes, result_small_b2a.tx_elapsed_ms, 0u, 0u);
    accumulate_summary(&summary_a, 0u, 0u, result_small_b2a.rx_bytes, result_small_b2a.rx_elapsed_ms);

    printf("\nInterface summary:\n");
    print_iface_summary(&summary_a);
    print_iface_summary(&summary_b);

    close_interface(&iface_b);
    close_interface(&iface_a);

    if (opt.mode == TEST_MODE_BENCH) {
        printf("\nRESULT: DONE\n");
        return 0;
    }

    if (ret == 0) {
        printf("\nRESULT: PASS\n");
        return 0;
    }

    printf("\nRESULT: FAIL\n");
    return 1;
}
