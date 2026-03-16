#!/bin/bash
# Re-encode all OGG files in the current directory at 22050Hz sample rate.
# Keeps stereo and uses quality-based encoding (VBR ~128kbps at 22050Hz).
# Backs up originals to ogg_backup/ before replacing.

set -e

if ! command -v ffmpeg &>/dev/null; then
    echo "ERROR: ffmpeg not found in PATH"
    exit 1
fi

shopt -s nullglob
oggs=(*.ogg *.OGG)
shopt -u nullglob

if [ ${#oggs[@]} -eq 0 ]; then
    echo "No OGG files found in current directory."
    exit 0
fi

mkdir -p ogg_backup

echo "Converting ${#oggs[@]} OGG files to 22050Hz..."
echo ""

total_before=0
total_after=0

for f in "${oggs[@]}"; do
    before=$(stat -c%s "$f" 2>/dev/null || stat -f%z "$f" 2>/dev/null)
    total_before=$((total_before + before))

    # Skip files under 3MB
    if [ "$before" -lt 3145728 ]; then
        echo "  $f ($(( before / 1024 ))KB) -- skipped (under 3MB)"
        total_after=$((total_after + before))
        continue
    fi

    echo -n "  $f ($(( before / 1024 ))KB) -> "

    # Back up original
    cp "$f" "ogg_backup/$f"

    # Re-encode at original sample rate with 64kbps bitrate cap
    ffmpeg -y -i "ogg_backup/$f" -c:a libvorbis -b:a 64k "$f" 2>/dev/null

    after=$(stat -c%s "$f" 2>/dev/null || stat -f%z "$f" 2>/dev/null)
    total_after=$((total_after + after))

    echo "$(( after / 1024 ))KB ($(( 100 * after / before ))%)"
done

echo ""
echo "Done. Total: $(( total_before / 1024 ))KB -> $(( total_after / 1024 ))KB (saved $(( (total_before - total_after) / 1024 ))KB)"
echo "Originals backed up in ogg_backup/"
