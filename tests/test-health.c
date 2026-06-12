/*
 * Unit test for lhsr_compute_health_score() and lhsr_health_label()
 *
 * Tests the composite health score formula against known inputs.
 * The trend DB is intentionally not initialized — trend_query returns -1,
 * so trend penalties are not applied. This tests the core formula:
 *   start: 100
 *   reallocated: -10 per 10 sectors (max -30)
 *   pending:     -15 per sector (max -30)
 *   uncorrectable: -20 per sector (max -40)
 *   temp > 60:   -15
 *   temp > 50:   -10
 *   errors:      -5 per error (max -15)
 *   clamp 0-100
 *
 * Labels: >=90 OK, >=70 WARNING, >=40 CRITICAL, <40 FAILING
 *
 * Build: cc -Wall -Wextra -O2 -g -I../userspace/daemon -I../include -I../lib \
 *            test-health.c ../userspace/daemon/lhsr-health.o \
 *            ../userspace/daemon/lhsr-trend.o -lsqlite3 -o test-health
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lhsr-health.h"

static int tests_run = 0;
static int tests_passed = 0;
static int tests_failed = 0;

#define ASSERT_EQ(expected, actual, msg) do { \
    tests_run++; \
    if ((expected) == (actual)) { \
        tests_passed++; \
    } else { \
        tests_failed++; \
        fprintf(stderr, "FAIL [%s:%d] %s: expected %d, got %d\n", \
                __FILE__, __LINE__, msg, (expected), (actual)); \
    } \
} while(0)

#define ASSERT_STR_EQ(expected, actual, msg) do { \
    tests_run++; \
    if (strcmp((expected), (actual)) == 0) { \
        tests_passed++; \
    } else { \
        tests_failed++; \
        fprintf(stderr, "FAIL [%s:%d] %s: expected \"%s\", got \"%s\"\n", \
                __FILE__, __LINE__, msg, (expected), (actual)); \
    } \
} while(0)

static void test_healthy_disk(void)
{
    struct disk_health dh;
    memset(&dh, 0, sizeof(dh));
    dh.smart_reallocated = 0;
    dh.smart_pending = 0;
    dh.smart_uncorrectable = 0;
    dh.temperature = 30;
    dh.consecutive_errors = 0;

    int score = lhsr_compute_health_score(&dh, "/dev/null");
    ASSERT_EQ(100, score, "healthy disk score");
    ASSERT_STR_EQ("OK", lhsr_health_label(score), "healthy disk label");
}

static void test_reallocated_penalty(void)
{
    struct disk_health dh;
    memset(&dh, 0, sizeof(dh));

    /* 5 reallocated → -0 (only full 10s count) */
    dh.smart_reallocated = 5;
    ASSERT_EQ(100, lhsr_compute_health_score(&dh, "/dev/null"),
              "5 reallocated (no penalty)");

    /* 10 reallocated → -10 */
    dh.smart_reallocated = 10;
    ASSERT_EQ(90, lhsr_compute_health_score(&dh, "/dev/null"),
              "10 reallocated (-10)");

    /* 30 reallocated → -30 (max) */
    dh.smart_reallocated = 30;
    ASSERT_EQ(70, lhsr_compute_health_score(&dh, "/dev/null"),
              "30 reallocated (-30 max)");

    /* 100 reallocated → capped at -30 */
    dh.smart_reallocated = 100;
    ASSERT_EQ(70, lhsr_compute_health_score(&dh, "/dev/null"),
              "100 reallocated (capped -30)");
}

static void test_pending_penalty(void)
{
    struct disk_health dh;
    memset(&dh, 0, sizeof(dh));

    /* 1 pending → -15 */
    dh.smart_pending = 1;
    ASSERT_EQ(85, lhsr_compute_health_score(&dh, "/dev/null"),
              "1 pending (-15)");

    /* 2 pending → -30 (max) */
    dh.smart_pending = 2;
    ASSERT_EQ(70, lhsr_compute_health_score(&dh, "/dev/null"),
              "2 pending (-30 max)");

    /* 10 pending → capped at -30 */
    dh.smart_pending = 10;
    ASSERT_EQ(70, lhsr_compute_health_score(&dh, "/dev/null"),
              "10 pending (capped -30)");
}

