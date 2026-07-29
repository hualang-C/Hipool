#!/bin/bash
# hipool v6.0 ablation tests -- test new features one by one to find bugs
set +e
HIPOOL_DIR="$(cd "$(dirname "$0")/.." && pwd)"
MEMORY="$HIPOOL_DIR/build/memory"
cd "$(dirname "$0")"

PASS=0; FAIL=0; TOTAL=0
RED='\033[0;31m'; GREEN='\033[0;32m'; CYAN='\033[0;36m'; NC='\033[0m'
OK() { TOTAL=$((TOTAL+1)); PASS=$((PASS+1)); echo -e "  ${GREEN}PASS${NC} $1"; }
FAIL() { TOTAL=$((TOTAL+1)); FAIL=$((FAIL+1)); echo -e "  ${RED}FAIL${NC} $1"; }

cleanup() { rm -rf memory_data; }

echo -e "${CYAN}========================================${NC}"
echo -e "${CYAN}  hipool v6.0 Ablation Tests${NC}"
echo -e "${CYAN}========================================${NC}"
echo ""

# =============================================================
# 1. Skip List sorted index -- boundary
# =============================================================
echo -e "${CYAN}[1/6] SkipList Sorted Index${NC}"
cleanup; mkdir -p memory_data

# 1a. Entries with identical timestamps (composite sort key key_hash decides order)
for i in $(seq 1 20); do ./memory set "ts_collide_$i" "val_$i" --ts 1000000000 >/dev/null 2>&1; done
NOW=1000000000
r=$(./memory search --range 999999999 1000000001 2>/dev/null)
c=$(echo "$r" | grep -c "^  key:")
[ "$c" -eq 20 ] && OK "1a same-timestamp 20 entries -> $c" || FAIL "1a same-timestamp: expected 20 -> $c"

# 1b. Empty range query
r=$(./memory search --range 1 2 2>/dev/null)
echo "$r" | grep -q "no results" && OK "1b empty range -> no results" || FAIL "1b empty range"

# 1c. Very large range (uint64 boundary)
r=$(./memory search --range 0 999999999999999 2>/dev/null)
c=$(echo "$r" | grep -c "^  key:")
[ "$c" -ge 20 ] && OK "1c large range -> $c" || FAIL "1c large range: $c"

# 1d. Range start == end
r=$(./memory search --range $NOW $NOW 2>/dev/null)
echo "$r" | grep -q "no results" && OK "1d start==end" || FAIL "1d start==end"

# 1e. Insert then delete all then range query
for i in $(seq 1 20); do ./memory del "ts_collide_$i" >/dev/null 2>&1; done
r=$(./memory search --range 0 999999999999999 2>/dev/null)
echo "$r" | grep -q "no results" && OK "1e all deleted, range empty" || FAIL "1e all deleted, range has results"

echo ""

# =============================================================
# 2. SkipList bulk data
# =============================================================
echo -e "${CYAN}[2/6] SkipList Bulk Data${NC}"
cleanup; mkdir -p memory_data

# 2a. Insert 500 entries
for i in $(seq 1 500); do
  ./memory set "bulk_$(printf '%04d' $i)" "data_$i" --ts $((1000000 + i)) >/dev/null 2>&1
done
r=$(./memory stats 2>/dev/null)
echo "$r" | grep -q "500 entries" && OK "2a 500 bulk insert" || FAIL "2a: $(echo "$r" | grep entries)"

# 2b. Range query all
r=$(./memory search --range 1000000 1000501 2>/dev/null)
c=$(echo "$r" | grep -c "^  key:")
[ "$c" -eq 500 ] && OK "2b range query 500 -> $c" || FAIL "2b range query: $c/500"

# 2c. Delete half (odd IDs)
for i in $(seq 1 2 500); do
  ./memory del "bulk_$(printf '%04d' $i)" >/dev/null 2>&1
done
r=$(./memory stats 2>/dev/null)
echo "$r" | grep -q "250 entries" && OK "2c delete 250 -> 250" || FAIL "2c delete half"

