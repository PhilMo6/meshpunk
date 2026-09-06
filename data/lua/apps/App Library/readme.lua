return { body = [[
The App Library installs apps, themes and LoRa protocols onto the device over WiFi, and updates ones already installed - no firmware reflash required. It reads its catalog from github.com/PhilMo6/meshpunk-apps.

- Apps are grouped by category; when anything installed is behind the catalog - an app, a LoRa protocol, its BLE companion or a USB driver - an Updates list appears at the top with one row each.
- LoRa Protocols is the pinned first category: every radio protocol (MeshCore, MTLite) listed like an app, with its version and description. Installing one asks where it goes (internal flash is recommended - it loads even without the SD card; a card-hosted protocol needs the card at boot or the radio stays off), offers to download its apps next, one visible install after another, and then points you to Settings > Lora to make it the boot protocol. Nothing about protocols installs in the background. Remove on a protocol takes out the package only - its apps and BLE companion stay (each has its own Remove: apps in their category, the companion on the protocol's page) and its settings and messages stay on storage; removing the running protocol means the radio is off after the next reboot until you pick another, and removing the selected BLE companion does the same for Bluetooth. Reinstalling a package brings its companion back.
- A protocol app whose protocol is not installed shows "Needs <protocol>"; picking Install on it asks whether to download the protocol first, then installs the app.
- Everything that ships with the firmware is tracked too, so even preinstalled apps, themes and the MeshCore protocol update OTA.
- System apps (App Library, Files, Map, Messenger, and the Settings pages) are non-removable, but can still be updated.
- An app that needs a newer firmware than you are running shows "Needs FW" instead of Install.

An installed app can bring its own help page with it. Those appear in the Apps tab of this guide, so the guide grows as you install things.

Themes: 15 are included - switch in Settings > Theme, and download more via Settings > Theme > Get.

Contributions (your own apps and themes) are welcome via pull request - see the catalog repo's README.]] }
