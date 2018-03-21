#include "common.c"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <string.h>
#include <libgen.h>
#include <signal.h>
#include <fcntl.h>
#include <assert.h>
#include <math.h>


#define GOTTHARD_MAX_OP  7

#define TYPE_REQ   0
#define TYPE_RES   1

#define STATUS_OK   0
#define STATUS_ABORT   1
#define STATUS_OPTIMISTIC_ABORT   2
#define STATUS_BADREQ   3

#define TXN_NOP       0
#define TXN_READ      1 // request: I would like to get the value of this obj
#define TXN_WRITE     2 // request: write this value to the object
#define TXN_VALUE     3 // fact: this is what (I think) the value is
#define TXN_UPDATED   4 // response: the object was just updated to this value
#define TXN_CPU_PCT   5 // request/response for CPU usage since last measurement

#define MAX_KEYS      65536 // 2^16
#define VALUE_SIZE    16
#define STORE_SIZE    MAX_KEYS * VALUE_SIZE


#define BUFSIZE 2048

char *dump_filename = 0;
char *recover_filename = 0;

char listen_hostname[256];

char value_store[STORE_SIZE];

char *progname;

int verbosity = 0;

int sockfd = 0;

void load_store() {
    FILE *fh = fopen(recover_filename, "rb");
    if (!fh)
        error("load_store fopen");
    if (!fread(value_store, STORE_SIZE, 1, fh))
        error("load_store fread");
    if (fclose(fh) != 0)
        error("load_store fclose");

    if (verbosity > 0)
        printf("Recovered store from %s\n", recover_filename);
}

void reset_store() {
    if (recover_filename)
        load_store();
    else
        bzero(value_store, STORE_SIZE);
}

void dump_store() {
    fprintf(stderr, "\nDumping store... ");
    FILE *fh = fopen(dump_filename, "wb");
    if (!fh)
        error("dump_store fopen");
    if (!fwrite(value_store, STORE_SIZE, 1, fh))
        error("dump_store fwrite");
    if (fclose(fh) != 0)
        error("dump_store fclose");
    fprintf(stderr, "OK\n");
}

void usage(int rc) {
    fprintf(rc == 0 ? stdout : stderr,
            "Usage: %s [-v VERBOSITY] [-o OPTIONS] [-r FILE] [-d FILE] [[LISTEN_HOST:]PORT]\n\
\n\
    -r FILE     Recover the store from this file.\n\
    -d FILE     Dump the store to this file on exit.\n\
\n\
OPTIONS is a string of chars, which can include:\n\
\n\
\n\
", progname);
    exit(rc);
}


void cleanup_and_exit() {
    if (dump_filename)
        dump_store();

    fprintf(stderr, "\nExiting\n");
    exit(0);
}

void catch_int(int signo) {
    cleanup_and_exit();
}


struct __attribute__((__packed__)) gotthard_hdr {
    uint8_t flags; // msg_type, from_switch, reset, store_commit
    uint32_t cl_id;
    uint32_t req_id;
    uint8_t frag_seq;
    uint8_t frag_cnt;
    uint8_t status;
    uint8_t op_cnt;
};

struct __attribute__((__packed__)) gotthard_op {
    uint8_t op_type;
    uint32_t key;
    char value[VALUE_SIZE];
};


