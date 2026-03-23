#!/usr/bin/env bash
#
# Fetches profanity word lists from multiple open source repos and merges
# them with any custom words in config/profanity.txt.
#
# Sources:
#   - LDNOOBW (plain text, one per line)
#   - zacanger/profane-words (JSON array)
#   - web-mech/badwords (JSON object with "words" array)
#   - better_profanity (plain text, one per line)
#
# Usage:
#   ./scripts/update-profanity.sh
#   make update-filter

set -euo pipefail

LOCAL_FILE="config/profanity.txt"
MERGED_FILE=$(mktemp)
TEMP_FILE=$(mktemp)

# ── Source list ──────────────────────────────────────────────────────────────
# Format: "URL|TYPE"
#   TYPE: text  = one word per line
#         json_array = JSON array of strings
#         json_words = JSON object with a "words" array

SOURCES=(
    "https://raw.githubusercontent.com/LDNOOBW/List-of-Dirty-Naughty-Obscene-and-Otherwise-Bad-Words/master/en|text"
    "https://raw.githubusercontent.com/zacanger/profane-words/master/words.json|json_array"
    "https://raw.githubusercontent.com/web-mech/badwords/master/lib/lang.json|json_words"
    "https://raw.githubusercontent.com/snguyenthanh/better_profanity/master/better_profanity/profanity_wordlist.txt|text"
)

# ── Start with existing local words ─────────────────────────────────────────
if [ -f "$LOCAL_FILE" ]; then
    local_count=$(wc -l < "$LOCAL_FILE" | tr -d ' ')
    echo "Local list: $local_count words"
    cp "$LOCAL_FILE" "$MERGED_FILE"
else
    echo "No local list found, starting fresh"
    mkdir -p "$(dirname "$LOCAL_FILE")"
    touch "$MERGED_FILE"
fi

# ── Fetch and parse each source ─────────────────────────────────────────────
total_fetched=0

for entry in "${SOURCES[@]}"; do
    url="${entry%%|*}"
    type="${entry##*|}"
    # Extract repo name for display
    repo=$(echo "$url" | sed 's|https://raw.githubusercontent.com/||' | cut -d'/' -f1-2)

    printf "  Fetching %-45s " "$repo..."

    if ! curl -sL "$url" -o "$TEMP_FILE" 2>/dev/null; then
        echo "FAILED (download error)"
        continue
    fi

    # Check for 404 or empty response
    if [ ! -s "$TEMP_FILE" ] || grep -q "^404" "$TEMP_FILE" 2>/dev/null; then
        echo "FAILED (not found)"
        continue
    fi

    # Parse based on type
    case "$type" in
        text)
            cat "$TEMP_FILE" >> "$MERGED_FILE"
            count=$(wc -l < "$TEMP_FILE" | tr -d ' ')
            ;;
        json_array)
            # Parse ["word1", "word2", ...] — extract quoted strings
            sed 's/\[//;s/\]//;s/",/\n/g;s/"//g' "$TEMP_FILE" \
                | sed 's/^[[:space:]]*//;s/[[:space:]]*$//' \
                | sed '/^$/d' \
                >> "$MERGED_FILE"
            count=$(sed 's/\[//;s/\]//;s/",/\n/g;s/"//g' "$TEMP_FILE" | sed '/^$/d' | wc -l | tr -d ' ')
            ;;
        json_words)
            # Parse {"words": ["word1", "word2", ...]} — extract from words array
            sed 's/.*"words":\[//;s/\].*//' "$TEMP_FILE" \
                | sed 's/",/\n/g;s/"//g' \
                | sed 's/^[[:space:]]*//;s/[[:space:]]*$//' \
                | sed '/^$/d' \
                >> "$MERGED_FILE"
            count=$(sed 's/.*"words":\[//;s/\].*//' "$TEMP_FILE" | sed 's/",/\n/g;s/"//g' | sed '/^$/d' | wc -l | tr -d ' ')
            ;;
        *)
            echo "FAILED (unknown type: $type)"
            continue
            ;;
    esac

    echo "$count words"
    total_fetched=$((total_fetched + count))
done

echo ""
echo "Total fetched from all sources: $total_fetched words"

# ── Merge, deduplicate, sort ────────────────────────────────────────────────
cat "$MERGED_FILE" \
    | tr '[:upper:]' '[:lower:]' \
    | sed 's/^[[:space:]]*//;s/[[:space:]]*$//' \
    | sed 's/\r$//' \
    | sed '/^$/d' \
    | sort -u \
    > "$LOCAL_FILE"

rm -f "$MERGED_FILE" "$TEMP_FILE"

final_count=$(wc -l < "$LOCAL_FILE" | tr -d ' ')
echo "Final deduplicated list: $final_count words (saved to $LOCAL_FILE)"
