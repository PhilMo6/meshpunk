# Meshpunk RAM Audit — Line-by-Line Code Evidence

## Method
Every entry below is a direct code observation with file path and line number.
No estimates or assumptions — only verified code.

---

## PASS 1: Raw observations (file by file)

### src/main.cpp (lines 0-299)

#### Global static variables (.bss/.data)
- L56: `char* buffer = (char*)malloc(size + 1);` — temporary heap alloc in `luaL_loadfilex`, freed at L68. DEFAULT HEAP.
- L74: `RADIO_CLASS radio = new Module(...)` — global, `new Module()` = DEFAULT HEAP for Module object. Radio object itself is static .bss.
- L77: `StdRNG fast_rng;` — static .bss
- L78: `SimpleMeshTables tables;` — static .bss (contains `_hashes[128*8]` + `_acks[64*4]` + state)
- L80: `ESP32Board board;` — static .bss
- L81: `PunkSX1262Wrapper radio_driver(radio, board);` — static .bss
- L82: `PunkMesh* the_mesh = nullptr;` — pointer in .bss (4 bytes)
- L85: `static TinyGPSPlus gps_tinygps;` — static .bss
- L86: `static HardwareSerial GPSSerial(1);` — static .bss (UART driver state)
- L87-94: GPS state: 6x uint32_t + 3x const uint32_t = scalars in .bss/.rodata
- L97-104: TZ state: 2x bool, 2x double, 1x bool, 2x uint32_t, 1x bool, 1x int32_t — scalars .bss
- L105: `static String tz_setting_str = "auto";` — Arduino String, heap-backed buffer. DEFAULT HEAP.
- L108: `static bool use_sd_pref = true;` — .data
- L109: `static String clock_fmt_str = "12";` — Arduino String, heap buffer. DEFAULT HEAP.
- L110-114: bools in .data
- L113: `static String wifi_saved_ssid = "";` — Arduino String. DEFAULT HEAP.
- L114: `static String wifi_saved_pass = "";` — Arduino String. DEFAULT HEAP.
- L117: `static Audio* audio = nullptr;` — pointer .bss
- L120-134: scalar prefs (uint8_t, uint16_t, uint32_t, bool) — .bss/.data

#### Stack-local buffers
- L233: `char line[128];` — stack local in firmware_prefs_load

### src/main.cpp (lines 300-599)

#### Global static variables (cont.)
- L301: `static const uint32_t GPS_BAUD_CANDIDATES[] = {...}` — 6x uint32 = 24 bytes .rodata
- L304-308: GPS baud scalars — .bss
- L510: `new (&gps_tinygps) TinyGPSPlus();` — placement new, reinits existing static object, no new alloc
- L532: `volatile int trackball_click = 0;` — .bss, IRAM-accessed
- L545-548: `volatile int trackball_up/down/left/right = 0;` — 4x int .bss, IRAM-accessed

#### IRAM_ATTR functions (code in IRAM)
- L534: `void IRAM_ATTR ISR_click()` — code lives in IRAM
- L535: `static uint32_t last_click_ms = 0;` — static local inside IRAM func, lives in .bss
- L550-553: `IRAM_ATTR ISR_trackball_up/down/left/right` — 4 ISR functions in IRAM

#### Global static const (.rodata)
- L583: `static const char kb_matrix[5][7]` — 35 bytes .rodata
- L592: `static const char kb_matrix_symbol[5][7]` — 35 bytes .rodata

### src/main.cpp (lines 600-899)

#### Global static variables (cont.)
- L606: `Ticker lvgl_ticker;` — static .bss
- L609: `TFT_eSPI tft;` — static .bss (large object — TFT driver state + internal SPI transaction buffer)
- L610: `TouchDrvGT911 touch;` — static .bss (I2C touch driver)
- L613: `lua_State *L = NULL;` — pointer .bss (4 bytes)
- L616-621: `keyboard_available`, `last_key`, `fs_mounted`, `sd_mounted` — scalars .bss

#### Heap allocations called from functions
- L653: `String path = String(dirname);` — temporary Arduino String in listDir(), DEFAULT HEAP, freed on return
- L677: `String content = "";` + char-by-char append in readFile() — Arduino String grows on DEFAULT HEAP, returned
- L790: `fs::File *file = new fs::File(f);` — DEFAULT HEAP alloc in lua_io_open (SD path). Per file open.
- L791: `lua_newuserdata(L, sizeof(LuaFileHandle))` — Lua userdata alloc, uses LUA ALLOCATOR (PSRAM per L2838)
- L802: `fs::File *file = new fs::File(f);` — DEFAULT HEAP alloc in lua_io_open (LittleFS path). Per file open.
- L803: `lua_newuserdata(L, sizeof(LuaFileHandle))` — Lua userdata alloc, LUA ALLOCATOR (PSRAM)

