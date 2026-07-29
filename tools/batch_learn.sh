#!/bin/bash
# hipool batch lexicon learning -- uses memory set command
# Users can add their own entries in the arrays below.
# Usage: bash batch_learn.sh [--dry-run]

MEMORY="$(cd "$(dirname "$0")"/.. && pwd)/build/memory"
DRY_RUN=0
if [ "$1" = "--dry-run" ]; then DRY_RUN=1; fi

echo "========================================"
echo "  hipool Batch Lexicon Import"
echo "========================================"
echo "  Mode: $([ $DRY_RUN -eq 1 ] && echo 'Preview' || echo 'Write')"
echo "  Path: $MEMORY"
echo ""

# Users can add their own entries here in the format "word:entity_id".
# Examples:
#   "CompanyName:18" for brand/company
#   "TechName:19" for technology/framework
#   "ConceptName:0" for general concept
BRANDS=()
TECHS=()
CONCEPTS=()

echo "  Brand/Company: ${#BRANDS[@]}"
echo "  Tech/Framework: ${#TECHS[@]}"
echo "  Concepts: ${#CONCEPTS[@]}"
echo ""

TOTAL=$((${#BRANDS[@]} + ${#TECHS[@]} + ${#CONCEPTS[@]}))

if [ $TOTAL -eq 0 ]; then
  echo "  No entries defined. Edit the BRANDS, TECHS, or CONCEPTS arrays above."
  echo "========================================"
  exit 0
fi

if [ $DRY_RUN -eq 1 ]; then
  echo "  (Preview mode, not written -- run without --dry-run to execute)"
  echo "========================================"
  exit 0
fi

echo "  Writing to hipool..."
OK=0
FAIL=0

write_word() {
  local word="$1" entity="$2"
  local val="{\"entity\":$entity,\"tag\":\"M\"}"
  local result
  result=$("$MEMORY" set "lexicon:$word" "$val" --tags "learned,entity_$entity" 2>&1)
  if echo "$result" | grep -q "ok"; then
    OK=$((OK+1))
  else
    FAIL=$((FAIL+1))
  fi
}

for entry in "${BRANDS[@]}"; do
  word="${entry%%:*}"
  entity="${entry##*:}"
  write_word "$word" "$entity"
done

for entry in "${TECHS[@]}"; do
  word="${entry%%:*}"
  entity="${entry##*:}"
  write_word "$word" "$entity"
done

for entry in "${CONCEPTS[@]}"; do
  word="${entry%%:*}"
  entity="${entry##*:}"
  write_word "$word" "$entity"
done

# flush to disk
"$MEMORY" flush > /dev/null 2>&1

echo ""
echo "  Write complete: OK $OK / Failed $FAIL"
echo "========================================"
