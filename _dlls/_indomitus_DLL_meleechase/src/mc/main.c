/* GEM MeleeChase - startup
 * Part of the unity build: #included by gem_meleechase.c, not compiled on its own.
 * ini, the finder/installer sequence (init) and DllMain. The Indomitus launcher injects the DLL by name.
 */

#ifndef FX_TEST
static float ini_float(const char *key, float def, float lo, float hi)
{
    char d[32], v[64]; snprintf(d, sizeof d, "%g", def);
    ini_str(key, d, v, sizeof v);
    char *end; float f = strtof(v, &end);
    if (end == v || f != f) f = def;
    return f < lo ? lo : f > hi ? hi : f;
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
    if (ini_int("crash_log", 1)) {
        if (AddVectoredExceptionHandler(1, fx_veh)) logf_("crash log                      ON (fatal exceptions are written here)");
        install_exit_hooks();
    }

    ini_str("melee_tag", "melee", g_meleeTag, sizeof g_meleeTag);
    g_meleeTag[sizeof g_meleeTag - 1] = 0;
    g_meleeTagLen = strlen(g_meleeTag);
    if (!g_meleeTagLen) { logf_("melee_tag is empty - nothing to do"); return; }
    ini_str("vehicle_melee_tag", "veh_melee", g_meleeTag2, sizeof g_meleeTag2);   /* 0.5.1; empty = off */
    g_meleeTag2[sizeof g_meleeTag2 - 1] = 0;
    g_meleeTag2Len = strlen(g_meleeTag2);
    int wantVehicle = ini_int("vehicle_chase", 1);       /* 0.5.0: human_fight_distance / fight_distance_mult are gone (ignored if still in an ini) */
    g_repathMin = ini_float("repath_min_m", 1.0f, 0.2f, 100.0f) * WORLD_SCALE;
    g_repathMax = ini_float("repath_max_m", 10.0f, 0.2f, 500.0f) * WORLD_SCALE;
    if (g_repathMax < g_repathMin) g_repathMax = g_repathMin;
    g_repathFrac = ini_float("repath_fraction", 0.25f, 0.0f, 1.0f);
    g_repathMs = (uint32_t)ini_int("repath_interval_ms", 500);
    if (g_repathMs < 100) g_repathMs = 100; if (g_repathMs > 10000) g_repathMs = 10000;
    g_repathMsClose = (uint32_t)ini_int("repath_interval_close_ms", 200);
    if (g_repathMsClose < 50) g_repathMsClose = 50; if (g_repathMsClose > 10000) g_repathMsClose = 10000;
    g_closeRange = ini_float("close_range_m", 10.0f, 0.0f, 200.0f) * WORLD_SCALE;
    g_contact = ini_float("contact_factor", 0.5f, 0.1f, 1.0f);
    g_leadSec = ini_float("lead_seconds", 0.6f, 0.0f, 5.0f);
    g_leadMax = ini_float("lead_max_m", 6.0f, 0.0f, 50.0f) * WORLD_SCALE;
    g_pursue = ini_int("pursue_retreating", 1);
    g_chaseAlways = ini_int("chase_always", 1);
    g_chaseMode = ini_int("chase_run_mode", 2);
    g_wantHold = ini_int("hold_ranged_in_melee", 1);
    g_holdFactor = ini_float("hold_ranged_reach_factor", 1.0f, 0.2f, 5.0f);
    if (g_chaseMode > 7) g_chaseMode = 2;
    g_recede = ini_float("retreat_speed_mps", 0.3f, 0.0f, 20.0f) * WORLD_SCALE;
    g_smooth = ini_int("smooth_repath", 1);
    g_logChase = ini_int("log_chase", 1);
    g_wantPriority = ini_int("weapon_priority", 1);
    ini_str("prop_prioritize_melee", "ai_prioritize_melee", g_propMelee, sizeof g_propMelee);
    ini_str("prop_prioritize_ranged", "ai_prioritize_ranged", g_propRanged, sizeof g_propRanged);
    g_propMelee[sizeof g_propMelee - 1] = 0; g_propRanged[sizeof g_propRanged - 1] = 0;

    g_okWeapons = find_weapon_layout();
    if (!g_okWeapons) { logf_("melee weapon test               OFF (engine code not recognised) - nothing to do"); return; }
    logf_("melee weapon = weapon whose item carries the tag \"%s\"", g_meleeTag);
    if (g_meleeTag2Len) logf_("                or the vehicle weapon tag \"%s\"", g_meleeTag2);

    /* 0.5.0: humans are not touched (infantry melee = GEM MeleeStance); nothing is hooked on human states */
    logf_("humans                         not touched (0.5.0: fight distance multiplier removed, infantry melee = GEM MeleeStance)");

    /* simulation (every exe): vehicles / fauna. Needs the weapon list found by the weapon finder. */
    if (!wantVehicle) logf_("vehicle melee chase            off (ini)");
    else if (!o_wlistBeg && !find_weapons()) logf_("vehicle melee chase            OFF (weapon list not found)");
    else {
        int found = find_vehicle();
        /* the priority props are set up before the tick hook goes live (vs_tick reads g_okPriority) */
        /* the priority finder also gives the weapon range getter and the record's entity, which the close-combat
           chase (contact radius, lead, pursuit) needs; it runs even when the props are off in the ini */
        int prio = found && find_priority();
        g_okClose = prio;
        g_okPriority = prio && g_wantPriority;
        if (found && g_chaseMode >= 0 && !find_run_mode()) o_modeSlot = 0;
        g_okVehicle = found && install_vehicle();
        if (!g_okVehicle) g_okPriority = g_okClose = 0;
        if (g_okVehicle) logf_("vehicle melee chase            ON (re-path when the goal moves %.0f%% of the distance, %.1f..%.1f m; checked every %u ms, %u ms within %.0f m%s)",
                               g_repathFrac * 100, g_repathMin / WORLD_SCALE, g_repathMax / WORLD_SCALE, g_repathMs, g_repathMsClose, g_closeRange / WORLD_SCALE,
                               g_smooth && g_finishSub ? ", smooth" : "");
        if (g_okVehicle && g_okClose) logf_("melee close combat             ON (goal radius %.0f%% of melee reach, lead %.2f s / max %.1f m, pursue retreating targets %s)",
                               g_contact * 100, g_leadSec, g_leadMax / WORLD_SCALE, g_pursue ? "on" : "off");
        else if (g_okVehicle) logf_("melee close combat             OFF (weapon range getter not found)");
        if (g_okVehicle && g_okClose) logf_("melee chase always             %s", g_chaseAlways ? "ON (melee units chase until the target is inside 90% of their reach)" : "off (ini)");
        if (g_okVehicle && g_okClose && g_wantHold) {
            g_okHold = find_hold() && install_hold();
            logf_("hold ranged fire in melee      %s", g_okHold ? "ON (non-melee weapons of a melee unit hold while the target is inside melee reach)" : "OFF (engine code not recognised)");
        } else if (g_okVehicle && !g_wantHold) logf_("hold ranged fire in melee      off (ini)");
        if (g_okVehicle) {
            if (g_chaseMode < 0) logf_("chase run mode                 off (ini)");
            else if (o_modeSlot) logf_("chase run mode                 ON (movement mode %d while chasing)", g_chaseMode);
            else logf_("chase run mode                 OFF (engine code not recognised)");
        }
        else logf_("vehicle melee chase            OFF (engine code not recognised)");
        if (!g_wantPriority) logf_("weapon priority props          off (ini)");
        else if (g_okPriority) logf_("weapon priority props          ON (props \"%s\" / \"%s\")", g_propMelee, g_propRanged);
        else logf_("weapon priority props          OFF (engine code not recognised)");
    }
    if (g_okVehicle) logf_("simulation change: every player of a multiplayer match needs this DLL");
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        init(h);
    } else if (reason == DLL_PROCESS_DETACH && !reserved) {
        logf_("DLL detach (FreeLibrary)");
    }
    return TRUE;
}
#endif /* FX_TEST */
