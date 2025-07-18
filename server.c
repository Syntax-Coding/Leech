// server.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define BUFFER_SIZE 1024

typedef struct Client {
    int  id;
    int  sock;
    char username[64];
    struct sockaddr_in addr;
    pthread_t thread;
    struct Client *next;
} Client;

static int          server_fd = -1;
static Client      *clients   = NULL;
static Client      *current   = NULL;
static pthread_mutex_t clients_mutex = PTHREAD_MUTEX_INITIALIZER;
static int          next_id   = 1;

// Graceful shutdown on Ctrl-C
void int_handler(int signo) {
    (void)signo;
    printf("\nShutting down server…\n");
    if (server_fd > 0) close(server_fd);
    exit(0);
}

// Add a new client
void add_client(Client *c) {
    pthread_mutex_lock(&clients_mutex);
    c->next = clients;
    clients = c;
    pthread_mutex_unlock(&clients_mutex);
}

// Remove & free a client
void remove_client(Client *c) {
    pthread_mutex_lock(&clients_mutex);
    Client **p = &clients;
    while (*p) {
        if (*p == c) {
            *p = c->next;
            close(c->sock);
            free(c);
            break;
        }
        p = &(*p)->next;
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Send cmd (with trailing '\n') to one client
void send_to_one(Client *c, const char *cmd) {
    if (c) send(c->sock, cmd, strlen(cmd), 0);
}

// Broadcast to all clients
void broadcast_cmd(const char *cmd) {
    pthread_mutex_lock(&clients_mutex);
    for (Client *c = clients; c; c = c->next) {
        send(c->sock, cmd, strlen(cmd), 0);
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Per-client reader thread
void *client_reader(void *arg) {
    Client *c = arg;
    char buf[BUFFER_SIZE];
    ssize_t n;
    while ((n = recv(c->sock, buf, sizeof(buf)-1, 0)) > 0) {
        buf[n] = '\0';
        printf("[#%d-%s] %s", c->id, c->username, buf);
        fflush(stdout);
    }
    printf("Client #%d (%s) disconnected\n", c->id, c->username);
    if (current == c) current = NULL;
    remove_client(c);
    return NULL;
}

// Accept loop: handshake + spawn reader
void *accept_loop(void *arg) {
    (void)arg;
    struct sockaddr_in cli_addr;
    socklen_t addrlen = sizeof(cli_addr);

    while (1) {
        int cl = accept(server_fd,
                        (struct sockaddr*)&cli_addr,
                        &addrlen);
        if (cl < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        // 1) receive username
        char uname[64];
        ssize_t r = recv(cl, uname, sizeof(uname)-1, 0);
        if (r <= 0) {
            close(cl);
            continue;
        }
        uname[r] = '\0';
        uname[strcspn(uname, "\r\n")] = 0;

        // 2) allocate client
        Client *c = malloc(sizeof(Client));
        c->id = next_id++;
        c->sock = cl;
        c->addr = cli_addr;
        strncpy(c->username, uname, sizeof(c->username)-1);
        c->username[sizeof(c->username)-1] = '\0';
        c->next = NULL;

        add_client(c);
        printf("New client #%d: %s @ %s:%d\n",
               c->id,
               c->username,
               inet_ntoa(c->addr.sin_addr),
               ntohs(c->addr.sin_port));

        pthread_create(&c->thread, NULL, client_reader, c);
        pthread_detach(c->thread);
    }
    return NULL;
}

// Print all clients
void list_clients() {
    pthread_mutex_lock(&clients_mutex);
    printf("Connected clients:\n");
    for (Client *c = clients; c; c = c->next) {
        printf("  #%d: %s @ %s:%d\n",
               c->id,
               c->username,
               inet_ntoa(c->addr.sin_addr),
               ntohs(c->addr.sin_port));
    }
    pthread_mutex_unlock(&clients_mutex);
}

// Find by numeric ID or by username
Client *find_client(const char *key) {
    if (isdigit((unsigned char)key[0])) {
        int id = atoi(key);
        for (Client *c = clients; c; c = c->next)
            if (c->id == id) return c;
    }
    for (Client *c = clients; c; c = c->next)
        if (strcmp(c->username, key) == 0)
            return c;
    return NULL;
}

int main(int argc, char *argv[]) {
    signal(SIGINT, int_handler);

    // Determine port to listen on
    int port = 8080;
    if (argc >= 2) port = atoi(argv[1]);

    // 1) create socket
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(1);
    }

    // 2) reuse addr
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#ifdef SO_REUSEPORT
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));
#endif

    // 3) bind to all interfaces
    struct sockaddr_in serv_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(port)
    };
    if (bind(server_fd,
             (struct sockaddr*)&serv_addr,
             sizeof(serv_addr)) < 0) {
        perror("bind");
        close(server_fd);
        exit(1);
    }

    // 4) listen
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        exit(1);
    }
    printf("Server listening on 0.0.0.0:%d\n", port);

    // 5) start accept thread
    pthread_t tid;
    pthread_create(&tid, NULL, accept_loop, NULL);
    pthread_detach(tid);

    // 6) command loop
    char line[BUFFER_SIZE];
    while (fgets(line, sizeof(line), stdin)) {
        line[strcspn(line, "\n")] = '\0';
        if (!*line) continue;

        if (strcmp(line, "exit") == 0) {
            broadcast_cmd("exit\n");
            sleep(1);
            break;
        }
        if (strcmp(line, "list") == 0) {
            list_clients();
            continue;
        }
        if (strncmp(line, "use ", 4) == 0) {
            Client *c = find_client(line + 4);
            if (!c) printf("No such client: %s\n", line + 4);
            else {
                current = c;
                printf("Selected #%d (%s)\n",
                       c->id, c->username);
            }
            continue;
        }
        if (strncmp(line, "all ", 4) == 0) {
            char cmd[BUFFER_SIZE];
            snprintf(cmd, sizeof(cmd), "%s\n", line + 4);
            broadcast_cmd(cmd);
            continue;
        }

        // Ensure we have a client selected
        pthread_mutex_lock(&clients_mutex);
        int cnt=0;
        for (Client *c=clients; c; c=c->next) cnt++;
        pthread_mutex_unlock(&clients_mutex);

        if (cnt>1 && !current) {
            printf("Multiple clients – use `use <id|username>` first\n");
            continue;
        }
        if (cnt==1 && !current) {
            pthread_mutex_lock(&clients_mutex);
            current = clients;
            pthread_mutex_unlock(&clients_mutex);
            printf("Auto-selected #%d (%s)\n",
                   current->id, current->username);
        }

        char cmd[BUFFER_SIZE];
        snprintf(cmd, sizeof(cmd), "%s\n", line);
        send_to_one(current, cmd);
    }

    close(server_fd);
    return 0;
}

