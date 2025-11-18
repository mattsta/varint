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
#include <errno.h>

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
    client->host = strdup(host);
    client->port = port;
    client->connected = false;

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

    client->connected = true;
    printf("Connected to %s:%d\n", host, port);
    return true;
}

void clientClose(TrieClient *client) {
    if (client->connected) {
        close(client->sockfd);
        client->connected = false;
    }
    free(client->host);
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

bool receiveResponse(TrieClient *client, StatusCode *status, uint8_t *data, size_t *dataLen) {
    fprintf(stderr, "DEBUG: receiveResponse - waiting for response...\n");
    if (!client->connected) {
        fprintf(stderr, "DEBUG: Not connected!\n");
        return false;
    }

    // Read length
    uint8_t lengthBuf[9];
    size_t bytesRead = 0;
    uint64_t messageLen;

    // Read at least one byte
    fprintf(stderr, "DEBUG: Reading first byte of length...\n");
    ssize_t n = read(client->sockfd, lengthBuf, 1);
    fprintf(stderr, "DEBUG: read() returned %zd (errno=%d)\n", n, n < 0 ? errno : 0);
    if (n != 1) {
        return false;
    }
    bytesRead = 1;

    // Keep reading until we have a complete varint
    fprintf(stderr, "DEBUG: Starting varint parsing loop, bytesRead=%zu\n", bytesRead);
    while (varintTaggedGet64(lengthBuf, &messageLen) == 0 && bytesRead < sizeof(lengthBuf)) {
        fprintf(stderr, "DEBUG: Varint incomplete, reading more... bytesRead=%zu\n", bytesRead);
        if (read(client->sockfd, lengthBuf + bytesRead, 1) != 1) {
            fprintf(stderr, "DEBUG: Read failed\n");
            return false;
        }
        bytesRead++;
    }
    fprintf(stderr, "DEBUG: Varint parsing complete, messageLen=%lu\n", messageLen);

    fprintf(stderr, "DEBUG: Checking message length validity\n");
    if (messageLen == 0 || messageLen > MAX_RESPONSE_SIZE) {
        fprintf(stderr, "Invalid message length: %lu\n", messageLen);
        return false;
    }

    // Read message
    fprintf(stderr, "DEBUG: Allocating %lu bytes for message\n", messageLen);
    uint8_t *msgBuf = (uint8_t *)malloc(messageLen);
    fprintf(stderr, "DEBUG: malloc() returned %p\n", (void*)msgBuf);
    if (!msgBuf) {
        fprintf(stderr, "DEBUG: malloc failed!\n");
        return false;
    }

    size_t totalRead = 0;
    fprintf(stderr, "DEBUG: Reading %lu bytes of message\n", messageLen);
    while (totalRead < messageLen) {
        fprintf(stderr, "DEBUG: totalRead=%zu, need %lu more bytes\n", totalRead, messageLen - totalRead);
        ssize_t n = read(client->sockfd, msgBuf + totalRead, messageLen - totalRead);
        fprintf(stderr, "DEBUG: read() returned %zd\n", n);
        if (n <= 0) {
            fprintf(stderr, "DEBUG: Read failed or EOF, freeing and returning\n");
            free(msgBuf);
            return false;
        }
        totalRead += n;
    }
    fprintf(stderr, "DEBUG: Message read complete\n");

    // Parse response
    fprintf(stderr, "DEBUG: Parsing response, msgBuf[0]=0x%02x\n", msgBuf[0]);
    *status = (StatusCode)msgBuf[0];
    fprintf(stderr, "DEBUG: Set status=%d\n", *status);
    *dataLen = messageLen - 1;
    fprintf(stderr, "DEBUG: Set dataLen=%zu\n", *dataLen);
    if (*dataLen > 0) {
        fprintf(stderr, "DEBUG: Copying %zu bytes to data buffer\n", *dataLen);
        memcpy(data, msgBuf + 1, *dataLen);
        fprintf(stderr, "DEBUG: memcpy complete\n");
    }

    fprintf(stderr, "DEBUG: About to free msgBuf\n");
    free(msgBuf);
    fprintf(stderr, "DEBUG: free() complete, returning true\n");
    return true;
}

bool cmdPing(TrieClient *client) {
    printf("Sending PING...\n");

    if (!sendCommand(client, CMD_PING, NULL, 0)) {
        return false;
    }

    StatusCode status;
    uint8_t data[MAX_RESPONSE_SIZE];
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen)) {
        fprintf(stderr, "Failed to receive response\n");
        return false;
    }

    if (status == STATUS_OK) {
        printf("PONG (OK)\n");
        return true;
    } else {
        printf("Error: status = 0x%02X\n", status);
        return false;
    }
}

bool cmdStats(TrieClient *client) {
    printf("Sending STATS...\n");

    if (!sendCommand(client, CMD_STATS, NULL, 0)) {
        return false;
    }

    StatusCode status;
    uint8_t data[MAX_RESPONSE_SIZE];
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen)) {
        fprintf(stderr, "Failed to receive response\n");
        return false;
    }

    if (status != STATUS_OK) {
        printf("Error: status = 0x%02X\n", status);
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

    return true;
}

int main(int argc, char *argv[]) {
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

    TrieClient client;
    if (!clientConnect(&client, host, port)) {
        fprintf(stderr, "Failed to connect to server\n");
        return 1;
    }

    bool success = false;

    if (strcmp(command, "ping") == 0) {
        success = cmdPing(&client);
    } else if (strcmp(command, "stats") == 0) {
        success = cmdStats(&client);
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
    }

    clientClose(&client);
    return success ? 0 : 1;
}