int main(int argc, char *argv[]) {
    int opt;
    char *listen_host_port = 0;
    strcpy(listen_hostname, "127.0.0.1");
    int port;
    short host_ok, port_ok;
    char *log_filename = 0;
    char *extra_options = 0;
    struct gotthard_hdr *req, *res;
    struct gotthard_op *op, *res_op;

    progname = basename(argv[0]);

    while ((opt = getopt(argc, argv, "hv:o:d:r:")) != -1) {
        switch (opt) {
            case 'o':
                extra_options = optarg;
                break;
            case 'v':
                verbosity = atoi(optarg);
                break;
            case 'd':
                dump_filename = optarg;
                break;
            case 'r':
                recover_filename = optarg;
                break;
            case 'h':
                usage(0);
            default: /* '?' */
                usage(-1);
        }
    }

    if (argc - optind > 1)
        usage(-1);
    else if (argc - optind == 1)
        listen_host_port = argv[optind];

    if (listen_host_port) {
        int parsed_port;
        parse_host_port(listen_host_port, 1, listen_hostname, &host_ok, &port, &port_ok);
        if (!port_ok)
            port = 1234;
    }

    if (extra_options) {
        if (strchr(extra_options, 'q')) {
        }
    }

    if (recover_filename)
        load_store();

    signal(SIGINT, catch_int);

    int i, size;
    struct sockaddr_in localaddr;
    struct sockaddr_in remoteaddr;
    char buf[BUFSIZE];

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        error("socket()");

    int optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
            (const void *)&optval , sizeof(int));

    bzero((char *)&localaddr, sizeof(localaddr));
    localaddr.sin_family = AF_INET;
    localaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    localaddr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&localaddr, sizeof(localaddr)) < 0)
        error("bind()");

    if (verbosity > 0)
        fprintf(stderr, "Listenning on port %d\n", port);

    int remoteaddr_len = sizeof(remoteaddr);
    uint8_t bad_read, type_flag, reset_flag;
    uint32_t key;
    char *my_val;
    char res_buf[BUFSIZE];
    res = (struct gotthard_hdr *)res_buf;

    while (1) {
        bad_read = 0;
        type_flag = 0;
        reset_flag = 0;

        size = recvfrom(sockfd, buf, BUFSIZE, 0,
                (struct sockaddr *)&remoteaddr, &remoteaddr_len);
        if (size < 0)
            error("recvfrom()");

        req = (struct gotthard_hdr *)buf;
        type_flag = req->flags >> 7 & 1;
        reset_flag = req->flags >> 5 & 1;

        assert(type_flag == TYPE_REQ && "Should only receive REQuests");

        res->op_cnt = 0;
        res->cl_id = req->cl_id;
        res->req_id = req->req_id;
        res->frag_seq = 1;
        res->frag_cnt = 1;
        res->flags = 0;
        res->flags = 1 << 7; // RES flag
        res->status = STATUS_OK;

        if (reset_flag)
            reset_store();

        for (i = 0; i < req->op_cnt; i++) {
            op = (struct gotthard_op *)(buf + sizeof(struct gotthard_hdr) + i*sizeof(struct gotthard_op));
            if (op->op_type != TXN_VALUE) continue;
            key = ntohl(op->key);
            my_val = &value_store[key * VALUE_SIZE];
            if (memcmp(op->value, my_val, VALUE_SIZE) != 0) {
                bad_read = 1;
                res_op = (struct gotthard_op *)(res_buf + sizeof(struct gotthard_hdr) + res->op_cnt*sizeof(struct gotthard_op));
                res_op->op_type = TXN_VALUE;
                res_op->key = op->key;
                memcpy(res_op->value, my_val, VALUE_SIZE);
                res->op_cnt++;
            }
        }

        if (bad_read) {
            res->status = STATUS_ABORT;
        }
        else {
            // Apply writes and do reads
            for (i = 0; i < req->op_cnt; i++) {
                if (op->op_type == TXN_WRITE) {
                    key = ntohl(op->key);
                    //printf("write key %d\n", key);
                    my_val = &value_store[key * VALUE_SIZE];
                    memcpy(my_val, op->value, VALUE_SIZE);

                    res_op = (struct gotthard_op *)(res_buf + sizeof(struct gotthard_hdr) + res->op_cnt*sizeof(struct gotthard_op));
                    res_op->op_type = TXN_UPDATED;
                    res_op->key = op->key;
                    memcpy(res_op->value, op->value, VALUE_SIZE);
                    res->op_cnt++;
                }
                else if (op->op_type == TXN_READ) {
                    key = ntohl(op->key);
                    //printf("read key %d\n", key);
                    my_val = &value_store[key * VALUE_SIZE];

                    res_op = (struct gotthard_op *)(res_buf + sizeof(struct gotthard_hdr) + res->op_cnt*sizeof(struct gotthard_op));
                    res_op->op_type = TXN_VALUE;
                    res_op->key = op->key;
                    memcpy(res_op->value, my_val, VALUE_SIZE);
                    res->op_cnt++;
                }
            }
        }

        size = sizeof(struct gotthard_hdr) + res->op_cnt*sizeof(struct gotthard_op);
        if (sendto(sockfd, res_buf, size, 0, (struct sockaddr *)&remoteaddr, sizeof(remoteaddr)) < 0)
            error("sendto");

    }

    cleanup_and_exit();

    return 0;
}
