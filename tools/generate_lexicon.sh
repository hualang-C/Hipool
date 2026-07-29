#!/bin/bash
# generate_lexicon.sh -- generate categorized lexicon entries and write to hipool
# Usage: bash generate_lexicon.sh [--dry-run]
# Users can add their own terms to the COMPANIES, TECHS, and CONCEPTS arrays below.

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MEMORY="$SCRIPT_DIR/../build/memory"
DRY_RUN=0
[ "$1" = "--dry-run" ] && DRY_RUN=1

TOTAL=0

log() { echo "$*"; }

# ============================================================
# Brand/Company -- entity=18
# Users can add their own entries here.
# ============================================================
COMPANIES=()

# ============================================================
# Technology/Framework/Platform -- entity=19
# Users can add their own entries here.
# ============================================================
TECHS=()

# ============================================================
# General Concepts -- entity=0
# Users can add their own entries here.
# ============================================================
CONCEPTS=()

# ============================================================
# Write to hipool
# ============================================================
echo "========================================"
echo "  hipool Lexicon Generator"
echo "========================================"
echo "  Mode: $([ $DRY_RUN -eq 1 ] && echo 'Preview' || echo 'Write')"
echo ""

TOTAL_COUNT=$(( ${#COMPANIES[@]} + ${#TECHS[@]} + ${#CONCEPTS[@]} ))

if [ $TOTAL_COUNT -eq 0 ]; then
  echo "  No entries defined. Edit the COMPANIES, TECHS, or CONCEPTS arrays."
  echo "========================================"
  exit 0
fi

echo "  Brand/Company (ent=18): ${#COMPANIES[@]}"
echo "  Tech/Framework (ent=19): ${#TECHS[@]}"
echo "  General Concepts (ent=0):  ${#CONCEPTS[@]}"
echo "  Total: $TOTAL_COUNT"
echo ""

write_batch() {
  local entity=$1
  shift
  local words=("$@")
  local count=0

  for w in "${words[@]}"; do
    local val="{\"entity\":$entity,\"tag\":\"M\"}"
    if [ $DRY_RUN -eq 0 ]; then
      "$MEMORY" set "lexicon:$w" "$val" --tags "generated,entity_$entity" >/dev/null 2>&1
    fi
    count=$((count + 1))
  done
  TOTAL=$((TOTAL + count))
  echo "  entity=$entity: $count words"
}

if [ $DRY_RUN -eq 0 ]; then
  echo "Writing to hipool..."
  [ ${#COMPANIES[@]} -gt 0 ] && write_batch 18 "${COMPANIES[@]}"
  [ ${#TECHS[@]} -gt 0 ] && write_batch 19 "${TECHS[@]}"
  [ ${#CONCEPTS[@]} -gt 0 ] && write_batch 0  "${CONCEPTS[@]}"
  "$MEMORY" flush >/dev/null 2>&1
fi

echo ""
echo "========================================"
echo "  Total: $TOTAL words"
echo "========================================"