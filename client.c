#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <argp.h>
#include <unistd.h>
#include <time.h>
#include <netdb.h>
#include <math.h>
#include <stdbool.h>
#include <endian.h>

#define SEND_BUFFER_SIZE 24
#define RECV_BUFFER_SIZE 40

struct client_arguments
{
    char ip_address[16];
    int port;
    int reqnum;
    int timeout;
    bool condensed;
};

typedef struct
{
    uint32_t seq;
    float theta;
    float delta;
} Response;

error_t client_parser(int key, char *arg, struct argp_state *state)
{
    struct client_arguments *args = state->input;
    switch (key)
    {
    case 'a':
        strncpy(args->ip_address, arg, sizeof(args->ip_address) - 1);
        break;
    case 'p':
        args->port = atoi(arg);
        break;
    case 'n':
        args->reqnum = atoi(arg);
        break;
    case 't':
        args->timeout = atoi(arg);
        break;
    case 'c':
        args->condensed = true;
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

void client_parseopt(struct client_arguments *args, int argc, char *argv[])
{
    bzero(args, sizeof(*args));

    struct argp_option options[] = {
        {"ip_address", 'a', "addr", 0, "Server IP address", 0},
        {"port", 'p', "port", 0, "Server port (>1024)", 0},
        {"req_num", 'n', "num", 0, "Number of TimeRequests to send (>=0)", 0},
        {"timeout", 't', "timeout", 0, "Timeout (seconds, >=0)", 0},
        {"condensed", 'c', 0, 0, "Use condensed message format", 0},
        {0}};
    struct argp argp_settings = {options, client_parser, 0, 0, 0, 0, 0};

    if (argp_parse(&argp_settings, argc, argv, 0, NULL, args) != 0)
    {
        fprintf(stderr, "Error parsing arguments\n");
        exit(EXIT_FAILURE);
    }

    if (strlen(args->ip_address) == 0)
    {
        fprintf(stderr, "Error: IP address must be specified\n");
        exit(EXIT_FAILURE);
    }
    if (args->port <= 1024)
    {
        fprintf(stderr, "Error: port must be > 1024\n");
        exit(EXIT_FAILURE);
    }
    if (args->reqnum < 0 || args->timeout < 0)
    {
        fprintf(stderr, "Error: req_num and timeout must be >= 0\n");
        exit(EXIT_FAILURE);
    }

    printf("Got %s on port %d with req_num=%d timeout=%d, condensed=%d\n",
           args->ip_address, args->port, args->reqnum, args->timeout, args->condensed);
}

int main(int argc, char *argv[])
{
    struct client_arguments args;
    client_parseopt(&args, argc, argv);

    struct addrinfo hints, *serv_addr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    char port_str[16];
    sprintf(port_str, "%d", args.port);

    if (getaddrinfo(args.ip_address, port_str, &hints, &serv_addr) != 0)
    {
        perror("getaddrinfo");
        exit(EXIT_FAILURE);
    }

    int sock = socket(serv_addr->ai_family, serv_addr->ai_socktype, serv_addr->ai_protocol);
    if (sock < 0)
    {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < args.reqnum; i++)
    {
        int send_len = SEND_BUFFER_SIZE + (args.condensed ? -2 : 0);
        uint8_t buffer[send_len];

        struct timespec tspec;
        clock_gettime(CLOCK_REALTIME, &tspec);

                uint64_t sec = (uint64_t)tspec.tv_sec;
        uint64_t nsec = (uint64_t)tspec.tv_nsec;

        uint32_t seq = htonl(i + 1);
        uint64_t sec_nb = htobe64(sec);
        uint64_t nsec_nb = htobe64(nsec);

        memcpy(buffer, &seq, 4);
        if (args.condensed)
        {
            uint16_t ver = htons(7);
            memcpy(buffer + 4, &ver, 2);
            memcpy(buffer + 6, &sec_nb, 8);
            memcpy(buffer + 14, &nsec_nb, 8);
        }
        else
        {
            uint32_t ver = htonl(7);
            memcpy(buffer + 4, &ver, 4);
            memcpy(buffer + 8, &sec_nb, 8);
            memcpy(buffer + 16, &nsec_nb, 8);
        }

        sendto(sock, buffer, send_len, 0, serv_addr->ai_addr, serv_addr->ai_addrlen);
    }

    struct timeval timeout = {args.timeout, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct sockaddr_storage fromAddr;
    socklen_t fromAddrLen = sizeof(fromAddr);

    Response *responses = calloc(args.reqnum, sizeof(Response));
    if (!responses)
    {
        perror("calloc");
        exit(EXIT_FAILURE);
    }

    int recv_len = RECV_BUFFER_SIZE + (args.condensed ? -2 : 0);

    for (int i = 0; i < args.reqnum; i++)
    {
        uint8_t buffer[recv_len];
        int recvlen = recvfrom(sock, buffer, recv_len, 0,
                               (struct sockaddr *)&fromAddr, &fromAddrLen);

        if (recvlen < 0)
            break;

        struct timespec tspec;
        clock_gettime(CLOCK_REALTIME, &tspec);
        uint64_t client_sec2 = (uint64_t)tspec.tv_sec;
        uint64_t client_nsec2 = (uint64_t)tspec.tv_nsec;

        uint32_t seq;
        uint64_t client_sec1, client_nsec1, serv_sec, serv_nsec;

        if (args.condensed)
        {
            uint16_t ver;
            memcpy(&seq, buffer, 4);
            memcpy(&ver, buffer + 4, 2);
            memcpy(&client_sec1, buffer + 6, 8);
            memcpy(&client_nsec1, buffer + 14, 8);
            memcpy(&serv_sec, buffer + 22, 8);
            memcpy(&serv_nsec, buffer + 30, 8);
        }
        else
        {
            uint32_t ver;
            memcpy(&seq, buffer, 4);
            memcpy(&ver, buffer + 4, 4);
            memcpy(&client_sec1, buffer + 8, 8);
            memcpy(&client_nsec1, buffer + 16, 8);
            memcpy(&serv_sec, buffer + 24, 8);
            memcpy(&serv_nsec, buffer + 32, 8);
        }

        seq = ntohl(seq);
        client_sec1 = be64toh(client_sec1);
        client_nsec1 = be64toh(client_nsec1);
        serv_sec = be64toh(serv_sec);
        serv_nsec = be64toh(serv_nsec);

        if (seq == 0 || seq > (uint32_t)args.reqnum)
            continue;

        long double t0 = client_sec1 + client_nsec1 / 1e9L;
        long double t1 = serv_sec + serv_nsec / 1e9L;
        long double t2 = client_sec2 + client_nsec2 / 1e9L;

        responses[seq - 1].seq = seq;
        responses[seq - 1].theta = (float)(((t1 - t0) + (t1 - t2)) / 2.0);
        responses[seq - 1].delta = (float)(t2 - t0);
    }

    close(sock);

    for (int i = 0; i < args.reqnum; i++)
    {
        if (responses[i].seq == 0)
            printf("%d: Dropped\n", i + 1);
        else
            printf("%d: %.4f %.4f\n", i + 1, responses[i].theta, responses[i].delta);
    }

    free(responses);
    freeaddrinfo(serv_addr);
    return 0;
}