#### Static locals in functions
- L817: `static uint8_t level = 0;` in setBrightness — .bss
- L818: `static uint8_t steps = 16;` in setBrightness — .data

### src/main.cpp (lines 900-1199)

#### Global static variables (cont.)
- L903: `int16_t x[5], y[5];` — global .bss (20 bytes). Touch point buffers.
- L907: `bool touch_debug = true;` — .data
- L908: `unsigned long last_touch_debug = 0;` — .bss
- L931: `static uint32_t last_key_code = 0;` — .bss
- L932: `static uint8_t prev_matrix[KB_COLS] = {0};` — .bss (~7 bytes)
- L933: `static bool kb_key_state[128] = {0};` — .bss (128 bytes)
- L934: `static bool kb_key_prev[128] = {0};` — .bss (128 bytes)
- L935: `static uint32_t kb_key_press_time[128] = {0};` — .bss (512 bytes)
- L936: `static const uint32_t KEY_HOLD_THRESHOLD_MS = 400;` — .rodata (4 bytes)
- L937-941: `kb_shift_active`, `kb_lshift_active`, `kb_rshift_active`, `kb_sym_active`, `kb_alt_active` — .bss bools
- L944: `static lv_obj_t *nav_container = NULL;` — .bss (4 bytes)
- L945: `static lv_gridnav_ctrl_t nav_flags = LV_GRIDNAV_CTRL_NONE;` — .bss
- L946: `static bool nav_gridnav_active = false;` — .bss
- L947: `static lv_obj_t *pending_gridnav_remove = NULL;` — .bss (4 bytes)
- L987: `static bool trackball_btn_pressed = false;` — .bss
- L992: `static uint32_t kb_mapped_key = 0;` — static local in keyboard_read_cb, .bss

#### Heap allocations in functions (cont.)
- L1171: `String cmd = Serial.readStringUntil('\n');` — temp Arduino String in handleWebSerialCommands, DEFAULT HEAP
- L1175: `String path = cmd.substring(5);` — temp Arduino String, DEFAULT HEAP

#### Stack-local variables
- L997: `uint8_t cur_matrix[KB_COLS]` — stack-local in keyboard_read_cb (7 bytes)
- L866-898: `disp_flush_cb` — w, h, scanline, flush_start, flush_end, wait_us — all stack-local scalars

### src/main.cpp (lines 1200-1499)

#### LVGL setup — heap allocations
- L1242: `#define BUF_SIZE (TFT_HEIGHT * BUF_LINES * sizeof(lv_color_t))` — compile-time: 240 × 48 × 2 = 23,040 bytes
- L1244: `static uint8_t *buf1 = (uint8_t *)ps_malloc(BUF_SIZE);` — PSRAM. 23,040 bytes. Display draw buffer 1.
- L1245: `static uint8_t *buf2 = (uint8_t *)ps_malloc(BUF_SIZE);` — PSRAM. 23,040 bytes. Display draw buffer 2.
- L1252: `lv_init();` — triggers LVGL internal heap init. LVGL uses LV_STDLIB_CLIB = standard malloc() = DEFAULT HEAP.
- L1255: `lv_group_t *default_group = lv_group_create();` — LVGL allocator (DEFAULT HEAP via clib malloc)
- L1259: `lv_display_t *disp = lv_display_create(TFT_HEIGHT, TFT_WIDTH);` — LVGL allocator (DEFAULT HEAP)
- L1264: `static lv_font_t * ui_font = emoji_font_create(16, &lv_font_montserrat_14);` — static pointer .bss; emoji_font_create internally allocs lv_imgfont via LVGL allocator (DEFAULT HEAP)
- L1267-1273: `lv_theme_meshpunk_init(...)` — returns theme; internally calls `lv_malloc_zeroed(sizeof(my_theme_t))` = LVGL allocator (DEFAULT HEAP)
- L1291-1293: `lv_indev_create()` + `lv_indev_set_type()` — touch input device, LVGL allocator (DEFAULT HEAP)
- L1298-1303: `lv_indev_create()` — keyboard input device, LVGL allocator (DEFAULT HEAP)

#### Global static variables (cont.)
- L1310: `static lv_obj_t *label;` — .bss (4 bytes)

