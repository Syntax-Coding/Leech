// client.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <pthread.h>

#define BUFFER_SIZE        1024
#define RECONNECT_INTERVAL 30

static int sock_fd = -1;
static pthread_mutex_t send_mutex = PTHREAD_MUTEX_INITIALIZER;

// Read until '\n'
static int recv_line(int sock, char *buf, int maxlen) {
    int total = 0, n;
    char c;
    while (total < maxlen-1) {
        n = recv(sock, &c, 1, 0);
        if (n <= 0) return n;
        if (c == '\n') break;
        buf[total++] = c;
    }
    buf[total] = '\0';
    return total;
}

// Handle one command
static void *handle_cmd(void *arg) {
    char *cmd = arg;
    size_t len = strlen(cmd);

    // background if ends '&'
    if (len>1 && cmd[len-1]=='&') {
        cmd[--len] = '\0';
        while (len>1 && cmd[len-1]==' ') cmd[--len] = '\0';

        pid_t pid = fork();
        if (pid<0) {
            const char *e="Error: fork failed\n";
            pthread_mutex_lock(&send_mutex);
            send(sock_fd, e, strlen(e),0);
            pthread_mutex_unlock(&send_mutex);
        }
        else if (pid==0) {
            setsid();
            execlp("xterm","xterm","-hold","-e",cmd,(char*)NULL);
            exit(1);
        } else {
            char m[BUFFER_SIZE];
            snprintf(m,sizeof(m),
                     "Background launched PID %d\n",pid);
            pthread_mutex_lock(&send_mutex);
            send(sock_fd,m,strlen(m),0);
            pthread_mutex_unlock(&send_mutex);
        }
    }
    else {
        FILE *fp = popen(cmd,"r");
        if (!fp) {
            const char *e="Error: popen failed\n";
            pthread_mutex_lock(&send_mutex);
            send(sock_fd,e,strlen(e),0);
            pthread_mutex_unlock(&send_mutex);
        } else {
            char out[BUFFER_SIZE];
            while (fgets(out,sizeof(out),fp)) {
                pthread_mutex_lock(&send_mutex);
                send(sock_fd,out,strlen(out),0);
                pthread_mutex_unlock(&send_mutex);
            }
            pclose(fp);
        }
    }

    free(cmd);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr,"Usage: %s <host> <port>\n",argv[0]);
        exit(1);
    }
    char *host = argv[1];
    char *port = argv[2];

    struct addrinfo hints={0}, *res;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    while (1) {
        if (getaddrinfo(host, port, &hints, &res)!=0) {
            fprintf(stderr,"getaddrinfo failed, retry in %d seconds…\n",
                    RECONNECT_INTERVAL);
            sleep(RECONNECT_INTERVAL);
            continue;
        }

        sock_fd = socket(res->ai_family,
                         res->ai_socktype,
                         res->ai_protocol);
        if (sock_fd<0) {
            perror("socket");
            freeaddrinfo(res);
            sleep(RECONNECT_INTERVAL);
            continue;
        }

        if (connect(sock_fd,
                    res->ai_addr,
                    res->ai_addrlen)<0) {
            perror("connect");
            close(sock_fd);
            freeaddrinfo(res);
            sleep(RECONNECT_INTERVAL);
            continue;
        }
        freeaddrinfo(res);

        printf("Connected to %s:%s\n",host,port);

        // send username handshake
        char *username = getlogin();
        if (!username) {
            struct passwd *pw = getpwuid(getuid());
            username = pw?pw->pw_name:"unknown";
        }
        char unb[BUFFER_SIZE];
        snprintf(unb,sizeof(unb),"%s\n",username);
        send(sock_fd,unb,strlen(unb),0);

        // receive commands
        char line[BUFFER_SIZE];
        while (1) {
            int n = recv_line(sock_fd,line,sizeof(line));
            if (n<=0) break;
            if (strcmp(line,"exit")==0) {
                close(sock_fd);
                return 0;
            }
            // dispatch
            char *cmd = strdup(line);
            pthread_t tid;
            pthread_create(&tid, NULL, handle_cmd, cmd);
            pthread_detach(tid);
        }

        fprintf(stderr,"Disconnected. Reconnecting in %d seconds…\n",
                RECONNECT_INTERVAL);
        close(sock_fd);
        sleep(RECONNECT_INTERVAL);
    }

    return 0;
}

