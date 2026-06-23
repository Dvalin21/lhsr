#!/bin/bash
#
# LHSR Automated Test Runner
# Usage: sudo ./tests/runner.sh [--red] [--root-tests] [test_glob...]
#
# Discovers test scripts, runs each, captures PASS/FAIL, stops on first failure.
#
# Test categories (tiered by portability):
#   Default:    tests/*.sh          — loopback-based, self-contained, any system
#   --root-tests: test_*.sh         — may reference real hardware (/dev/sd*)
#   --red:       RED-phase tests    — expected to fail (document known bugs)
#
set -euo pipefail

LHSR_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$LHSR_ROOT"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

pass=0
fail=0
skip=0
total=0
failed_names=""

log_info()  { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_test()  { echo -e "${BLUE}[TEST]${NC} $1"; }
log_debug() { echo -e "${CYAN}[DEBUG]${NC} $1"; }

# Parse flags
INCLUDE_RED=false
INCLUDE_ROOT=false
LIST_ONLY=false
TEST_FILTERS=()
for arg in "$@"; do
    case "$arg" in
        --red)        INCLUDE_RED=true ;;
        --root-tests) INCLUDE_ROOT=true ;;
        --list)       LIST_ONLY=true ;;
        --help)       echo "Usage: $0 [--red] [--root-tests] [test_glob...]" && exit 0 ;;
        *)            TEST_FILTERS+=("$arg") ;;
    esac
done

# Root check
if [ "$EUID" -ne 0 ] && [ "$LIST_ONLY" = false ]; then
    log_error "Must run as root (need insmod/rmmod/dmsetup)"
    exit 1
fi

# Discover tests
declare -a TEST_SCRIPTS=()
declare -A TEST_STATUS=()  # "pass" | "red" | "root"

