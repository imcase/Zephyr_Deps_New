#!/bin/bash
# run_prod_copy.sh (Located INSIDE Zephyr_Deps)
# Usage: ./run_prod_copy.sh <folder_name>

# 1. Detect the script's actual location
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" &>/dev/null && pwd)

TARGET_SUBFOLDER=$1

# 2. Argument Check
if [ -z "$TARGET_SUBFOLDER" ]; then
    echo -e "\n**ERROR**: No folder specified. Usage: $0 <folder_name>\n"
    exit 1
fi

SOURCE_DIR="$SCRIPT_DIR/$TARGET_SUBFOLDER"
PARENT_DIR=$(dirname "$SCRIPT_DIR")

# 3. Source Existence Check
if [ ! -d "$SOURCE_DIR" ]; then
    echo "**ERROR**: Source folder '$SOURCE_DIR' does not exist."
    exit 1
fi

echo "Phase 1: Pre-flight Validation..."
echo "Targeting: $PARENT_DIR"

MISSING_PATHS=0
while read -r REL_PATH; do
    CLEAN_PATH="${REL_PATH#./}"
    DIR_NAME=$(dirname "$CLEAN_PATH")
    TARGET_DIR="$PARENT_DIR/$DIR_NAME"

    if [ "$DIR_NAME" != "." ] && [ ! -d "$TARGET_DIR" ]; then
        echo "**ERROR**: MISSING DIRECTORY: '$TARGET_DIR'"
        MISSING_PATHS=$((MISSING_PATHS + 1))
    fi
done < <(cd "$SOURCE_DIR" && find . -type f -not -path '*/.*')

if [ $MISSING_PATHS -gt 0 ]; then
    echo -e "\n**ERROR**: $MISSING_PATHS directory path(s) missing. Aborting.\n"
    exit 1
fi

# --- PHASE: DELETION ---
echo "Phase 2: Cleaning target files in Parent Repo..."
while read -r REL_PATH; do
    CLEAN_PATH="${REL_PATH#./}"
    DESTINATION="$PARENT_DIR/$CLEAN_PATH"
    
    if [ -f "$DESTINATION" ]; then
        rm "$DESTINATION"
        echo "   Deleted: $CLEAN_PATH"
    fi
done < <(cd "$SOURCE_DIR" && find . -type f -not -path '*/.*')

# --- EXECUTION: COPY ---
echo "Phase 3: Production Copy Process..."
while read -r REL_PATH; do
    CLEAN_PATH="${REL_PATH#./}"
    DESTINATION="$PARENT_DIR/$CLEAN_PATH"
    
    mkdir -p "$(dirname "$DESTINATION")"
    cp "$SOURCE_DIR/$CLEAN_PATH" "$DESTINATION"
    echo "   Copied: $CLEAN_PATH"
done < <(cd "$SOURCE_DIR" && find . -type f -not -path '*/.*')

echo -e "\nDone. Production deployment complete."
echo "----------------------------------------------------------------------"
echo "REMINDER: Please REMOVE 'Zephyr_Deps' now to finalize."
echo "----------------------------------------------------------------------\n"