#ifndef CTEST_H
#define CTEST_H

#include <inttypes.h> /* for using PRIu64, etc */
#include <stdio.h>
#include <stdlib.h>

/* Allocation for tests. A test cannot do anything useful without its
 * buffers, so a failed allocation ends the run here instead of handing
 * NULL to the code under test. */
_Noreturn static inline void ctestAllocFailed_(const size_t bytes) {
    fprintf(stderr, "test allocation of %zu bytes failed\n", bytes);
    abort();
}

static inline void *ctestMalloc(const size_t bytes) {
    void *p = malloc(bytes);
    if (!p) {
        ctestAllocFailed_(bytes);
    }
    return p;
}

static inline void *ctestCalloc(const size_t n, const size_t size) {
    void *p = calloc(n, size);
    if (!p) {
        ctestAllocFailed_(n * size);
    }
    return p;
}

static inline void *ctestRealloc(void *old, const size_t bytes) {
    void *p = realloc(old, bytes);
    if (!p) {
        ctestAllocFailed_(bytes);
    }
    return p;
}

/* Generate new string concatenating integer i against string 'prefix' */
#define CTEST_INCLUDE_GEN(named)                                               \
    static char *gen##named(const char *prefix, int i) {                       \
        static char result[64] = {0};                                          \
        snprintf(result, sizeof(result), "%s%d", prefix, i);                   \
        return result;                                                         \
    }

#ifdef CTEST_INCLUDE_KEYGEN
CTEST_INCLUDE_GEN(key)
#endif

#ifdef CTEST_INCLUDE_VALGEN
CTEST_INCLUDE_GEN(val)
#endif

/* Only print the filename as debug prefix, not entire file path.
 * Just: detect the last '/' in file path, then increment one after it.
 *       if no '/' detected, use entire filename. */
#define currentFilename()                                                      \
    do {                                                                       \
        char *filenameOnly = strrchr(__FILE__, '/');                           \
        char *filePos = filenameOnly ? ++filenameOnly : __FILE__;              \
        printf("%s:%s:%d\t", filePos, __func__, __LINE__);                     \
    } while (0)

#define ERROR                                                                  \
    do {                                                                       \
        printf("\tERROR!\n");                                                  \
        err++;                                                                 \
    } while (0)

#define ERR(x, ...)                                                            \
    do {                                                                       \
        currentFilename();                                                     \
        printf("ERROR! " x "\n", __VA_ARGS__);                                 \
        err++;                                                                 \
    } while (0)

#define ERRR(x)                                                                \
    do {                                                                       \
        currentFilename();                                                     \
        printf("ERROR! " x "\n");                                              \
        err++;                                                                 \
    } while (0)

#define TEST(name) printf("test — %s\n", name);
#define TEST_DESC(name, ...) printf("test — " name "\n", __VA_ARGS__);

#define TEST_FINAL_RESULT                                                      \
    do {                                                                       \
        if (!err)                                                              \
            printf("ALL TESTS PASSED!\n");                                     \
        else                                                                   \
            ERR("Sorry, not all tests passed!  In fact, %d tests failed.",     \
                err);                                                          \
        return err;                                                            \
    } while (0)

#endif /* CTEST_H */
