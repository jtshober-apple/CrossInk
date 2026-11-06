#!/bin/bash
# Generates the built-in fonts: iA Writer Mono (SIL OFL 1.1, see ../builtinFonts/source/IAWriterMono/LICENSE-OFL.md)
# for reading and for the UI. After running, rebuild src/fontIds.h with build-font-ids.sh.
#
# NOTE (OFL): the generated headers are a modified version of the font, and iA Writer Mono's licence reserves the names
# "iA Writer" and "Plex": the compiled-in family is therefore shown as plain "Mono" and the headers are named mono_*.
# The unmodified TTFs may keep their own name when installed from the SD card (/fonts/iAWriterMono/).
set -e

cd "$(dirname "$0")"

SRC="../builtinFonts/source/IAWriterMono/iAWriterMonoS"
READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
# Mono is ~20% wider than a proportional face, so the useful reading sizes are smaller than 12-18. (150 dpi: 11 pt is
# ~31 characters across the page.)  Keep in sync with BUILTIN_READER_POINT_SIZES in src/ReaderFontSizes.h and with the
# registration in src/main.cpp, src/CrossPointSettings.cpp.
MONO_FONT_SIZES=(9 10 11 12 14 16)

for size in ${MONO_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="mono_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size ${SRC}-${style}.ttf --2bit --compress --zopfli > $output_path
    echo "Generated $output_path"
  done
done

# UI fonts: uncompressed (no per-glyph inflate on menus), regular + bold at 10/12 and regular at 8.
for size in 10 12; do
  for style in Regular Bold; do
    font_name="monoui_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size ${SRC}-${style}.ttf --2bit > $output_path
    echo "Generated $output_path"
  done
done
python fontconvert.py monoui_8_regular 8 ${SRC}-Regular.ttf --2bit > ../builtinFonts/monoui_8_regular.h
echo "Generated ../builtinFonts/monoui_8_regular.h"

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
