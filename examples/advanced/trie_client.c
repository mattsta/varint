/*
 * Trie Server Client
 *
 * Simple client for testing the async trie server
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../../src/varint.h"

#define MAX_RESPONSE_SIZE 65536

typedef enum {
    CMD_PING = 0x09,
    CMD_STATS = 0x07,
    CMD_AUTH = 0x0A
} CommandType;

typedef enum {
    STATUS_OK = 0x00,
    STATUS_ERROR = 0x01,
    STATUS_AUTH_REQUIRED = 0x02,
    STATUS_RATE_LIMITED = 0x03,
    STATUS_INVALID_CMD = 0x04
} StatusCode;

typedef struct {
    int sockfd;
    char *host;
    uint16_t port;
    bool connected;
} TrieClient;

bool clientConnect(TrieClient *client, const char *host, uint16_t port) {
    // Initialize all fields FIRST
    client->port = port;
    client->connected = false;
    client->host = NULL;  // Set to NULL initially - allocate only on success

    client->sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (client->sockfd < 0) {
        perror("socket");
        return false;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(client->sockfd);
        return false;
    }

    if (connect(client->sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(client->sockfd);
        return false;
    }

    // Connection succeeded - NOW allocate host string
    // Use manual allocation instead of strdup to avoid potential 32/64-bit issues
    size_t hostLen = strlen(host);
    char *hostCopy = (char *)malloc(hostLen + 1);
    fprintf(stderr, "DEBUG: malloc(%zu) returned %p (0x%lx) for host\n", hostLen + 1, (void*)hostCopy, (unsigned long)(uintptr_t)hostCopy);
    if (!hostCopy) {
        close(client->sockfd);
        return false;
    }
    memcpy(hostCopy, host, hostLen + 1);

    client->host = hostCopy;
    fprintf(stderr, "DEBUG: After assignment, client->host = %p (0x%lx)\n", (void*)client->host, (unsigned long)(uintptr_t)client->host);

    client->connected = true;
    printf("Connected to %s:%d\n", host, port);
    fprintf(stderr, "DEBUG: Returning from clientConnect, client->host = %p\n", (void*)client->host);
    return true;
}

void clientClose(TrieClient *client) {
    fprintf(stderr, "DEBUG: clientClose() entry, client=%p\n", (void*)client);
    fprintf(stderr, "DEBUG: client->sockfd=%d, host=%p, port=%u, connected=%d\n",
            client->sockfd, (void*)client->host, client->port, client->connected);
    if (client->connected) {
        fprintf(stderr, "DEBUG: BEFORE close: client->host=%p\n", (void*)client->host);
        close(client->sockfd);
        fprintf(stderr, "DEBUG: AFTER close: client->host=%p\n", (void*)client->host);
        client->connected = false;
        fprintf(stderr, "DEBUG: AFTER connected=false: client->host=%p\n", (void*)client->host);
    }
    fprintf(stderr, "DEBUG: About to check host pointer, client->host=%p\n", (void*)client->host);
    if (client->host) {
        fprintf(stderr, "DEBUG: About to free host string at %p\n", (void*)client->host);
        free(client->host);
        fprintf(stderr, "DEBUG: free(host) completed\n");
        client->host = NULL;
    }
    fprintf(stderr, "DEBUG: clientClose() exit\n");
}

bool sendCommand(TrieClient *client, CommandType cmd, const uint8_t *payload, size_t payloadLen) {
    if (!client->connected) {
        fprintf(stderr, "Not connected\n");
        return false;
    }

    // Build message: [Length:varint][CommandID:1byte][Payload]
    uint8_t buffer[4096];
    size_t offset = 0;

    // Reserve space for length
    offset += 5;

    // Command ID
    buffer[offset++] = (uint8_t)cmd;

    // Payload
    if (payload && payloadLen > 0) {
        memcpy(buffer + offset, payload, payloadLen);
        offset += payloadLen;
    }

    // Calculate message length
    uint64_t messageLen = offset - 5;

    // Write length
    size_t lengthBytes = varintTaggedPut64(buffer, messageLen);

    // Shift message
    memmove(buffer + lengthBytes, buffer + 5, messageLen);

    size_t totalSize = lengthBytes + messageLen;

    // Send
    ssize_t sent = write(client->sockfd, buffer, totalSize);
    if (sent != (ssize_t)totalSize) {
        perror("write");
        return false;
    }

    return true;
}

bool receiveResponse(TrieClient *client, StatusCode *status, uint8_t *data, size_t *dataLen, size_t maxDataLen) {
    if (!client->connected) {
        return false;
    }

    // Read length
    uint8_t lengthBuf[9];
    size_t bytesRead = 0;
    uint64_t messageLen;

    // Read at least one byte
    if (read(client->sockfd, lengthBuf, 1) != 1) {
        return false;
    }
    bytesRead = 1;

    // Keep reading until we have a complete varint
    while (varintTaggedGet64(lengthBuf, &messageLen) == 0 && bytesRead < sizeof(lengthBuf)) {
        if (read(client->sockfd, lengthBuf + bytesRead, 1) != 1) {
            return false;
        }
        bytesRead++;
    }

    if (messageLen == 0 || messageLen > MAX_RESPONSE_SIZE) {
        fprintf(stderr, "Invalid message length: %lu\n", messageLen);
        return false;
    }

    // Read message
    uint8_t *msgBuf = (uint8_t *)malloc(messageLen);
    if (!msgBuf) {
        return false;
    }

    size_t totalRead = 0;
    while (totalRead < messageLen) {
        ssize_t n = read(client->sockfd, msgBuf + totalRead, messageLen - totalRead);
        if (n <= 0) {
            free(msgBuf);
            return false;
        }
        totalRead += n;
    }

    // Parse response
    *status = (StatusCode)msgBuf[0];
    *dataLen = messageLen - 1;

    // CRITICAL: Bounds check before copying data
    if (*dataLen > maxDataLen) {
        fprintf(stderr, "Response data too large: %zu > %zu\n", *dataLen, maxDataLen);
        free(msgBuf);
        return false;
    }

    if (*dataLen > 0) {
        memcpy(data, msgBuf + 1, *dataLen);
    }

    free(msgBuf);
    return true;
}

bool cmdPing(TrieClient *client) {
    fprintf(stderr, "DEBUG: cmdPing() entry, client=%p\n", (void*)client);
    printf("Sending PING...\n");

    fprintf(stderr, "DEBUG: About to sendCommand\n");
    if (!sendCommand(client, CMD_PING, NULL, 0)) {
        fprintf(stderr, "DEBUG: sendCommand failed\n");
        return false;
    }
    fprintf(stderr, "DEBUG: sendCommand succeeded\n");

    StatusCode status;
    fprintf(stderr, "DEBUG: About to malloc %d bytes\n", MAX_RESPONSE_SIZE);
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) {
        fprintf(stderr, "Failed to allocate response buffer\n");
        return false;
    }
    fprintf(stderr, "DEBUG: malloc succeeded, data=%p\n", (void*)data);
    size_t dataLen;

    fprintf(stderr, "DEBUG: About to receiveResponse\n");
    if (!receiveResponse(client, &status, data, &dataLen, MAX_RESPONSE_SIZE)) {
        fprintf(stderr, "Failed to receive response\n");
        fprintf(stderr, "DEBUG: About to free data after receive failure\n");
        free(data);
        fprintf(stderr, "DEBUG: free completed after receive failure\n");
        return false;
    }
    fprintf(stderr, "DEBUG: receiveResponse succeeded, status=%d, dataLen=%zu\n", status, dataLen);

    fprintf(stderr, "DEBUG: About to free data after receive success\n");
    free(data);
    fprintf(stderr, "DEBUG: free completed after receive success\n");

    if (status == STATUS_OK) {
        fprintf(stderr, "DEBUG: status is OK, about to print PONG\n");
        printf("PONG (OK)\n");
        fprintf(stderr, "DEBUG: cmdPing() returning true\n");
        return true;
    } else {
        fprintf(stderr, "DEBUG: status is error, about to print error\n");
        printf("Error: status = 0x%02X\n", status);
        fprintf(stderr, "DEBUG: cmdPing() returning false\n");
        return false;
    }
}

bool cmdStats(TrieClient *client) {
    printf("Sending STATS...\n");

    if (!sendCommand(client, CMD_STATS, NULL, 0)) {
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) {
        fprintf(stderr, "Failed to allocate response buffer\n");
        return false;
    }
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen, MAX_RESPONSE_SIZE)) {
        fprintf(stderr, "Failed to receive response\n");
        free(data);
        return false;
    }

    if (status != STATUS_OK) {
        printf("Error: status = 0x%02X\n", status);
        free(data);
        return false;
    }

    // Parse stats
    size_t offset = 0;
    uint64_t patterns, subscribers, nodes, connections, commands, uptime;

    varintTaggedGet64(data + offset, &patterns);
    offset += varintTaggedGetLen(data + offset);

    varintTaggedGet64(data + offset, &subscribers);
    offset += varintTaggedGetLen(data + offset);

    varintTaggedGet64(data + offset, &nodes);
    offset += varintTaggedGetLen(data + offset);

    varintTaggedGet64(data + offset, &connections);
    offset += varintTaggedGetLen(data + offset);

    varintTaggedGet64(data + offset, &commands);
    offset += varintTaggedGetLen(data + offset);

    varintTaggedGet64(data + offset, &uptime);
    offset += varintTaggedGetLen(data + offset);

    printf("\nServer Statistics:\n");
    printf("  Patterns:     %lu\n", patterns);
    printf("  Subscribers:  %lu\n", subscribers);
    printf("  Nodes:        %lu\n", nodes);
    printf("  Connections:  %lu\n", connections);
    printf("  Commands:     %lu\n", commands);
    printf("  Uptime:       %lu seconds\n", uptime);

    free(data);
    return true;
}

int main(int argc, char *argv[]) {
    fprintf(stderr, "DEBUG: main() entry\n");
    if (argc < 2) {
        printf("Usage: %s <command> [options]\n", argv[0]);
        printf("Commands:\n");
        printf("  ping [host] [port]   - Send PING command\n");
        printf("  stats [host] [port]  - Get server statistics\n");
        printf("\nDefault host: 127.0.0.1\n");
        printf("Default port: 9999\n");
        return 1;
    }

    const char *command = argv[1];
    const char *host = argc > 2 ? argv[2] : "127.0.0.1";
    uint16_t port = argc > 3 ? atoi(argv[3]) : 9999;

    fprintf(stderr, "DEBUG: About to allocate client struct on heap\n");
    TrieClient *client = (TrieClient *)calloc(1, sizeof(TrieClient));  // Allocate on heap to avoid stack corruption
    if (!client) {
        fprintf(stderr, "Failed to allocate client\n");
        return 1;
    }
    fprintf(stderr, "DEBUG: client struct allocated, client=%p\n", (void*)client);

    fprintf(stderr, "DEBUG: About to connect to %s:%d\n", host, port);
    if (!clientConnect(client, host, port)) {
        fprintf(stderr, "Failed to connect to server\n");
        free(client);
        return 1;
    }
    fprintf(stderr, "DEBUG: Connected successfully\n");
    fprintf(stderr, "DEBUG: AFTER connect: client.sockfd=%d, host=%p, port=%u, connected=%d\n",
            client->sockfd, (void*)client->host, client->port, client->connected);

    bool success = false;

    fprintf(stderr, "DEBUG: About to execute command: %s\n", command);
    if (strcmp(command, "ping") == 0) {
        fprintf(stderr, "DEBUG: Calling cmdPing()\n");
        success = cmdPing(client);
        fprintf(stderr, "DEBUG: cmdPing() returned %d\n", success);
        fprintf(stderr, "DEBUG: AFTER cmdPing: client.sockfd=%d, host=%p, port=%u, connected=%d\n",
                client->sockfd, (void*)client->host, client->port, client->connected);
    } else if (strcmp(command, "stats") == 0) {
        fprintf(stderr, "DEBUG: Calling cmdStats()\n");
        success = cmdStats(client);
        fprintf(stderr, "DEBUG: cmdStats() returned %d\n", success);
        fprintf(stderr, "DEBUG: AFTER cmdStats: client.sockfd=%d, host=%p, port=%u, connected=%d\n",
                client->sockfd, (void*)client->host, client->port, client->connected);
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
    }

    fprintf(stderr, "DEBUG: About to call clientClose()\n");
    clientClose(client);
    fprintf(stderr, "DEBUG: clientClose() returned\n");
    free(client);
    fprintf(stderr, "DEBUG: About to return from main() with code %d\n", success ? 0 : 1);
    return success ? 0 : 1;
}
