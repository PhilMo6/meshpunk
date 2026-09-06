-- LoRa protocols: installable protocol packages, the boot picker, per-protocol apps.

local body = [[
The LoRa radio protocol is not part of the firmware - it is an installable package. MeshCore ships preinstalled as the default; other protocols (like MTLite, the Meshtastic-compatible protocol) install from the App Library.

GETTING A PROTOCOL
The App Library's LoRa Protocols category lists every protocol like an app, with its version and description. Installing one asks where it goes - internal flash is recommended because it loads even without the SD card; a protocol on the card needs the card in place at boot, or the radio stays off for that boot (with a notification). It then offers to download its apps (Messenger, Radio, Notifications, Identity) - say yes and they arrive one after another, each with its own progress. Nothing about protocols ever installs in the background: picking a protocol app whose protocol is missing asks first whether to download the protocol, then installs it before the app.

CHOOSING THE BOOT PROTOCOL
Settings > Lora lists every installed protocol plus "none". Your selection is saved and honored on every boot; the switch happens at the next restart, and the screen offers the reboot when your choice differs from what is running.

If the selected protocol is not installed, the device boots with the radio off and tells you with a notification. Nothing is substituted silently - install the protocol, or pick another one.

"None" is a real choice: the device boots with the LoRa chip parked. Everything except radio traffic works.

PER-PROTOCOL APPS
Each protocol keeps its apps in its own launcher category: its Messenger plus its Radio, Notifications and Identity settings. An app that needs a protocol you have not installed shows that in the App Library and offers the protocol download when you pick it.

The master alert switches (sound and keyboard blink) cover every protocol and stay in Settings > Alerts. The Map, Packets and the topbar unread counter work under every protocol.

BLUETOOTH IS ITS OWN SLOT
BLE protocols are chosen separately in Settings > Ble. The MeshCore phone-app link is a BLE protocol that needs the MeshCore LoRa protocol running; picking it under another protocol is refused with the reason named.

UPDATES
Protocol packages update through the App Library like apps do. The running protocol is the copy loaded at boot, so a protocol update takes effect at the next restart - the update prompt offers it.
]]

return {
    title   = "LoRa protocols",
    section = "Guide",
    order   = 78,
    body    = body,
}
