#!/bin/bash
# Terminal Recovery Script for Pickle Video Player
# Use this if pickle crashes and leaves your terminal in raw mode

echo "=== Pickle Terminal Recovery ==="
echo "Attempting to restore terminal to normal mode..."

# Reset terminal to sane defaults
stty sane

# Additional terminal fixes
stty echo      # Re-enable echo
stty icanon    # Enable canonical mode
stty icrnl     # Enable CR to NL translation
stty onlcr     # Enable NL to CRLF translation

# Reset terminal using tput
tput reset 2>/dev/null || true

# Try to restore from saved state if available
if [ -f /tmp/pickle_term_state ]; then
    echo "Found saved terminal state, attempting restore..."
    # Note: Actual restoration would require parsing the saved termios structure
    # For now, stty sane should be sufficient
    rm -f /tmp/pickle_term_state
fi

echo ""
echo "Terminal should now be restored."
echo "If you still have issues, try:"
echo "  1. Close and reopen your terminal"
echo "  2. Run: reset"
echo "  3. Log out and log back in"
echo ""
