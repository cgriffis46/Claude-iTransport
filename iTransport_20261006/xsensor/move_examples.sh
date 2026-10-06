#!/bin/sh
# Moves every STM32CubeIDE project in the STM32 xSensor Library into
# one top-level examples/ folder.
#
# Run from the root of the repository (the folder holding README.md),
# in Git Bash on Windows or any shell elsewhere:
#
#     sh move_examples.sh
#
# Uses "git mv", so each file's history follows it, and nothing is
# committed: look at "git status" afterwards and commit when happy.
# Uncommitted edits inside a project move with it.
#
# The projects only refer to files inside their own folder (and to the
# STM32Cube repository by absolute path), so they build the same from
# the new place. STM32CubeIDE remembers where each project was, though:
# remove the projects from the workspace (without deleting the files)
# and bring them back in with File > Open Projects from File System.

set -e

if [ ! -d .git ] || [ ! -f README.md ]; then
    echo "Run this from the root of the STM32 xSensor Library repository." >&2
    exit 1
fi

mkdir -p examples

move() {
    if [ -d "$1" ]; then
        git mv "$1" "examples/$(basename "$1")"
        echo "moved  $1"
    else
        echo "skip   $1 (not found)"
    fi
}

move aht20/examples/STM32_AHT20_TEST
move bme280/examples/stm32_bme280_test
move bmp280/examples/stm32_bmp280_test
move bmp390/examples/stm32_bmp390_example
move can_test/stm32_can_test
move HMC6352/examples/stm32_HMC6352_example
move htu21df/examples/stm32_htu21df_example
move lps35hw/examples/stm32_lps35hw_example
move mpl3115a2/examples/STM32_mpl3115a2_test
move PM25/examples/stm32_pm25_example
move rtc-lib/stm32-rtclib-example
move sht31/examples/stm32_sht31_example
move si7021/examples/stm32_si7021_example

# Clear away the folders the moves left empty. rmdir only removes a
# folder with nothing in it, so anything still holding files stays.
for d in aht20 bme280 bmp280 bmp390 HMC6352 htu21df lps35hw mpl3115a2 PM25 sht31 si7021; do
    rmdir "$d/examples" 2>/dev/null || true
    rmdir "$d" 2>/dev/null || true
done
rmdir can_test rtc-lib 2>/dev/null || true

echo
echo "Done. Nothing is committed yet: check \"git status\"."
