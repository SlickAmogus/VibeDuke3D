#!/bin/bash
# Convert all .MID files in MIDI/ folder to .ogg using FluidSynth + ffmpeg
# Usage: ./convert_midi_to_ogg.sh [soundfont.sf2]

SOUNDFONT="${1:-soundfont.sf2}"
INDIR="MIDI"
OUTDIR="music"

if [ ! -f "$SOUNDFONT" ]; then
    echo "Error: Soundfont not found: $SOUNDFONT"
    echo "Usage: $0 path/to/soundfont.sf2"
    exit 1
fi

mkdir -p "$OUTDIR"

for mid in "$INDIR"/*.MID "$INDIR"/*.mid; do
    [ -f "$mid" ] || continue
    base="$(basename "${mid%.*}")"
    wav="/tmp/${base}.wav"
    ogg="${OUTDIR}/${base}.ogg"

    echo "Converting: $mid -> $ogg"
    fluidsynth -ni -F "$wav" -r 44100 "$SOUNDFONT" "$mid" && \
    ffmpeg -y -i "$wav" -c:a libvorbis -q:a 5 "$ogg" && \
    rm -f "$wav"
done

echo "Done. OGG files are in $OUTDIR/"
