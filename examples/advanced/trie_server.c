/*
 * High-Performance Async Trie Server
 *
 * Architecture:
 * - Non-blocking async event loop (select-based for portability)
 * - Binary protocol with varint encoding
 * - Concurrent client support (1000+ connections)
 * - Auto-save persistence with configurable intervals
 * - Token-based authentication (optional)
 * - Per-connection rate limiting
 * - Comprehensive error handling and validation
 *
 * Protocol Format:
 *   Request:  [Length:varint][CommandID:1byte][Payload:varies]
 *   Response: [Length:varint][Status:1byte][Data:varies]
 *
 * Commands:
 *   0x01 ADD         - Add pattern with subscriber
 *   0x02 REMOVE      - Remove entire pattern
 *   0x03 SUBSCRIBE   - Add subscriber to pattern
 *   0x04 UNSUBSCRIBE - Remove subscriber from pattern
 *   0x05 MATCH       - Query pattern matching
 *   0x06 LIST        - List all patterns
 *   0x07 STATS       - Get server statistics
 *   0x08 SAVE        - Trigger manual save
 *   0x09 PING        - Keepalive
 *   0x0A AUTH        - Authenticate with token
 *
 * Status Codes:
 *   0x00 OK             - Success
 *   0x01 ERROR          - Generic error
 *   0x02 AUTH_REQUIRED  - Authentication needed
 *   0x03 RATE_LIMITED   - Too many requests
 *   0x04 INVALID_CMD    - Unknown command
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

#include "../../src/varint.h"
#include "../../src/varintTagged.h"
#include "../../src/varintBitstream.h"

// ============================================================================
// CONFIGURATION
// ============================================================================

#define DEFAULT_PORT 9999
#define MAX_CLIENTS 1024
#define MAX_MESSAGE_SIZE (64 * 1024)  // 64KB max message
#define READ_BUFFER_SIZE 8192
#define WRITE_BUFFER_SIZE 8192
#define AUTH_TOKEN_MAX_LEN 256
#define RATE_LIMIT_WINDOW 1  // seconds
#define RATE_LIMIT_MAX_COMMANDS 1000  // commands per window
#define AUTO_SAVE_INTERVAL 60  // seconds
#define AUTO_SAVE_THRESHOLD 1000  // commands
#define CLIENT_TIMEOUT 300  // seconds (5 minutes idle)

// ============================================================================
// TRIE DATA STRUCTURES (from trie_interactive.c)
// ============================================================================

#define MAX_PATTERN_LENGTH 256
#define MAX_SEGMENT_LENGTH 64
#define MAX_SEGMENTS 16
#define MAX_SUBSCRIBERS 256
#define MAX_SUBSCRIBER_NAME 64

typedef enum {
    SEGMENT_LITERAL = 0,
    SEGMENT_STAR = 1,
    SEGMENT_HASH = 2
} SegmentType;

typedef struct {
    uint32_t id;
    char name[MAX_SUBSCRIBER_NAME];
} Subscriber;

typedef struct {
    Subscriber subscribers[MAX_SUBSCRIBERS];
    size_t count;
} SubscriberList;

typedef struct TrieNode {
    char segment[MAX_SEGMENT_LENGTH];
    SegmentType type;
    bool isTerminal;
    SubscriberList subscribers;
    struct TrieNode **children;
    size_t childCount;
    size_t childCapacity;
} TrieNode;

typedef struct {
    TrieNode *root;
    size_t patternCount;
    size_t nodeCount;
    size_t subscriberCount;
} PatternTrie;

typedef struct {
    uint32_t subscriberIds[MAX_SUBSCRIBERS];
    char subscriberNames[MAX_SUBSCRIBERS][MAX_SUBSCRIBER_NAME];
    size_t count;
} MatchResult;

// ============================================================================
// PROTOCOL DEFINITIONS
// ============================================================================

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

// ============================================================================
// CONNECTION STATE
// ============================================================================

typedef enum {
    CONN_READING_LENGTH,
    CONN_READING_MESSAGE,
    CONN_PROCESSING,
    CONN_WRITING_RESPONSE,
    CONN_CLOSED
} ConnectionState;

typedef struct {
    int fd;
    ConnectionState state;
    bool authenticated;
    time_t lastActivity;

    // Rate limiting
    time_t rateLimitWindowStart;
    uint32_t commandsInWindow;

    // Read state
    uint8_t readBuffer[READ_BUFFER_SIZE];
    size_t readOffset;
    size_t messageLength;
    size_t messageBytesRead;

    // Write state
    uint8_t writeBuffer[WRITE_BUFFER_SIZE];
    size_t writeOffset;
    size_t writeLength;
} ClientConnection;

// ============================================================================
// SERVER STATE
// ============================================================================

typedef struct {
    int listenFd;
    int epollFd;
    PatternTrie trie;
    ClientConnection clients[MAX_CLIENTS];
    bool running;

    // Configuration
    uint16_t port;
    char *authToken;
    bool requireAuth;
    char *saveFilePath;

    // Auto-save state
    time_t lastSaveTime;
    uint64_t commandsSinceLastSave;

    // Statistics
    uint64_t totalConnections;
    uint64_t totalCommands;
    uint64_t totalErrors;
    time_t startTime;
} TrieServer;

// ============================================================================
// FORWARD DECLARATIONS - TRIE OPERATIONS
// ============================================================================

void trieInit(PatternTrie *trie);
void trieFree(PatternTrie *trie);
bool trieInsert(PatternTrie *trie, const char *pattern, uint32_t subscriberId, const char *subscriberName);
bool trieRemovePattern(PatternTrie *trie, const char *pattern);
bool trieRemoveSubscriber(PatternTrie *trie, const char *pattern, uint32_t subscriberId);
void trieMatch(PatternTrie *trie, const char *input, MatchResult *result);
void trieListPatterns(const PatternTrie *trie, char patterns[][MAX_PATTERN_LENGTH], size_t *count, size_t maxCount);
void trieStats(const PatternTrie *trie, size_t *totalNodes, size_t *terminalNodes, size_t *wildcardNodes, size_t *maxDepth);
bool trieSave(const PatternTrie *trie, const char *filename);
bool trieLoad(PatternTrie *trie, const char *filename);

// ============================================================================
// FORWARD DECLARATIONS - SERVER
// ============================================================================

bool serverInit(TrieServer *server, uint16_t port, const char *authToken, const char *saveFilePath);
void serverRun(TrieServer *server);
void serverShutdown(TrieServer *server);
void handleClient(TrieServer *server, ClientConnection *client);
bool processCommand(TrieServer *server, ClientConnection *client, const uint8_t *data, size_t length);
void sendResponse(int epollFd, ClientConnection *client, StatusCode status, const uint8_t *data, size_t dataLen);

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

static void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static bool checkRateLimit(ClientConnection *client) {
    time_t now = time(NULL);

    if (now - client->rateLimitWindowStart >= RATE_LIMIT_WINDOW) {
        // New window
        client->rateLimitWindowStart = now;
        client->commandsInWindow = 0;
    }

    if (client->commandsInWindow >= RATE_LIMIT_MAX_COMMANDS) {
        return false;  // Rate limited
    }

    client->commandsInWindow++;
    return true;
}

static void resetClient(int epollFd, ClientConnection *client) {
    if (client->fd >= 0) {
        // Remove from epoll before closing
        if (epollFd >= 0) {
            epoll_ctl(epollFd, EPOLL_CTL_DEL, client->fd, NULL);
        }
        close(client->fd);
    }
    memset(client, 0, sizeof(ClientConnection));
    client->fd = -1;
    client->state = CONN_CLOSED;
}

// ============================================================================
// TRIE IMPLEMENTATION (Core functions from trie_interactive.c)
// ============================================================================

// Secure string copy with bounds checking (for null-terminated strings)
static void secureStrCopy(char *dst, size_t dstSize, const char *src) {
    if (!dst || !src || dstSize == 0) return;

    size_t i;
    for (i = 0; i < dstSize - 1 && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

// Secure binary copy with explicit length (for non-null-terminated binary data)
static void secureBinaryCopy(char *dst, size_t dstSize, const uint8_t *src, size_t srcLen) {
    if (!dst || !src || dstSize == 0) return;

    // Copy up to the smaller of srcLen or dstSize-1
    size_t copyLen = (srcLen < dstSize - 1) ? srcLen : (dstSize - 1);
    memcpy(dst, src, copyLen);
    dst[copyLen] = '\0';  // Always null-terminate
}

// Validate pattern string (alphanumeric, dots, wildcards only)
static bool validatePattern(const char *pattern) {
    if (!pattern || strlen(pattern) == 0 || strlen(pattern) >= MAX_PATTERN_LENGTH) {
        return false;
    }

    for (size_t i = 0; pattern[i] != '\0'; i++) {
        char c = pattern[i];
        if (!isalnum(c) && c != '.' && c != '*' && c != '#' && c != '_' && c != '-') {
            return false;
        }
    }

    return true;
}

// Validate subscriber ID (non-zero, reasonable range)
static bool validateSubscriberId(uint32_t id) {
    return id > 0 && id < 0xFFFFFF; // Max 16 million subscribers
}

// Validate subscriber name
static bool validateSubscriberName(const char *name) {
    if (!name || strlen(name) == 0 || strlen(name) >= MAX_SUBSCRIBER_NAME) {
        return false;
    }

    for (size_t i = 0; name[i] != '\0'; i++) {
        if (!isalnum(name[i]) && name[i] != '_' && name[i] != '-') {
            return false;
        }
    }

    return true;
}

// ============================================================================
// SUBSCRIBER LIST OPERATIONS
// ============================================================================

static void subscriberListInit(SubscriberList *list) {
    list->count = 0;
}

static bool subscriberListAdd(SubscriberList *list, uint32_t id, const char *name) {
    if (list->count >= MAX_SUBSCRIBERS) {
        return false;
    }

    // Check for duplicates
    for (size_t i = 0; i < list->count; i++) {
        if (list->subscribers[i].id == id) {
            return false; // Already exists
        }
    }

    list->subscribers[list->count].id = id;
    secureStrCopy(list->subscribers[list->count].name, MAX_SUBSCRIBER_NAME, name);
    list->count++;
    return true;
}

static bool subscriberListRemove(SubscriberList *list, uint32_t id) {
    for (size_t i = 0; i < list->count; i++) {
        if (list->subscribers[i].id == id) {
            // Shift remaining elements
            for (size_t j = i; j < list->count - 1; j++) {
                list->subscribers[j] = list->subscribers[j + 1];
            }
            list->count--;
            return true;
        }
    }
    return false;
}

static bool subscriberListContains(const SubscriberList *list, uint32_t id) {
    for (size_t i = 0; i < list->count; i++) {
        if (list->subscribers[i].id == id) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// PATTERN PARSING
// ============================================================================

typedef struct {
    char segments[MAX_SEGMENTS][MAX_SEGMENT_LENGTH];
    SegmentType types[MAX_SEGMENTS];
    size_t count;
} ParsedPattern;

static bool parsePattern(const char *pattern, ParsedPattern *parsed) {
    if (!pattern || !parsed) return false;

    parsed->count = 0;
    const char *start = pattern;
    const char *end = pattern;

    while (*end != '\0' && parsed->count < MAX_SEGMENTS) {
        if (*end == '.') {
            size_t len = end - start;
            if (len == 0 || len >= MAX_SEGMENT_LENGTH) {
                return false;
            }

            if (len == 1 && *start == '*') {
                parsed->types[parsed->count] = SEGMENT_STAR;
                parsed->segments[parsed->count][0] = '*';
                parsed->segments[parsed->count][1] = '\0';
            } else if (len == 1 && *start == '#') {
                parsed->types[parsed->count] = SEGMENT_HASH;
                parsed->segments[parsed->count][0] = '#';
                parsed->segments[parsed->count][1] = '\0';
            } else {
                parsed->types[parsed->count] = SEGMENT_LITERAL;
                memcpy(parsed->segments[parsed->count], start, len);
                parsed->segments[parsed->count][len] = '\0';
            }

            parsed->count++;
            start = end + 1;
        }
        end++;
    }

    // Handle last segment
    if (start != end && parsed->count < MAX_SEGMENTS) {
        size_t len = end - start;
        if (len >= MAX_SEGMENT_LENGTH) {
            return false;
        }

        if (len == 1 && *start == '*') {
            parsed->types[parsed->count] = SEGMENT_STAR;
            parsed->segments[parsed->count][0] = '*';
            parsed->segments[parsed->count][1] = '\0';
        } else if (len == 1 && *start == '#') {
            parsed->types[parsed->count] = SEGMENT_HASH;
            parsed->segments[parsed->count][0] = '#';
            parsed->segments[parsed->count][1] = '\0';
        } else {
            parsed->types[parsed->count] = SEGMENT_LITERAL;
            memcpy(parsed->segments[parsed->count], start, len);
            parsed->segments[parsed->count][len] = '\0';
        }

        parsed->count++;
    }

    return parsed->count > 0;
}

// ============================================================================
// TRIE NODE OPERATIONS
// ============================================================================

static TrieNode *trieNodeCreate(const char *segment, SegmentType type) {
    TrieNode *node = (TrieNode *)calloc(1, sizeof(TrieNode));
    if (!node) return NULL;

    secureStrCopy(node->segment, MAX_SEGMENT_LENGTH, segment);
    node->type = type;
    node->isTerminal = false;
    subscriberListInit(&node->subscribers);
    node->children = NULL;
    node->childCount = 0;
    node->childCapacity = 0;

    return node;
}

static void trieNodeFree(TrieNode *node) {
    if (!node) return;

    for (size_t i = 0; i < node->childCount; i++) {
        trieNodeFree(node->children[i]);
    }
    free(node->children);
    free(node);
}

static bool trieNodeAddChild(TrieNode *parent, TrieNode *child) {
    if (!parent || !child) return false;

    if (parent->childCount >= parent->childCapacity) {
        size_t newCapacity = parent->childCapacity == 0 ? 4 : parent->childCapacity * 2;
        TrieNode **newChildren = (TrieNode **)realloc(parent->children, newCapacity * sizeof(TrieNode *));
        if (!newChildren) return false;

        parent->children = newChildren;
        parent->childCapacity = newCapacity;
    }

    parent->children[parent->childCount++] = child;
    return true;
}

static TrieNode *trieNodeFindChild(TrieNode *parent, const char *segment, SegmentType type) {
    if (!parent) return NULL;

    for (size_t i = 0; i < parent->childCount; i++) {
        TrieNode *child = parent->children[i];
        if (child->type == type && strcmp(child->segment, segment) == 0) {
            return child;
        }
    }

    return NULL;
}

void trieInit(PatternTrie *trie) {
    trie->root = trieNodeCreate("", SEGMENT_LITERAL);
    trie->patternCount = 0;
    trie->nodeCount = 1;
    trie->subscriberCount = 0;
}

void trieFree(PatternTrie *trie) {
    if (trie->root) {
        trieNodeFree(trie->root);
        trie->root = NULL;
    }
}

// ============================================================================
// TRIE OPERATIONS - Full implementations
// ============================================================================

static TrieNode *trieFindNode(TrieNode *root, const ParsedPattern *parsed) {
    if (!root || !parsed) return NULL;

    TrieNode *current = root;

    for (size_t i = 0; i < parsed->count; i++) {
        TrieNode *child = trieNodeFindChild(current, parsed->segments[i], parsed->types[i]);
        if (!child) {
            return NULL;
        }
        current = child;
    }

    return current;
}

bool trieInsert(PatternTrie *trie, const char *pattern, uint32_t subscriberId, const char *subscriberName) {
    if (!trie || !validatePattern(pattern) || !validateSubscriberId(subscriberId) || !validateSubscriberName(subscriberName)) {
        return false;
    }

    ParsedPattern parsed;
    if (!parsePattern(pattern, &parsed)) {
        return false;
    }

    TrieNode *current = trie->root;

    for (size_t i = 0; i < parsed.count; i++) {
        TrieNode *child = trieNodeFindChild(current, parsed.segments[i], parsed.types[i]);

        if (!child) {
            child = trieNodeCreate(parsed.segments[i], parsed.types[i]);
            if (!child) return false;

            if (!trieNodeAddChild(current, child)) {
                trieNodeFree(child);
                return false;
            }

            trie->nodeCount++;
        }

        current = child;
    }

    bool isNewPattern = !current->isTerminal;
    bool isNewSubscriber = !subscriberListContains(&current->subscribers, subscriberId);

    if (!subscriberListAdd(&current->subscribers, subscriberId, subscriberName)) {
        return false;
    }

    current->isTerminal = true;

    if (isNewPattern) {
        trie->patternCount++;
    }
    if (isNewSubscriber) {
        trie->subscriberCount++;
    }

    return true;
}

bool trieRemovePattern(PatternTrie *trie, const char *pattern) {
    if (!trie || !validatePattern(pattern)) {
        return false;
    }

    ParsedPattern parsed;
    if (!parsePattern(pattern, &parsed)) {
        return false;
    }

    // Find the node
    TrieNode *node = trieFindNode(trie->root, &parsed);
    if (!node || !node->isTerminal) {
        return false; // Pattern doesn't exist
    }

    // Remove all subscribers and mark as non-terminal
    size_t removedSubscribers = node->subscribers.count;
    node->subscribers.count = 0;
    node->isTerminal = false;

    trie->patternCount--;
    trie->subscriberCount -= removedSubscribers;

    // TODO: Could implement node pruning here if node has no children
    // For now, we keep the structure (lazy deletion)

    return true;
}

bool trieRemoveSubscriber(PatternTrie *trie, const char *pattern, uint32_t subscriberId) {
    if (!trie || !validatePattern(pattern) || !validateSubscriberId(subscriberId)) {
        return false;
    }

    ParsedPattern parsed;
    if (!parsePattern(pattern, &parsed)) {
        return false;
    }

    TrieNode *node = trieFindNode(trie->root, &parsed);
    if (!node || !node->isTerminal) {
        return false;
    }

    if (!subscriberListRemove(&node->subscribers, subscriberId)) {
        return false;
    }

    trie->subscriberCount--;

    // If no more subscribers, mark as non-terminal
    if (node->subscribers.count == 0) {
        node->isTerminal = false;
        trie->patternCount--;
    }

    return true;
}

// ============================================================================
// PATTERN MATCHING
// ============================================================================

static void matchResultInit(MatchResult *result) {
    result->count = 0;
}

static void matchResultAdd(MatchResult *result, const SubscriberList *subscribers) {
    for (size_t i = 0; i < subscribers->count && result->count < MAX_SUBSCRIBERS; i++) {
        // Check for duplicates
        bool exists = false;
        for (size_t j = 0; j < result->count; j++) {
            if (result->subscriberIds[j] == subscribers->subscribers[i].id) {
                exists = true;
                break;
            }
        }

        if (!exists) {
            result->subscriberIds[result->count] = subscribers->subscribers[i].id;
            secureStrCopy(result->subscriberNames[result->count], MAX_SUBSCRIBER_NAME,
                         subscribers->subscribers[i].name);
            result->count++;
        }
    }
}

static void trieMatchRecursive(TrieNode *node, const char **segments, size_t segmentCount,
                               size_t currentSegment, MatchResult *result) {
    if (currentSegment >= segmentCount) {
        if (node->isTerminal) {
            matchResultAdd(result, &node->subscribers);
        }

        // Check for # wildcards that can match zero segments
        for (size_t i = 0; i < node->childCount; i++) {
            TrieNode *child = node->children[i];
            if (child->type == SEGMENT_HASH) {
                trieMatchRecursive(child, segments, segmentCount, currentSegment, result);
            }
        }
        return;
    }

    const char *segment = segments[currentSegment];

    for (size_t i = 0; i < node->childCount; i++) {
        TrieNode *child = node->children[i];

        if (child->type == SEGMENT_LITERAL) {
            if (strcmp(child->segment, segment) == 0) {
                trieMatchRecursive(child, segments, segmentCount, currentSegment + 1, result);
            }
        } else if (child->type == SEGMENT_STAR) {
            trieMatchRecursive(child, segments, segmentCount, currentSegment + 1, result);
        } else if (child->type == SEGMENT_HASH) {
            // Try matching 0 segments
            trieMatchRecursive(child, segments, segmentCount, currentSegment, result);
            // Try matching 1+ segments
            for (size_t j = currentSegment; j < segmentCount; j++) {
                trieMatchRecursive(child, segments, segmentCount, j + 1, result);
            }
        }
    }
}

void trieMatch(PatternTrie *trie, const char *input, MatchResult *result) {
    if (!trie || !input || !result) return;

    matchResultInit(result);

    ParsedPattern parsed;
    if (!parsePattern(input, &parsed)) {
        return;
    }

    const char *segments[MAX_SEGMENTS];
    for (size_t i = 0; i < parsed.count; i++) {
        segments[i] = parsed.segments[i];
    }

    trieMatchRecursive(trie->root, segments, parsed.count, 0, result);
}

// ============================================================================
// LISTING AND STATISTICS
// ============================================================================

static void trieListPatternsRecursive(TrieNode *node, char *currentPath, size_t pathLen,
                                     char patterns[][MAX_PATTERN_LENGTH], size_t *count, size_t maxCount) {
    if (!node || *count >= maxCount) return;

    if (node->isTerminal) {
        secureStrCopy(patterns[*count], MAX_PATTERN_LENGTH, currentPath);
        (*count)++;
    }

    for (size_t i = 0; i < node->childCount && *count < maxCount; i++) {
        TrieNode *child = node->children[i];

        size_t newLen = pathLen;
        if (pathLen > 0) {
            if (newLen + 1 < MAX_PATTERN_LENGTH) {
                currentPath[newLen++] = '.';
            }
        }

        size_t segLen = strlen(child->segment);
        if (newLen + segLen < MAX_PATTERN_LENGTH) {
            memcpy(currentPath + newLen, child->segment, segLen);
            currentPath[newLen + segLen] = '\0';

            trieListPatternsRecursive(child, currentPath, newLen + segLen, patterns, count, maxCount);
            currentPath[pathLen] = '\0'; // Restore path
        }
    }
}

void trieListPatterns(const PatternTrie *trie, char patterns[][MAX_PATTERN_LENGTH], size_t *count, size_t maxCount) {
    if (!trie || !patterns || !count) return;

    *count = 0;
    char currentPath[MAX_PATTERN_LENGTH] = "";

    trieListPatternsRecursive(trie->root, currentPath, 0, patterns, count, maxCount);
}

void trieStats(const PatternTrie *trie, size_t *totalNodes, size_t *terminalNodes,
               size_t *wildcardNodes, size_t *maxDepth) {
    if (!trie) return;

    *totalNodes = 0;
    *terminalNodes = 0;
    *wildcardNodes = 0;
    *maxDepth = 0;

    TrieNode *queue[4096];
    size_t depths[4096];
    size_t front = 0, back = 0;

    queue[back] = trie->root;
    depths[back] = 0;
    back++;

    while (front < back) {
        TrieNode *node = queue[front];
        size_t depth = depths[front];
        front++;

        (*totalNodes)++;
        if (node->isTerminal) (*terminalNodes)++;
        if (node->type != SEGMENT_LITERAL) (*wildcardNodes)++;
        if (depth > *maxDepth) *maxDepth = depth;

        for (size_t i = 0; i < node->childCount && back < 4096; i++) {
            queue[back] = node->children[i];
            depths[back] = depth + 1;
            back++;
        }
    }
}

// ============================================================================
// PERSISTENCE (SERIALIZATION/DESERIALIZATION)
// ============================================================================

static size_t trieNodeSerialize(const TrieNode *node, uint8_t *buffer) {
    size_t offset = 0;

    // Node flags: isTerminal(1) | type(2) | reserved(5)
    uint64_t flags = 0;
    varintBitstreamSet(&flags, 0, 1, node->isTerminal ? 1 : 0);
    varintBitstreamSet(&flags, 1, 2, node->type);
    flags >>= 56;
    buffer[offset++] = (uint8_t)flags;

    // Segment length and data
    size_t segLen = strlen(node->segment);
    offset += varintTaggedPut64(buffer + offset, segLen);
    memcpy(buffer + offset, node->segment, segLen);
    offset += segLen;

    // Subscriber count and data
    offset += varintTaggedPut64(buffer + offset, node->subscribers.count);
    for (size_t i = 0; i < node->subscribers.count; i++) {
        offset += varintTaggedPut64(buffer + offset, node->subscribers.subscribers[i].id);

        size_t nameLen = strlen(node->subscribers.subscribers[i].name);
        offset += varintTaggedPut64(buffer + offset, nameLen);
        memcpy(buffer + offset, node->subscribers.subscribers[i].name, nameLen);
        offset += nameLen;
    }

    // Child count
    offset += varintTaggedPut64(buffer + offset, node->childCount);

    // Serialize children recursively
    for (size_t i = 0; i < node->childCount; i++) {
        offset += trieNodeSerialize(node->children[i], buffer + offset);
    }

    return offset;
}

static size_t trieNodeDeserialize(TrieNode **node, const uint8_t *buffer) {
    size_t offset = 0;

    *node = trieNodeCreate("", SEGMENT_LITERAL);
    if (!*node) return 0;

    // Read flags
    uint8_t flagsByte = buffer[offset++];
    uint64_t flags = (uint64_t)flagsByte << 56;
    (*node)->isTerminal = varintBitstreamGet(&flags, 0, 1) ? true : false;
    (*node)->type = (SegmentType)varintBitstreamGet(&flags, 1, 2);

    // Read segment
    uint64_t segLen;
    varintTaggedGet64(buffer + offset, &segLen);
    offset += varintTaggedGetLen(buffer + offset);

    if (segLen < MAX_SEGMENT_LENGTH) {
        memcpy((*node)->segment, buffer + offset, segLen);
        (*node)->segment[segLen] = '\0';
    }
    offset += segLen;

    // Read subscribers
    uint64_t subCount;
    varintTaggedGet64(buffer + offset, &subCount);
    offset += varintTaggedGetLen(buffer + offset);

    for (size_t i = 0; i < subCount && i < MAX_SUBSCRIBERS; i++) {
        uint64_t id;
        varintTaggedGet64(buffer + offset, &id);
        offset += varintTaggedGetLen(buffer + offset);

        uint64_t nameLen;
        varintTaggedGet64(buffer + offset, &nameLen);
        offset += varintTaggedGetLen(buffer + offset);

        char name[MAX_SUBSCRIBER_NAME];
        if (nameLen < MAX_SUBSCRIBER_NAME) {
            memcpy(name, buffer + offset, nameLen);
            name[nameLen] = '\0';
        } else {
            name[0] = '\0';
        }
        offset += nameLen;

        subscriberListAdd(&(*node)->subscribers, (uint32_t)id, name);
    }

    // Read children
    uint64_t childCount;
    varintTaggedGet64(buffer + offset, &childCount);
    offset += varintTaggedGetLen(buffer + offset);

    for (size_t i = 0; i < childCount; i++) {
        TrieNode *child;
        size_t childSize = trieNodeDeserialize(&child, buffer + offset);
        if (childSize == 0) break;

        trieNodeAddChild(*node, child);
        offset += childSize;
    }

    return offset;
}

bool trieSave(const PatternTrie *trie, const char *filename) {
    if (!trie || !filename) return false;

    FILE *file = fopen(filename, "wb");
    if (!file) {
        fprintf(stderr, "Error: Failed to open file for writing: %s (%s)\n",
                filename, strerror(errno));
        return false;
    }

    // Allocate buffer (max 16MB for safety)
    size_t bufferSize = 16 * 1024 * 1024;
    uint8_t *buffer = (uint8_t *)malloc(bufferSize);
    if (!buffer) {
        fprintf(stderr, "Error: Failed to allocate save buffer\n");
        fclose(file);
        return false;
    }

    size_t offset = 0;

    // Write magic header
    const char *magic = "TRIE";
    memcpy(buffer + offset, magic, 4);
    offset += 4;

    // Write version
    buffer[offset++] = 1;

    // Write metadata
    offset += varintTaggedPut64(buffer + offset, trie->patternCount);
    offset += varintTaggedPut64(buffer + offset, trie->nodeCount);
    offset += varintTaggedPut64(buffer + offset, trie->subscriberCount);

    // Serialize trie
    offset += trieNodeSerialize(trie->root, buffer + offset);

    // Write to file
    size_t written = fwrite(buffer, 1, offset, file);
    bool success = (written == offset);

    if (!success) {
        fprintf(stderr, "Error: Failed to write complete data to file\n");
    }

    free(buffer);
    fclose(file);

    return success;
}

bool trieLoad(PatternTrie *trie, const char *filename) {
    if (!trie || !filename) return false;

    FILE *file = fopen(filename, "rb");
    if (!file) {
        // Don't print error for missing file on initial load
        if (errno != ENOENT) {
            fprintf(stderr, "Error: Failed to open file for reading: %s (%s)\n",
                    filename, strerror(errno));
        }
        return false;
    }

    // Get file size
    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (fileSize <= 0 || fileSize > 16 * 1024 * 1024) {
        fprintf(stderr, "Error: Invalid file size: %ld bytes\n", fileSize);
        fclose(file);
        return false;
    }

    uint8_t *buffer = (uint8_t *)malloc(fileSize);
    if (!buffer) {
        fprintf(stderr, "Error: Failed to allocate load buffer\n");
        fclose(file);
        return false;
    }

    size_t readSize = fread(buffer, 1, fileSize, file);
    fclose(file);

    if (readSize != (size_t)fileSize) {
        fprintf(stderr, "Error: Failed to read complete file\n");
        free(buffer);
        return false;
    }

    size_t offset = 0;

    // Read and verify magic header
    if (memcmp(buffer + offset, "TRIE", 4) != 0) {
        fprintf(stderr, "Error: Invalid file format (bad magic header)\n");
        free(buffer);
        return false;
    }
    offset += 4;

    // Read version
    uint8_t version = buffer[offset++];
    if (version != 1) {
        fprintf(stderr, "Error: Unsupported file version: %u\n", version);
        free(buffer);
        return false;
    }

    // Read metadata
    uint64_t patternCount, nodeCount, subscriberCount;
    varintTaggedGet64(buffer + offset, &patternCount);
    offset += varintTaggedGetLen(buffer + offset);
    varintTaggedGet64(buffer + offset, &nodeCount);
    offset += varintTaggedGetLen(buffer + offset);
    varintTaggedGet64(buffer + offset, &subscriberCount);
    offset += varintTaggedGetLen(buffer + offset);

    // Clear existing trie (preserve root structure but reset it)
    trieNodeFree(trie->root);
    trie->root = trieNodeCreate("", SEGMENT_LITERAL);
    if (!trie->root) {
        fprintf(stderr, "Error: Failed to create root node\n");
        free(buffer);
        return false;
    }

    // Deserialize root node
    TrieNode *loadedRoot;
    size_t deserializedSize = trieNodeDeserialize(&loadedRoot, buffer + offset);
    if (deserializedSize == 0) {
        fprintf(stderr, "Error: Failed to deserialize trie structure\n");
        free(buffer);
        return false;
    }

    // Copy loaded root's data to existing root
    trieNodeFree(trie->root);
    trie->root = loadedRoot;

    // Update trie metadata
    trie->patternCount = patternCount;
    trie->nodeCount = nodeCount;
    trie->subscriberCount = subscriberCount;

    free(buffer);
    return true;
}

// ============================================================================
// SERVER INITIALIZATION
// ============================================================================

bool serverInit(TrieServer *server, uint16_t port, const char *authToken, const char *saveFilePath) {
    memset(server, 0, sizeof(TrieServer));

    server->port = port;
    server->running = false;
    server->startTime = time(NULL);

    // Initialize all client slots
    for (int i = 0; i < MAX_CLIENTS; i++) {
        server->clients[i].fd = -1;
        server->clients[i].state = CONN_CLOSED;
    }

    // Authentication
    if (authToken && strlen(authToken) > 0) {
        server->authToken = strdup(authToken);
        server->requireAuth = true;
    } else {
        server->authToken = NULL;
        server->requireAuth = false;
    }

    // Save file
    if (saveFilePath) {
        server->saveFilePath = strdup(saveFilePath);
    }

    // Initialize trie
    trieInit(&server->trie);

    // Load existing data if save file exists
    if (server->saveFilePath && access(server->saveFilePath, F_OK) == 0) {
        printf("Loading existing trie from %s...\n", server->saveFilePath);
        if (!trieLoad(&server->trie, server->saveFilePath)) {
            fprintf(stderr, "Warning: Failed to load trie from %s\n", server->saveFilePath);
        }
    }

    // Create listen socket
    server->listenFd = socket(AF_INET, SOCK_STREAM, 0);
    if (server->listenFd < 0) {
        perror("socket");
        return false;
    }

    // Set socket options
    int opt = 1;
    setsockopt(server->listenFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Bind
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server->listenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server->listenFd);
        return false;
    }

    // Listen
    if (listen(server->listenFd, 128) < 0) {
        perror("listen");
        close(server->listenFd);
        return false;
    }

    setNonBlocking(server->listenFd);

    // Create epoll instance
    server->epollFd = epoll_create1(0);
    if (server->epollFd < 0) {
        perror("epoll_create1");
        close(server->listenFd);
        return false;
    }

    // Register listen socket with epoll
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = server->listenFd;
    printf("DEBUG: Registering listen socket (fd=%d) with epoll (fd=%d)\n", server->listenFd, server->epollFd);
    if (epoll_ctl(server->epollFd, EPOLL_CTL_ADD, server->listenFd, &ev) < 0) {
        perror("epoll_ctl: listen socket");
        close(server->epollFd);
        close(server->listenFd);
        return false;
    }
    printf("DEBUG: Listen socket registered successfully\n");

    printf("Trie server listening on port %d (using epoll for high-performance async I/O)\n", port);
    if (server->requireAuth) {
        printf("Authentication: ENABLED\n");
    }
    if (server->saveFilePath) {
        printf("Auto-save: %s (every %d seconds or %d commands)\n",
               server->saveFilePath, AUTO_SAVE_INTERVAL, AUTO_SAVE_THRESHOLD);
    }

    return true;
}

// ============================================================================
// MAIN EVENT LOOP
// ============================================================================

static volatile bool g_shutdown = false;

static void signalHandler(int sig) {
    g_shutdown = true;
}

void serverRun(TrieServer *server) {
    server->running = true;
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    fprintf(stderr, "Server ready. Press Ctrl+C to stop.\n");
    fprintf(stderr, "DEBUG: Entering event loop with epollFd=%d, listenFd=%d\n", server->epollFd, server->listenFd);

    #define MAX_EVENTS 64
    struct epoll_event events[MAX_EVENTS];

    int loopCount = 0;
    while (server->running && !g_shutdown) {
        // Wait for events (1 second timeout)
        int nfds = epoll_wait(server->epollFd, events, MAX_EVENTS, 1000);

        if (loopCount < 5 || nfds > 0) {
            fprintf(stderr, "DEBUG: epoll_wait iteration %d returned nfds=%d\n", loopCount, nfds);
        }
        loopCount++;

        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        if (nfds > 0) {
            fprintf(stderr, "DEBUG: epoll_wait returned %d events\n", nfds);
        }

        // Process all events
        for (int n = 0; n < nfds; n++) {
            int fd = events[n].data.fd;
            fprintf(stderr, "DEBUG: Event on fd=%d (events=0x%x)\n", fd, events[n].events);

            // New connection on listen socket
            if (fd == server->listenFd) {
                printf("DEBUG: New connection attempt on listen socket\n");
                struct sockaddr_in clientAddr;
                socklen_t addrLen = sizeof(clientAddr);
                int clientFd = accept(server->listenFd, (struct sockaddr *)&clientAddr, &addrLen);

                if (clientFd >= 0) {
                    // Find free slot
                    int slot = -1;
                    for (int i = 0; i < MAX_CLIENTS; i++) {
                        if (server->clients[i].fd < 0) {
                            slot = i;
                            break;
                        }
                    }

                    if (slot >= 0) {
                        setNonBlocking(clientFd);
                        ClientConnection *client = &server->clients[slot];
                        resetClient(server->epollFd, client);
                        client->fd = clientFd;
                        client->state = CONN_READING_LENGTH;
                        client->authenticated = !server->requireAuth;
                        client->lastActivity = time(NULL);
                        client->rateLimitWindowStart = time(NULL);

                        // Register client with epoll for edge-triggered reading
                        struct epoll_event ev;
                        ev.events = EPOLLIN | EPOLLET;  // Edge-triggered prevents busy-loop
                        ev.data.fd = clientFd;
                        if (epoll_ctl(server->epollFd, EPOLL_CTL_ADD, clientFd, &ev) < 0) {
                            perror("epoll_ctl: client socket");
                            close(clientFd);
                            client->fd = -1;
                        } else {
                            server->totalConnections++;
                            printf("New connection from %s (slot %d, total connections: %lu)\n",
                                   inet_ntoa(clientAddr.sin_addr), slot, server->totalConnections);
                        }
                    } else {
                        fprintf(stderr, "Max clients reached, rejecting connection\n");
                        close(clientFd);
                    }
                }
                continue;
            }

            // Find client for this fd
            ClientConnection *client = NULL;
            int clientSlot = -1;
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (server->clients[i].fd == fd) {
                    client = &server->clients[i];
                    clientSlot = i;
                    break;
                }
            }

            if (!client) continue;

            bool active = false;

            // Handle read events
            if (events[n].events & EPOLLIN) {
                handleClient(server, client);
                active = true;
            }

            // Handle write events
            if (events[n].events & EPOLLOUT) {
                fprintf(stderr, "DEBUG: EPOLLOUT event on fd=%d, state=%d\n", fd, client->state);
            }
            if (events[n].events & EPOLLOUT && client->state == CONN_WRITING_RESPONSE) {
                fprintf(stderr, "DEBUG: Writing response, writeOffset=%zu, writeLength=%zu\n",
                        client->writeOffset, client->writeLength);
                ssize_t sent = write(client->fd,
                                    client->writeBuffer + client->writeOffset,
                                    client->writeLength - client->writeOffset);
                fprintf(stderr, "DEBUG: write() returned %zd (errno=%d)\n", sent, sent < 0 ? errno : 0);
                if (sent > 0) {
                    client->writeOffset += sent;
                    fprintf(stderr, "DEBUG: Sent %zd bytes, writeOffset now %zu\n", sent, client->writeOffset);
                    if (client->writeOffset >= client->writeLength) {
                        // Response fully sent, switch back to reading
                        client->state = CONN_READING_LENGTH;
                        client->readOffset = 0;
                        client->writeOffset = 0;
                        client->writeLength = 0;

                        // Modify epoll to monitor for read only (edge-triggered)
                        struct epoll_event ev;
                        ev.events = EPOLLIN | EPOLLET;
                        ev.data.fd = client->fd;
                        epoll_ctl(server->epollFd, EPOLL_CTL_MOD, client->fd, &ev);
                    }
                    active = true;
                } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    resetClient(server->epollFd, client);
                }
            }

            if (active) {
                client->lastActivity = time(NULL);
            }
        }

        // Check for client timeouts (periodic maintenance)
        time_t now = time(NULL);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            ClientConnection *client = &server->clients[i];
            if (client->fd >= 0 && now - client->lastActivity > CLIENT_TIMEOUT) {
                printf("Client %d timed out\n", i);
                resetClient(server->epollFd, client);
            }
        }

        // Auto-save check
        if (server->saveFilePath) {
            bool shouldSave = false;

            if (now - server->lastSaveTime >= AUTO_SAVE_INTERVAL) {
                shouldSave = true;
            }
            if (server->commandsSinceLastSave >= AUTO_SAVE_THRESHOLD) {
                shouldSave = true;
            }

            if (shouldSave && server->commandsSinceLastSave > 0) {
                printf("Auto-saving trie (%lu commands since last save)...\n",
                       server->commandsSinceLastSave);
                if (trieSave(&server->trie, server->saveFilePath)) {
                    server->lastSaveTime = now;
                    server->commandsSinceLastSave = 0;
                } else {
                    fprintf(stderr, "Auto-save failed!\n");
                }
            }
        }
    }

    printf("\nShutting down gracefully...\n");
}

void serverShutdown(TrieServer *server) {
    // Close all client connections
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (server->clients[i].fd >= 0) {
            resetClient(server->epollFd, &server->clients[i]);
        }
    }

    // Final save
    if (server->saveFilePath && server->commandsSinceLastSave > 0) {
        printf("Saving trie before shutdown...\n");
        trieSave(&server->trie, server->saveFilePath);
    }

    // Close listen socket
    if (server->listenFd >= 0) {
        close(server->listenFd);
    }

    // Close epoll fd
    if (server->epollFd >= 0) {
        close(server->epollFd);
    }

    // Free resources
    trieFree(&server->trie);
    free(server->authToken);
    free(server->saveFilePath);

    printf("Server shutdown complete.\n");
    printf("Statistics:\n");
    printf("  Total connections: %lu\n", server->totalConnections);
    printf("  Total commands: %lu\n", server->totalCommands);
    printf("  Total errors: %lu\n", server->totalErrors);
    printf("  Uptime: %ld seconds\n", time(NULL) - server->startTime);
}

// ============================================================================
// PROTOCOL HANDLING
// ============================================================================

void sendResponse(int epollFd, ClientConnection *client, StatusCode status, const uint8_t *data, size_t dataLen) {
    fprintf(stderr, "DEBUG: sendResponse called - fd=%d status=0x%02X dataLen=%zu\n", client->fd, status, dataLen);
    // Build response: [Length:varint][Status:1byte][Data]
    uint8_t tempBuf[MAX_MESSAGE_SIZE];
    size_t offset = 0;

    // Reserve space for length (will fill in later)
    size_t lengthOffset = 0;
    offset += 5;  // Max varint size for length

    // Status code
    tempBuf[offset++] = (uint8_t)status;

    // Data payload
    if (data && dataLen > 0) {
        if (offset + dataLen > sizeof(tempBuf)) {
            return;  // Too large
        }
        memcpy(tempBuf + offset, data, dataLen);
        offset += dataLen;
    }

    // Calculate message length (status + data)
    uint64_t messageLen = (offset - 5);

    // Write length at beginning
    size_t lengthBytes = varintTaggedPut64(tempBuf + lengthOffset, messageLen);

    // Copy to write buffer (length + status + data)
    size_t totalSize = lengthBytes + messageLen;
    if (totalSize > WRITE_BUFFER_SIZE) {
        return;  // Response too large
    }

    // Shift message to remove extra length padding
    memmove(tempBuf + lengthBytes, tempBuf + 5, messageLen);
    memcpy(client->writeBuffer, tempBuf, totalSize);
    client->writeLength = totalSize;
    client->writeOffset = 0;
    client->state = CONN_WRITING_RESPONSE;

    // Modify epoll to monitor for both read and write (edge-triggered)
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
    ev.data.fd = client->fd;
    fprintf(stderr, "DEBUG: Modifying epoll for fd=%d to EPOLLIN|EPOLLOUT|EPOLLET, writeLength=%zu\n",
            client->fd, client->writeLength);
    int ret = epoll_ctl(epollFd, EPOLL_CTL_MOD, client->fd, &ev);
    fprintf(stderr, "DEBUG: epoll_ctl MOD returned %d (errno=%d)\n", ret, ret < 0 ? errno : 0);
}

bool processCommand(TrieServer *server, ClientConnection *client, const uint8_t *data, size_t length) {
    fprintf(stderr, "DEBUG: processCommand called - length=%zu\n", length);
    if (length == 0) {
        sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
        return false;
    }

    CommandType cmd = (CommandType)data[0];
    fprintf(stderr, "DEBUG: Command ID: 0x%02X\n", cmd);
    size_t offset = 1;

    // Check authentication
    if (server->requireAuth && !client->authenticated && cmd != CMD_AUTH) {
        sendResponse(server->epollFd, client, STATUS_AUTH_REQUIRED, NULL, 0);
        return false;
    }

    // Check rate limit
    if (!checkRateLimit(client)) {
        sendResponse(server->epollFd, client, STATUS_RATE_LIMITED, NULL, 0);
        server->totalErrors++;
        return false;
    }

    server->totalCommands++;
    server->commandsSinceLastSave++;

    uint8_t responseBuf[MAX_MESSAGE_SIZE];
    size_t responseLen = 0;

    switch (cmd) {
        case CMD_PING: {
            // PING - just respond OK
            sendResponse(server->epollFd, client, STATUS_OK, NULL, 0);
            break;
        }

        case CMD_ADD: {
            // ADD <pattern_len:varint><pattern:bytes><subscriber_id:varint><subscriber_name_len:varint><subscriber_name:bytes>
            uint64_t patternLen;
            varintWidth width = varintTaggedGet64(data + offset, &patternLen);
            if (width == VARINT_WIDTH_INVALID) {
                fprintf(stderr, "Error: Invalid varint for patternLen in CMD_ADD\n");
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }
            offset += width;

            if (offset + patternLen > length) {
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }

            char pattern[MAX_PATTERN_LENGTH];
            secureBinaryCopy(pattern, MAX_PATTERN_LENGTH, data + offset, patternLen);
            offset += patternLen;

            uint64_t subscriberId;
            width = varintTaggedGet64(data + offset, &subscriberId);
            if (width == VARINT_WIDTH_INVALID) {
                fprintf(stderr, "Error: Invalid varint for subscriberId in CMD_ADD\n");
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }
            offset += width;

            uint64_t subscriberNameLen;
            width = varintTaggedGet64(data + offset, &subscriberNameLen);
            if (width == VARINT_WIDTH_INVALID) {
                fprintf(stderr, "Error: Invalid varint for subscriberNameLen in CMD_ADD\n");
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }
            offset += width;

            if (offset + subscriberNameLen > length) {
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }

            char subscriberName[MAX_SUBSCRIBER_NAME];
            secureBinaryCopy(subscriberName, MAX_SUBSCRIBER_NAME, data + offset, subscriberNameLen);

            if (trieInsert(&server->trie, pattern, (uint32_t)subscriberId, subscriberName)) {
                sendResponse(server->epollFd, client, STATUS_OK, NULL, 0);
            } else {
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
            }
            break;
        }

        case CMD_REMOVE: {
            // REMOVE <pattern_len:varint><pattern:bytes>
            uint64_t patternLen;
            varintWidth width = varintTaggedGet64(data + offset, &patternLen);
            if (width == VARINT_WIDTH_INVALID) {
                fprintf(stderr, "Error: Invalid varint for patternLen in CMD_REMOVE\n");
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }
            offset += width;

            if (offset + patternLen > length) {
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }

            char pattern[MAX_PATTERN_LENGTH];
            secureBinaryCopy(pattern, MAX_PATTERN_LENGTH, data + offset, patternLen);

            if (trieRemovePattern(&server->trie, pattern)) {
                sendResponse(server->epollFd, client, STATUS_OK, NULL, 0);
            } else {
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
            }
            break;
        }

        case CMD_SUBSCRIBE: {
            // SUBSCRIBE <pattern_len:varint><pattern:bytes><subscriber_id:varint><subscriber_name_len:varint><subscriber_name:bytes>
            uint64_t patternLen;
            varintWidth width = varintTaggedGet64(data + offset, &patternLen);
            if (width == VARINT_WIDTH_INVALID) {
                fprintf(stderr, "Error: Invalid varint for patternLen in CMD_SUBSCRIBE\n");
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }
            offset += width;

            if (offset + patternLen > length) {
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }

            char pattern[MAX_PATTERN_LENGTH];
            secureBinaryCopy(pattern, MAX_PATTERN_LENGTH, data + offset, patternLen);
            offset += patternLen;

            uint64_t subscriberId;
            width = varintTaggedGet64(data + offset, &subscriberId);
            if (width == VARINT_WIDTH_INVALID) {
                fprintf(stderr, "Error: Invalid varint for subscriberId in CMD_SUBSCRIBE\n");
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }
            offset += width;

            uint64_t subscriberNameLen;
            width = varintTaggedGet64(data + offset, &subscriberNameLen);
            if (width == VARINT_WIDTH_INVALID) {
                fprintf(stderr, "Error: Invalid varint for subscriberNameLen in CMD_SUBSCRIBE\n");
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }
            offset += width;

            if (offset + subscriberNameLen > length) {
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }

            char subscriberName[MAX_SUBSCRIBER_NAME];
            secureBinaryCopy(subscriberName, MAX_SUBSCRIBER_NAME, data + offset, subscriberNameLen);

            // CMD_SUBSCRIBE is the same as CMD_ADD - both insert pattern with subscriber
            if (trieInsert(&server->trie, pattern, (uint32_t)subscriberId, subscriberName)) {
                sendResponse(server->epollFd, client, STATUS_OK, NULL, 0);
            } else {
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
            }
            break;
        }

        case CMD_UNSUBSCRIBE: {
            // UNSUBSCRIBE <pattern_len:varint><pattern:bytes><subscriber_id:varint>
            uint64_t patternLen;
            varintWidth width = varintTaggedGet64(data + offset, &patternLen);
            if (width == VARINT_WIDTH_INVALID) {
                fprintf(stderr, "Error: Invalid varint for patternLen in CMD_UNSUBSCRIBE\n");
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }
            offset += width;

            if (offset + patternLen > length) {
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }

            char pattern[MAX_PATTERN_LENGTH];
            secureBinaryCopy(pattern, MAX_PATTERN_LENGTH, data + offset, patternLen);
            offset += patternLen;

            uint64_t subscriberId;
            width = varintTaggedGet64(data + offset, &subscriberId);
            if (width == VARINT_WIDTH_INVALID) {
                fprintf(stderr, "Error: Invalid varint for subscriberId in CMD_UNSUBSCRIBE\n");
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }
            offset += width;

            if (trieRemoveSubscriber(&server->trie, pattern, (uint32_t)subscriberId)) {
                sendResponse(server->epollFd, client, STATUS_OK, NULL, 0);
            } else {
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
            }
            break;
        }

        case CMD_MATCH: {
            // MATCH <input_len:varint><input:bytes>
            // Response: <count:varint>[<subscriber_id:varint><subscriber_name_len:varint><subscriber_name:bytes>]*
            uint64_t inputLen;
            varintWidth width = varintTaggedGet64(data + offset, &inputLen);
            if (width == VARINT_WIDTH_INVALID) {
                fprintf(stderr, "Error: Invalid varint for inputLen in CMD_MATCH\n");
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }
            offset += width;

            if (offset + inputLen > length) {
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }

            char input[MAX_PATTERN_LENGTH];
            secureBinaryCopy(input, MAX_PATTERN_LENGTH, data + offset, inputLen);

            MatchResult result;
            trieMatch(&server->trie, input, &result);

            // Build response
            size_t pos = 0;
            pos += varintTaggedPut64(responseBuf + pos, result.count);

            for (size_t i = 0; i < result.count; i++) {
                pos += varintTaggedPut64(responseBuf + pos, result.subscriberIds[i]);
                size_t nameLen = strlen(result.subscriberNames[i]);
                pos += varintTaggedPut64(responseBuf + pos, nameLen);
                memcpy(responseBuf + pos, result.subscriberNames[i], nameLen);
                pos += nameLen;
            }

            sendResponse(server->epollFd, client, STATUS_OK, responseBuf, pos);
            break;
        }

        case CMD_LIST: {
            // LIST - return all patterns
            // Response: <count:varint>[<pattern_len:varint><pattern:bytes>]*
            char patterns[MAX_SUBSCRIBERS][MAX_PATTERN_LENGTH];
            size_t count;
            trieListPatterns(&server->trie, patterns, &count, MAX_SUBSCRIBERS);

            size_t pos = 0;
            pos += varintTaggedPut64(responseBuf + pos, count);

            for (size_t i = 0; i < count; i++) {
                size_t patternLen = strlen(patterns[i]);
                pos += varintTaggedPut64(responseBuf + pos, patternLen);
                memcpy(responseBuf + pos, patterns[i], patternLen);
                pos += patternLen;
            }

            sendResponse(server->epollFd, client, STATUS_OK, responseBuf, pos);
            break;
        }

        case CMD_AUTH: {
            // AUTH <token_len:varint><token:bytes>
            if (!server->requireAuth) {
                sendResponse(server->epollFd, client, STATUS_OK, NULL, 0);
                break;
            }

            uint64_t tokenLen;
            varintWidth width = varintTaggedGet64(data + offset, &tokenLen);
            if (width == VARINT_WIDTH_INVALID) {
                fprintf(stderr, "Error: Invalid varint for tokenLen in CMD_AUTH\n");
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }
            offset += width;

            if (offset + tokenLen > length) {
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
                return false;
            }

            if (tokenLen == strlen(server->authToken) &&
                memcmp(data + offset, server->authToken, tokenLen) == 0) {
                client->authenticated = true;
                sendResponse(server->epollFd, client, STATUS_OK, NULL, 0);
            } else {
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                server->totalErrors++;
            }
            break;
        }

        case CMD_STATS: {
            // STATS - return server statistics
            // Format: patterns:varint, subscribers:varint, nodes:varint,
            //         connections:varint, commands:varint, uptime:varint
            size_t pos = 0;
            size_t totalNodes, terminalNodes, wildcardNodes, maxDepth;
            trieStats(&server->trie, &totalNodes, &terminalNodes, &wildcardNodes, &maxDepth);

            pos += varintTaggedPut64(responseBuf + pos, server->trie.patternCount);
            pos += varintTaggedPut64(responseBuf + pos, server->trie.subscriberCount);
            pos += varintTaggedPut64(responseBuf + pos, totalNodes);
            pos += varintTaggedPut64(responseBuf + pos, server->totalConnections);
            pos += varintTaggedPut64(responseBuf + pos, server->totalCommands);
            pos += varintTaggedPut64(responseBuf + pos, time(NULL) - server->startTime);

            sendResponse(server->epollFd, client, STATUS_OK, responseBuf, pos);
            break;
        }

        case CMD_SAVE: {
            // SAVE - trigger manual save
            if (server->saveFilePath) {
                if (trieSave(&server->trie, server->saveFilePath)) {
                    server->lastSaveTime = time(NULL);
                    server->commandsSinceLastSave = 0;
                    sendResponse(server->epollFd, client, STATUS_OK, NULL, 0);
                } else {
                    sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
                    server->totalErrors++;
                }
            } else {
                sendResponse(server->epollFd, client, STATUS_ERROR, NULL, 0);
            }
            break;
        }

        default:
            sendResponse(server->epollFd, client, STATUS_INVALID_CMD, NULL, 0);
            server->totalErrors++;
            return false;
    }

    return true;
}

void handleClient(TrieServer *server, ClientConnection *client) {
    // In edge-triggered mode, we must read until EAGAIN
    while (client->state == CONN_READING_LENGTH || client->state == CONN_READING_MESSAGE) {
        ssize_t bytesRead = read(client->fd,
                                 client->readBuffer + client->readOffset,
                                 READ_BUFFER_SIZE - client->readOffset);

        fprintf(stderr, "DEBUG: handleClient fd=%d bytesRead=%zd errno=%d state=%d\n",
                client->fd, bytesRead, errno, client->state);

        if (bytesRead <= 0) {
            if (bytesRead < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                fprintf(stderr, "DEBUG: EAGAIN, returning\n");
                return;  // No more data available
            }
            // Connection closed or error
            fprintf(stderr, "DEBUG: Connection closed or error, resetting client\n");
            resetClient(server->epollFd, client);
            return;
        }

        fprintf(stderr, "DEBUG: Read %zd bytes, readOffset=%zu\n", bytesRead, client->readOffset);
        // Print hex dump of what we read
        fprintf(stderr, "DEBUG: Data (hex): ");
        for (ssize_t i = 0; i < bytesRead && i < 16; i++) {
            fprintf(stderr, "%02x ", client->readBuffer[client->readOffset + i]);
        }
        fprintf(stderr, "\n");

        client->readOffset += bytesRead;

        // Parse message length if we're in that state
        if (client->state == CONN_READING_LENGTH) {
            fprintf(stderr, "DEBUG: Trying to parse varint length from %zu bytes\n", client->readOffset);
            // Try to read varint length
            if (client->readOffset > 0) {
                uint64_t msgLen;
                size_t varintLen = varintTaggedGet64(client->readBuffer, &msgLen);
                fprintf(stderr, "DEBUG: varintTaggedGet64 returned %zu, msgLen=%lu\n", varintLen, msgLen);
                if (varintLen == 0) {
                    // Not enough bytes yet for complete varint
                    if (client->readOffset >= 9) {
                        // Invalid varint (too long)
                        resetClient(server->epollFd, client);
                        return;
                    }
                    continue;  // Try reading more data
                }

                client->messageLength = (size_t)msgLen;
                if (client->messageLength == 0 || client->messageLength > MAX_MESSAGE_SIZE) {
                    resetClient(server->epollFd, client);
                    return;
                }

                // We have the length, move to reading message
                size_t lengthBytes = varintTaggedGetLen(client->readBuffer);
                client->messageBytesRead = client->readOffset - lengthBytes;

                // Move any extra bytes to beginning of buffer
                if (client->messageBytesRead > 0) {
                    memmove(client->readBuffer, client->readBuffer + lengthBytes, client->messageBytesRead);
                }
                client->readOffset = client->messageBytesRead;
                client->state = CONN_READING_MESSAGE;
            }
        }

        // Read message body
        if (client->state == CONN_READING_MESSAGE) {
            client->messageBytesRead = client->readOffset;

            if (client->messageBytesRead >= client->messageLength) {
                // Complete message received, process it
                processCommand(server, client, client->readBuffer, client->messageLength);

                // Reset for next message
                // Only reset state if processCommand didn't change it (e.g., to CONN_WRITING_RESPONSE)
                fprintf(stderr, "DEBUG: After processCommand, client state=%d\n", client->state);
                if (client->state == CONN_READING_MESSAGE) {
                    fprintf(stderr, "DEBUG: State is still CONN_READING_MESSAGE, resetting to CONN_READING_LENGTH\n");
                    size_t extraBytes = client->messageBytesRead - client->messageLength;
                    if (extraBytes > 0) {
                        memmove(client->readBuffer,
                               client->readBuffer + client->messageLength,
                               extraBytes);
                    }
                    client->readOffset = extraBytes;
                    client->messageLength = 0;
                    client->messageBytesRead = 0;
                    client->state = CONN_READING_LENGTH;
                } else {
                    fprintf(stderr, "DEBUG: State changed to %d, breaking out of loop\n", client->state);
                }
                // If state changed to CONN_WRITING_RESPONSE, exit loop to let event loop handle it
                break;
            } else {
                // Need more data, continue reading
                continue;
            }
        }
    }
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char *argv[]) {
    uint16_t port = DEFAULT_PORT;
    char *authToken = NULL;
    char *saveFile = NULL;

    printf("DEBUG: Starting trie_server\n");

    // Simple argument parsing
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--auth") == 0 && i + 1 < argc) {
            authToken = argv[++i];
        } else if (strcmp(argv[i], "--save") == 0 && i + 1 < argc) {
            saveFile = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [OPTIONS]\n", argv[0]);
            printf("Options:\n");
            printf("  --port <port>     Listen port (default: %d)\n", DEFAULT_PORT);
            printf("  --auth <token>    Require authentication token\n");
            printf("  --save <file>     Auto-save file path\n");
            printf("  --help            Show this help\n");
            return 0;
        }
    }

    printf("DEBUG: Calling serverInit\n");
    fflush(stdout);

    // Allocate server on heap (struct is 16MB, too large for stack)
    TrieServer *server = (TrieServer *)malloc(sizeof(TrieServer));
    if (!server) {
        fprintf(stderr, "Failed to allocate server memory\n");
        return 1;
    }

    if (!serverInit(server, port, authToken, saveFile)) {
        fprintf(stderr, "Failed to initialize server\n");
        free(server);
        return 1;
    }

    printf("DEBUG: serverInit complete, calling serverRun\n");
    fflush(stdout);

    serverRun(server);
    serverShutdown(server);
    free(server);

    return 0;
}
