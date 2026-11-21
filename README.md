# Adventure Game!

Adventure Game! is a handheld interactive fiction player by bpwagner. It runs
Frotz on the 4-inch Freenove FNK0103 ESP32 board and uses an M5Stack Unit CardKB
v1.1 keyboard with a microSD story library.

## Build and play

1. Open this folder in VS Code with the PlatformIO extension installed.
2. Connect the Freenove board over USB and run PlatformIO's **Upload** task
   for the `freenove_fnk0103` environment.
3. Copy your Z-machine stories to the root of a FAT32 microSD card and insert
   it in the board. The player supports files such as `.z3`, `.z5`, and Z-code
   `.zblorb` files. Supply your own story files.

The keyboard uses I²C address `0x5F`, with SDA on GPIO32 and SCL on GPIO25.

Use **Up/Down** and **Enter** to choose a story. **F** switches between titles
and filenames, **S** opens display settings, **A** shows credits, and **R**
rescans the card. Use **Esc** during play to return to the menu. Type `SAVE`
and `RESTORE` in a story to manage progress on the microSD card.

## Interpreter

See the [Frotz attribution](lib/Frotz/README.md) and [GPL terms](lib/Frotz/COPYING)
for the interpreter bundled with this project.
