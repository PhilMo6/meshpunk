// Included at the end of mclua.cpp (same TU — the binding bodies above are
// file-static). Registration + the RX-event drain.

// ── Registration (the firmware _mesh_* register block, package-owned now) ───
// ALL the _mesh_ names — protocol AND store-reader aliases — so released apps
// behave identically under the package. The firmware's module-boot stubs
// register first; lua_open runs after and overrides them with these.
void mc_lua_register(lua_State* L) {
  lua_register(L, "_mesh_send_public", lua_mesh_send_public);
  lua_register(L, "_mesh_send_direct", lua_mesh_send_direct);
  lua_register(L, "_mesh_get_node_info", lua_mesh_get_node_info);
  lua_register(L, "_mesh_get_contacts", lua_mesh_get_contacts);
  lua_register(L, "_mesh_drop_contacts_cache", lua_mesh_drop_contacts_cache);
  lua_register(L, "_mesh_search_contact_names", lua_mesh_search_contact_names);
  lua_register(L, "_mesh_send_advert", lua_mesh_send_advert);
  lua_register(L, "_mesh_get_num_contacts", lua_mesh_get_num_contacts);
  lua_register(L, "_mesh_set_config", lua_mesh_set_config);
  lua_register(L, "_mesh_get_channels", lua_mesh_get_channels);
  lua_register(L, "_mesh_delete_public", lua_mesh_delete_public);
  lua_register(L, "_mesh_restore_public", lua_mesh_restore_public);
  lua_register(L, "_mesh_public_deleted", lua_mesh_public_deleted);
  lua_register(L, "_mesh_set_channel", lua_mesh_set_channel);
  lua_register(L, "_mesh_send_channel", lua_mesh_send_channel);
  lua_register(L, "_mesh_remove_contact", lua_mesh_remove_contact);
  lua_register(L, "_mesh_readd_contact", lua_mesh_readd_contact);
  lua_register(L, "_mesh_archive_read", lua_mesh_archive_read);
  lua_register(L, "_mesh_archive_compact", lua_mesh_archive_compact);
  lua_register(L, "_mesh_archive_count", lua_mesh_archive_count);
  lua_register(L, "_mesh_clear_contacts", lua_mesh_clear_contacts);
  lua_register(L, "_mesh_reset_path", lua_mesh_reset_path);
  lua_register(L, "_mesh_export_contact", lua_mesh_export_contact);
  lua_register(L, "_mesh_import_contact", lua_mesh_import_contact);
  lua_register(L, "_mesh_share_contact", lua_mesh_share_contact);
  lua_register(L, "_mesh_set_contact_favorite", lua_mesh_set_contact_favorite);
  lua_register(L, "_mesh_login_room", lua_mesh_login_room);  // legacy alias of _mesh_login
  lua_register(L, "_mesh_login", lua_mesh_login_room);
  lua_register(L, "_mesh_logout", lua_mesh_logout);
  lua_register(L, "_mesh_is_connected", lua_mesh_is_connected);
  lua_register(L, "_mesh_send_command", lua_mesh_send_command);
  lua_register(L, "_mesh_send_request", lua_mesh_send_request);
  lua_register(L, "_mesh_get_rx_info", lua_mesh_get_rx_info);
  lua_register(L, "_mesh_pkt_capture", lua_mesh_pkt_capture);
  lua_register(L, "_mesh_pkt_poll", lua_mesh_pkt_poll);
  lua_register(L, "_mesh_get_rx_boost", lua_mesh_get_rx_boost);
  lua_register(L, "_mesh_set_rx_boost", lua_mesh_set_rx_boost);
  lua_register(L, "_mesh_get_autoadd", lua_mesh_get_autoadd);
  lua_register(L, "_mesh_set_autoadd", lua_mesh_set_autoadd);
  lua_register(L, "_mesh_get_autoadd_max_hops", [](lua_State *L) -> int {
    MESH_LOCK();
    int v = the_mesh ? the_mesh->_prefs.autoadd_max_hops : 0;
    MESH_UNLOCK();
    lua_pushinteger(L, v);
    return 1;
  });
  lua_register(L, "_mesh_set_autoadd_max_hops", [](lua_State *L) -> int {
    int v = (int)luaL_checkinteger(L, 1);
    if (v < 0) v = 0;
    if (v > 64) v = 64;
    MESH_LOCK();
    if (the_mesh) { the_mesh->_prefs.autoadd_max_hops = (uint8_t)v; the_mesh->savePrefs(); }
    MESH_UNLOCK();
    lua_pushboolean(L, 1);
    return 1;
  });
  lua_register(L, "_mesh_get_advert_loc", [](lua_State *L) -> int {
    MESH_LOCK();
    bool on = the_mesh ? (the_mesh->_prefs.advert_loc_policy != 0) : false;
    MESH_UNLOCK();
    lua_pushboolean(L, on);
    return 1;
  });
  lua_register(L, "_mesh_set_advert_loc", [](lua_State *L) -> int {
    bool on = lua_toboolean(L, 1);
    MESH_LOCK();
    if (the_mesh) { the_mesh->_prefs.advert_loc_policy = on ? 1 : 0; the_mesh->savePrefs(); }
    MESH_UNLOCK();
    lua_pushboolean(L, 1);
    return 1;
  });
  lua_register(L, "_mesh_get_flood_scope", [](lua_State *L) -> int {
    MESH_LOCK();
    const char* n = the_mesh ? the_mesh->getDefaultScopeName() : "";
    lua_pushstring(L, n ? n : "");
    MESH_UNLOCK();
    return 1;
  });
  lua_register(L, "_mesh_set_flood_scope", [](lua_State *L) -> int {
    const char* name = luaL_optstring(L, 1, "");
    MESH_LOCK();
    if (the_mesh) the_mesh->setDefaultScope(name);
    MESH_UNLOCK();
    lua_pushboolean(L, 1);
    return 1;
  });
  lua_register(L, "_mesh_get_channel_scope", [](lua_State *L) -> int {
    const char* chan = luaL_checkstring(L, 1);
    MESH_LOCK();
    const char* n = the_mesh ? the_mesh->getChannelScope(chan) : "";
    lua_pushstring(L, n ? n : "");
    MESH_UNLOCK();
    return 1;
  });
  lua_register(L, "_mesh_set_channel_scope", [](lua_State *L) -> int {
    const char* chan = luaL_checkstring(L, 1);
    const char* region = luaL_optstring(L, 2, "");
    MESH_LOCK();
    if (the_mesh) the_mesh->setChannelScope(chan, region);
    MESH_UNLOCK();
    lua_pushboolean(L, 1);
    return 1;
  });
  lua_register(L, "_mesh_ble_scope_active", [](lua_State *L) -> int {
    bool on = false;
    MESH_LOCK();
    if (the_mesh) {
      for (size_t i = 0; i < sizeof(the_mesh->_ble_send_scope_key); i++) {
        if (the_mesh->_ble_send_scope_key[i]) { on = true; break; }
      }
    }
    MESH_UNLOCK();
    lua_pushboolean(L, on);
    return 1;
  });
  lua_register(L, "_mesh_get_contact_paths", lua_mesh_get_contact_paths);
  lua_register(L, "_mesh_set_contact_path", lua_mesh_set_contact_path);
  lua_register(L, "_mesh_get_message_paths", lua_mesh_get_message_paths);
  lua_register(L, "_mesh_get_msg_repeat", lua_mesh_get_msg_repeat);
  lua_register(L, "_mesh_set_msg_repeat", lua_mesh_set_msg_repeat);
  lua_register(L, "_mesh_get_repeat_status", lua_mesh_get_repeat_status);
  lua_register(L, "_mesh_get_channel_messages", lua_mesh_get_channel_messages);
  lua_register(L, "_mesh_routing_query", lua_mesh_routing_query);
  lua_register(L, "_mesh_routing_senders", lua_mesh_routing_senders);
  lua_register(L, "_mesh_get_dm_messages", lua_mesh_get_dm_messages);
  lua_register(L, "_mesh_chat_page_channel", lua_mesh_chat_page_channel);
  lua_register(L, "_mesh_chat_page_dm", lua_mesh_chat_page_dm);
  lua_register(L, "_mesh_get_dm_threads", lua_mesh_get_dm_threads);
  lua_register(L, "_mesh_get_msg_summaries", lua_mesh_get_msg_summaries);
  // Overrides the firmware's _store_summaries (registered first; its
  // no-the_mesh fallback enumerates DM threads only, no channel table —
  // Messenger's inbox then sorts every channel at ts=0). The package binds
  // the same the_mesh-backed handler the builtin branch uses.
  lua_register(L, "_store_summaries", lua_mesh_get_msg_summaries);
  // Same override rationale: the firmware pair no-ops without its the_mesh.
  lua_register(L, "_notify_channel_get", lua_notify_channel_get);
  lua_register(L, "_notify_channel_set", lua_notify_channel_set);
  lua_register(L, "_mesh_set_max_messages", lua_mesh_set_max_messages);
  lua_register(L, "_mesh_unread_total", lua_mesh_unread_total);
  lua_register(L, "_mesh_unread_channel", lua_mesh_unread_channel);
  lua_register(L, "_mesh_unread_dm", lua_mesh_unread_dm);
  lua_register(L, "_mesh_unread_clear_channel", lua_mesh_unread_clear_channel);
  lua_register(L, "_mesh_unread_clear_dm", lua_mesh_unread_clear_dm);
  lua_register(L, "_mesh_export_private_key", lua_mesh_export_private_key);
  lua_register(L, "_mesh_import_private_key", lua_mesh_import_private_key);
  lua_register(L, "_mesh_generate_identity", lua_mesh_generate_identity);
  lua_register(L, "_mesh_get_path_hash_mode", [](lua_State *L) -> int {
    lua_pushinteger(L, the_mesh ? the_mesh->_prefs.path_hash_mode : 0);
    return 1;
  });
  lua_register(L, "_mesh_set_path_hash_mode", [](lua_State *L) -> int {
    int v = (int)luaL_checkinteger(L, 1);
    if (v < 0) v = 0;
    if (v > 2) v = 2;
    if (the_mesh) {
      the_mesh->_prefs.path_hash_mode = (uint8_t)v;
      the_mesh->savePrefs();
    }
    lua_pushboolean(L, 1);
    return 1;
  });
  lua_register(L, "_mesh_get_client_repeat", [](lua_State *L) -> int {
    lua_pushboolean(L, the_mesh && the_mesh->_prefs.client_repeat);
    return 1;
  });
  lua_register(L, "_mesh_set_client_repeat", [](lua_State *L) -> int {
    bool on = lua_toboolean(L, 1);
    if (!the_mesh) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, "mesh not ready");
      return 2;
    }
    the_mesh->_prefs.client_repeat = on ? 1 : 0;
    the_mesh->savePrefs();
    SLog.printf("Client repeat set to: %s\n", on ? "ON" : "OFF");
    lua_pushboolean(L, 1);
    return 1;
  });
}