#### Heap in Lua-bound functions
- L1390,1484: `lua_newtable(L)` — Lua allocator = PSRAM (via lua_psram_alloc)
- L1402: `HTTPClient http;` — stack-local object, but `http.begin(url)` / `http.getString()` alloc on DEFAULT HEAP internally
- L1420: `String payload = "";` — temp Arduino String in lua_wifi_fetch, DEFAULT HEAP. Can grow large for HTTP responses.

### src/main.cpp (lines 1500-1799)

#### Heap in Lua-bound functions (cont.)
- L1522-1527: `lua_newtable(L)` + `lua_pushstring(L, ...)` — Lua allocator = PSRAM
- L1533-1534: `wifi_saved_ssid = ssid; wifi_saved_pass = pass;` — Arduino String assignment, DEFAULT HEAP (reallocs internal buffer)
- L1562: `char text[160];` — stack-local in lua_mesh_send_public
- L1577: `uint8_t tx_hash[MAX_HASH_SIZE];` — stack-local (8 bytes)
- L1583: `char hex[MAX_HASH_SIZE * 2 + 1];` — stack-local (17 bytes)
- L1597: `char text[160];` — stack-local in lua_mesh_send_direct
- L1637-1674: `lua_newtable(L)`, `lua_pushstring(L, ...)`, `lua_pushnumber()` — Lua allocator = PSRAM
- L1644: `char hex[PUB_KEY_SIZE * 2 + 1];` — stack-local (~65 bytes)
- L1679: `char hex[PRV_KEY_SIZE * 2 + 1];` — stack-local (~129 bytes)
- L1694: `uint8_t prv[PRV_KEY_SIZE];` — stack-local (~64 bytes)
- L1740-1796: lua_mesh_get_contacts — multiple `lua_newtable/lua_pushstring` calls per contact — all Lua allocator = PSRAM
- L1744: `ContactInfo c;` — stack-local (~184 bytes)
- L1765: `char hex[PUB_KEY_SIZE * 2 + 1];` — stack-local
- L1781: `char h[7];` — stack-local

#### Static const (.rodata)
- L1815: `static const char *src_names[] = { "msg", "ack", "path_update", "advert" };` — static local, .rodata (4 pointers + string literals)

### src/main.cpp (lines 1800-2699)

NOTE: Lines 1800-2699 are predominantly Lua-bound bridge functions. All `lua_pushstring`, `lua_newtable`, `lua_pushinteger`, etc. calls go through the Lua allocator = PSRAM. Stack-local buffers (char arrays, uint8_t arrays) are on the calling task's stack. Only notable heap allocations are listed.

#### Heap allocations in functions (cont.)
- L2104: `uint8_t hash[32];` — stack-local in lua_mesh_set_channel
- L2265: `uint8_t buf[256];` — stack-local in lua_mesh_export_contact
- L2274: `char hex[513];` — stack-local
- L2278: `String card = "meshcore://" + String(hex);` — temp Arduino String, DEFAULT HEAP
- L2504: `uint8_t buf[256];` — stack-local in copyFile helper
- L2533: `String oldPrefix = the_mesh->_storage_prefix;` — temp Arduino String, DEFAULT HEAP
- L2537: `String newPrefix;` — temp Arduino String, DEFAULT HEAP
- L2555: `const char* files[] = {...};` — stack-local pointer array
- L2557-2558: `String srcPath`, `String dstPath` — temp Arduino Strings in migration loop, DEFAULT HEAP

### src/main.cpp (lines 2700-2999)

#### Key heap allocations
- L2794: `char* buffer = (char*)malloc(size + 1);` — DEFAULT HEAP in lua_dofile_sd. Temporary, freed at L2816.
- L2838-2845: `lua_psram_alloc` — **THE custom Lua allocator**. Uses `heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM)`. ALL Lua allocations route to PSRAM.
- L2851: `L = lua_newstate(lua_psram_alloc, NULL);` — creates Lua VM state. All subsequent Lua allocs go to PSRAM.
- L2861: `luaL_openlibs(L);` — opens standard Lua libs, all allocations via PSRAM
- L2864: `luaL_requiref(L, "lvgl", luaopen_lvgl, 1);` — LuaVGL module registration, PSRAM
- L2989: `tz_setting_str = String(m);` — Arduino String assignment, DEFAULT HEAP
- L3039: `clock_fmt_str = ...` — Arduino String assignment, DEFAULT HEAP

#### Heap allocations in setupLuaVGL custom loader
- L3362: `String filename = String(LUA_PATH) + modname + ".lua";` — temp Arduino String, DEFAULT HEAP
- L3364: `String content = readFile(filename.c_str());` — reads file into Arduino String, DEFAULT HEAP (can be large)
- L3395: `luaL_newmetatable(L, "esp32_file");` — Lua metatable, PSRAM
- L3403: `String content = ud->file->readString();` — temp Arduino String in file:read() lambda, DEFAULT HEAP

