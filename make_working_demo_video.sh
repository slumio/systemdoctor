#!/bin/bash
set -e

# Setup directories
cd /home/joyboy/Systempilot/syspilot

echo "=================================================="
echo "🎬 Creating Working Demo Video (MP4)"
echo "=================================================="

# Create the ffmpeg concat file
echo "📋 Step 1: Generating concat descriptor..."
mkdir -p /tmp/working_demo
cat << 'EOF' > /tmp/working_demo/concat_list.txt
file '/home/joyboy/Systempilot/syspilot/working_demo_frames/frame_001.png'
duration 3
file '/home/joyboy/Systempilot/syspilot/working_demo_frames/frame_002.png'
duration 3
file '/home/joyboy/Systempilot/syspilot/working_demo_frames/frame_003.png'
duration 3
file '/home/joyboy/Systempilot/syspilot/working_demo_frames/frame_004.png'
duration 3
file '/home/joyboy/Systempilot/syspilot/working_demo_frames/frame_005.png'
duration 3
file '/home/joyboy/Systempilot/syspilot/working_demo_frames/frame_006.png'
duration 6
file '/home/joyboy/Systempilot/syspilot/working_demo_frames/frame_007.png'
duration 3
file '/home/joyboy/Systempilot/syspilot/working_demo_frames/frame_007.png'
EOF

# Render using local static ffmpeg
echo "🎥 Step 2: Rendering 1080p MP4 video..."
./bin/ffmpeg -y -f concat -safe 0 -i /tmp/working_demo/concat_list.txt \
  -vsync vfr -pix_fmt yuv420p \
  -vf "scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2:black" \
  /home/joyboy/Systempilot/syspilot/working_demo.mp4

# Copy to assets/
mkdir -p assets
cp /home/joyboy/Systempilot/syspilot/working_demo.mp4 assets/working_demo.mp4

echo "=================================================="
echo "🎉 Working demo video created successfully!"
echo "Location: /home/joyboy/Systempilot/syspilot/working_demo.mp4"
echo "Asset: /home/joyboy/Systempilot/syspilot/assets/working_demo.mp4"
echo "=================================================="