// ── RX-event drain (the firmware drain_rx_events, L now a parameter) ────────
void mc_drain_rx_events(lua_State* L) {
  if (!rx_event_queue || !L) return;
  RxEvent ev;
  int budget = 8; // cap messages per tick to keep UI responsive
  while (budget-- > 0 && xQueueReceive(rx_event_queue, &ev, 0) == pdTRUE) {
    if (ev.kind == RxEvent::DIRECT_MSG) {
      lua_mesh_push_direct_message(L, ev.sender, ev.hops, ev.direct,
                                   ev.timestamp, ev.text, ev.snr, ev.rssi,
                                   ev.path_len, ev.path, ev.pkt_hash);
    } else if (ev.kind == RxEvent::CHANNEL_MSG) {
      lua_mesh_push_channel_message(L, ev.sender, ev.hops, ev.direct,
                                    ev.timestamp, ev.text, ev.snr, ev.rssi,
                                    ev.channel_idx, ev.path_len, ev.path,
                                    ev.pkt_hash);
    } else if (ev.kind == RxEvent::CONTACT_UPDATE) {
      lua_mesh_push_contact_update(L, ev.sender, ev.hops);
    } else if (ev.kind == RxEvent::ACK) {
      lua_mesh_push_ack(L, ev.ack, ev.rtt);
    } else if (ev.kind == RxEvent::ROOM_MSG) {
      lua_mesh_push_room_message(L, ev.sender, ev.origin, ev.hops, ev.direct,
                                 ev.timestamp, ev.text, ev.snr, ev.rssi,
                                 ev.path_len, ev.path, ev.pkt_hash);
    } else if (ev.kind == RxEvent::CLI_RESPONSE) {
      lua_mesh_push_cli_response(L, ev.sender, ev.text, ev.timestamp);
    } else if (ev.kind == RxEvent::LOGIN_RESULT) {
      lua_mesh_push_login_result(L, ev.sender, ev.channel_idx != 0, ev.hops, ev.ack);
    } else if (ev.kind == RxEvent::STATUS_TEXT) {
      lua_mesh_push_status_text(L, ev.sender, ev.text);
    } else if (ev.kind == RxEvent::SEND_RETRY) {
      lua_mesh_push_send_retry(L, ev.ack, ev.hops, (uint8_t)ev.channel_idx);
    } else if (ev.kind == RxEvent::CONN_LOST) {
      lua_mesh_push_conn_lost(L, ev.sender);
    }
  }
}
