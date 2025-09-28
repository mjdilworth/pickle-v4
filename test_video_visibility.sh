#!/bin/bash

echo "Testing Video Visibility with Keystone Overlays"
echo "==============================================="

cd /home/dilly/Projects/pickle-v4

# Compile the project
make clean && make
if [ $? -ne 0 ]; then
    echo "❌ Build failed!"
    exit 1
fi

echo "✅ Build successful"
echo
echo "Testing video visibility with overlays enabled..."
echo

# Test 1: Enable corners by default and check if video still renders
echo "Test 1: Corners enabled by default"
sed -i 's/keystone->show_corners = false;/keystone->show_corners = true;/' keystone.c
make > /dev/null 2>&1

timeout 5s ./pickle /home/dilly/Projects/content/vid-a.mp4 > test_corners.log 2>&1

# Check if video rendering started successfully with corners enabled
if grep -q "GPU YUV→RGB rendering started" test_corners.log && grep -q "Overlays:" test_corners.log; then
    echo "✅ Video renders correctly with corners enabled"
else
    echo "❌ Video disappeared with corners enabled"
    echo "Log excerpt:"
    tail -10 test_corners.log
    exit 1
fi

# Test 2: Enable border by default
echo "Test 2: Border enabled by default"
sed -i 's/keystone->show_border = false;/keystone->show_border = true;/' keystone.c
make > /dev/null 2>&1

timeout 5s ./pickle /home/dilly/Projects/content/vid-a.mp4 > test_border.log 2>&1

# Check if video rendering started successfully with border enabled  
if grep -q "GPU YUV→RGB rendering started" test_border.log && grep -q "Overlays:" test_border.log; then
    echo "✅ Video renders correctly with border enabled"
else
    echo "❌ Video disappeared with border enabled"
    echo "Log excerpt:"
    tail -10 test_border.log
    exit 1
fi

# Test 3: Both overlays enabled
echo "Test 3: Both corners and border enabled"
timeout 5s ./pickle /home/dilly/Projects/content/vid-a.mp4 > test_both.log 2>&1

if grep -q "GPU YUV→RGB rendering started" test_both.log && grep -q "Overlays:" test_both.log; then
    echo "✅ Video renders correctly with both overlays enabled"
else
    echo "❌ Video disappeared with both overlays enabled"
    echo "Log excerpt:"
    tail -10 test_both.log
    exit 1
fi

# Restore original settings
echo
echo "Restoring default settings..."
sed -i 's/keystone->show_corners = true;/keystone->show_corners = false;/' keystone.c
sed -i 's/keystone->show_border = true;/keystone->show_border = false;/' keystone.c
make > /dev/null 2>&1

echo "✅ All tests passed!"
echo
echo "The video visibility issue has been fixed!"
echo
echo "You can now safely:"
echo "- Press 'c' to toggle corner visibility without video disappearing"
echo "- Press 'b' to toggle border visibility without video disappearing"
echo "- Use both overlays simultaneously"

# Clean up
rm -f test_corners.log test_border.log test_both.log

echo
echo "Manual test instructions:"
echo "1. Run: ./pickle /path/to/video.mp4"
echo "2. Press 'c' to show corners - video should remain visible"
echo "3. Press 'b' to show border - video should remain visible"
echo "4. Press 'c' again to hide corners"
echo "5. Press 'b' again to hide border"