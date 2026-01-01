#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <time.h>
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
#include <netdb.h>
#include <endian.h>
#include <math.h>
#include <stdbool.h>

#define SEND_BUFFER_SIZE 40
#define RECV_BUFFER_SIZE 24

struct server_arguments
{
    int port;
    int drop_percent;
    bool condensed;
};

typedef struct listnode
{
    time_t last_update;
    int seq;
    char addr[1025];
    char port[1025];
    struct listnode *next;
} Node;

error_t server_parser(int key, char *arg, struct argp_state *state)
{
    struct server_arguments *args = state->input;
    switch (key)
    {
    case 'p':
        args->port = atoi(arg);
        break;
    case 'd':
        args->drop_percent = atoi(arg);
        break;
    case 'c':
        args->condensed = true;
        break;
    default:
        return ARGP_ERR_UNKNOWN;
    }
    return 0;
}

void server_parseopt(struct server_arguments *args, int argc, char *argv[])
{
    bzero(args, sizeof(*args));

    struct argp_option options[] = {
        {"port", 'p', "port", 0, "Server port (>1024)", 0},
        {"drop", 'd', "drop", 0, "Drop percentage [0–100]", 0},
        {"condensed", 'c', 0, 0, "Use condensed message format", 0},
        {0}};
    struct argp argp_settings = {options, server_parser, 0, 0, 0, 0, 0};

    if (argp_parse(&argp_settings, argc, argv, 0, NULL, args) != 0)
    {
        fprintf(stderr, "Error parsing arguments\n");
        exit(EXIT_FAILURE);
    }

    if (args->port <= 1024)
    {
        fprintf(stderr, "Error: port must be > 1024\n");
        exit(EXIT_FAILURE);
    }
    if (args->drop_percent < 0 || args->drop_percent > 100)
    {
        fprintf(stderr, "Error: drop rate must be 0–100\n");
        exit(EXIT_FAILURE);
    }
}

Node *update_clients(Node *head, char *addr, char *port, int new_seq, time_t cur_time)
{
    Node *current = head;
    Node *prev = NULL;

    while (current)
    {
        if (difftime(cur_time, current->last_update) >= 120)
        {
            current->seq = 0;
            current->last_update = cur_time;
        }

        if (!strcmp(current->addr, addr) && !strcmp(current->port, port))
        {
            if (new_seq < current->seq)
            {
                printf("%s:%s %d %d\n", current->addr, current->port, new_seq, current->seq);
            }
            current->seq = new_seq;
            current->last_update = cur_time;
            return head;
        }
        prev = current;
        current = current->next;
    }

    Node *newnode = calloc(1, sizeof(Node));
    newnode->seq = new_seq;
    newnode->last_update = cur_time;
    strcpy(newnode->addr, addr);
    strcpy(newnode->port, port);
    newnode->next = NULL;

    if (prev)
        prev->next = newnode;
    else
        head = newnode;

    return head;
}

int main(int argc, char *argv[])
{
    struct server_arguments args;
    server_parseopt(&args, argc, argv);
    fprintf(stderr, "Running server on port %d (drop=%d%%, condensed=%d)\n",
            args.port, args.drop_percent, args.condensed);

    struct addrinfo hints, *serv_addr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_PASSIVE;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    char port_str[16];
    sprintf(port_str, "%d", args.port);
    if (getaddrinfo(NULL, port_str, &hints, &serv_addr) != 0)
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

    if (bind(sock, serv_addr->ai_addr, serv_addr->ai_addrlen) < 0)
    {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    freeaddrinfo(serv_addr);

    srand(time(NULL));
    Node *clients = NULL;

    int recv_len = RECV_BUFFER_SIZE + (args.condensed ? -2 : 0);
    int send_len = SEND_BUFFER_SIZE + (args.condensed ? -2 : 0);

    while (1)
    {
        struct sockaddr_storage client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        uint8_t buffer[recv_len];

        ssize_t bytes = recvfrom(sock, buffer, recv_len, 0,
                                 (struct sockaddr *)&client_addr, &client_addr_len);
        if (bytes < 0)
            continue;

        int random = rand() % 101;
        if (random < args.drop_percent)
            continue;

        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);

        char addr_str[NI_MAXHOST], port_str[NI_MAXSERV];
        getnameinfo((struct sockaddr *)&client_addr, client_addr_len,
                    addr_str, sizeof(addr_str), port_str, sizeof(port_str),
                    NI_NUMERICHOST | NI_NUMERICSERV);

        uint32_t client_seq;
        uint64_t csec_nb, cnsec_nb; // Keep these in network byte order

        if (args.condensed)
        {
            memcpy(&client_seq, buffer, 4);
            client_seq = ntohl(client_seq);
            // Extract the values temporarily
            uint64_t csec_temp, cnsec_temp;
            memcpy(&csec_temp, buffer + 6, 8);
            memcpy(&cnsec_temp, buffer + 14, 8);

            memcpy(&csec_nb, buffer + 6, 8);
            memcpy(&cnsec_nb, buffer + 14, 8);
        }
        else
        {
            memcpy(&client_seq, buffer, 4);
            client_seq = ntohl(client_seq);

            memcpy(&csec_nb, buffer + 8, 8);
            memcpy(&cnsec_nb, buffer + 16, 8);
        }

        clients = update_clients(clients, addr_str, port_str, client_seq, ts.tv_sec);

        // Prepare server timestamps in network byte order
        uint64_t ssec = htobe64(ts.tv_sec);
        uint64_t snsec = htobe64(ts.tv_nsec);

        uint8_t outbuf[send_len];

        if (args.condensed)
        {
            uint16_t ver16 = htons(7);
            uint32_t seqn = htonl(client_seq);
            memcpy(outbuf, &seqn, 4);
            memcpy(outbuf + 4, &ver16, 2);
            memcpy(outbuf + 6, &csec_nb, 8);
            memcpy(outbuf + 14, &cnsec_nb, 8);
            memcpy(outbuf + 22, &ssec, 8);
            memcpy(outbuf + 30, &snsec, 8);
        }
        else
        {
            uint32_t ver32 = htonl(7);
            uint32_t seqn = htonl(client_seq);
            memcpy(outbuf, &seqn, 4);
            memcpy(outbuf + 4, &ver32, 4);
            memcpy(outbuf + 8, &csec_nb, 8);
            memcpy(outbuf + 16, &cnsec_nb, 8);
            memcpy(outbuf + 24, &ssec, 8);
            memcpy(outbuf + 32, &snsec, 8);
        }

        sendto(sock, outbuf, send_len, 0,
               (struct sockaddr *)&client_addr, client_addr_len);
    }
}
