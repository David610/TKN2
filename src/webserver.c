// GNU extension
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>

// Konfigurationskonstanten
#define BUFFER_SIZE 8192
#define STATIC_RESP_COUNT 3
#define DYNAMIC_RESOURCES_COUNT 100
#define MAX_HEADERS 40
#define MAX_HEADER_LENGTH 256

typedef struct {
    const char *path;
    const char *content;
    size_t content_length;
} StaticResource;

typedef struct {
    char path[256];
    char content[BUFFER_SIZE];
    bool in_use;
    size_t content_length;
} DynamicResource;

// Statische Ressourcen
StaticResource static_resources[] = {
    {"/static/foo", "Foo", 3},
    {"/static/bar", "Bar", 3},
    {"/static/baz", "Baz", 3}
};

// Array zum Speichern dynamischer Ressourcen
DynamicResource dynamic_resources[DYNAMIC_RESOURCES_COUNT] = {0};

int count_headers(const char *request) {
    int count = 0;
    const char *ptr = request;
    const char *end = strstr(request, "\r\n\r\n");
    
    if (!end) return 0;
    
    while (ptr < end && (ptr = strstr(ptr, "\r\n")) != NULL) {
        count++;
        ptr += 2;
    }

    // Subtrahiere die Leerzeile am Ende
    return count - 1;
}

// Überprüft, ob die Headerzeilen gültig sind
bool validate_headers(const char *request) {
    const char *header_start = strstr(request, "\r\n");
    if (!header_start) return false;
    
    header_start += 2;
    const char *header_end = strstr(request, "\r\n\r\n");
    if (!header_end) return false;

    const char *line = header_start;
    while (line < header_end) {
        const char *next_line = strstr(line, "\r\n");
        if (!next_line || (next_line - line) > MAX_HEADER_LENGTH) {
            return false;
        }
        line = next_line + 2;
    }
    
    return true;
}

// Ermittelt die Länge des Request-Bodys
ssize_t get_content_length(const char *request) {
    const char *cl_header = strcasestr(request, "Content-Length:");
    
    if (!cl_header) {
        return -1;
    }
    
    ssize_t length = 0;
    if (sscanf(cl_header + 15, "%zd", &length) != 1) {
        return -1;
    }
    
    return length;
}

// Sendet alle Daten an den Client
ssize_t send_all(int sock_fd, const char *buffer, size_t length) {
    size_t total_sent = 0;
    
    while (total_sent < length) {
        ssize_t sent = send(sock_fd, buffer + total_sent, length - total_sent, MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EPIPE) return -1;
            perror("Error: send failed");
            return -1;
        }
        total_sent += sent;
    }
    
    return total_sent;
}

// Sendet eine HTTP-Antwort an den Client
int send_response(int client_fd, int status_code, const char *status_text,
                 const char *body, size_t content_length) {
    char header[BUFFER_SIZE];
    
    int header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code, status_text, content_length);
    
    if (send_all(client_fd, header, header_len) < 0) {
        return -1;
    }
    
    if (body && content_length > 0) {
        if (send_all(client_fd, body, content_length) < 0) {
            return -1;
        }
    }
    
    return 0;
}

