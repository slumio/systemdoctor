#!/bin/bash
set -e

# Setup directories
cd /home/joyboy/Systempilot/syspilot

echo "=================================================="
echo "🎬 Creating Presentation Slide Video (MP4)"
echo "=================================================="

# Extract PDF pages as images
echo "🔍 Step 1: Extracting PDF pages..."
mkdir -p /tmp/slides
rm -f /tmp/slides/page-*.png
pdftoppm -png -r 150 /home/joyboy/Systempilot/syspilot/presentation_script.pdf /tmp/slides/page

# Create the ffmpeg concat file
echo "📋 Step 2: Generating concat descriptor..."
cat << 'EOF' > /tmp/slides/concat_list.txt
file '/tmp/slides/page-1.png'
duration 30
file '/tmp/slides/page-2.png'
duration 60
file '/tmp/slides/page-3.png'
duration 30
file '/tmp/slides/page-4.png'
duration 120
file '/tmp/slides/page-5.png'
duration 120
file '/tmp/slides/page-5.png'
EOF

# Render using local static ffmpeg
echo "🎥 Step 3: Rendering 1080p MP4 video..."
./bin/ffmpeg -y -f concat -safe 0 -i /tmp/slides/concat_list.txt \
  -vsync vfr -pix_fmt yuv420p \
  -vf "scale=1920:1080:force_original_aspect_ratio=decrease,pad=1920:1080:(ow-iw)/2:(oh-ih)/2:black" \
  /home/joyboy/Systempilot/syspilot/presentation.mp4

echo "📦 Step 4: Copying to assets..."
mkdir -p assets
cp /home/joyboy/Systempilot/syspilot/presentation.mp4 assets/presentation.mp4

echo "=================================================="
echo "🎉 Presentation video created successfully!"
echo "Location: /home/joyboy/Systempilot/syspilot/presentation.mp4"
echo "Asset: /home/joyboy/Systempilot/syspilot/assets/presentation.mp4"
echo "=================================================="
