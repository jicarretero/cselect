#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>

/*
* 3  - 0=stdin, 1=stdout, 2=stderr, 3 socket accepting connections
*/
#define MIN_SOCKET_FD   3 
#define LISTEN_BACKLOG   10
#define MAX_BUFFER 4096
#define SERVER_PORT 4141
#define MAX_SELECTOR 1024
#define LOCAL_BUFFERS 1024

#define MAX(a,b) a>=b?a:b
#define TMAX(a,b,c) MAX(MAX(a,b),c)

int fds [LOCAL_BUFFERS] = {0};
char remote_ip[LOCAL_BUFFERS] = {0};
char send_ip[LOCAL_BUFFERS] = {0};
int remote_port;
int server_port;
int send_port;
int send_fd=-1;
int max_fd = 2;

typedef struct  {
  int32_t from_fd;
  int32_t to_fd;
  int32_t nbytes;
  char buffer[MAX_BUFFER];
} rmt_transfer;


void print_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s -r <remote_ip>:<remote_port> [-p <local_port>] [-s <send_ip>:<send_port>]\n", program_name);
    fprintf(stderr, "   by default local_port is 4141\n\n");
    fprintf(stderr, "Example: %s -r 127.0.0.1:8081 -p 4141\n", program_name);
    exit(EXIT_FAILURE);
}

int new_server(const int port) {
    int server_fd = -1;
    int opt = 1;
    struct sockaddr_in server_addr;

    // Create server socket
    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Server socket creation failed");
        exit(EXIT_FAILURE);
    }

    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("setsockopt(SO_REUSEADDR) failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Set server address
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    // Bind server socket
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind failed");
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections
    if (listen(server_fd, LISTEN_BACKLOG) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "Server listening on port %d...\n", server_port);
    return server_fd;
}

int new_client(const char * host, const int port) {
    int remote_fd = -1;
    struct sockaddr_in remote_addr;

    // Connect to remote server as a client
    if ((remote_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Remote socket creation failed");
        exit(EXIT_FAILURE);
    }

    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &remote_addr.sin_addr) <= 0) {
        perror("Invalid remote server address");
        exit(EXIT_FAILURE);
    }

    if (connect(remote_fd, (struct sockaddr *)&remote_addr, sizeof(remote_addr)) < 0) {
        perror("Connection to remote server failed");
        remote_fd = -1;
    }
    return remote_fd;
}

int connect_with_timeout(const char * host, const int port) {
    int sockfd;
    struct sockaddr_in remote_addr;
    int timeout_sec=5;

    // Create and connect remote socket with timeout
    if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        perror("Remote socket creation failed");
        exit(EXIT_FAILURE);
    }

    remote_addr.sin_family = AF_INET;
    remote_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, host, &remote_addr.sin_addr) <= 0) {
        perror("Invalid remote server address");
        close(sockfd);
        exit(EXIT_FAILURE);
    }


    // Set socket to non-blocking
    int flags = fcntl(sockfd, F_GETFL, 0);
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(sockfd);
        return -1;
    }

    // Start connection
    int rc = connect(sockfd, (struct sockaddr *)&remote_addr, sizeof(remote_addr));
    if (rc == 0) {
        fprintf(stderr, "   CONNNECTION SUCEEDED IMMEDIATELY\n");
        // Connection succeeded immediately
        fcntl(sockfd, F_SETFL, flags); // Restore original flags
        return sockfd;
    }

    if (errno != EINPROGRESS) {
        close(sockfd);
        return -1;
    }

    // Wait for connection with timeout
    fd_set fdset;
    struct timeval tv;
    FD_ZERO(&fdset);
    FD_SET(sockfd, &fdset);
    tv.tv_sec = timeout_sec;
    tv.tv_usec = 0;

    rc = select(MAX_SELECTOR, NULL, &fdset, NULL, &tv);
    if (rc <= 0) {
        fprintf(stderr, ".... TIMEOUT connecting %s:%d\n", host, port);
        close(sockfd);
        if (rc == 0) errno = ETIMEDOUT;
        return -1;
    }

    // Check socket error status
    int so_error;
    socklen_t len = sizeof(so_error);
    getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &so_error, &len);
    if (so_error != 0) {
        close(sockfd);
        errno = so_error;
        return -1;
    }

    // Restore original flags
    fcntl(sockfd, F_SETFL, flags);
    return sockfd;
}


int accept_connection(int server_fd) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int client_fd = -1;
    if ((client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &addr_len)) < 0) {
        perror("Accept failed");
    }
    fprintf(stderr, "New client connected: %s:%d\n", 
            inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    return client_fd;
}