// Verarbeitet den HTTP-Request
int process_request(const char *request, int client_fd) {
    char method[16] = {0};
    char path[256] = {0};
    char version[16] = {0};
    
    printf("\n=== New Request ===\n");
    
    // Validiere Request-Zeile
    if (sscanf(request, "%15s %255s %15s", method, path, version) != 3) {
        printf("Failed to parse request line\n");
        return send_response(client_fd, 400, "Bad Request", "Invalid Request Format", 21);
    }
    
    // Validate HTTP version
    if (count_headers(request) > MAX_HEADERS) {
        return send_response(client_fd, 400, "Bad Request", "Too many headers", 15);
    }
    
    if (!validate_headers(request)) {
        return send_response(client_fd, 400, "Bad Request", "Invalid headers", 14);
    }
    
    printf("Method: %s\nPath: %s\nVersion: %s\n", method, path, version);
    
    if (strcasecmp(method, "HEAD") == 0) {
        return send_response(client_fd, 501, "Not Implemented", NULL, 0);
    }
    
    // Handle statische Ressourcen
    if (strncmp(path, "/static/", 8) == 0) {
        if (strcasecmp(method, "GET") != 0) {
            return send_response(client_fd, 405, "Method Not Allowed", NULL, 0);
        }
        
        for (int i = 0; i < STATIC_RESP_COUNT; i++) {
            if (strcmp(path, static_resources[i].path) == 0) {
                return send_response(client_fd, 200, "OK",
                                  static_resources[i].content,
                                  static_resources[i].content_length);
            }
        }
        
        return send_response(client_fd, 404, "Not Found", NULL, 0);
    }
    
    // Handle dynamische Ressourcen
    if (strncmp(path, "/dynamic/", 9) == 0) {
        printf("\nDynamic resource handle for path: '%s'\n", path);
        int resource_index = -1;
        int available_slot = -1;
        
        for (int i = 0; i < DYNAMIC_RESOURCES_COUNT; i++) {
            if (dynamic_resources[i].in_use) {
                if (strcmp(dynamic_resources[i].path, path) == 0) {
                    resource_index = i;
                    printf("Found existing resource at index %d\n", i);
                    break;
                }
            } else if (available_slot == -1) {
                available_slot = i;
            }
        }
        
        if (strcasecmp(method, "PUT") == 0) {
            const char *headers_end = strstr(request, "\r\n\r\n");
            if (!headers_end) {
                return send_response(client_fd, 400, "Bad Request", "Missing headers", 14);
            }
            
            const char *body = headers_end + 4;
            ssize_t content_length = get_content_length(request);
            printf("PUT request - Content-Length: %zd\n", content_length);
            
            if (content_length < 0 || content_length >= BUFFER_SIZE) {
                return send_response(client_fd, 411, "Length Required",
                                  "Invalid Content-Length", 20);
            }
            
            if (resource_index != -1) {
                memset(dynamic_resources[resource_index].content, 0, BUFFER_SIZE);
                memcpy(dynamic_resources[resource_index].content, body, content_length);
                dynamic_resources[resource_index].content_length = content_length;
                printf("Updated resource %d with %zd bytes\n", resource_index, content_length);
                return send_response(client_fd, 204, "No Content", NULL, 0);
            } else if (available_slot != -1) {
                dynamic_resources[available_slot].in_use = true;
                strncpy(dynamic_resources[available_slot].path, path,
                        sizeof(dynamic_resources[available_slot].path) - 1);
                dynamic_resources[available_slot].path[sizeof(dynamic_resources[available_slot].path) - 1] = '\0';
                memset(dynamic_resources[available_slot].content, 0, BUFFER_SIZE);
                memcpy(dynamic_resources[available_slot].content, body, content_length);
                dynamic_resources[available_slot].content_length = content_length;
                printf("Created resource at slot %d with path '%s', content length %zd\n",
                       available_slot, dynamic_resources[available_slot].path, content_length);
                return send_response(client_fd, 201, "Created", NULL, 0);
            } else {
                return send_response(client_fd, 507, "Insufficient Storage", NULL, 0);
            }
        }
        
        if (strcasecmp(method, "GET") == 0) {
            if (resource_index != -1) {
                size_t content_length = dynamic_resources[resource_index].content_length;
                printf("GET request - Serving content from resource %d, length: %zu\n",
                       resource_index, content_length);
                return send_response(client_fd, 200, "OK",
                                  dynamic_resources[resource_index].content,
                                  content_length);
            } else {
                printf("Resource not found for path: '%s'\n", path);
                return send_response(client_fd, 404, "Not Found", NULL, 0);
            }
        }
        
        if (strcasecmp(method, "DELETE") == 0) {
            if (resource_index != -1) {
                dynamic_resources[resource_index].in_use = false;
                memset(dynamic_resources[resource_index].content, 0, BUFFER_SIZE);
                dynamic_resources[resource_index].content_length = 0;
                return send_response(client_fd, 204, "No Content", NULL, 0);
            } else {
                return send_response(client_fd, 404, "Not Found", NULL, 0);
            }
        }
        
        return send_response(client_fd, 405, "Method Not Allowed", NULL, 0);
    }
    
    return send_response(client_fd, 404, "Not Found", NULL, 0);
}

int handle_client(int client_fd) {
    char buffer[BUFFER_SIZE] = {0};
    size_t total_bytes = 0;
    
    while (1) {
        ssize_t bytes_read = recv(client_fd, buffer + total_bytes, 
                                sizeof(buffer) - total_bytes - 1, 0);
        
        if (bytes_read <= 0) {
            if (bytes_read < 0) {
                perror("Error: recv failed");
                return -1;
            }
            break;
        }
        
        total_bytes += bytes_read;
        buffer[total_bytes] = '\0';
        
        while (1) {
            char *headers_end = strstr(buffer, "\r\n\r\n");
            if (!headers_end) {
                break;
            }
            
            size_t headers_length = headers_end - buffer + 4;
            ssize_t content_length = get_content_length(buffer);
            
            if (content_length < 0) {
                content_length = 0;
            }
            
            size_t total_request_length = headers_length + content_length;
            if (total_bytes < total_request_length) {
                break;
            }
            
            buffer[total_request_length] = '\0';
            
            int process_result = process_request(buffer, client_fd);
            if (process_result < 0) {
                fprintf(stderr, "Error: request processing failed\n");
                return -1;
            }
            
            size_t remaining = total_bytes - total_request_length;
            memmove(buffer, buffer + total_request_length, remaining);
            total_bytes = remaining;
            buffer[total_bytes] = '\0';
        }
    }
    
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <IP> <Port>\n", argv[0]);
        return EXIT_FAILURE;
    }
    
    const char *ip = argv[1];
    int port = atoi(argv[2]);
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    if (server_fd < 0) {
        perror("Error: socket creation failed");
        return EXIT_FAILURE;
    }
    
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(server_fd);
        return EXIT_FAILURE;
    }
    
    struct sockaddr_in server_addr = {0};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid address");
        close(server_fd);
        return EXIT_FAILURE;
    }
    
    if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind failed");
        close(server_fd);
        return EXIT_FAILURE;
    }
    
    if (listen(server_fd, 10) < 0) {
        perror("listen failed");
        close(server_fd);
        return EXIT_FAILURE;
    }
    
    printf("Server listening on %s:%d\n", ip, port);
    
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        
        if (client_fd < 0) {
            perror("accept failed");
            break;
        }
        
        if (handle_client(client_fd) < 0) {
            fprintf(stderr, "Error handling client\n");
        }
        
        close(client_fd);
    }
    
    close(server_fd);
    return EXIT_FAILURE;
}