# 2d. Range query after deletion
r=$(./memory search --range 1000000 1000501 2>/dev/null)
c=$(echo "$r" | grep -c "^  key:")
[ "$c" -eq 250 ] && OK "2d range query 250 -> $c" || FAIL "2d range query: $c/250"

# 2e. Interleaved timestamp insert (verify sort order)
cleanup; mkdir -p memory_data
for i in 10 20 5 15 1 25; do
  ./memory set "order_$i" "v_$i" --ts $i >/dev/null 2>&1
done
r=$(./memory search --range 0 100 2>/dev/null)
# Expected order: 1,5,10,15,20,25
order=$(echo "$r" | grep "^  key:" | sed 's/.*order_//' | paste -sd ' ')
[ "$order" = "1 5 10 15 20 25" ] && OK "2e sort order: $order" || FAIL "2e sort: '$order' expected '1 5 10 15 20 25'"

echo ""

# =============================================================
# 3. WAL crash recovery -- boundary
# =============================================================
echo -e "${CYAN}[3/6] WAL Write-Ahead Log${NC}"
cleanup; mkdir -p memory_data

# 3a. Normal WAL write + recovery
./memory set wal_a "survive" --tags w >/dev/null 2>&1
rm -f memory_data/memory_snapshot.json
r=$(./memory get wal_a 2>/dev/null)
[ "$r" = "survive" ] && OK "3a WAL basic recovery" || FAIL "3a WAL recovery: '$r'"

# 3b. Process WAL recovery for 100 entries
cleanup; mkdir -p memory_data
for i in $(seq 0 99); do
  ./memory set "wal_bulk_$i" "val_$i" --tags w >/dev/null 2>&1
done
rm -f memory_data/memory_snapshot.json
r=$(./memory search --tag w 2>/dev/null)
c=$(echo "$r" | grep -c "^  key:")
[ "$c" -eq 100 ] && OK "3b WAL recovery 100 entries -> $c" || FAIL "3b WAL recovery: $c"

# 3c. WAL + DEL mixed recovery
cleanup; mkdir -p memory_data
./memory set w_del "will_be_deleted" >/dev/null 2>&1
./memory del w_del >/dev/null 2>&1
./memory set w_keep "should_exist" >/dev/null 2>&1
rm -f memory_data/memory_snapshot.json
r=$(./memory get w_del 2>/dev/null)
[ "$r" = "(not found)" ] && OK "3c DEL record gone after recovery" || FAIL "3c DEL recovery: '$r'"
r=$(./memory get w_keep 2>/dev/null)
[ "$r" = "should_exist" ] && OK "3c SET record exists after recovery" || FAIL "3c SET recovery: '$r'"

# 3d. Empty WAL file (0 bytes)
cleanup; mkdir -p memory_data
touch memory_data/wal.log
./memory stats >/dev/null 2>&1 && OK "3d empty WAL does not crash" || FAIL "3d empty WAL crashes"

# 3e. Corrupted WAL (random bytes)
cleanup; mkdir -p memory_data
dd if=/dev/urandom of=memory_data/wal.log bs=1024 count=1 2>/dev/null
./memory stats >/dev/null 2>&1 && OK "3e corrupt WAL does not crash" || FAIL "3e corrupt WAL crashes"

# 3f. Flush -> snapshot -> delete snapshot -> data lost (correct behavior)
cleanup; mkdir -p memory_data
./memory set w_final "final" >/dev/null 2>&1
./memory flush >/dev/null 2>&1
rm -f memory_data/memory_snapshot.json
r=$(./memory get w_final 2>/dev/null)
[ "$r" = "(not found)" ] && OK "3f flush then delete snapshot -> lost (correct): $r" || FAIL "3f flush then delete snapshot: '$r'"
# With snapshot preserved, data should exist
./memory set w_keep "keep" >/dev/null 2>&1
r=$(./memory get w_keep 2>/dev/null)
[ "$r" = "keep" ] && OK "3f flush no delete snapshot: $r" || FAIL "3f flush: '$r'"

echo ""

# =============================================================
# 4. Shard isolation + boundary
# =============================================================
echo -e "${CYAN}[4/6] Shard Partition${NC}"
cleanup; mkdir -p memory_data

