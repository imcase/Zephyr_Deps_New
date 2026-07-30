#!/bin/bash
# run_symlinks.sh (Located INSIDE Zephyr_Deps)
# Usage: ./Zephyr_Deps/run_symlinks.sh <folder_name>

# 1. Detect the script's actual location
SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" &>/dev/null && pwd)

TARGET_SUBFOLDER=$1

# 2. Argument Check
if [ -z "$TARGET_SUBFOLDER" ]; then
    echo -e "\n**ERROR**: No folder specified. Usage: $0 <SOURCE_RELATIVE_PATH>\n"
    exit 1
fi

# Define Source and Parent relative to the SCRIPT location
SOURCE_DIR="$SCRIPT_DIR/$TARGET_SUBFOLDER"
PARENT_DIR=$(dirname "$SCRIPT_DIR")

# 3. Source Existence Check
if [ ! -d "$SOURCE_DIR" ]; then
    echo -e "\n**ERROR**: Source folder '$SOURCE_DIR' does not exist.\n"
    exit 1
fi

echo -e "\nPhase 1: Pre-flight Validation..."
echo "Parent Repo Root: $PARENT_DIR"

MISSING_PATHS=0
# Validate that the structure in Parent Repo exists before trying to link into it
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

# --- PHASE 2: DELETION ---
echo "Phase 2: Removing existing files/links in Parent Repo..."
while read -r REL_PATH; do
    CLEAN_PATH="${REL_PATH#./}"
    DESTINATION="$PARENT_DIR/$CLEAN_PATH"
    
    # Remove if it's a regular file or a symbolic link
    if [ -f "$DESTINATION" ] || [ -L "$DESTINATION" ]; then
        rm "$DESTINATION"
        echo "   Removed: $CLEAN_PATH"
    fi
done < <(cd "$SOURCE_DIR" && find . -type f -not -path '*/.*')

# --- PHASE 3: SYMLINK ---
echo "Phase 3: Relative Symlink Process..."
while read -r REL_PATH; do
    CLEAN_PATH="${REL_PATH#./}"
    REAL_SOURCE="$SOURCE_DIR/$CLEAN_PATH"
    DESTINATION="$PARENT_DIR/$CLEAN_PATH"
    
    # -s: symbolic, -f: force, -r: relative
    ln -sfr "$REAL_SOURCE" "$DESTINATION"
    echo "   Linked: $CLEAN_PATH"
done < <(cd "$SOURCE_DIR" && find . -type f -not -path '*/.*')

echo -e "\nDone. Symlinks are active.\n"