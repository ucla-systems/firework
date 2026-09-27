#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>


#define BUFFER_SIZE 1024


int write_to_debugfs(const char *path, const char *format, uint32_t value) {
    FILE *file = fopen(path, "w");
    if (file == NULL) {
        perror("fopen failed");
        return -1;
    }

    if (fprintf(file, format, value) < 0) {
        perror("fprintf failed");
        fclose(file);
        return -1;
    }

    fclose(file);
    return 0;
}

int get_ip_address(char *hostname, struct sockaddr_in *ipv4) {
    struct addrinfo hints, *res;
    
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(hostname, NULL, &hints, &res) != 0) {
        perror("getaddrinfo failed");
        return -1;
    }

    *ipv4 = *(struct sockaddr_in *)res->ai_addr;

    char ipstr[INET_ADDRSTRLEN];
    inet_ntop(res->ai_family, &(ipv4->sin_addr), ipstr, sizeof(ipstr));
    printf("IP Address: %s\n", ipstr);

    freeaddrinfo(res);
    return 0;
}

int write_sockinfo_to_file(int server_fd, struct sockaddr_in *address) {
    char hostname[256];
    int addrlen = sizeof(*address);

    if (gethostname(hostname, sizeof(hostname)) == -1) {
        perror("gethostname failed");
        return -1;
    }

    // Get IP address
    struct sockaddr_in ipv4;
    if (get_ip_address(hostname, &ipv4) == -1) {
        return -1;
    }

    // Write IP address to debugfs
    uint32_t addr = ntohl(ipv4.sin_addr.s_addr);
    if (write_to_debugfs("/sys/kernel/debug/firework/ipaddr", "%u\n", addr) == -1) {
        return -1;
    }

    // Get socket's port information
    if (getsockname(server_fd, (struct sockaddr *)address, (socklen_t *)&addrlen) == -1) {
        perror("getsockname failed");
        close(server_fd);
        return -1;
    }

    // Write port to debugfs
    uint16_t port = ntohs(address->sin_port);
    if (write_to_debugfs("/sys/kernel/debug/firework/port", "%u\n", port) == -1) {
        return -1;
    }

    return port;
}

int main() {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    char buffer[BUFFER_SIZE] = {0};
    const char *response = "Hello from server";

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) {
        perror("Setsockopt failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = 0;

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        perror("Bind failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    int port = write_sockinfo_to_file(server_fd, &address);

    if (listen(server_fd, 3) < 0) {
        perror("Listen failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("Listening on port %u...\n", port);

    if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t *)&addrlen)) < 0) {
        perror("Accept failed");
        close(server_fd);
        exit(EXIT_FAILURE);
    }
    printf("Connection accepted\n");

    int bytes_read = read(new_socket, buffer, BUFFER_SIZE);
    printf("Received message: %s\n", buffer);

    send(new_socket, response, strlen(response), 0);
    printf("Response sent to client\n");

    close(new_socket);
    close(server_fd);
    printf("Connection closed\n");

    return 0;
}