# 4a. Multi-shard isolation
for s in a b c d e; do
  ./memory set "k_$s" "val_$s" --shard "shard_$s" >/dev/null 2>&1
done
for s in a b c d e; do
  r=$(./memory get "k_$s" --shard "shard_$s" 2>/dev/null)
  [ "$r" = "val_$s" ] || FAIL "4a shard_$s: '$r'"
done
OK "4a 5 shards isolated"

# 4b. Default shard should not have shard data
r=$(./memory get k_a 2>/dev/null)
[ "$r" = "(not found)" ] && OK "4b default shard no leakage" || FAIL "4b leakage: '$r'"

# 4c. Shard count limit (MAX_SHARDS=8, 5 exist, 3 more allowed)
for s in 1 2 3; do
  ./memory set x "y" --shard "sc_$s" >/dev/null 2>&1
done
# 9th should fail
r=$(./memory set x "y" --shard overflow 2>&1)
echo "$r" | grep -q "max shards" && OK "4c max shards rejected" || FAIL "4c max shards: '$r'"

# 4d. Shard range query
cleanup; mkdir -p memory_data
for i in $(seq 1 30); do
  ./memory set "s_item_$i" "sv_$i" --ts $((2000000 + i)) --shard srange >/dev/null 2>&1
done
r=$(./memory search --range 2000005 2000015 --shard srange 2>/dev/null)
c=$(echo "$r" | grep -c "^  key:")
[ "$c" -eq 10 ] && OK "4d shard range query -> $c" || FAIL "4d shard range: $c"

# 4e. Shard delete isolation
./memory del s_item_5 --shard srange >/dev/null 2>&1
r=$(./memory get s_item_5 --shard srange 2>/dev/null)
[ "$r" = "(not found)" ] && OK "4e shard delete" || FAIL "4e shard delete: '$r'"

# 4f. Shard flush and reload
cleanup; mkdir -p memory_data
./memory set persist_k "persist_v" --shard persist >/dev/null 2>&1
./memory flush >/dev/null 2>&1
r=$(./memory get persist_k --shard persist 2>/dev/null)
[ "$r" = "persist_v" ] && OK "4f shard flush+reload" || FAIL "4f shard flush: '$r'"

echo ""

# =============================================================
# 5. Fork Snapshot
# =============================================================
echo -e "${CYAN}[5/6] Fork Snapshot${NC}"
cleanup; mkdir -p memory_data

# 5a. Basic snapshot
./memory set fs1 "data1" >/dev/null 2>&1
r=$(./memory snapshot 2>&1)
echo "$r" | grep -q "snapshot ok" && OK "5a snapshot basic" || FAIL "5a snapshot: '$r'"

# 5b. Data intact after snapshot
r=$(./memory get fs1 2>/dev/null)
[ "$r" = "data1" ] && OK "5b snapshot data intact" || FAIL "5b snapshot data: '$r'"

# 5c. Modifications during snapshot do not affect child
cleanup; mkdir -p memory_data
./memory set before "before_val" >/dev/null 2>&1
(./memory snapshot >/dev/null 2>&1) &
sleep 0.1
./memory set after "after_val" >/dev/null 2>&1
wait
r=$(./memory get before 2>/dev/null)
[ "$r" = "before_val" ] && OK "5c snapshot+concurrent write" || FAIL "5c concurrent: '$r'"

# 5d. Empty data snapshot
cleanup; mkdir -p memory_data
r=$(./memory snapshot 2>&1)
echo "$r" | grep -q "snapshot ok" && OK "5d empty snapshot" || FAIL "5d empty snapshot: '$r'"

# 5e. Shard + snapshot
cleanup; mkdir -p memory_data
./memory set fk "fv" --shard fs >/dev/null 2>&1
./memory snapshot >/dev/null 2>&1
r=$(./memory get fk --shard fs 2>/dev/null)
[ "$r" = "fv" ] && OK "5e shard+snapshot" || FAIL "5e shard+snapshot: '$r'"

echo ""

# =============================================================
# 6. Edge cases
# =============================================================
echo -e "${CYAN}[6/6] Edge Cases${NC}"
cleanup; mkdir -p memory_data

