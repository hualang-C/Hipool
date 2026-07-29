#!/bin/bash
# hipool adaptive cruise learning -- automated learning task
# Usage: bash cruise_learn.sh
# Users can customize seed words, classification, and fetching logic.

set -o pipefail

cd "$(dirname "$0")" || exit 1
MEMORY="$(cd "$(dirname "$0")"/.. && pwd)/build/memory"
SEEN_FILE="seen_words.txt"
LOG_FILE="crawl_learn.log"

# Ensure seen_words.txt exists
touch "$SEEN_FILE"

# ============================================================
# Initial seed queue -- users can add their own seeds here
# ============================================================
SEEDS=(
  # Users: add seed words here
)

echo "=========================================="
echo "  hipool Adaptive Cruise Learning -- Start"
echo "=========================================="
echo "  Seed count: ${#SEEDS[@]}"
echo "  Log: $LOG_FILE"
echo "  Seen words: $(wc -l < $SEEN_FILE)"
echo "  Time: $(date '+%Y-%m-%d %H:%M')"
echo "=========================================="
echo ""

# Load seen words into memory
declare -A SEEN_SET
while IFS= read -r line; do
  SEEN_SET["$line"]=1
done < "$SEEN_FILE"

# Load already learned lexicon words
ALREADY_LEARNED=$("$MEMORY" search --tag learned 2>&1 | grep "^  key: lexicon:" | sed 's/.*lexicon://' | tr ' ' '\n')
declare -A LEARNED_SET
while IFS= read -r word; do
  [ -n "$word" ] && LEARNED_SET["$word"]=1
done <<< "$ALREADY_LEARNED"
echo "  Already learned: ${#LEARNED_SET[@]}"
echo ""

# Learning queue
QUEUE_FILE="/tmp/hipool_learn_queue.txt"
TEMP_QUEUE="/tmp/hipool_learn_queue_new.txt"

# Initialize queue: only add unlearned seeds
: > "$QUEUE_FILE"
for w in "${SEEDS[@]}"; do
  if [ -z "${LEARNED_SET[$w]}" ] && [ -z "${SEEN_SET[$w]}" ]; then
    echo "$w" >> "$QUEUE_FILE"
  fi
done
echo "  Initial queue: $(wc -l < $QUEUE_FILE) unlearned words"
echo ""

# ============================================================
# Helper functions
# ============================================================

# Write to hipool and track
write_to_hipool() {
  local word="$1"
  local entity="$2"
  local val="{\"entity\":$entity,\"tag\":\"M\"}"
  local tags="learned,crawl,entity_$entity"

  local result
  result=$("$MEMORY" set "lexicon:$word" "$val" --tags "$tags" 2>&1)
  if [ $? -ne 0 ]; then
    # Retry once
    sleep 1
    result=$("$MEMORY" set "lexicon:$word" "$val" --tags "$tags" 2>&1)
    if [ $? -ne 0 ]; then
      echo "  FAIL: $word (write failed)" >&2
      return 1
    fi
  fi

  # Record as seen
  echo "$word" >> "$SEEN_FILE"
  SEEN_SET["$word"]=1

  return 0
}

# Enqueue new words
enqueue_words() {
  local words=("$@")
  local added=0
  for w in "${words[@]}"; do
    if [ -z "${LEARNED_SET[$w]}" ] && [ -z "${SEEN_SET[$w]}" ]; then
      echo "$w" >> "$QUEUE_FILE"
      SEEN_SET["$w"]=1
      added=$((added + 1))
    fi
  done
  return $added
}

# Classify word (simple heuristic -- users can customize)
classify_word() {
  local word="$1"
  echo 0
}

# ============================================================
# Main Loop
# ============================================================

START_TIME=$(date +%s)
ITERATION=0
TOTAL_LEARNED=0
CONSECUTIVE_EMPTY=0
LAST_FLUSH_TIME=0
LAST_REPORT_TIME=$START_TIME

echo "[$(date '+%H:%M')] Starting loop..."

