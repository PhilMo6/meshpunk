return { body = [[
Chooses which LoRa protocol runs the radio. Exactly one protocol runs at a time, picked at boot.

- MeshCore comes preinstalled and is the default.
- Other protocols are downloads from the App Library's LoRa Protocols category. They install to internal flash (recommended) or the SD card and load when the device starts - a card-hosted protocol needs the card at boot, otherwise the radio stays off for that boot; installing one offers its apps too.
- No radio boots the device with the LoRa chip switched off. Everything except mesh traffic works - useful for testing, and it is how the device runs if no protocol is installed.
- Selecting a protocol here saves the choice; the radio actually switches on the next reboot. A Reboot to apply button appears when the saved choice differs from what is running now.

Each protocol's apps live in its own launcher category (Meshcore, MTLite): its Messenger plus its Radio, Notifications and Identity settings. A protocol's Messenger runs only under its own protocol - opening it under another shows a notice, and MeshCore's Radio and Identity settings do the same. MTLite's settings work under any protocol - saved values apply when MTLite next runs. The Map and the Packets monitor work under every protocol.

Your selection is honored on every boot. If the selected protocol's installed files are missing or unreadable, the device boots with the radio off and posts a notification saying so - nothing is substituted.

Bluetooth is a second, independent protocol slot - BLE and LoRa are different radios, so they run side by side. It has its own app: Settings > Ble.]] }