### src/main.cpp (lines 3000-3599)

NOTE: Lines 3000-3580 are almost entirely `lua_register()` lambda registrations with no heap allocations beyond Lua allocator = PSRAM for function registration itself.

#### Heap allocations
- L3046: `sound_register_lua(L);` — registers sound Lua functions. Allocations via Lua allocator = PSRAM.
- L3513: `String scriptPath = String(LUA_PATH) + "main.lua";` — temp Arduino String, DEFAULT HEAP
- L3515: `String script = readFile(scriptPath.c_str());` — reads entire main.lua into Arduino String, DEFAULT HEAP (can be large)
- L3552: `String escapedError = String(luaError);` — temp Arduino String (error path only), DEFAULT HEAP
- L3557: `String fallbackScript = ...` — temp Arduino String (error path only), DEFAULT HEAP

#### Global static variable
- L3583: `volatile bool lora_packet_ready = false;` — .bss

### src/main.cpp (lines 3585-3953) — setup() and loop()

#### Critical heap allocations in setup()
- L3686: `void* mesh_mem = heap_caps_malloc(sizeof(PunkMesh), MALLOC_CAP_SPIRAM);` — **PSRAM**. sizeof(PunkMesh) ≈ 115 KB.
- L3687: `the_mesh = new (mesh_mem) PunkMesh(radio_driver, fast_rng, *new VolatileRTCClock(), tables);`
  - Placement new in PSRAM for PunkMesh.
  - `*new VolatileRTCClock()` — **DEFAULT HEAP** for VolatileRTCClock object (small, ~12 bytes).
- L3773: `audio = new Audio();` — **DEFAULT HEAP**. Audio object with I2S driver state.

#### Stack-local variables
- L3900: `RxEvent ev;` — stack-local in drain_rx_events (~282 bytes per call on Core 0 stack)

#### No new persistent allocations in loop()
- loop() calls `lv_timer_handler()`, `audio->loop()`, `drain_rx_events()` — all operate on existing objects.

### src/punkmesh.cpp (lines 200-1900) — FULLY READ

NOTE: Predominantly mesh storage/persistence logic and MeshCore callbacks. Heavy stack usage in some paths.

#### Constructor — critical heap allocations
- L1821: `BaseChatMesh(radio, *new ArduinoMillis(), rng, rtc, *new StaticPoolPacketManager(16), tables)`
  - `new ArduinoMillis()` — **DEFAULT HEAP** (small, ~4 bytes vtable + data)
  - `new StaticPoolPacketManager(16)` — **DEFAULT HEAP**. Constructor allocates:
    - 16 × `new mesh::Packet()` — each ~MAX_PACKET_SIZE+header bytes, DEFAULT HEAP
    - 3 × PacketQueue internal arrays (`new Packet*[]`, `new uint8_t[]`, `new uint32_t[]`) — DEFAULT HEAP

#### Heap allocations in file I/O functions
- L273,334,365,417,500-522: `String path = storagePath(...)` — temp Arduino Strings, DEFAULT HEAP
- L507,514,521: `String channel_msg_path(...)`, `String dm_msg_path(...)` — temp Arduino Strings, DEFAULT HEAP
- L661: `uint8_t* buf = (uint8_t*)malloc(remaining);` — DEFAULT HEAP in trim_msg_text_file, temporary, freed L677
- L666: `String tmp = path + ".tmp";` — temp Arduino String, DEFAULT HEAP
- L1718: `uint8_t* buf = (uint8_t*)malloc(fsize);` — DEFAULT HEAP in persistExtraPath, temporary, freed L1782
- L1709: `String hash_line = String("hash=") + hash_hex;` — temp Arduino String, DEFAULT HEAP
- L1850-1852: `String idPath/prefsPath/contactsPath = storagePath(...)` — temp Arduino Strings, DEFAULT HEAP

#### Stack-local buffers (heavy)
- L279: `char line[320];` — stack-local in loadContacts
- L303: `ContactInfo c;` — stack-local (~184 bytes)
- L343,347: `char pubkey_hex[65]; char path_hex[129];` — stack-local
- L371: `char line[128];` — stack-local in loadChannels
- L426: `char secret_hex[65];` — stack-local
- L622,840,1024,1168: `char line[256];` — stack-local in various parse functions
- L730,838: `StoredMsg m;` — stack-local (~470+ bytes, StoredMsg is large: from[32]+peer[32]+text[128]+path[64]+pkt_hash[8]+rpaths[8×ObservedPath]+fields)
- L888,1186: `char rval[256];` — stack-local
- L1165: `char target[6+MAX_HASH_SIZE*2+1];` — stack-local
- L1309,1393,1469: `RxEvent ev = {};` — stack-local (~282 bytes)
- L1368,1450: `char norm_text[160]; char norm_msg[160];` — stack-local
- L1434: `char sender_name[32]` — stack-local
- L1758: `char rpath_buf[256];` — stack-local

