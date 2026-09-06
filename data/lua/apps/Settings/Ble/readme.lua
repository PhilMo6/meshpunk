return { body = [[
Everything Bluetooth, in one place.

BLE is its own protocol slot, separate from the LoRa protocol in Settings > Lora. They are different radios, so they run at the same time - what you pick here does not affect your mesh, and the mesh does not affect this.

- The protocol list selects what BLE does. Choices apply immediately, no reboot. none switches BLE off entirely and frees its memory.
- MeshCore app link is the phone companion: it carries your MeshCore mesh to the app. It needs the MeshCore LoRa protocol running, so selecting it under another protocol is refused and the reason appears in the status line. Your choice is remembered - it starts working again the next time MeshCore is the running protocol.
- The status line under the app link section says whether BLE is off, another protocol is selected, the link is waiting for the app, or a phone is connected.
- Bond Clear ON makes the phone re-enter the pairing PIN on every reconnect. Leave it OFF unless you want that.
- Sync limit is how many of the newest messages per chat are sent to the app on a fresh connection; 0 sends everything. Lower it if the first sync after a busy stretch takes too long.

WiFi settings live in Settings > Wifi.]] }
