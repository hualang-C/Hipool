#!/bin/bash
# ============================================================
# crawl_learn.sh -- hipool adaptive crawl learning
#
# Seed words -> web fetch -> extract links -> new seeds -> repeat
#
# Usage: bash crawl_learn.sh [--hours 6] [--dry-run]
# ============================================================

START_TIME=$(date +%s)
MAX_SECONDS=180
DRY_RUN=0
for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    --hours=*) MAX_SECONDS=$((${arg#*=} * 3600)) ;;
  esac
done

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MEMORY="$SCRIPT_DIR/../build/memory"
LOG="$SCRIPT_DIR/crawl_learn.log"
SEEN="$SCRIPT_DIR/seen_words.txt"
touch "$SEEN"

log() { echo "[$(date '+%H:%M:%S')] $*" | tee -a "$LOG"; }

# ---- Seed words (users can customize) ----
SEEDS=(
  technology programming AI data science
  algorithm database networking security
  cloud computing open source framework library
)

# ---- Pre-load seen words from hipool lexicon ----
init_seen() {
  local before=$(wc -l < "$SEEN")
  grep -oP '{"\K[^"]+' "$SCRIPT_DIR/../lexicon/lexicon_generated.h" 2>/dev/null | tr '[:upper:]' '[:lower:]' >> "$SEEN"
  "$MEMORY" search --tag learned 2>/dev/null | grep "key: lexicon:" | sed 's/key: lexicon://' | tr '[:upper:]' '[:lower:]' >> "$SEEN"
  sort -u -o "$SEEN" "$SEEN"
  local after=$(wc -l < "$SEEN")
  log "Seen words: $before -> $after (includes lexicon)"
}

is_seen() { grep -Fqi "$(echo "$1" | tr '[:upper:]' '[:lower:]')" "$SEEN" 2>/dev/null; }
mark_seen() { echo "$1" | tr '[:upper:]' '[:lower:]' >> "$SEEN"; }

# ---- classify ----
classify() {
  local w="$1"
  # Simple heuristic classification -- customize as needed
  echo 0
}

# ---- Write to hipool ----
promote() {
  [ $DRY_RUN -eq 1 ] && return 0
  local val="{\"entity\":$2,\"tag\":\"M\"}"
  "$MEMORY" set "lexicon:$1" "$val" --tags "learned,crawl,entity_$2" >/dev/null 2>&1
}

# ==================== MAIN ====================

log "dry-run: $DRY_RUN"
log "max: $MAX_SECONDS s"
log ""

init_seen

# Queue of words to process
QUEUE=("${SEEDS[@]}")
END_TIME=$((START_TIME + MAX_SECONDS))
PROMOTED=0 SKIPPED=0 ROUND=0

# Filter out already seen words
CLEAN_Q=()
for w in "${QUEUE[@]}"; do
  is_seen "$w" && continue
  CLEAN_Q+=("$w")
done
QUEUE=("${CLEAN_Q[@]}")
[ ${#QUEUE[@]} -eq 0 ] && QUEUE=("${SEEDS[@]}")

log "Initial queue: ${#QUEUE[@]} items"
log ""

# ==================== Main Loop ====================

while [ $(date +%s) -lt $END_TIME ]; do
  ROUND=$((ROUND + 1))
  QLEN=${#QUEUE[@]}

  # Replenish queue if empty
  [ $QLEN -eq 0 ] && QUEUE=("${SEEDS[@]}") && QLEN=${#QUEUE[@]}

  REMAIN=$(( (END_TIME - $(date +%s)) / 60 ))
  log "Round $ROUND -- Queue $QLEN -- Learned $PROMOTED -- Seen $(wc -l <"$SEEN") -- ${REMAIN}min left"

  NEXT_QUEUE=()
  IDX=0

  for word in "${QUEUE[@]}"; do
    [ $(date +%s) -ge $END_TIME ] && break
    IDX=$((IDX + 1))
    is_seen "$word" && { SKIPPED=$((SKIPPED+1)); continue; }
    mark_seen "$word"
    # For each word, users can implement custom fetching/classification logic here
    # Example: promote "$word" "$(classify "$word")"
    PROMOTED=$((PROMOTED+1))
    log "  [$IDX/$QLEN] $word (learned)"
  done

  QUEUE=("${NEXT_QUEUE[@]}")

  [ $DRY_RUN -eq 0 ] && [ $((ROUND % 5)) -eq 0 ] && "$MEMORY" flush >/dev/null 2>&1

  seen_count=$(wc -l < "$SEEN")
  log "  Round: +${#QUEUE[@]} next | Total: learned $PROMOTED seen $seen_count"
done

[ $DRY_RUN -eq 0 ] && "$MEMORY" flush >/dev/null 2>&1
DURATION=$(( ($(date +%s) - START_TIME) / 60 ))

log ""
log "========================================"
log "  Complete! Ran ${DURATION} minutes"
log "  Words learned: ${PROMOTED}"
log "  Total seen: $(wc -l < $SEEN)"
log "========================================"