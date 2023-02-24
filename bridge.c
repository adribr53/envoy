#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#define MAX_DATA_SIZE 1024

int main(int argc, char* argv[]) {

    // Check for correct number of arguments
    if (argc != 5) {
        printf("Usage: %s <server-ip> <server-port> <upstream-ip> <upstream-port>\n", argv[0]);
        exit(1);
    }

    // Get server IP and port from command line arguments
    char* server_ip = argv[1];
    int server_port = atoi(argv[2]);

    // Get upstream IP and port from command line arguments
    char* upstream_ip = argv[3];
    int upstream_port = atoi(argv[4]);

    // Create socket for server
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);

    // Set up server address
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(server_ip);
    server_addr.sin_port = htons(server_port);

    // Bind socket to server address
    bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr));

    // Listen for incoming connections
    listen(server_sock, 1);

    // Accept incoming connection from client
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &client_len);
    printf("Client connected\n");

    // Create socket for upstream connection
    int upstream_sock = socket(AF_INET, SOCK_STREAM, 0);

    // Set up upstream address
    struct sockaddr_in upstream_addr;
    memset(&upstream_addr, 0, sizeof(upstream_addr));
    upstream_addr.sin_family = AF_INET;
    upstream_addr.sin_addr.s_addr = inet_addr(upstream_ip);
    upstream_addr.sin_port = htons(upstream_port);

    // Connect to upstream host
    connect(upstream_sock, (struct sockaddr*)&upstream_addr, sizeof(upstream_addr));
    printf("Connected to upstream host\n");

    // Forward data between client and upstream
    char buffer[MAX_DATA_SIZE];
    int bytes_read;
    while ((bytes_read = recv(client_sock, buffer, MAX_DATA_SIZE, 0)) > 0) {
        send(upstream_sock, buffer, bytes_read, 0);
        printf("Sent %d bytes to second client\n", bytes_read);
    }

    // Close sockets
    close(client_sock);
    close(upstream_sock);

    return 0;
}