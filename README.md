# HeadsetBatteryPlugin

A Rainmeter plugin that displays the battery level of a SteelSeries Arctis Nova 7 (and other headsets supported by HeadsetControl).

## Requirements

- [HeadsetControl](https://github.com/Sapd/HeadsetControl/releases) - download the Windows release and place `headsetcontrol.exe` somewhere on your PC
- [Rainmeter](https://www.rainmeter.net/) 4.0 or later (64-bit)

## Installation

1. Download `HeadsetBatteryPlugin.dll` from the Actions tab (click the latest successful run, then download the artifact at the bottom)
2. Copy it to `%APPDATA%\Rainmeter\Plugins\`
3. Copy `HeadsetBattery.ini` into a new folder: `%USERPROFILE%\Documents\Rainmeter\Skins\HeadsetBattery\`
4. Edit the `HeadsetControlPath` variable in `HeadsetBattery.ini` to point to your `headsetcontrol.exe`
5. In Rainmeter, go to Manage > Skins, find HeadsetBattery and click Load

## Skin options

| Option | Default | Description |
|--------|---------|-------------|
| `HeadsetControlPath` | `C:\Tools\HeadsetControl\headsetcontrol.exe` | Full path to headsetcontrol.exe |
| `UpdateInterval` | `300` | Seconds between headset queries |

## Notes

- The plugin queries the headset once on load, then repeats on your chosen interval.
- Shows "Charging" when the headset is on charge, "N/A" when the dongle is unplugged or headset is off.
