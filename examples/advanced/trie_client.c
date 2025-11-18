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
    CMD_ADD = 0x01,
    CMD_REMOVE = 0x02,
    CMD_SUBSCRIBE = 0x03,
    CMD_UNSUBSCRIBE = 0x04,
    CMD_MATCH = 0x05,
    CMD_LIST = 0x06,
    CMD_STATS = 0x07,
    CMD_SAVE = 0x08,
    CMD_PING = 0x09,
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

    client->host = strdup(host);
    if (!client->host) {
        close(client->sockfd);
        return false;
    }

    client->port = port;
    client->connected = true;
    printf("Connected to %s:%d\n", host, port);
    return true;
}

void clientClose(TrieClient *client) {
    if (client->connected) {
        close(client->sockfd);
        client->connected = false;
    }
    if (client->host) {
        free(client->host);
        client->host = NULL;
    }
}

bool sendCommand(TrieClient *client, CommandType cmd, const uint8_t *payload, size_t payloadLen) {
    if (!client->connected) {
        fprintf(stderr, "Not connected\n");
        return false;
    }

    // Allocate buffer on heap to avoid stack overflow
    const size_t bufferSize = 4096;
    uint8_t *buffer = (uint8_t *)malloc(bufferSize);
    if (!buffer) {
        fprintf(stderr, "Failed to allocate send buffer\n");
        return false;
    }

    // Build message: [Length:varint][CommandID:1byte][Payload]
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
    free(buffer);

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
    printf("Sending PING...\n");

    if (!sendCommand(client, CMD_PING, NULL, 0)) {
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

    free(data);

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

bool cmdAdd(TrieClient *client, const char *pattern, uint32_t subscriberId, const char *subscriberName) {
    printf("Sending ADD pattern='%s' subscriberId=%u subscriberName='%s'...\n", pattern, subscriberId, subscriberName);

    // Build payload: <pattern_len:varint><pattern:bytes><subscriber_id:varint><subscriber_name_len:varint><subscriber_name:bytes>
    uint8_t *payload = (uint8_t *)malloc(4096);
    if (!payload) return false;

    size_t offset = 0;
    size_t patternLen = strlen(pattern);
    offset += varintTaggedPut64(payload + offset, patternLen);
    memcpy(payload + offset, pattern, patternLen);
    offset += patternLen;

    offset += varintTaggedPut64(payload + offset, subscriberId);

    size_t nameLen = strlen(subscriberName);
    offset += varintTaggedPut64(payload + offset, nameLen);
    memcpy(payload + offset, subscriberName, nameLen);
    offset += nameLen;

    bool result = sendCommand(client, CMD_ADD, payload, offset);
    free(payload);

    if (!result) {
        fprintf(stderr, "Failed to send command\n");
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) return false;
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen, MAX_RESPONSE_SIZE)) {
        fprintf(stderr, "Failed to receive response\n");
        free(data);
        return false;
    }

    free(data);

    if (status == STATUS_OK) {
        printf("ADD successful\n");
        return true;
    } else {
        printf("Error: status = 0x%02X\n", status);
        return false;
    }
}

bool cmdRemove(TrieClient *client, const char *pattern) {
    printf("Sending REMOVE pattern='%s'...\n", pattern);

    uint8_t *payload = (uint8_t *)malloc(4096);
    if (!payload) return false;

    size_t offset = 0;
    size_t patternLen = strlen(pattern);
    offset += varintTaggedPut64(payload + offset, patternLen);
    memcpy(payload + offset, pattern, patternLen);
    offset += patternLen;

    bool result = sendCommand(client, CMD_REMOVE, payload, offset);
    free(payload);

    if (!result) {
        fprintf(stderr, "Failed to send command\n");
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) return false;
    size_t dataLen;

    if (!receiveResponse(client, &status, data, &dataLen, MAX_RESPONSE_SIZE)) {
        fprintf(stderr, "Failed to receive response\n");
        free(data);
        return false;
    }

    free(data);

    if (status == STATUS_OK) {
        printf("REMOVE successful\n");
        return true;
    } else {
        printf("Error: status = 0x%02X\n", status);
        return false;
    }
}

