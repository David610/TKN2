#define _GNU_SOURCE  // Enable GNU extensions before including headers

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <stdbool.h>
#include <errno.h>
#include <ctype.h>

/* Configuration constants */
#define BUFFER_SIZE 8192             // Maximum size for request/response buffers
#define STATIC_RESP_COUNT 3          // Number of static resources
#define DYNAMIC_RESOURCES_COUNT 100  // Maximum number of dynamic resources

/* Data structures for resource management */
typedef struct {
    const char *path;    // URL path for the resource
    const char *content; // Content of the resource
} StaticResource;

typedef struct {
    char path[256];             // URL path for the resource
    char content[BUFFER_SIZE];  // Content of the resource
    bool in_use;                // Whether this slot is currently used
} DynamicResource;

/* Predefined static resources */
StaticResource static_resources[] = {
    {"/static/foo", "Foo"},
    {"/static/bar", "Bar"},
    {"/static/baz", "Baz"}
};

/* Array for storing dynamic resources */
DynamicResource dynamic_resources[DYNAMIC_RESOURCES_COUNT] = {0};

/**
 * Helper function to search for headers case-insensitively.
 * Returns pointer to the header value or NULL if not found.
 * The caller is responsible for freeing the returned string.
 */
char *find_header(const char *request, const char *header_name) {
    char *header_loc = strcasestr(request, header_name);
    if (!header_loc) return NULL;

    // Move pointer to the end of the header name
    header_loc += strlen(header_name);

    // Skip any whitespace and the ':' character
    while (*header_loc && (*header_loc == ' ' || *header_loc == ':' || *header_loc == '\t')) {
        header_loc++;
    }

    // Find end of header value
    char *end = strstr(header_loc, "\r\n");
    if (!end) return NULL;

    size_t value_length = end - header_loc;
    char *value = (char *)malloc(value_length + 1);
    if (!value) return NULL;

    strncpy(value, header_loc, value_length);
    value[value_length] = '\0';

    return value;
}

/**
 * Sends an HTTP response with proper formatting.
 * Ensures complete sending of all data even with partial sends.
 */
void send_response(int client_fd, int status_code, const char *status_text, const char *body) {
    char response[BUFFER_SIZE];
    int content_length = body ? strlen(body) : 0;

    // Format the complete response
    int total_len = snprintf(response, sizeof(response),
             "HTTP/1.1 %d %s\r\n"
             "Content-Length: %d\r\n"
             "Connection: keep-alive\r\n"
             "\r\n"
             "%s",
             status_code, status_text, content_length, body ? body : "");

    // Send with reliable delivery
    size_t sent = 0;
    while (sent < total_len) {
        ssize_t result = send(client_fd, response + sent, total_len - sent, MSG_NOSIGNAL);
        if (result < 0) {
            if (errno == EPIPE) return;  // Client disconnected
            perror("send failed");
            return;
        }
        sent += result;
    }
}

/**
 * Processes a single complete HTTP request.
 * Handles method validation, resource location, and response generation.
 */
void process_request(const char *request, int client_fd) {
    // Parse request line
    char method[16] = {0};
    char path[256] = {0};
    char version[16] = {0};

    if (sscanf(request, "%15s %255s %15s", method, path, version) != 3) {
        send_response(client_fd, 400, "Bad Request", "Invalid Request Format");
        return;
    }

    // Handle HEAD requests specifically
    if (strcasecmp(method, "HEAD") == 0) {
        send_response(client_fd, 501, "Not Implemented", NULL);
        return;
    }

    // Handle static resources
    if (strncmp(path, "/static/", 8) == 0) {
        for (int i = 0; i < STATIC_RESP_COUNT; i++) {
            if (strcmp(path, static_resources[i].path) == 0) {
                send_response(client_fd, 200, "OK", static_resources[i].content);
                return;
            }
        }
        send_response(client_fd, 404, "Not Found", "Resource Not Found");
        return;
    }

    // Handle dynamic resources
    if (strncmp(path, "/dynamic/", 9) == 0) {
        int resource_index = -1;
        int available_slot = -1;

        // Find existing resource or available slot
        for (int i = 0; i < DYNAMIC_RESOURCES_COUNT; i++) {
            if (dynamic_resources[i].in_use) {
                if (strcmp(dynamic_resources[i].path, path) == 0) {
                    resource_index = i;
                    break;
                }
            } else if (available_slot == -1) {
                available_slot = i;
            }
        }

        // Handle GET requests
        if (strcasecmp(method, "GET") == 0) {
            if (resource_index != -1) {
                send_response(client_fd, 200, "OK", dynamic_resources[resource_index].content);
            } else {
                send_response(client_fd, 404, "Not Found", "Resource Not Found");
            }
            return;
        }

        // Handle PUT requests
        if (strcasecmp(method, "PUT") == 0) {
            // Find the start of the body
            const char *body = strstr(request, "\r\n\r\n");
            if (!body) {
                send_response(client_fd, 400, "Bad Request", "Missing Content");
                return;
            }
            body += 4;  // Skip past \r\n\r\n

            // Calculate the body length based on Content-Length
            char *content_length_str = find_header(request, "Content-Length");
            int content_length = 0;
            if (content_length_str) {
                content_length = atoi(content_length_str);
                free(content_length_str);
            } else {
                // No Content-Length provided
                send_response(client_fd, 411, "Length Required", "Content-Length Header Missing");
                return;
            }

            // Ensure that the body length matches the Content-Length
            if ((int)strlen(body) < content_length) {
                send_response(client_fd, 400, "Bad Request", "Incomplete Body");
                return;
            }

            // Create or update the resource
            if (resource_index != -1) {
                // Update existing resource
                strncpy(dynamic_resources[resource_index].content, body, 
                        sizeof(dynamic_resources[resource_index].content) - 1);
                dynamic_resources[resource_index].content[sizeof(dynamic_resources[resource_index].content) - 1] = '\0';
                send_response(client_fd, 204, "No Content", NULL);
            } else if (available_slot != -1) {
                // Create new resource
                dynamic_resources[available_slot].in_use = true;
                strncpy(dynamic_resources[available_slot].path, path,
                        sizeof(dynamic_resources[available_slot].path) - 1);
                dynamic_resources[available_slot].path[sizeof(dynamic_resources[available_slot].path) - 1] = '\0';
                strncpy(dynamic_resources[available_slot].content, body,
                        sizeof(dynamic_resources[available_slot].content) - 1);
                dynamic_resources[available_slot].content[sizeof(dynamic_resources[available_slot].content) - 1] = '\0';
                send_response(client_fd, 201, "Created", NULL);
            } else {
                send_response(client_fd, 507, "Insufficient Storage", NULL);
            }
            return;
        }

        // Handle DELETE requests
        if (strcasecmp(method, "DELETE") == 0) {
            if (resource_index != -1) {
                dynamic_resources[resource_index].in_use = false;
                send_response(client_fd, 204, "No Content", NULL);
            } else {
                send_response(client_fd, 404, "Not Found", NULL);
            }
            return;
        }
    }

    // Default response for unknown resources or methods
    send_response(client_fd, 404, "Not Found", "Resource Not Found");
}