while true; do
  ITERATION=$((ITERATION + 1))
  ITER_START=$(date +%s)

  # Check queue
  QUEUE_LEN=$(wc -l < "$QUEUE_FILE" 2>/dev/null || echo 0)
  if [ "$QUEUE_LEN" -eq 0 ]; then
    echo "[$(date '+%H:%M')] Queue empty, consecutive empty: $CONSECUTIVE_EMPTY"
    CONSECUTIVE_EMPTY=$((CONSECUTIVE_EMPTY + 1))
    if [ "$CONSECUTIVE_EMPTY" -ge 30 ]; then
      echo "[$(date '+%H:%M')] 30 consecutive empty iterations, exiting."
      break
    fi
    sleep 30
    continue
  fi
  CONSECUTIVE_EMPTY=0

  # Get first word from queue
  CURRENT_WORD=$(head -1 "$QUEUE_FILE")
  tail -n +2 "$QUEUE_FILE" > "$TEMP_QUEUE" 2>/dev/null
  mv "$TEMP_QUEUE" "$QUEUE_FILE"

  echo "[$(date '+%H:%M')] #$ITERATION learning: $CURRENT_WORD"

  # Write current search word to hipool
  ENTITY=$(classify_word "$CURRENT_WORD")
  write_to_hipool "$CURRENT_WORD" "$ENTITY"
  TOTAL_LEARNED=$((TOTAL_LEARNED + 1))

  ITER_END=$(date +%s)
  ITER_DURATION=$((ITER_END - ITER_START))
  echo "  Write complete (ent=$ENTITY), duration ${ITER_DURATION}s"

  # Every 20 iterations record stats
  if [ $((ITERATION % 20)) -eq 0 ]; then
    STATS=$("$MEMORY" stats 2>&1)
    echo "$(date '+%Y-%m-%d %H:%M:%S') -- Iter $ITERATION -- ${STATS}" >> "$LOG_FILE"
    echo "[$(date '+%H:%M')] Progress: iter #$ITERATION, total learned ${TOTAL_LEARNED}"
  fi

  # Every 30 minutes flush
  NOW=$(date +%s)
  if [ $((NOW - LAST_FLUSH_TIME)) -ge 1800 ]; then
    "$MEMORY" flush > /dev/null 2>&1
    LAST_FLUSH_TIME=$NOW
    echo "  Flush complete"
  fi

  # Every 1 hour output summary
  if [ $((NOW - LAST_REPORT_TIME)) -ge 3600 ]; then
    ELAPSED=$((NOW - START_TIME))
    ELAPSED_H=$((ELAPSED / 3600))
    ELAPSED_M=$(((ELAPSED % 3600) / 60))
    QUEUE_LEN=$(wc -l < "$QUEUE_FILE" 2>/dev/null || echo 0)
    echo ""
    echo "=========================================="
    echo "  Hourly Summary -- Running ${ELAPSED_H}h${ELAPSED_M}m"
    echo "  Iterations: $ITERATION | Learned: $TOTAL_LEARNED"
    echo "  Queue: $QUEUE_LEN"
    echo "  Seen words: $(wc -l < $SEEN_FILE)"
    echo "=========================================="
    echo ""
    LAST_REPORT_TIME=$NOW
  fi

  # Wait 2-3 minutes
  SLEEP_TIME=$((120 + RANDOM % 60))
  echo "  Waiting ${SLEEP_TIME}s..."
  sleep "$SLEEP_TIME"
done

# Cleanup
"$MEMORY" flush > /dev/null 2>&1

# Final report
END_TIME=$(date +%s)
TOTAL_TIME=$((END_TIME - START_TIME))
echo ""
echo "=========================================="
echo "  Cruise learning complete"
echo "  Run time: $((TOTAL_TIME / 3600))h $(((TOTAL_TIME % 3600) / 60))m"
echo "  Total iterations: $ITERATION"
echo "  Total learned: $TOTAL_LEARNED"
echo "  Final pool stats:"
"$MEMORY" stats 2>&1
echo "=========================================="
echo "" >> "$LOG_FILE"
echo "$(date '+%Y-%m-%d %H:%M:%S') -- End -- iter=$ITERATION learned=$TOTAL_LEARNED" >> "$LOG_FILE"
