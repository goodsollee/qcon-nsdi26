#!/usr/bin/env bash
#
# print_code_structure.sh
#
# Prints the directory tree (or falls back to find) and then reports the
# total lines of C++ source/header code (*.cpp, *.hpp), excluding the
# "emulator_logs" and "external" folders.
#
# Usage:
#   ./print_code_structure.sh         # current directory
#   ./print_code_structure.sh /path   # specified path
#

TARGET_DIR="${1:-.}"
EXCLUDE_DIRS="emulator_logs|external"

#######################################
# 1. Show the directory structure
#######################################
if command -v tree &>/dev/null; then
  echo "Using 'tree' to display the directory structure..."
  tree "$TARGET_DIR" \
       -I "${EXCLUDE_DIRS}|*.o|*.pyc|*.pyo|__pycache__|obj" \
       -a
else
  echo "'tree' not found, falling back to 'find' command..."
  echo
  find "$TARGET_DIR" \
    \( -path "$TARGET_DIR/emulator_logs" -o -path "$TARGET_DIR/external" \) -prune -o \
    -type d -print | while read -r dir; do
      # Indent according to depth
      indent="${dir#"$TARGET_DIR"}"
      indent="${indent//[^\/]/}"
      printf '%*s%s\n' $(( ${#indent} / 1 * 2 )) '' "$dir"
    done
  echo
  echo "You can install 'tree' for a nicer display (e.g., 'sudo apt-get install tree')."
fi

#######################################
# 2. Count C++ source/header lines
#######################################
echo
echo "Calculating total lines of *.cpp and *.hpp (excluding ${EXCLUDE_DIRS//|/, })..."
# Find all *.cpp, *.hpp under TARGET_DIR, skipping excluded dirs, then run wc -l
TOTAL_LINES=$(find "$TARGET_DIR" \
    \( -path "$TARGET_DIR/emulator_logs" -o -path "$TARGET_DIR/external" \) -prune -o \
    -type f \( -name '*.cpp' -o -name '*.hpp' \) -print0 \
    | xargs -0 cat | wc -l)

printf "Total C++ LOC: %'d lines\n" "$TOTAL_LINES"