# Tier 1: tests/*.sh — loopback-based, self-contained, run everywhere
for f in "$LHSR_ROOT"/tests/*.sh; do
    [ -f "$f" ] || continue
    name="$(basename "$f")"
    [ "$name" = "runner.sh" ] && continue
    TEST_SCRIPTS+=("$f")
    TEST_STATUS["$f"]="pass"
done

# Tier 2: root-level test_*.sh — may reference real hardware
for f in "$LHSR_ROOT"/test_*.sh; do
    [ -f "$f" ] || continue
    name="$(basename "$f")"
    TEST_SCRIPTS+=("$f")
    TEST_STATUS["$f"]="root"
done

# Mark RED-phase tests (known expected failures = bug documentation)
# TDD RED phase tests are expected to FAIL — they document bugs before fixes.
declare -a RED_TESTS=(
    "test_red_phase.sh"
    "test_ctr_timeout.sh"
    "test_hang_regression.sh"
    "test_raid6_write.sh"
)
for red in "${RED_TESTS[@]}"; do
    for f in "${!TEST_STATUS[@]}"; do
        if [[ "$(basename "$f")" == "$red" ]]; then
            TEST_STATUS["$f"]="red"
        fi
    done
done

# If no filter args, use all tests (filtered by tier flags below)
if [ ${#TEST_FILTERS[@]} -eq 0 ]; then
    FILTERED_SCRIPTS=("${TEST_SCRIPTS[@]}")
else
    FILTERED_SCRIPTS=()
    for f in "${TEST_SCRIPTS[@]}"; do
        name="$(basename "$f")"
        for pattern in "${TEST_FILTERS[@]}"; do
            if [[ "$name" == $pattern ]]; then
                FILTERED_SCRIPTS+=("$f")
                break
            fi
        done
    done
fi

# Handle --list
if [ "$LIST_ONLY" = true ]; then
    echo ""
    echo "Discovered test scripts:"
    echo "========================="
    for f in "${FILTERED_SCRIPTS[@]}"; do
        name="$(basename "$f")"
        status="${TEST_STATUS[$f]}"
        case "$status" in
            red)  echo "  [RED]   $name (expected to fail)" ;;
            root) echo "  [ROOT]  $name (hw-dependent)" ;;
            pass) echo "  [LOOP]  $name (loopback, auto)" ;;
        esac
    done
    echo ""
    echo "Legend: LOOP=loopback self-contained | ROOT=hw-dependent | RED=expected-fail"
    exit 0
fi

echo ""
echo "=============================================="
echo "  LHSR Automated Test Runner"
echo "  $(date -u '+%Y-%m-%d %H:%M UTC')"
echo "  Host: $(uname -n) | Kernel: $(uname -r)"
echo "=============================================="
echo ""

# Build first
log_info "Building all targets..."
if ! make all > /dev/null 2>&1; then
    log_error "Build failed! Aborting."
    exit 1
fi
log_info "Build OK."

echo ""

# Run each test
for f in "${FILTERED_SCRIPTS[@]}"; do
    name="$(basename "$f")"
    expected="${TEST_STATUS[$f]}"

    # Skip RED-phase tests unless --red flag given
    if [ "$expected" = "red" ] && [ "$INCLUDE_RED" = false ]; then
        log_warn "Skipping RED-phase test: $name (use --red to include)"
        skip=$((skip + 1))
        total=$((total + 1))
        continue
    fi

    # Skip HW-dependent root tests unless --root-tests flag given
    if [ "$expected" = "root" ] && [ "$INCLUDE_ROOT" = false ]; then
        log_warn "Skipping HW-dependent test: $name (use --root-tests to include)"
        skip=$((skip + 1))
        total=$((total + 1))
        continue
    fi

    total=$((total + 1))
    echo "──────────────────────────────────────────────"
    log_test "Running: $name"
    echo ""

    # Save kernel cursor before test
    dmesg_cursor=$(dmesg | wc -l)
    start_time=$(date +%s%N)

    # Run with timeout (5 minutes per test)
    set +e
    timeout 300 bash "$f" 2>&1
    rc=$?
    set -e

    end_time=$(date +%s%N)
    elapsed_ms=$(( (end_time - start_time) / 1000000 ))

    # Extract relevant kernel messages from this test run
    dmesg_lines=$(dmesg | tail -n +$((dmesg_cursor + 1)))

    echo ""

    case "$expected" in
        "red")
            # RED-phase: expected to fail
            if [ $rc -ne 0 ]; then
                log_info "RED phase expected failure: $name exited $rc (${elapsed_ms}ms)"
                pass=$((pass + 1))
            else
                log_error "RED phase UNEXPECTED PASS: $name succeeded when it should have failed! Bug may be fixed. (${elapsed_ms}ms)"
                log_warn "  Consider moving $name from RED to GREEN phase."
                fail=$((fail + 1))
                failed_names="$failed_names $name"
            fi
            ;;
        *)
            # Normal test: expected to pass
            if [ $rc -eq 0 ]; then
                log_info "PASS: $name (${elapsed_ms}ms)"
                pass=$((pass + 1))
            else
                log_error "FAIL: $name (exit code $rc, ${elapsed_ms}ms)"
                fail=$((fail + 1))
                failed_names="$failed_names $name"

                # Show kernel messages
                if [ -n "$dmesg_lines" ]; then
                    log_error "Kernel messages from this test:"
                    echo "$dmesg_lines" | grep -i 'lhsr\|dm-[0-9]\|device-mapper' | tail -20 | sed 's/^/  /'
                fi

                # STOP on first failure — don't cascade
                echo ""
                log_error "FAILURE: $name — aborting test run."
                echo ""
                echo "=============================================="
                echo "  RESULTS: $pass passed, $fail failed, $skip skipped"
                echo "  FAILED: $failed_names"
                echo "=============================================="
                exit 1
            fi
            ;;
    esac

    # Slight delay between tests for clean state transitions
    sleep 1
    echo ""
done

echo "=============================================="
echo "  ALL TESTS COMPLETE"
echo "  $pass passed, $fail failed, $skip skipped"
echo "=============================================="
exit $fail
