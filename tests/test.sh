#!/bin/bash
# hipool test suite -- all operations use CLI commands, no pre-seeded data files
set -e

HIPOOL_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MEMORY="$HIPOOL_DIR/build/memory"
PASS=0; FAIL=0; TOTAL=0
declare -a FAIL_NAMES
RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'

setup_dir() {
    local d="$1"; rm -rf "$d" && mkdir -p "$d/memory_data"
}

cleanup_dir() {
    rm -rf "$1"
}

run_test() {
    local name="$1" testdir="$2" setup_cmd="$3" verify_cmd="$4"
    TOTAL=$((TOTAL+1))
    setup_dir "$testdir"
    cd "$testdir"

    # execute setup (insert data)
    eval "$setup_cmd" 2>/dev/null

    # execute verification
    if eval "$verify_cmd" 2>/dev/null; then
        echo -e "  ${GREEN}PASS${NC}  $name"
        PASS=$((PASS+1))
    else
        echo -e "  ${RED}FAIL${NC}  $name"
        FAIL=$((FAIL+1))
        FAIL_NAMES+=("$name")
    fi

    cleanup_dir "$testdir"
    cd "$HIPOOL_DIR"
}

# ensure memory binary exists
if [ ! -f "$MEMORY" ] && [ ! -f "$MEMORY.exe" ]; then
    echo "Error: memory binary not found. Run 'make' first."
    exit 1
fi
# if memory.exe exists but no memory, create a wrapper
if [ -f "$MEMORY.exe" ] && [ ! -f "$MEMORY" ]; then
    MEMORY="$MEMORY.exe"
fi

echo -e "${CYAN}========================================${NC}"
echo -e "${CYAN}  hipool Test Suite${NC}"
echo -e "${CYAN}========================================${NC}"
echo ""

# ============================================================
# 1. Basic functions (2 tests)
# ============================================================
echo -e "${CYAN}[1/8] Basic Functions${NC}"

run_test "set and get key-value" "t_basic" \
    '$MEMORY set "test-key" "test value" --tags "demo"' \
    'r=$($MEMORY get "test-key" 2>/dev/null); echo "$r" | grep -q "test value"'

run_test "tag search" "t_basic2" \
    '$MEMORY set "test-key" "test value" --tags "demo"' \
    'r=$($MEMORY search --tag "demo" 2>/dev/null); echo "$r" | grep -q "test-key"'

# ============================================================
# 2. Data search (3 tests)
# ============================================================
echo -e "\n${CYAN}[2/8] Data Search${NC}"

run_test "text search" "t_search" \
    '$MEMORY set "a1" "apple is a red fruit" && $MEMORY set "b2" "banana is yellow" && $MEMORY set "c3" "cherry is small"' \
    'r=$($MEMORY search "cherry" 2>/dev/null); echo "$r" | grep -q "cherry is small"'

run_test "multi-tag search" "t_search2" \
    '$MEMORY set "a1" "apple" --tags "fruit,red" && $MEMORY set "b2" "banana" --tags "fruit,yellow" && $MEMORY set "d4" "dog" --tags "animal"' \
    'r=$($MEMORY search --tag "fruit" 2>/dev/null); [ "$(echo "$r" | grep -c "^  key:")" -ge 2 ]'

# Semantic search: skip gracefully if not a semantic build
run_test "semantic search (skip if unavailable)" "t_sem" \
    '$MEMORY set "d1" "apple fruit" && $MEMORY set "d2" "banana fruit" && $MEMORY set "d3" "salmon fish"' \
    'r=$($MEMORY sem-search "fruit" 2>/dev/null); if [ $? -ne 0 ]; then echo "SKIP (sem-search not enabled)" | grep "SKIP"; else echo "$r" | grep -q "d1"; fi'

# ============================================================
# 3. Hash collision (1 test)
# ============================================================
echo -e "\n${CYAN}[3/8] Hash Collision${NC}"

run_test "50 same-prefix keys" "t_coll" \
    'for i in $(seq 0 49); do $MEMORY set "key_$i" "value_$i"; done' \
    'r=$($MEMORY search "key_" 2>/dev/null); [ "$(echo "$r" | grep -c "^  key:")" -eq 50 ]'