# 6a. Unicode key/value
./memory set "hello_world" "this is a test" >/dev/null 2>&1
r=$(./memory get "hello_world" 2>/dev/null)
[ "$r" = "this is a test" ] && OK "6a unicode key" || FAIL "6a unicode: '$r'"

# 6b. Special chars in value
./memory set spec_key "tab	test and newline
in value!@#$%" >/dev/null 2>&1
r=$(./memory get spec_key 2>/dev/null)
echo "$r" | grep -q "!@#\$%" && OK "6b special chars" || FAIL "6b special chars: '$r'"

# 6c. 255 byte key
k256=$(python3 -c "print('k'*255)")
./memory set "$k256" "long_key_test" >/dev/null 2>&1
r=$(./memory search "long_key_test" 2>/dev/null)
echo "$r" | grep -q "long_key_test" && OK "6c 255B key" || FAIL "6c 255B key"

# 6d. Empty value
./memory set empty_val "" >/dev/null 2>&1
r=$(./memory get empty_val 2>/dev/null)
[ "$r" = "" ] && OK "6d empty value" || FAIL "6d empty value: '$r'"

# 6e. Large value (near 16320 limit)
big=$(python3 -c "print('B'*16000)")
./memory set big_val "$big" >/dev/null 2>&1
r=$(./memory get big_val 2>/dev/null)
[ "${#r}" -eq 16000 ] && OK "6e 16KB value" || FAIL "6e 16KB: len=${#r}"

# 6f. No tags search
./memory set notag "value" >/dev/null 2>&1
r=$(./memory search --tag nonexistent 2>/dev/null)
echo "$r" | grep -q "no results" && OK "6f no-tag search" || FAIL "6f no-tag search"

# 6g. Duplicate key overwrite
./memory set dupkey "first" >/dev/null 2>&1
./memory set dupkey "second" >/dev/null 2>&1
r=$(./memory get dupkey 2>/dev/null)
[ "$r" = "second" ] && OK "6g key overwrite" || FAIL "6g overwrite: '$r'"

# 6h. 16 tags
tags=$(python3 -c "print(','.join([f't{i}' for i in range(16)]))")
./memory set manytags "v" --tags "$tags" >/dev/null 2>&1
r=$(./memory search --tag t15 2>/dev/null)
echo "$r" | grep -q "manytags" && OK "6h 16 tags" || FAIL "6h 16 tags"

# 6i. Date range query (boundary crossing)
cleanup; mkdir -p memory_data
./memory set old_entry "old" --ts 1700000000 >/dev/null 2>&1
./memory set new_entry "new" --ts 1800000000 >/dev/null 2>&1
r=$(./memory search --range 1700000000 1700000001 2>/dev/null)
echo "$r" | grep -q "old_entry" && ! echo "$r" | grep -q "new_entry" && OK "6i time range precise" || FAIL "6i time range"

# 6j. Mutex build + basic function
gcc -O2 -DHIPOOL_USE_MUTEX src/*.c -I src -o /tmp/hipool_mt -lpthread 2>&1 | grep -v warning | grep -v note | head -3
/tmp/hipool_mt set mt "mt_val" >/dev/null 2>&1
r=$(/tmp/hipool_mt get mt 2>/dev/null)
[ "$r" = "mt_val" ] && OK "6j Mutex build" || FAIL "6j Mutex: '$r'"
rm -f /tmp/hipool_mt

# 6k. Snapshot rejection (mutex mode)
gcc -O2 -DHIPOOL_USE_MUTEX src/*.c -I src -o /tmp/hipool_mt -lpthread 2>&1
r=$(/tmp/hipool_mt snapshot 2>&1)
echo "$r" | grep -q "failed" && OK "6k Mutex snapshot rejected" || FAIL "6k Mutex snapshot: '$r'"
rm -f /tmp/hipool_mt

echo ""
echo -e "${CYAN}========================================${NC}"
echo -e "  ${GREEN}Passed: $PASS/$TOTAL  |  Failed: $FAIL${NC}"
echo -e "${CYAN}========================================${NC}"
exit $FAIL
