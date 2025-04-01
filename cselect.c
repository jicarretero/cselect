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


#define MAX_BUFFER 4096
#define SERVER_PORT 4141
#define MAX_SELECTOR 1024


void print_usage(const char *program_name) {
    fprintf(stderr, "Usage: %s <remote_ip> <remote_port> [<local_port>]\n", program_name);
    fprintf(stderr, "   by default local_port is 4141\n\n");
    fprintf(stderr, "Example: %s 127.0.0.1 8081 4141\n", program_name);
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
    if (listen(server_fd, 3) < 0) {
        perror("Listen failed");
        exit(EXIT_FAILURE);
    }

    fprintf(stderr, "Server listening on port %d...\n", SERVER_PORT);
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

    rc = select(1024, NULL, &fdset, NULL, &tv);
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

void write_that(const char * who, int n_bytes, const char * buffer) {
    char who_str[MAX_BUFFER];
    snprintf(who_str, MAX_BUFFER, "-------------- [%s] ---------------\n", who);
    write(STDOUT_FILENO, who_str, strlen(who_str));
    write(STDOUT_FILENO, buffer, n_bytes);
    fsync(STDOUT_FILENO);  // Ensure data is flushed to terminal
}

void talk(int remote_fd, int client_fd) {
    fd_set readfds;
    char buffer[MAX_BUFFER] = {0};

    while (1) {
        FD_ZERO(&readfds);
        FD_SET(remote_fd, &readfds);  // Add server socket to watch for new connections
        FD_SET(client_fd, &readfds);  // Add remote connection

        // Wait for activity on any socket
        int activity = select(MAX_SELECTOR, &readfds, NULL, NULL, NULL);
        if ((activity < 0) && (errno != EINTR)) {
            perror("Select error");
        }

        // Handle data from client
        if (FD_ISSET(client_fd, &readfds)) {
            int n_read = read(client_fd, buffer, MAX_BUFFER);
            if (n_read == 0) {
                // Client disconnected
                fprintf(stderr, "Client disconnected\n");
                break;
            } else if (n_read > 0) {
                write_that("CLIENT", n_read, buffer);
                // Forward to remote server
                send(remote_fd, buffer, n_read, 0);
                memset(buffer, 0, MAX_BUFFER);
            }
        }

        // Handle data from remote server
        if (FD_ISSET(remote_fd, &readfds)) {
            int n_read = read(remote_fd, buffer, MAX_BUFFER);
            if (n_read == 0) {
                // Remote server disconnected
                fprintf(stderr, "Remote server disconnected\n");
                break;
            } else if (n_read > 0) {
                write_that("REMOTE", n_read, buffer);
                // Forward to client if connected
                if (client_fd > 0) {
                    send(client_fd, buffer, n_read, 0);
                }
                memset(buffer, 0, MAX_BUFFER);
            }
        }
    }
    close(remote_fd);
    close(client_fd);
}

int main(int argc, char * argv[]) {
    int server_fd=-1, client_fd=-1, remote_fd=-1;

    if (argc!=3 && argc !=4) {
        print_usage(argv[0]);
    }

    const char *remote_ip = argv[1];
    int remote_port = atoi(argv[2]);
    int server_port = argc==4 ? atoi(argv[3]) : SERVER_PORT;

    printf("Local port: %d ; Remoter_server: %s:%d\n", server_port, remote_ip, remote_port);
    if (server_port <=0 || remote_port<=0) {
        print_usage(argv[0]);
    }

    server_fd = new_server(server_port);

    while (1) {
        client_fd = accept_connection(server_fd);
        if (client_fd>0) {
          remote_fd = connect_with_timeout(remote_ip, remote_port);
          if (remote_fd>0) {
              fprintf(stderr, "Connected to remote server at %s:%d\n", remote_ip, remote_port);
              talk(remote_fd, client_fd);
          } else {
              close(client_fd);
              client_fd = -1;
          }
        }
    }

    close(server_fd);
    close(remote_fd);
    if (client_fd > 0) {
        close(client_fd);
    }
    return 0;
}
