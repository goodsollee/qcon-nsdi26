#!/usr/bin/env bash
#
# print_code_structure.sh
#
# Prints the code structure (directory tree) for the current directory
# or for an optionally specified directory passed as the first argument,
# excluding the "emulator_logs" and "external" folders.
#
# Usage:
#   ./print_code_structure.sh         # prints structure of current directory
#   ./print_code_structure.sh /path   # prints structure for /path

TARGET_DIR="${1:-.}"

if command -v tree &>/dev/null; then
  echo "Using 'tree' to display the directory structure..."
  # Exclude build artifacts, __pycache__, obj, emulator_logs, and external folders.
  tree "$TARGET_DIR" \
       -I "emulator_logs|external|*.o|*.pyc|*.pyo|__pycache__|obj" \
       -a
else
  echo "'tree' not found, falling back to 'find' command..."
  echo
  # Use 'find', but prune the emulator_logs and external directories
  find "$TARGET_DIR" \
    \( -path "$TARGET_DIR/emulator_logs" -o -path "$TARGET_DIR/external" \) \
    -prune -o \
    -type d -print | while read -r dir; do
      # Count how many slashes are in the path to determine indentation
      indent="$(echo "$dir" | sed 's|[^/]||g' | sed 's|/|  |g')"
      echo "${indent}${dir}"
    done
  echo
  echo "You can install 'tree' for a nicer display (on Ubuntu/Debian: sudo apt-get install tree)."
fi
