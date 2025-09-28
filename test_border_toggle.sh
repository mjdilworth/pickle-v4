#!/bin/bash

echo "Testing Border Toggle Video Visibility Issue"
echo "============================================"

cd /home/dilly/Projects/pickle-v4

# Test 1: Start with border disabled, enable it after a few frames
echo "Test 1: Testing border toggle during playback..."

# Create a modified version that auto-enables border after 3 seconds
cat > test_border_toggle.c << 'EOF'
#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

int main() {
    pid_t pid = fork();
    
    if (pid == 0) {
        // Child: run video player
        execl("./pickle", "./pickle", "/home/dilly/Projects/content/vid-a.mp4", NULL);
        return 1;
    } else {
        // Parent: wait 3 seconds then send 'b' key
        sleep(3);
        printf("Sending 'b' to toggle border...\n");
        kill(pid, SIGUSR1); // This won't work for key input, but demonstrates the concept
        sleep(2);
        kill(pid, SIGTERM);
        wait(NULL);
        return 0;
    }
}
EOF

# Instead, let's just test with border enabled from start and see the behavior
echo "Testing border rendering stability..."

# Enable borders by default for testing
sed -i 's/keystone->show_border = false;/keystone->show_border = true;/' keystone.c
make > /dev/null 2>&1

echo "Running with border enabled (5 second test)..."
timeout 5s ./pickle /home/dilly/Projects/content/vid-a.mp4 > border_test.log 2>&1

# Check if video rendered successfully with border
if grep -q "GPU YUV→RGB rendering started" border_test.log; then
    echo "✅ Video renders with border enabled"
    if grep -q "Overlays:" border_test.log; then
        echo "✅ Border overlay is active"
    else
        echo "⚠️  Border overlay timing not detected"
    fi
else
    echo "❌ Video failed to render with border enabled"
    echo "Error details:"
    tail -5 border_test.log
fi

# Restore default
sed -i 's/keystone->show_border = true;/keystone->show_border = false;/' keystone.c
make > /dev/null 2>&1

# Now test corner toggle to compare
echo
echo "Testing corner rendering for comparison..."
sed -i 's/keystone->show_corners = false;/keystone->show_corners = true;/' keystone.c
make > /dev/null 2>&1

timeout 5s ./pickle /home/dilly/Projects/content/vid-a.mp4 > corner_test.log 2>&1

if grep -q "GPU YUV→RGB rendering started" corner_test.log; then
    echo "✅ Video renders with corners enabled"
else
    echo "❌ Video failed to render with corners enabled"
fi

# Restore default
sed -i 's/keystone->show_corners = true;/keystone->show_corners = false;/' keystone.c
make > /dev/null 2>&1

# Compare results
echo
echo "Results Analysis:"
echo "================"

border_overlays=$(grep -o "Overlays: [0-9.]*ms" border_test.log | head -3)
corner_overlays=$(grep -o "Overlays: [0-9.]*ms" corner_test.log | head -3)

echo "Border overlay timing: $border_overlays"
echo "Corner overlay timing: $corner_overlays"

if [ -n "$border_overlays" ] && [ -n "$corner_overlays" ]; then
    echo "✅ Both overlays working - the issue might be specifically with the toggle timing"
else
    echo "❌ One or both overlays have issues"
fi

rm -f test_border_toggle.c border_test.log corner_test.log

echo
echo "To manually test the toggle issue:"
echo "1. Run: ./pickle /path/to/video.mp4"
echo "2. Wait for video to start playing (you'll see 'GPU YUV→RGB rendering started')"
echo "3. Press 'b' to toggle border"
echo "4. Observe if video disappears"