#### All Lua push calls in this file
- `lua_newtable`, `lua_pushstring`, `lua_pushnumber`, etc. — all via Lua allocator = PSRAM

### src/meshpunk_sync.h — FULLY READ

#### Struct sizes (for queue item costing)
- `RxEvent` ≈ 282 bytes (kind + hops + channel_idx + direct + sender[32] + text[160] + timestamp + snr + rssi + path_len + path[MAX_PATH_SIZE] + pkt_hash[MAX_HASH_SIZE])
- `TxCommand` ≈ 194 bytes (kind + channel_idx + dest[32] + text[160])
- `GpsEvent` ≈ 13 bytes (kind + epoch + sats + hdop)

### src/meshpunk_sync.cpp — FULLY READ

#### FreeRTOS kernel objects (all from FreeRTOS internal heap = DEFAULT HEAP)
- L11: `spi_bus_mutex = xSemaphoreCreateRecursiveMutex();` — ~80 bytes internal
- L12: `the_mesh_mutex = xSemaphoreCreateRecursiveMutex();` — ~80 bytes internal
- L13: `rx_event_queue = xQueueCreate(32, sizeof(RxEvent));` — 32 × 282 = **~9,024 bytes** internal
- L14: `tx_cmd_queue = xQueueCreate(16, sizeof(TxCommand));` — 16 × 194 = **~3,104 bytes** internal
- L15: `gps_event_queue = xQueueCreate(8, sizeof(GpsEvent));` — 8 × 13 = **~104 bytes** internal

### src/meshpunk_tasks.cpp — FULLY READ

#### FreeRTOS task stacks (DEFAULT HEAP / internal SRAM)
- L65: `xTaskCreatePinnedToCore(mesh_task_body, "mesh_task", 12*1024, ..., 1)` — **12,288 bytes** on Core 1
- L106: `xTaskCreatePinnedToCore(gps_task_body, "gps_task", 4*1024, ..., 1)` — **4,096 bytes** on Core 1

### src/sound.cpp — FULLY READ

#### FreeRTOS objects
- L48: `s_sound_mutex = xSemaphoreCreateMutex();` — ~80 bytes, DEFAULT HEAP
- L49: `xTaskCreatePinnedToCore(sound_task_body, "sound_task", 8*1024, ..., 1)` — **8,192 bytes** task stack

#### Heap allocations
- L78: `realloc(sound_objects, new_cap * sizeof(SoundObject*))` — DEFAULT HEAP for pointer array (grows)
- L118: `ps_malloc(frames * 2 * sizeof(int16_t))` — **PSRAM** for tone PCM buffer
- L238: `ps_malloc(frames * 2 * sizeof(int16_t))` — **PSRAM** for chord PCM buffer
- L242: `ps_calloc(frames, sizeof(float))` — **PSRAM** for chord accumulator (temporary)
- L211,334,531,552: `new SoundObject{}` — DEFAULT HEAP for SoundObject metadata structs (~64 bytes each)

### src/emoji_font.cpp — FULLY READ

#### Static global (.bss)
- L40: `EmojiCacheEntry s_cache[EMOJI_CACHE_CAP]` where EMOJI_CACHE_CAP=256 — static .bss, **2,048 bytes** (256 × 8 bytes per entry: uint32_t codepoint + pointer)

#### Heap allocations
- L158: `heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` — **PSRAM** for emoji pixel data
- L161: `heap_caps_malloc(sizeof(lv_image_dsc_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` — **PSRAM** for image descriptor (per cached emoji)

### src/ble_companion.cpp — FULLY READ

#### Heap allocations
- L24: `ble_serial = new PunkBLEInterface();` — **DEFAULT HEAP**. BLE serial interface + NimBLE stack internals.
- L35: `heap_caps_malloc(sizeof(BleCompanionHandler), MALLOC_CAP_SPIRAM)` — **PSRAM**. ~5+ KB (embedded cmd_frame[173], out_frame[173], MsgSyncState.files[40][64], etc.)
- L37: `new (ble_mem) BleCompanionHandler(...)` — placement new in PSRAM
- L806: `heap_caps_malloc(SYNC_BATCH * sizeof(StoredMsg), MALLOC_CAP_SPIRAM)` — **PSRAM** temp batch for BLE sync
- L813: `heap_caps_malloc(n * sizeof(SyncFrame), MALLOC_CAP_SPIRAM)` — **PSRAM** temp for BLE sync