bool cmdMatch(TrieClient *client, const char *input) {
    printf("Sending MATCH input='%s'...\n", input);

    uint8_t *payload = (uint8_t *)malloc(4096);
    if (!payload) return false;

    size_t offset = 0;
    size_t inputLen = strlen(input);
    offset += varintTaggedPut64(payload + offset, inputLen);
    memcpy(payload + offset, input, inputLen);
    offset += inputLen;

    bool result = sendCommand(client, CMD_MATCH, payload, offset);
    free(payload);

    if (!result) {
        fprintf(stderr, "Failed to send command\n");
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) return false;
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

    // Parse response: <count:varint>[<subscriber_id:varint><subscriber_name_len:varint><subscriber_name:bytes>]*
    size_t respOffset = 0;
    uint64_t count;
    varintTaggedGet64(data + respOffset, &count);
    respOffset += varintTaggedGetLen(data + respOffset);

    printf("\nMatches found: %lu\n", count);
    for (uint64_t i = 0; i < count; i++) {
        uint64_t subscriberId;
        varintTaggedGet64(data + respOffset, &subscriberId);
        respOffset += varintTaggedGetLen(data + respOffset);

        uint64_t nameLen;
        varintTaggedGet64(data + respOffset, &nameLen);
        respOffset += varintTaggedGetLen(data + respOffset);

        char name[256];
        memcpy(name, data + respOffset, nameLen);
        name[nameLen] = '\0';
        respOffset += nameLen;

        printf("  [%lu] ID=%lu Name='%s'\n", i+1, subscriberId, name);
    }

    free(data);
    return true;
}

bool cmdList(TrieClient *client) {
    printf("Sending LIST...\n");

    if (!sendCommand(client, CMD_LIST, NULL, 0)) {
        fprintf(stderr, "Failed to send command\n");
        return false;
    }

    StatusCode status;
    uint8_t *data = (uint8_t *)malloc(MAX_RESPONSE_SIZE);
    if (!data) return false;
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

    // Parse response: <count:varint>[<pattern_len:varint><pattern:bytes>]*
    size_t respOffset = 0;
    uint64_t count;
    varintTaggedGet64(data + respOffset, &count);
    respOffset += varintTaggedGetLen(data + respOffset);

    printf("\nPatterns (%lu total):\n", count);
    for (uint64_t i = 0; i < count; i++) {
        uint64_t patternLen;
        varintTaggedGet64(data + respOffset, &patternLen);
        respOffset += varintTaggedGetLen(data + respOffset);

        char pattern[256];
        memcpy(pattern, data + respOffset, patternLen);
        pattern[patternLen] = '\0';
        respOffset += patternLen;

        printf("  %lu. %s\n", i+1, pattern);
    }

    free(data);
    return true;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <command> [args] [host] [port]\n", argv[0]);
        printf("Commands:\n");
        printf("  ping                                   - Send PING command\n");
        printf("  stats                                  - Get server statistics\n");
        printf("  add <pattern> <id> <name>              - Add pattern with subscriber\n");
        printf("  remove <pattern>                       - Remove pattern\n");
        printf("  match <input>                          - Match input against patterns\n");
        printf("  list                                   - List all patterns\n");
        printf("\nDefault host: 127.0.0.1\n");
        printf("Default port: 9999\n");
        printf("\nExamples:\n");
        printf("  %s add \"sensors.*.temperature\" 1 \"temp-monitor\"\n", argv[0]);
        printf("  %s match \"sensors.room1.temperature\"\n", argv[0]);
        printf("  %s list\n", argv[0]);
        return 1;
    }

    const char *command = argv[1];

    // Determine host and port based on command
    const char *host;
    uint16_t port;

    if (strcmp(command, "add") == 0) {
        if (argc < 5) {
            fprintf(stderr, "Usage: %s add <pattern> <id> <name> [host] [port]\n", argv[0]);
            return 1;
        }
        host = argc > 5 ? argv[5] : "127.0.0.1";
        port = argc > 6 ? atoi(argv[6]) : 9999;
    } else if (strcmp(command, "remove") == 0 || strcmp(command, "match") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s %s <pattern> [host] [port]\n", argv[0], command);
            return 1;
        }
        host = argc > 3 ? argv[3] : "127.0.0.1";
        port = argc > 4 ? atoi(argv[4]) : 9999;
    } else {
        host = argc > 2 ? argv[2] : "127.0.0.1";
        port = argc > 3 ? atoi(argv[3]) : 9999;
    }

    TrieClient client = {0};

    if (!clientConnect(&client, host, port)) {
        fprintf(stderr, "Failed to connect to server\n");
        return 1;
    }

    bool success = false;

    if (strcmp(command, "ping") == 0) {
        success = cmdPing(&client);
    } else if (strcmp(command, "stats") == 0) {
        success = cmdStats(&client);
    } else if (strcmp(command, "add") == 0) {
        success = cmdAdd(&client, argv[2], atoi(argv[3]), argv[4]);
    } else if (strcmp(command, "remove") == 0) {
        success = cmdRemove(&client, argv[2]);
    } else if (strcmp(command, "match") == 0) {
        success = cmdMatch(&client, argv[2]);
    } else if (strcmp(command, "list") == 0) {
        success = cmdList(&client);
    } else {
        fprintf(stderr, "Unknown command: %s\n", command);
    }

    clientClose(&client);
    return success ? 0 : 1;
}