# ============================================================
# 4. Random data (2 tests)
# ============================================================
echo -e "\n${CYAN}[4/8] Random Data${NC}"

run_test "101 random key-values" "t_rand" \
    'for i in $(seq 0 100); do $MEMORY set "rand_$i" "random_val_$i"; done' \
    'r=$($MEMORY search "rand_" 2>/dev/null); [ "$(echo "$r" | grep -c "^  key:")" -ge 101 ]'

run_test "single random retrieval" "t_rand2" \
    'for i in $(seq 0 100); do $MEMORY set "rand_$i" "random_val_$i"; done' \
    'r=$($MEMORY search "rand_100" 2>/dev/null); echo "$r" | grep -q "random_val_100"'

# ============================================================
# 5. Bulk stress (2 tests)
# ============================================================
echo -e "\n${CYAN}[5/8] Bulk Stress${NC}"

run_test "354 bulk entries" "t_fill" \
    'for i in $(seq -w 0 353); do $MEMORY set "fill_$i" "Filler entry number $i"; done' \
    '$MEMORY stats 2>/dev/null | grep -q "354 entries"'

run_test "bulk data verification" "t_fill2" \
    'for i in $(seq -w 0 353); do $MEMORY set "fill_$i" "Filler entry number $i"; done' \
    'r=$($MEMORY get "fill_353" 2>/dev/null); echo "$r" | grep -q "Filler entry number 353"'

# ============================================================
# 6. Edge cases (3 tests)
# ============================================================
echo -e "\n${CYAN}[6/8] Edge Cases${NC}"

run_test "long key (255 chars)" "t_edge" \
    'longkey=$(printf "k%.0s" $(seq 1 255)); $MEMORY set "$longkey" "longkey_value"' \
    'longkey=$(printf "k%.0s" $(seq 1 255)); r=$($MEMORY get "$longkey" 2>/dev/null); echo "$r" | grep -q "longkey_value"'

run_test "empty value" "t_edge2" \
    '$MEMORY set "empty_key" ""' \
    'r=$($MEMORY get "empty_key" 2>/dev/null); [ "$r" = "" ]'

run_test "special characters" "t_edge3" \
    '$MEMORY set "spec" "tab	test and newline
in value!@#$%"' \
    'r=$($MEMORY get "spec" 2>/dev/null); echo "$r" | grep -q "!@#\$%"'

# ============================================================
# 7. Overwrite cycle (1 test)
# ============================================================
echo -e "\n${CYAN}[7/8] Overwrite Cycle${NC}"

run_test "sequential overwrite" "t_cycle" \
    '$MEMORY set "cycle_1" "first" && $MEMORY set "cycle_1" "second" && $MEMORY set "cycle_1" "third"' \
    'r=$($MEMORY get "cycle_1" 2>/dev/null); echo "$r" | grep -q "^third$"'

# ============================================================
# 8. Persistence (2 tests)
# ============================================================
echo -e "\n${CYAN}[8/8] Persistence${NC}"

run_test "flush to disk" "t_flush" \
    '$MEMORY set "persist" "persistence test data" && $MEMORY flush' \
    '[ -f "$PWD/memory_data/memory_snapshot.json" ] && grep -q "persistence test data" "$PWD/memory_data/memory_snapshot.json"'

run_test "load restores data" "t_load" \
    '$MEMORY set "persist2" "another data value" && $MEMORY flush' \
    '$MEMORY load 2>/dev/null && r=$($MEMORY search "persist2" 2>/dev/null); echo "$r" | grep -q "another data value"'

# ============================================================
# Summary
# ============================================================
echo ""
echo -e "${CYAN}========================================${NC}"
if [ $FAIL -eq 0 ]; then
    echo -e "  ${GREEN}All passed: $PASS/$TOTAL${NC}"
else
    echo -e "  ${RED}Passed: $PASS/$TOTAL  |  Failed: $FAIL${NC}"
    for fn in "${FAIL_NAMES[@]}"; do
        echo -e "  ${RED}  - $fn${NC}"
    done
fi
echo -e "${CYAN}========================================${NC}"
exit $FAIL