/**
 * Handles client connection and request processing.
 * Maintains connection state and handles partial requests.
 */
void handle_client(int client_fd) {
    char buffer[BUFFER_SIZE] = {0};
    size_t total_bytes = 0;

    while (1) {
        // Read new data
        ssize_t bytes_read = recv(client_fd, buffer + total_bytes, 
                                  sizeof(buffer) - total_bytes - 1, 0);
        
        if (bytes_read < 0) {
            perror("recv failed");
            break;
        } else if (bytes_read == 0) {
            // Connection closed by client
            break;
        }

        total_bytes += bytes_read;
        buffer[total_bytes] = '\0';

        // Process all complete requests in the buffer
        char *current = buffer;
        while (1) {
            // Find the end of headers
            char *end_of_headers = strstr(current, "\r\n\r\n");
            if (!end_of_headers) {
                // Incomplete headers
                break;
            }

            // Calculate header length
            size_t headers_length = end_of_headers - current + 4;

            // Parse headers to find Content-Length
            char *content_length_str = find_header(current, "Content-Length");
            int content_length = 0;
            if (content_length_str) {
                content_length = atoi(content_length_str);
                free(content_length_str);
            }

            // Calculate total request length (headers + body)
            size_t total_request_length = headers_length + content_length;

            // Check if the entire request has been received
            if (total_bytes < (current - buffer) + total_request_length) {
                // Entire request not yet received
                break;
            }

            // Extract the complete request (headers + body)
            char *complete_request = (char *)malloc(total_request_length + 1);
            if (!complete_request) {
                perror("malloc failed");
                return;
            }
            memcpy(complete_request, current, total_request_length);
            complete_request[total_request_length] = '\0';

            // Process the complete request
            process_request(complete_request, client_fd);
            free(complete_request);

            // Move to the next request in the buffer
            current += total_request_length;
        }

        // Calculate remaining bytes and move them to the start of the buffer
        size_t processed_bytes = current - buffer;
        size_t remaining = total_bytes - processed_bytes;
        if (remaining > 0) {
            memmove(buffer, current, remaining);
            total_bytes = remaining;
            buffer[total_bytes] = '\0';
        } else {
            total_bytes = 0;
            buffer[0] = '\0';
        }

        // Handle buffer overflow
        if (total_bytes >= sizeof(buffer) - 1) {
            send_response(client_fd, 400, "Bad Request", "Request Too Long");
            break;
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <IP> <PORT>\n", argv[0]);
        return EXIT_FAILURE;
    }

    const char *ip = argv[1];
    int port = atoi(argv[2]);

    // Create and configure server socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket creation failed");
        return EXIT_FAILURE;
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt failed");
        close(server_fd);
        return EXIT_FAILURE;
    }

    // Set up server address
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));  // Ensure zeroed memory
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        perror("Invalid IP address");
        close(server_fd);
        return EXIT_FAILURE;
    }

    // Bind and listen
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

    // Main server loop
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_fd = accept(server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            perror("accept failed");
            continue;
        }

        handle_client(client_fd);
        close(client_fd);
    }

    close(server_fd);
    return EXIT_SUCCESS;
}