void write_that(const char * who, int peer, int n_bytes, const char * buffer) {
    char who_str[MAX_BUFFER];
    snprintf(who_str, MAX_BUFFER, "-------------- [%s] ---------------\n", who);
    write(STDOUT_FILENO, who_str, strlen(who_str));
    write(STDOUT_FILENO, buffer, n_bytes);
    fsync(STDOUT_FILENO);  // Ensure data is flushed to terminal
    
    if (send_fd > 0) {
      rmt_transfer tr = {peer, fds[peer], n_bytes};
      memcpy(tr.buffer, buffer, n_bytes);
    }
}

void close_both(int peer1) {
  int peer2 = fds[peer1];
  close(peer1);
  close(peer2);
  fds[peer1] = 0;
  fds[peer2] = 0;
  while (fds[max_fd] == 0 && max_fd>MIN_SOCKET_FD) {
    max_fd--;
  }
  fprintf(stderr, "Closed %d, %d -> max=%d\n",peer1, peer2, max_fd);
}

void talk(int listen_fd) {
    fd_set readfds;
    char buffer[MAX_BUFFER] = {0};

    max_fd = listen_fd;

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(listen_fd, &readfds);
        
        for (int i=MIN_SOCKET_FD; i<=max_fd; i++) {
          if (i == listen_fd || fds[i] == 0)
            continue;
           FD_SET(i, &readfds);
        }

        
        int activity = select(MAX_SELECTOR, &readfds, NULL, NULL, NULL);
        if ((activity < 0) && (errno != EINTR)) {
            perror("Select error");
        }

        if (FD_ISSET(listen_fd, &readfds)) {
          int client_fd = accept_connection(listen_fd);
          int remote_fd = -1;
          if (client_fd >= MAX_SELECTOR) {
              close(client_fd);
              client_fd = -1;
          }
          if (client_fd >0) {
            remote_fd = connect_with_timeout(remote_ip, remote_port);
            if (remote_fd>0) {
              fds[remote_fd] = client_fd;
              fds[client_fd] = remote_fd;
              max_fd = TMAX(max_fd, client_fd, remote_fd);
              fprintf(stderr, "Connected to remote server at %s:%d  (l=%d,r=%d) - %d\n", remote_ip, remote_port, client_fd, remote_fd, max_fd);
            } else {
              close(client_fd);
            }
          }
        }
        
        for (int i=MIN_SOCKET_FD; i<max_fd; i++) {
          if (i == listen_fd || fds[i] == 0)
            continue;

          if (FD_ISSET(i, &readfds)) {
              int n_read = read(i, buffer, MAX_BUFFER);
              if (n_read == 0) {
                  fprintf(stderr, "disconnected\n");
                  close_both(i);
                  break;
              } else if (n_read > 0) {
                  char s[LOCAL_BUFFERS];
                  snprintf(s, LOCAL_BUFFERS, "-- PEER (%d -> %d)--", i, fds[i]);
                  write_that(s, i, n_read, buffer);
                  // Forward to remote server
                  send(fds[i], buffer, n_read, 0);
                  memset(buffer, 0, MAX_BUFFER);
              }
          }
        }
    }
}



int t_main(int argc, char * argv[]) {
    int c;
    char *cvalue = NULL;
    server_port = SERVER_PORT;
    int cflag = 0;

    while ((c = getopt (argc, argv, "r:p:c:")) != -1) {
        switch (c) {
            case 'r':
              /* Remote connection - Host:port to connect to for every connection */
              sscanf(optarg ,"%1024[^:]:%5d", remote_ip, &remote_port);
              cflag = 1;
              break;
            case 'p':
              /* port - This program listens this port*/
              sscanf(optarg, "%d", &server_port);
            case 's':
              /* Send data to host:port*/
              sscanf(optarg, "%1024s[^:]:%d", send_ip, &send_port);
              send_fd = 0;
              break;
        }
    }
    if (!cflag)
        print_usage(argv[0]);
}

int main(int argc, char * argv[]) {
    t_main(argc, argv);
    int server_fd=-1;

    printf("Local port: %d ; Remoter_server: %s:%d\n", server_port, remote_ip, remote_port);

    if (server_port <=0 || remote_port<=0) {
        print_usage(argv[0]);
    }

    server_fd = new_server(server_port);

    if (send_fd == 0) {
      printf("%s %d\n", send_ip, send_port);
      send_fd = connect_with_timeout(send_ip, send_port);
      if (send_fd > 0) {
          fds[send_fd] = send_fd;
          max_fd = MAX(max_fd, send_fd);
      }
    }

    talk(server_fd);

    close(server_fd);
    return 0;
}