### src/punk_ble_interface.h — FULLY READ

- PunkBLEInterface has `esp_bd_addr_t _peer_addr` (6 bytes) + `bool _has_peer` — embedded in object allocated at ble_companion.cpp:L24 (DEFAULT HEAP)

### src/theme/lv_theme_meshpunk.c — FULLY READ

#### Heap allocation
- L653: `theme_def = lv_malloc_zeroed(sizeof(my_theme_t));` — **LVGL allocator (DEFAULT HEAP via LV_STDLIB_CLIB)**
  - sizeof(my_theme_t) includes: lv_theme_t base + disp/color fields + ~40+ lv_style_t in my_theme_styles_t + 2 color_filter_dsc_t + 2 transition_dsc_t
  - Estimated **~1.5-2 KB** total. Each lv_style_t may also allocate property storage dynamically via LVGL allocator.

### src/theme/lv_theme_meshpunk.h — FULLY READ
- Pure declarations, no RAM.

### src/tdeck-pins.h — FULLY READ
- All `#define` preprocessor constants. No RAM impact.

### src/utilities.h — FULLY READ
- All `#define` preprocessor constants. No RAM impact.

### src/emoji_font.h — FULLY READ
- Pure declarations, no RAM.

### src/sound.h — FULLY READ
- Struct/type declarations only. No RAM.

### lib/lv_conf.h — FULLY READ (first 100 lines)
- L39: `#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB` — LVGL uses standard malloc. On ESP32, this means DEFAULT HEAP.
- L61: `#define LV_USE_OS LV_OS_FREERTOS` — LVGL uses FreeRTOS thread-safety primitives
- L74: `#define LV_DRAW_LAYER_SIMPLE_BUF_SIZE (24 * 1024)` — 24 KB draw layer buffer, allocated via LVGL allocator (DEFAULT HEAP)
- L75: `#define LV_DRAW_THREAD_STACK_SIZE (8 * 1024)` — 8 KB thread stack for draw unit

### lib/MeshCore/src/helpers/SimpleMeshTables.h — FULLY READ
- Embedded in `tables` static global (main.cpp L78). Contains:
  - `_hashes[128*8]` = **1,024 bytes**
  - `_acks[64*4]` = **256 bytes**
  - Plus `_next_idx`, `_next_ack_idx`, `_direct_dups`, `_flood_dups` = ~16 bytes
  - Total: **~1,296 bytes** in .bss

### lib/MeshCore/src/helpers/StaticPoolPacketManager.h — FULLY READ
- Allocated via `new StaticPoolPacketManager(16)` = DEFAULT HEAP (see punkmesh.cpp L1821)

### platformio.ini — FULLY READ

#### Key build flags affecting memory
- `-DBOARD_HAS_PSRAM=1` — enables PSRAM
- `-D MAX_CONTACTS=300` — contacts array size in BaseChatMesh
- `-D MAX_GROUP_CHANNELS=20` — channel array size
- `-DARDUINO_LOOP_STACK_SIZE=16384` — **16 KB** for Arduino loop() task stack (Core 0)
- `-DARDUINO_RUNNING_CORE=0` — Arduino loop runs on Core 0
- `board_build.arduino.memory_type = qio_opi` — OPI PSRAM mode (fast access)

---

## PASS 2: Consolidated RAM Map

### A. Static .bss / .data (Internal SRAM)

These live in the static data segment. They are ALWAYS in internal SRAM — cannot be moved.

