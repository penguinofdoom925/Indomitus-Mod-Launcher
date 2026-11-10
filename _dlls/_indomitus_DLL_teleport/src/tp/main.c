/* GEM Teleport - startup
 * Part of the unity build: #included by gem_teleport.c, not compiled on its own.
 * ini, the finder/installer sequence (init), the instruction-registry watch thread and DllMain.
 * No dbghelp proxy: the Indomitus launcher injects the DLL by name.
 */

#ifndef FX_TEST
static int g_okCore;

/* The engine fills the instruction registry from its static initialisers (on the main thread, while this thread
   already runs), each with a plain 'node->next = head; head = node'. So: wait until the registry static is
   constructed and its head has stopped changing, prepend with a compare-exchange, and for ~2 s re-check that an
   engine insert racing ours did not drop the node. A NULL head afterwards means the registry is being destroyed. */
static DWORD WINAPI watch_thread(LPVOID arg)
{
    (void)arg;
    int waitedLogged = 0, stable = 0, verify = 0; void *lastHead = NULL;
    for (;;) {
        if (!g_instrInstalled) {
            void *h = instr_registry_built() ? *(void *volatile *)g_instrHead : NULL;
            stable = h && h == lastHead ? stable + 1 : 0; lastHead = h;
            if (stable >= 3) { install_instruction(); verify = 100; }
            else if (!waitedLogged++) logf_("{teleport}: waiting for the instruction registry to initialise");
        } else if (verify > 0) verify = check_instruction() ? verify - 1 : 0;
        if (g_instrInstalled && verify <= 0) break;
        Sleep(20);
    }
    return 0;
}

static float ini_float(const char *key, float def)
{
    char buf[64]; ini_str(key, "", buf, sizeof buf);
    if (!buf[0]) return def;
    float v = (float)atof(buf);
    return v == v ? v : def;
}

static void init(HMODULE self)
{
    g_self = self;
    GetModuleFileNameW(self, g_dir, MAX_PATH);
    wchar_t *bs = wcsrchr(g_dir, L'\\'); if (bs) *bs = 0;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(NULL, exe, MAX_PATH);
    wchar_t *en = wcsrchr(exe, L'\\'); en = en ? en + 1 : exe;
    if (_wcsnicmp(en, L"call_to_arms", 12)) return;
    if (!ini_int("enabled", 1)) return;
    g_logfile = ini_int("log", 1); g_console = ini_int("console", 0); g_verbose = ini_int("verbose", 1);
    if (g_logfile) { wchar_t lp[MAX_PATH]; wcscpy(lp, g_dir); wcscat(lp, FX_LOG); g_log = _wfopen(lp, L"w"); }
    if (g_console) { AllocConsole(); SetConsoleTitleA(FX_NAME); freopen("CONOUT$", "w", stdout); }
    if (!get_sections()) { logf_("not a PE I understand"); return; }
    logf_(FX_NAME " " FX_VERSION " loaded into %ls (timestamp %08X)", en, g_timestamp);
    g_logCpp = ini_int("log_cpp_exceptions", 0);
    if (ini_int("crash_log", 1) && AddVectoredExceptionHandler(1, fx_veh))
        logf_("crash log                      ON (fatal exceptions%s are written here)", g_logCpp ? " and C++ throws" : "");
    g_defRadius = ini_float("default_max_radius", 30.0f);
    g_defPre    = ini_float("default_pre_use_delay", 0.5f);
    g_defPost   = ini_float("default_post_use_delay", 0.5f);
    g_defSpread = ini_float("default_spread", 0.0f);
    g_defFlight   = ini_float("default_flight_time", 0.0f);
    g_defArcRatio = ini_float("default_arc_ratio", 0.2f);
    g_defArcMax   = ini_float("default_arc_max", 8.0f);
    g_defFace     = ini_float("default_face_offset", 0.0f);
    g_defBandMid  = ini_float("default_band_mid", 15.0f);
    g_defBandLong = ini_float("default_band_long", 28.0f);
    g_lift       = ini_int("lift", 1);
    g_aimEvents  = ini_int("aim_events", 1);
    ini_str("cooldown_tag", "tp_cooldown", g_cdTag, sizeof g_cdTag);         /* 0.2.10; empty = no cooldown check */
    { char h[32]; ini_str("ent_tags", "0x4E8", h, sizeof h); unsigned long v = strtoul(h, NULL, 0); if (v >= 0x100 && v < 0x4000 && !(v & 7)) g_entTagsOff = (uint32_t)v; }
    logf_("cooldown: units tagged \"%s\" are refused with {on \"teleport_cooldown\"} (entity tags +0x%X)", g_cdTag[0] ? g_cdTag : "(off)", g_entTagsOff);
    g_fireEvents = ini_int("events", 1);
    g_wantLock   = ini_int("lock_orders", 1);
    g_wantInvuln = ini_int("invulnerable", 1);
    if (!(g_defRadius > 0)) g_defRadius = 30.0f;
    if (!(g_defPre >= 0)) g_defPre = 0.5f;
    if (!(g_defPost >= 0)) g_defPost = 0.5f;
    if (!(g_defSpread >= 0) || g_defSpread > 1000.0f) g_defSpread = 0.0f;

    g_okCore = find_instr_registry() && find_read_float() && find_custom_action() && find_order_base() && find_set_pos() && find_world_scale();
    if (!g_okCore) { logf_("{teleport} instruction         OFF (engine code not recognised)"); return; }
    g_okEvents = find_fire_named();
    if (!g_okEvents) logf_("teleport events                OFF (engine code not recognised)");
    else logf_("teleport events                %s (teleport_begin / teleport_jump / teleport_end)", g_fireEvents ? "ON" : "off (ini)");
    if (g_wantLock && !find_give_order()) g_wantLock = 0;
    if (g_wantInvuln && !find_damage()) g_wantInvuln = 0;
    install_hooks();
    logf_("defaults: max_radius %g m, pre_use_delay %g s, post_use_delay %g s, spread %g m, flight_time %g s, arc %g x dist (max %g m), face_offset %g, bands <%g / <%g m",
          g_defRadius, g_defPre, g_defPost, g_defSpread, g_defFlight, g_defArcRatio, g_defArcMax, g_defFace, g_defBandMid, g_defBandLong);
    HANDLE t = CreateThread(NULL, 0, watch_thread, NULL, 0, NULL); if (t) CloseHandle(t);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        init(h);
    } else if (reason == DLL_PROCESS_DETACH) {
        if (!reserved) logf_("DLL detach (FreeLibrary)");
    }
    return TRUE;
}
#endif /* FX_TEST */
