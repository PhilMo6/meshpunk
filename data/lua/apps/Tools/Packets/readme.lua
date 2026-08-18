return { body = [[
A live view of raw radio traffic - every packet the device sends, receives or fails to send, as it happens.

Each entry carries its direction, timestamp, SNR and RSSI, length, hash and the full wire frame in hex. You can export a capture as CSV for reading elsewhere.

Worth knowing: packets that fail their CRC never reach this app. They are discarded lower down, so a quiet monitor does not prove a quiet band.

The capture ring lives in memory only while the monitor is armed, so leaving the app frees it again.]] }