static void test_uncorrectable_penalty(void)
{
    struct disk_health dh;
    memset(&dh, 0, sizeof(dh));

    /* 1 uncorrectable → -20 */
    dh.smart_uncorrectable = 1;
    ASSERT_EQ(80, lhsr_compute_health_score(&dh, "/dev/null"),
              "1 uncorrectable (-20)");

    /* 2 uncorrectable → -40 (max) */
    dh.smart_uncorrectable = 2;
    ASSERT_EQ(60, lhsr_compute_health_score(&dh, "/dev/null"),
              "2 uncorrectable (-40 max)");

    /* 5 uncorrectable → capped at -40 */
    dh.smart_uncorrectable = 5;
    ASSERT_EQ(60, lhsr_compute_health_score(&dh, "/dev/null"),
              "5 uncorrectable (capped -40)");
}

static void test_temperature_penalty(void)
{
    struct disk_health dh;
    memset(&dh, 0, sizeof(dh));

    dh.temperature = 40;
    ASSERT_EQ(100, lhsr_compute_health_score(&dh, "/dev/null"),
              "40°C (no penalty)");

    dh.temperature = 50;
    ASSERT_EQ(100, lhsr_compute_health_score(&dh, "/dev/null"),
              "50°C (no penalty, threshold is >50)");

    dh.temperature = 51;
    ASSERT_EQ(90, lhsr_compute_health_score(&dh, "/dev/null"),
              "51°C (-10, >50 threshold)");

    dh.temperature = 60;
    ASSERT_EQ(90, lhsr_compute_health_score(&dh, "/dev/null"),
              "60°C (-10, >60 threshold is >60)");

    dh.temperature = 61;
    ASSERT_EQ(85, lhsr_compute_health_score(&dh, "/dev/null"),
              "61°C (-15, >60 threshold)");
}

static void test_errors_penalty(void)
{
    struct disk_health dh;
    memset(&dh, 0, sizeof(dh));

    dh.consecutive_errors = 1;
    ASSERT_EQ(95, lhsr_compute_health_score(&dh, "/dev/null"),
              "1 error (-5)");

    dh.consecutive_errors = 3;
    ASSERT_EQ(85, lhsr_compute_health_score(&dh, "/dev/null"),
              "3 errors (-15 max)");

    dh.consecutive_errors = 10;
    ASSERT_EQ(85, lhsr_compute_health_score(&dh, "/dev/null"),
              "10 errors (capped -15)");
}

static void test_combined_penalties(void)
{
    struct disk_health dh;
    memset(&dh, 0, sizeof(dh));

    /* Reallocated 20 (-20) + pending 1 (-15) = 65 */
    dh.smart_reallocated = 20;
    dh.smart_pending = 1;
    ASSERT_EQ(65, lhsr_compute_health_score(&dh, "/dev/null"),
              "realloc 20 + pending 1 = 65");

    /* All max penalties: realloc=-30, pending=-30, uncorr=-40, temp=-15, errors=-15
     * Total: -130 → clamped to 0 */
    memset(&dh, 0, sizeof(dh));
    dh.smart_reallocated = 100;
    dh.smart_pending = 10;
    dh.smart_uncorrectable = 5;
    dh.temperature = 70;
    dh.consecutive_errors = 10;
    ASSERT_EQ(0, lhsr_compute_health_score(&dh, "/dev/null"),
              "all max penalties clamped to 0");
}

static void test_label_boundaries(void)
{
    /* >= 90 → OK */
    ASSERT_STR_EQ("OK",      lhsr_health_label(100), "label 100");
    ASSERT_STR_EQ("OK",      lhsr_health_label(90),  "label 90");
    /* >= 70 → WARNING */
    ASSERT_STR_EQ("WARNING", lhsr_health_label(89),  "label 89");
    ASSERT_STR_EQ("WARNING", lhsr_health_label(70),  "label 70");
    /* >= 40 → CRITICAL */
    ASSERT_STR_EQ("CRITICAL", lhsr_health_label(69), "label 69");
    ASSERT_STR_EQ("CRITICAL", lhsr_health_label(40), "label 40");
    /* < 40 → FAILING */
    ASSERT_STR_EQ("FAILING", lhsr_health_label(39),  "label 39");
    ASSERT_STR_EQ("FAILING", lhsr_health_label(0),   "label 0");
    ASSERT_STR_EQ("FAILING", lhsr_health_label(-1),  "label -1 (clamped)");
}

int main(void)
{
    printf("LHSR Health Score Unit Tests\n");
    printf("============================\n\n");

    test_healthy_disk();
    test_reallocated_penalty();
    test_pending_penalty();
    test_uncorrectable_penalty();
    test_temperature_penalty();
    test_errors_penalty();
    test_combined_penalties();
    test_label_boundaries();

    printf("\n");
    printf("Results: %d/%d passed, %d/%d failed\n",
           tests_passed, tests_run, tests_failed, tests_run);

    return tests_failed ? 1 : 0;
}
