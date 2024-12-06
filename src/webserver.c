#define _POSIX_C_SOURCE 200112L  // Enable POSIX features

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>      // Required for addrinfo structure
#include <sys/socket.h>
#include <sys/types.h>  // Additional system types

#define BUFFER_SIZE 8192
#define STATIC_RESP_COUNT 3

// Function declarations
void handle_client(int client_fd);
struct sockaddr_in derive_sockaddr(const char *host, const char *port);

// Structure for static resources
typedef struct {
    const char *path;
    const char *content;
} StaticResource;

// Static content resources
StaticResource static_resources[] = {
    {"/static/foo", "Foo"},
    {"/static/bar", "Bar"},
    {"/static/baz", "Baz"}
};

// Convert hostname and port to sockaddr_in
struct sockaddr_in derive_sockaddr(const char *host, const char *port) {
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;

    struct addrinfo *result_info;

    int ret = getaddrinfo(host, port, &hints, &result_info);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        exit(EXIT_FAILURE);
    }

    struct sockaddr_in result = *((struct sockaddr_in *)result_info->ai_addr);
    freeaddrinfo(result_info);

    return result;
}

// Handle client connections
void handle_client(int client_fd) {
    char buffer[BUFFER_SIZE];
    int bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    if (bytes_read < 0) {
        perror("recv");
        return;
    } else if (bytes_read == 0) {
        printf("Client disconnected.\n");
        return;
    }

    buffer[bytes_read] = '\0';

    // Parse HTTP request
    char method[16], path[256];
    if (sscanf(buffer, "%15s %255s", method, path) != 2) {
        const char *response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 11\r\n\r\nBad Request";
        send(client_fd, response, strlen(response), 0);
        return;
    }

    // Handle GET requests
    if (strcmp(method, "GET") != 0) {
        const char *response = "HTTP/1.1 405 Method Not Allowed\r\nContent-Length: 18\r\n\r\nMethod Not Allowed";
        send(client_fd, response, strlen(response), 0);
        return;
    }

    // Check static content
    for (int i = 0; i < STATIC_RESP_COUNT; i++) {
        if (strcmp(path, static_resources[i].path) == 0) {
            char response[BUFFER_SIZE];
            snprintf(response, sizeof(response),
                     "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n\r\n%s",
                     strlen(static_resources[i].content),
                     static_resources[i].content);
            send(client_fd, response, strlen(response), 0);
            return;
        }
    }

    // 404 response
    const char *response = "HTTP/1.1 404 Not Found\r\nContent-Length: 13\r\n\r\n404 Not Found";
    send(client_fd, response, strlen(response), 0);
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <IP> <PORT>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *ip = argv[1];
    const char *port = argv[2];

    struct sockaddr_in server_addr = derive_sockaddr(ip, port);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return EXIT_FAILURE;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        close(server_fd);
        return EXIT_FAILURE;
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        return EXIT_FAILURE;
    }

    printf("Server listening on %s:%s\n", ip, port);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);

        if (client_fd < 0) {
            perror("accept");
            continue;
        }

        handle_client(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return EXIT_SUCCESS;
}
