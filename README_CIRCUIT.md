# NAA TV — Circuit Wiring Reference

This package documents the NAA TV ESP32-S3 pin assignments from the project’s current `config.h`. It is intended for the GitHub repository as a wiring reference.

## Files
- `NAA_TV_Circuit.svg` — readable wiring diagram for GitHub and documentation.
- `NAA_TV_Circuit.kicad_sch` — editable KiCad drawing/reference page. This file is a layout overview, **not** a fully electrically connected, ERC-checked schematic.
- `NAA_TV_Pin_Map.csv` — pin-by-pin connection table.

## Key connections
- TFT and XPT2046 touch controller are shown on a shared SPI bus: SCK GPIO12, MOSI GPIO11, MISO GPIO13. Separate chip-select pins: TFT GPIO10 and touch GPIO7.
- TFT control pins: DC GPIO9, RESET GPIO14, backlight GPIO21.
- BMP280 I²C: SDA GPIO15, SCL GPIO16.
- KY-040 encoder: CLK/A GPIO4, DT/B GPIO5, SW GPIO6.
- KY-023 joystick: VRx GPIO1, VRy GPIO2, SW GPIO3.

## Important hardware verification
These pin numbers are based on the firmware configuration, not a physical inspection of the exact modules. Confirm the labels and pin order on your actual display/touch breakout and sensor boards before wiring. Verify supply voltage, logic-level compatibility, and TFT backlight current requirements from each module’s documentation. Use a common ground. Do not treat this reference as a manufacturing-ready schematic until the actual modules and connections have been checked.