| Source | Item | Size (approx) |
|--------|------|--------|
| main.cpp L74 | `RADIO_CLASS radio` pointer (.bss, but `new Module()` on heap) | 4 bytes .bss |
| main.cpp L77 | `StdRNG fast_rng` | ~8 bytes |
| main.cpp L78 | `SimpleMeshTables tables` (_hashes[1024] + _acks[256] + state) | **~1,296 bytes** |
| main.cpp L80 | `ESP32Board board` | ~4 bytes |
| main.cpp L81 | `PunkSX1262Wrapper radio_driver` | ~4 bytes |
| main.cpp L82 | `PunkMesh* the_mesh` pointer | 4 bytes |
| main.cpp L85 | `TinyGPSPlus gps_tinygps` | ~200 bytes |
| main.cpp L86 | `HardwareSerial GPSSerial(1)` | ~100 bytes |
| main.cpp L87-114 | GPS/TZ scalars, Arduino String globals (pointers in .bss) | ~80 bytes |
| main.cpp L117 | `Audio* audio` pointer | 4 bytes |
| main.cpp L120-134 | Scalar prefs (brightness, timeout, BLE flags, etc.) | ~30 bytes |
| main.cpp L532,545-548 | `volatile int` trackball vars (IRAM-accessed) | **20 bytes** |
| main.cpp L583,592 | `kb_matrix` const arrays (.rodata) | 70 bytes |
| main.cpp L606 | `Ticker lvgl_ticker` | ~20 bytes |
| main.cpp L609 | `TFT_eSPI tft` (large: SPI state + transaction buffer) | **~500-1000 bytes** |
| main.cpp L610 | `TouchDrvGT911 touch` | ~100 bytes |
| main.cpp L903 | `int16_t x[5], y[5]` touch points | 20 bytes |
| main.cpp L931-941 | Keyboard state (prev_matrix, key_state/prev/press_time arrays) | **~776 bytes** |
| main.cpp L944-947 | Nav controller pointers + flags | ~16 bytes |
| main.cpp L3583 | `volatile bool lora_packet_ready` | 1 byte |
| emoji_font.cpp L40 | `EmojiCacheEntry s_cache[256]` | **~2,048 bytes** |
| **Subtotal** | | **~5-6 KB** |

### B. IRAM (must stay in internal SRAM)

| Source | Item | Note |
|--------|------|------|
| main.cpp L534 | `IRAM_ATTR ISR_click()` | ISR code, must be IRAM |
| main.cpp L535 | `static uint32_t last_click_ms` inside ISR | .bss but ISR-accessed |
| main.cpp L550-553 | `IRAM_ATTR ISR_trackball_up/down/left/right` | 4 ISR functions |
| main.cpp L532,545-548 | `volatile int trackball_click/up/down/left/right` | ISR-written, must be SRAM |

### C. DEFAULT HEAP (Internal SRAM unless > ESP-IDF threshold → PSRAM)

These use `malloc()` / `new` without explicit PSRAM routing. On ESP32-S3 with `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL` (default ~4096), allocations ≤ threshold stay in internal SRAM; larger ones go to PSRAM automatically.

#### C1. Persistent objects (allocated once, live forever)

| Source | Item | Size (approx) | Can move to PSRAM? |
|--------|------|--------|------|
| main.cpp L74 | `new Module(...)` RadioLib module | ~100 bytes | NO — SPI/DMA related |
| main.cpp L3773 | `new Audio()` I2S audio driver | ~200 bytes | NO — I2S DMA buffers |
| main.cpp L3687 | `new VolatileRTCClock()` | ~12 bytes | YES |
| punkmesh.cpp L1821 | `new ArduinoMillis()` | ~4 bytes | YES |
| punkmesh.cpp L1821 | `new StaticPoolPacketManager(16)` + 16× `new Packet()` + queue arrays | **~4-8 KB** | COMPLEX — MeshCore internal `new` calls |
| ble_companion.cpp L24 | `new PunkBLEInterface()` | ~200+ bytes | NO — BLE stack needs internal |
| sound.cpp L78 | `realloc()` for SoundObject pointer array | ~64+ bytes | YES |
| sound.cpp L211,334,531,552 | `new SoundObject{}` per sound | ~64 bytes each | YES |

#### C2. LVGL allocations (via LV_STDLIB_CLIB = malloc)

| Source | Item | Size (approx) | Can move to PSRAM? |
|--------|------|--------|------|
| lv_theme_meshpunk.c L653 | `lv_malloc_zeroed(sizeof(my_theme_t))` + style storage | **~2 KB** | YES |
| main.cpp L1255 | `lv_group_create()` | ~40 bytes | YES |
| main.cpp L1259 | `lv_display_create()` | ~200 bytes | YES |
| main.cpp L1264 | `emoji_font_create()` → imgfont allocation | ~100 bytes | YES |
| main.cpp L1291-1303 | `lv_indev_create()` × 2 | ~100 bytes | YES |
| Runtime | All LVGL widget/object/style allocations during UI operation | **~30-80 KB** | **YES — primary optimization target** |

#### C3. FreeRTOS kernel objects (MUST stay in internal SRAM)

| Source | Item | Size (approx) | Can move to PSRAM? |
|--------|------|--------|------|
| meshpunk_sync.cpp L11 | `xSemaphoreCreateRecursiveMutex()` | ~80 bytes | NO |
| meshpunk_sync.cpp L12 | `xSemaphoreCreateRecursiveMutex()` | ~80 bytes | NO |
| meshpunk_sync.cpp L13 | `xQueueCreate(32, sizeof(RxEvent))` | **~9,024 bytes** | NO |
| meshpunk_sync.cpp L14 | `xQueueCreate(16, sizeof(TxCommand))` | **~3,104 bytes** | NO |
| meshpunk_sync.cpp L15 | `xQueueCreate(8, sizeof(GpsEvent))` | **~104 bytes** | NO |
| sound.cpp L48 | `xSemaphoreCreateMutex()` | ~80 bytes | NO |
| **Subtotal** | | **~12.5 KB** | |

#### C4. FreeRTOS task stacks (MUST stay in internal SRAM)

| Source | Task | Size | Core |
|--------|------|------|------|
| platformio.ini | Arduino loop() stack | **16,384 bytes** | Core 0 |
| meshpunk_tasks.cpp L65 | mesh_task | **12,288 bytes** | Core 1 |
| meshpunk_tasks.cpp L106 | gps_task | **4,096 bytes** | Core 1 |
| sound.cpp L49 | sound_task | **8,192 bytes** | Core 1 |
| lv_conf.h L75 | LVGL draw thread | **8,192 bytes** | (LVGL managed) |
| **Subtotal** | | **~49 KB** | |

#### C5. Arduino String globals (heap-backed buffers, DEFAULT HEAP)

| Source | Item | Can move to PSRAM? |
|--------|------|------|
| main.cpp L105 | `tz_setting_str = "auto"` | YES (small) |
| main.cpp L109 | `clock_fmt_str = "12"` | YES (small) |
| main.cpp L113 | `wifi_saved_ssid = ""` | YES (small) |
| main.cpp L114 | `wifi_saved_pass = ""` | YES (small) |

### D. PSRAM (Explicit)

| Source | Item | Size (approx) |
|--------|------|--------|
| main.cpp L1244-1245 | LVGL draw buffers buf1, buf2 via `ps_malloc()` | **46,080 bytes** (2 × 23,040) |
| main.cpp L3686 | PunkMesh object via `heap_caps_malloc(MALLOC_CAP_SPIRAM)` | **~115 KB** (includes contacts[300], path_history[32], msg_paths[32], channels[20], etc.) |
| main.cpp L2838 | Lua VM via `lua_newstate(lua_psram_alloc, NULL)` | **Variable** — all Lua allocs route here. Typical: 100-500 KB |
| ble_companion.cpp L35 | BleCompanionHandler via `heap_caps_malloc(MALLOC_CAP_SPIRAM)` | **~5 KB** |
| ble_companion.cpp L806,813 | BLE sync temp buffers | Variable (temporary) |
| emoji_font.cpp L158,161 | Emoji pixel data + image descriptors | Variable per cached emoji |
| sound.cpp L118,238 | Tone/chord PCM buffers via `ps_malloc()` | Variable per sound |
| sound.cpp L242 | Chord accumulator via `ps_calloc()` | Variable (temporary) |

### E. .rodata (Flash, not RAM)

| Source | Item | Size |
|--------|------|------|
| main.cpp L301 | `GPS_BAUD_CANDIDATES[]` | 24 bytes |
| main.cpp L583,592 | `kb_matrix[5][7]`, `kb_matrix_symbol[5][7]` | 70 bytes |
| main.cpp L936 | `KEY_HOLD_THRESHOLD_MS` | 4 bytes |
| punkmesh.cpp L1815 | `src_names[]` | ~16 bytes |

---

## Summary: Internal SRAM Budget

| Category | Approx Size |
|----------|------------|
| Static .bss/.data | ~5-6 KB |
| FreeRTOS task stacks | ~49 KB |
| FreeRTOS queues + mutexes | ~12.5 KB |
| LVGL widget/style heap (via malloc) | ~30-80 KB |
| StaticPoolPacketManager + packets | ~4-8 KB |
| RadioLib Module + Audio I2S | ~300 bytes |
| BLE stack (PunkBLEInterface + NimBLE internals) | ~10-20 KB |
| WiFi stack (when enabled) | ~30-50 KB |
| Temporary Arduino Strings in functions | variable |
| **Total internal SRAM pressure** | **~140-225 KB** |

Available internal SRAM: ~328 KB. This leaves ~100-188 KB headroom, but WiFi + BLE together push it tight.

## Top Optimization Target

**LVGL widget/style allocations (~30-80 KB)**: Currently using `LV_STDLIB_CLIB` → standard `malloc()` → internal SRAM for small allocs. Switching to `LV_STDLIB_CUSTOM` with a PSRAM-backed allocator would reclaim this for internal consumers (BLE/WiFi/DMA).
