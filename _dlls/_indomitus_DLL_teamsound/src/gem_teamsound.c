/* GEM TeamSound 0.3 - per-player sound variants for Call to Arms: Gates of Hell (GEM 2, 1.067)
 *
 * Entity scripts play sounds with {play_sound "<link>"} and {start_sound "<path>"}. In a
 * multiplayer game every machine runs the same script, so everybody hears the same sound.
 * This dll rewrites the token "@side" in the link name / path on each machine, based on how
 * the LOCAL player relates to the player who owns the entity:
 *
 *   {start_sound "world/call-ins/support_incoming_@side"}
 *     the owner and his allies hear  .../support_incoming_ally
 *     his enemies hear               .../support_incoming_enemy
 *     owner unknown / no local player .../support_incoming_neutral
 *   {play_sound "support_@side"} picks the link "support_ally" / "support_enemy" / "support_neutral".
 *
 * The owner is the script entity's player (eActor byte, the one {player N} sets) or, when the
 * entity is not an actor or has no player, the effector's. Words are ini-configurable
 * (OwnWord lets the owning player hear a third variant).
 *
 * Engine (all found at runtime, nothing hard-coded):
 *   eAction_SoundPlay / eAction_SoundSpawn (RTTI) vtable slot 1 = Execute(action, ctx);
 *     action+0x30 = std::string name/path, ctx+0 = entity, ctx+8 = effector.
 *   eAction_ProcessView Execute ({view show "x" my/ally/enemy/neutral}) gives the rest:
 *     test byte [entity+0x30],2 (actor) -> movzx [entity+0xDA8] (player byte)
 *     GetLocalPlayer = movzx eax,byte [rip+g]; mov [rcx],al; mov rax,rcx; ret
 *     Relation(table, &a, &b) -> 2 same/ally, 1 enemy, 0 neutral.
 * Only sound selection changes; nothing that affects the simulation, so it cannot desync.
 *
 * 0.2: the listener's army.
 *   {play_sound "@army=ig"} (link name starting with the hint prefix) is never played: when the
 *     entity belongs to the LOCAL player it records "ig" as the local player's army. Human scripts
 *     send it from "breed_applied", so the dll learns the army from the player's own troops.
 *   "@army" in a name becomes that word (ArmyDefault until a hint was seen).
 *   "@who"  becomes "enemy_<army>" for enemies of the owner, else the ally / own / neutral word:
 *     {play_sound "vo_w_@who"} -> vo_w_enemy_ig / vo_w_enemy_tg / vo_w_enemy_sm / vo_w_ally / vo_w_neutral.
 *   {play_sound "@army!ig"} = forced army (conquest: the mission script knows the player's nation and has a
 *     human send it every 15 s): no owner test, and for ForceHoldMs after it the "@army=" hints are ignored.
 *
 * 0.3: the army is read from the engine. The multiplayer session object keeps one record per player
 *   (the one a bot's BotApi.Instance.army comes from):
 *     session = *g_session (getter "mov rax,[rip+g]; ret"), record = Lookup(session, playerIndex),
 *     record + army offset = std::string with the army name ("ig", "tg", "sm").
 *   Found from eBotGameClient: the Lua property "army" (registered right after "spawnPointName") gives the
 *   getter and so the string offset; the code that builds the bot clients gives the getter + Lookup calls.
 *   Order of sources: engine record, then the script hints above (fallback), then ArmyDefault.
 *
 * Build: build.bat   Offline finder test: build.bat test
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#define TS_VERSION "0.3"

/* ---------------------------------------------------------------- config */
static int  cfg_enabled = 1;
static char cfg_token[32]   = "@side";
static char cfg_own[32]     = "ally";
static char cfg_ally[32]    = "ally";
static char cfg_enemy[32]   = "enemy";
static char cfg_neutral[32] = "neutral";
static char cfg_tok_army[32] = "@army";
static char cfg_tok_who[32]  = "@who";
static char cfg_hint[32]     = "@army=";
static char cfg_army_def[32] = "ig";
static char cfg_force[32]    = "@army!";
static int  cfg_force_ms = 60000;
static ULONGLONG g_force_until;             /* tick until which a forced army holds */
static char g_army[32];                     /* local player's army, from hints; "" = not seen yet */
static uint8_t g_army_player = 0xFF;        /* local player index the hint was seen for */
static int  cfg_effector = 1;
static int  cfg_log = 0;

/* ---------------------------------------------------------------- found layout */
typedef int  (__attribute__((ms_abi)) *rel_fn_t)(void *table, const uint8_t *a, const uint8_t *b);
typedef void (__attribute__((ms_abi)) *exec_fn_t)(void *action, void *ctx);

static uint8_t  o_flag = 0x30, m_flag = 2;  /* actor flag byte / mask */
static uint32_t o_player;                   /* actor player byte */
static volatile uint8_t *g_local;           /* local player byte */
static void    *g_reltab;
static rel_fn_t f_rel;
static void   **vt_play, **vt_spawn;        /* vtables */
static exec_fn_t orig_play, orig_spawn;

/* ---------------------------------------------------------------- log */
static char  g_logpath[MAX_PATH];
static FILE *g_log;
static int   g_lines;
static void logf_(const char *fmt, ...)
{
    if (!g_log) {
        if (!g_logpath[0]) return;
        g_log = fopen(g_logpath, "a");
        if (!g_log) return;
    }
    va_list ap; va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* ---------------------------------------------------------------- MSVC std::string (x64 release) */
typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;
static const char *ms_cstr(const MsString *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }

/* ---------------------------------------------------------------- side resolution */
static int owner_of(const uint8_t *ent, uint8_t *out)
{
    if (!ent || IsBadReadPtr(ent, o_player + 1)) return 0;
    if (!(ent[o_flag] & m_flag)) return 0;
    uint8_t p = ent[o_player];
    if (p == 0xFF) return 0;
    *out = p;
    return 1;
}

/* returns the word to use, and fills why[] for the log */
static int engine_army(uint8_t local, char *out, size_t outn);

/* the local player's army word and where it came from */
static const char *local_army(char *buf, size_t n, const char **src)
{
    uint8_t local = g_local ? *g_local : 0xFF;
    if (engine_army(local, buf, n)) { *src = "engine"; return buf; }
    if (g_army[0] && local == g_army_player) { *src = "script"; return g_army; }
    *src = "default";
    return cfg_army_def;
}

static const char *pick_side(uint8_t **ctx, char *why, size_t whyn, int *is_enemy)
{
    *is_enemy = 0;
    uint8_t owner = 0xFF, local = g_local ? *g_local : 0xFF;
    const char *src = "none";
    if (owner_of(ctx[0], &owner)) src = "entity";
    else if (cfg_effector && owner_of(ctx[1], &owner)) src = "effector";
    if (owner == 0xFF || local == 0xFF) {
        snprintf(why, whyn, "owner %d (%s) local %d", owner == 0xFF ? -1 : owner, src,
                 local == 0xFF ? -1 : local);
        return cfg_neutral;
    }
    int rel = f_rel(g_reltab, &local, &owner);
    snprintf(why, whyn, "owner %d (%s) local %d rel %d", owner, src, local, rel);
    if (owner == local) return cfg_own;
    if (rel == 2) return cfg_ally;
    if (rel == 1) { *is_enemy = 1; return cfg_enemy; }
    return cfg_neutral;
}

/* ---------------------------------------------------------------- the hook */
static CRITICAL_SECTION g_cs;

/* ---------------------------------------------------------------- army from the engine (0.3) */
typedef void *(__attribute__((ms_abi)) *look_fn_t)(void *session, int player);
static void *volatile *g_session;           /* address of the session pointer */
static look_fn_t f_look;                    /* record = f_look(session, player index) */
static uint32_t  o_army;                    /* std::string army in the record */
static int       cfg_engine = 1;
static const char *g_eng_why = "";         /* why the engine record was not used (log) */

/* copies the local player's army (lower case) into out; 0 = not available */
static int engine_army(uint8_t local, char *out, size_t outn)
{
    g_eng_why = "engine lookup not found";
    if (!cfg_engine || !g_session || !f_look || !o_army || local == 0xFF) return 0;
    void *sess = *g_session;
    g_eng_why = "no session";
    if (!sess || IsBadReadPtr(sess, 0x188)) return 0;
    uint8_t *rec = (uint8_t *)f_look(sess, (int)local);
    g_eng_why = "no record for the local player";
    if (!rec || IsBadReadPtr(rec + o_army, sizeof(MsString))) return 0;
    g_eng_why = "army string empty or odd";
    const MsString *st = (const MsString *)(rec + o_army);
    if (st->size == 0 || st->size >= outn || st->cap < st->size) return 0;
    const char *c = ms_cstr(st);
    if (st->cap > 15 && IsBadReadPtr(c, st->size)) return 0;
    for (size_t i = 0; i < st->size; i++) {
        char ch = c[i];
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch + 32);
        if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return 0;
        out[i] = ch;
    }
    out[st->size] = 0;
    g_eng_why = "";
    return 1;
}

/* "@army=<word>" hint: remember the local player's army when the entity is his; never played */
static void army_hint(const char *name, uint8_t **ctx)
{
    uint8_t owner = 0xFF, local = g_local ? *g_local : 0xFF;
    const char *w = name + strlen(cfg_hint);
    if (!owner_of(ctx[0], &owner) && !(cfg_effector && owner_of(ctx[1], &owner))) return;
    if (local == 0xFF || owner != local || !w[0] || strlen(w) >= sizeof g_army) return;
    if (GetTickCount64() < g_force_until) return;        /* the mission script's word wins */
    if (g_army_player == local && !strcmp(g_army, w)) return;
    EnterCriticalSection(&g_cs);
    strcpy(g_army, w);
    g_army_player = local;
    LeaveCriticalSection(&g_cs);
    if (g_lines < 5000) { g_lines++; logf_("army hint: local player %d plays \"%s\"", local, g_army); }
}

/* "@army!<word>" forced army: whoever sends it, it is the local player's army (conquest has one human) */
static void army_force(const char *name)
{
    uint8_t local = g_local ? *g_local : 0xFF;
    const char *w = name + strlen(cfg_force);
    if (local == 0xFF || !w[0] || strlen(w) >= sizeof g_army) return;
    int same = g_army_player == local && !strcmp(g_army, w);
    EnterCriticalSection(&g_cs);
    strcpy(g_army, w);
    g_army_player = local;
    g_force_until = GetTickCount64() + (ULONGLONG)cfg_force_ms;
    LeaveCriticalSection(&g_cs);
    if (!same && g_lines < 5000) { g_lines++; logf_("army forced by the mission: local player %d plays \"%s\"", local, g_army); }
}

static void run(exec_fn_t orig, const char *what, uint8_t *action, uint8_t **ctx)
{
    MsString *s = (MsString *)(action + 0x30);
    const char *name = ms_cstr(s);
    if (!cfg_enabled || !ctx || s->size >= 480) {
        orig(action, ctx);
        return;
    }
    if (cfg_force[0] && !strncmp(name, cfg_force, strlen(cfg_force))) { army_force(name); return; }
    if (cfg_hint[0] && !strncmp(name, cfg_hint, strlen(cfg_hint))) { army_hint(name, ctx); return; }
    if (!strstr(name, cfg_token) && !strstr(name, cfg_tok_army) && !strstr(name, cfg_tok_who)) {
        orig(action, ctx);
        return;
    }
    char why[96], who[80];
    int enemy = 0;
    const char *word = pick_side(ctx, why, sizeof why, &enemy);
    char abuf[32];
    const char *asrc = "";
    const char *army = local_army(abuf, sizeof abuf, &asrc);
    if (enemy) snprintf(who, sizeof who, "%s_%s", cfg_enemy, army);
    else       snprintf(who, sizeof who, "%s", word);
    /* replace every token; the longest token that matches at a position wins */
    const char *toks[3] = { cfg_tok_army, cfg_tok_who, cfg_token };
    const char *vals[3] = { army, who, word };
    char buf[600];
    size_t n = 0;
    for (const char *p = name; *p; ) {
        int hit = -1;
        for (int k = 0; k < 3; k++) {
            size_t tl = strlen(toks[k]);
            if (tl && !strncmp(p, toks[k], tl) && (hit < 0 || tl > strlen(toks[hit]))) hit = k;
        }
        if (hit >= 0) {
            size_t vl = strlen(vals[hit]);
            if (n + vl >= sizeof buf - 1) { orig(action, ctx); return; }
            memcpy(buf + n, vals[hit], vl); n += vl; p += strlen(toks[hit]);
        } else {
            if (n >= sizeof buf - 2) { orig(action, ctx); return; }
            buf[n++] = *p++;
        }
    }
    buf[n] = 0;

    if (cfg_log && g_lines < 5000) {
        g_lines++;
        logf_("%s \"%s\" -> \"%s\" | %s army %s (%s%s%s)", what, name, buf, why, army, asrc,
              g_eng_why[0] ? "; engine: " : "", g_eng_why);
    }
    /* swap the action's string for ours just for this call: the engine only reads it
     * (Tag ctor / string assign), so no allocator ever sees our buffer */
    EnterCriticalSection(&g_cs);
    MsString saved = *s, tmp;
    memset(&tmp, 0, sizeof tmp);
    if (n < 16) { memcpy(tmp.u.buf, buf, n + 1); tmp.cap = 15; }
    else { tmp.u.ptr = buf; tmp.cap = n; }
    tmp.size = n;
    *s = tmp;
    orig(action, ctx);
    *s = saved;
    LeaveCriticalSection(&g_cs);
}

static void __attribute__((ms_abi)) hk_play(void *a, void *c)  { run(orig_play,  "play_sound",  a, c); }
static void __attribute__((ms_abi)) hk_spawn(void *a, void *c) { run(orig_spawn, "start_sound", a, c); }

/* ---------------------------------------------------------------- finders */
static int section(uint8_t *base, const char *nm, uint8_t **lo, uint8_t **hi)
{
    IMAGE_DOS_HEADER *d = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + d->e_lfanew);
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++)
        if (!strncmp((char *)s->Name, nm, 8)) {
            *lo = base + s->VirtualAddress;
            *hi = *lo + s->Misc.VirtualSize;
            return 1;
        }
    return 0;
}

/* vtable of a class by its RTTI name (complete object locator with offset 0) */
static void **find_vtable(uint8_t *base, const char *rtti)
{
    uint8_t *dlo, *dhi, *rlo, *rhi;
    if (!section(base, ".data", &dlo, &dhi) || !section(base, ".rdata", &rlo, &rhi)) return NULL;
    size_t n = strlen(rtti) + 1;
    uint8_t *td = NULL;
    for (uint8_t *p = dlo; p + n <= dhi; p++)
        if (*p == '.' && !memcmp(p, rtti, n)) { td = p - 0x10; break; }
    if (!td) return NULL;
    uint32_t tdr = (uint32_t)(td - base);
    for (uint8_t *p = rlo; p + 24 <= rhi; p += 4) {
        uint32_t *col = (uint32_t *)p;
        if (col[0] != 1 || col[1] != 0 || col[3] != tdr || col[5] != (uint32_t)(p - base)) continue;
        uint64_t va = (uint64_t)(uintptr_t)p;
        for (uint8_t *q = rlo; q + 16 <= rhi; q += 8)
            if (*(uint64_t *)q == va) return (void **)(q + 8);
    }
    return NULL;
}

static uint8_t *rel32(uint8_t *at, int len) { return at + len + *(int32_t *)(at + len - 4); }

static const char *find_all(uint8_t *base)
{
    uint8_t *tlo, *thi;
    if (!section(base, ".text", &tlo, &thi)) return "no .text";
    vt_play  = find_vtable(base, ".?AUeAction_SoundPlay@Interaction@@");
    vt_spawn = find_vtable(base, ".?AUeAction_SoundSpawn@Interaction@@");
    void **vt_view = find_vtable(base, ".?AUeAction_ProcessView@Interaction@@");
    if (!vt_play || !vt_spawn || !vt_view) return "action vtables not found";
    /* parse slot 5 of both sound actions starts by storing the name at +0x30 */
    for (int k = 0; k < 2; k++) {
        uint8_t *parse = (uint8_t *)(k ? vt_spawn : vt_play)[5];
        int ok = 0;
        for (int i = 0; i < 0x20; i++) if (!memcmp(parse + i, "\x48\x8D\x71\x30", 4)) ok = 1;
        if (!ok) return "sound action name offset is not +0x30";
    }
    /* ProcessView::Execute (slot 1) */
    uint8_t *f = (uint8_t *)vt_view[1], *end = f + 0x180;
    if (f < tlo || end > thi) return "ProcessView execute out of range";
    uint8_t *flag = NULL;
    o_player = 0; g_local = NULL; g_reltab = NULL; f_rel = NULL;
    for (uint8_t *p = f; p < end; p++) {
        /* test byte [r64+disp8], imm8 : F6 4x disp imm */
        if (!flag && p[0] == 0xF6 && (p[1] & 0xF8) == 0x40) { flag = p; o_flag = p[2]; m_flag = p[3]; continue; }
        /* movzx r32, byte [r64+disp32] : 0F B6 8x/9x.. (mod 10) */
        if (flag && !o_player && p[0] == 0x0F && p[1] == 0xB6 && (p[2] & 0xC0) == 0x80 && (p[2] & 7) != 4)
            { o_player = *(uint32_t *)(p + 3); continue; }
        if (p[0] == 0xE8 && !g_local) {
            uint8_t *t = rel32(p, 5);
            if (t >= tlo && t + 13 <= thi && t[0] == 0x0F && t[1] == 0xB6 && t[2] == 0x05 &&
                t[7] == 0x88 && t[8] == 0x01 && t[12] == 0xC3)
                g_local = rel32(t, 7);
            continue;
        }
        /* lea rcx,[rip+table]; call Relation (starts movzx eax, byte [rdx]) */
        if (g_local && !f_rel && p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x0D && p[7] == 0xE8) {
            uint8_t *t = rel32(p + 7, 5);
            if (t >= tlo && t + 3 <= thi && t[0] == 0x0F && t[1] == 0xB6 && t[2] == 0x02) {
                g_reltab = rel32(p, 7);
                f_rel = (rel_fn_t)t;
            }
        }
    }
    if (!flag || !o_player || o_player > 0x4000) return "actor player offset not found";
    if (!g_local) return "local player not found";
    if (!f_rel) return "relation table not found";
    return NULL;
}

/* 0.3: session pointer, record lookup and army offset. NULL = found. Not fatal when it fails. */
static const char *find_army(uint8_t *base)
{
    uint8_t *tlo, *thi, *rlo, *rhi;
    g_session = NULL; f_look = NULL; o_army = 0;
    if (!section(base, ".text", &tlo, &thi) || !section(base, ".rdata", &rlo, &rhi)) return "no sections";
    /* the string "spawnPointName" */
    static const char key[] = "spawnPointName";
    uint8_t *sp = NULL;
    for (uint8_t *p = rlo; p + sizeof key <= rhi; p++)
        if (*p == 's' && !memcmp(p, key, sizeof key)) { sp = p; break; }
    if (!sp) return "no spawnPointName string";
    /* lea rdx,[rip+spawnPointName] ... lea r8,[rip+getter]; lea rdx,[rip+"army"] */
    uint8_t *getter = NULL;
    for (uint8_t *p = tlo; p + 7 <= thi && !getter; p++) {
        if (p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x15 || rel32(p, 7) != sp) continue;
        for (uint8_t *q = p + 7; q < p + 0x60 && q + 14 <= thi; q++) {
            if (q[0] != 0x4C || q[1] != 0x8D || q[2] != 0x05) continue;
            if (q[7] != 0x48 || q[8] != 0x8D || q[9] != 0x15) continue;
            uint8_t *str = rel32(q + 7, 7), *g = rel32(q, 7);
            if (str < rlo || str + 5 > rhi || memcmp(str, "army", 5)) continue;
            if (g >= tlo && g + 0x40 <= thi) { getter = g; break; }
        }
    }
    if (!getter) return "army property getter not found";
    /* getter: mov r8,[rcx+..]; ... lea rdx,[r8+disp32] (49 8D 90) or disp8 (49 8D 50) */
    for (int i = 0; i < 0x28 && !o_army; i++) {
        if (getter[i] == 0x49 && getter[i + 1] == 0x8D && getter[i + 2] == 0x90) o_army = *(uint32_t *)(getter + i + 3);
        else if (getter[i] == 0x49 && getter[i + 1] == 0x8D && getter[i + 2] == 0x50) o_army = getter[i + 3];
    }
    if (!o_army || o_army > 0x2000) { o_army = 0; return "army offset not found"; }
    /* the code that creates the bot clients: lea reg,[rip+vtable eBotGameClient] ... call GetSession;
     * mov rcx,rax; mov edx,ebx; call Lookup */
    void **vt = find_vtable(base, ".?AVeBotGameClient@Multiplayer@@");
    if (!vt) return "eBotGameClient vtable not found";
    for (uint8_t *p = tlo; p + 7 <= thi; p++) {
        if ((p[0] & 0xFB) != 0x48 || p[1] != 0x8D || (p[2] & 0xC7) != 0x05 || rel32(p, 7) != (uint8_t *)vt) continue;
        for (uint8_t *q = p + 7; q < p + 0xA0 && q + 15 <= thi; q++) {
            if (q[0] != 0xE8 || q[5] != 0x48 || q[6] != 0x8B || q[7] != 0xC8 || q[8] != 0x8B || q[9] != 0xD3 || q[10] != 0xE8) continue;
            uint8_t *gs = rel32(q, 5), *lk = rel32(q + 10, 5);
            if (gs < tlo || gs + 8 > thi || lk < tlo || lk + 8 > thi) continue;
            if (gs[0] != 0x48 || gs[1] != 0x8B || gs[2] != 0x05 || gs[7] != 0xC3) continue;   /* mov rax,[rip+g]; ret */
            if (lk[0] != 0x48 || lk[1] != 0x8B || lk[2] != 0x81) continue;                    /* mov rax,[rcx+disp32] */
            g_session = (void *volatile *)rel32(gs, 7);
            f_look = (look_fn_t)lk;
            return NULL;
        }
    }
    o_army = 0;
    return "session lookup not found";
}

/* ---------------------------------------------------------------- ini */
static void ini_s(const char *ini, const char *key, char *dst, size_t n)
{
    char buf[64];
    GetPrivateProfileStringA("TeamSound", key, dst, buf, sizeof buf, ini);
    if (buf[0] && strlen(buf) < n) strcpy(dst, buf);
}

static void load_ini(const char *ini)
{
    cfg_enabled  = GetPrivateProfileIntA("TeamSound", "Enabled", 1, ini);
    ini_s(ini, "Token",       cfg_token,   sizeof cfg_token);
    ini_s(ini, "OwnWord",     cfg_own,     sizeof cfg_own);
    ini_s(ini, "AllyWord",    cfg_ally,    sizeof cfg_ally);
    ini_s(ini, "EnemyWord",   cfg_enemy,   sizeof cfg_enemy);
    ini_s(ini, "NeutralWord", cfg_neutral, sizeof cfg_neutral);
    ini_s(ini, "ArmyToken",   cfg_tok_army, sizeof cfg_tok_army);
    ini_s(ini, "WhoToken",    cfg_tok_who,  sizeof cfg_tok_who);
    ini_s(ini, "ArmyHint",    cfg_hint,     sizeof cfg_hint);
    ini_s(ini, "ArmyDefault", cfg_army_def, sizeof cfg_army_def);
    ini_s(ini, "ArmyForce",   cfg_force,    sizeof cfg_force);
    cfg_force_ms = GetPrivateProfileIntA("TeamSound", "ForceHoldMs", 60000, ini);
    cfg_engine   = GetPrivateProfileIntA("TeamSound", "EngineArmy", 1, ini);
    cfg_effector = GetPrivateProfileIntA("TeamSound", "UseEffector", 1, ini);
    cfg_log      = GetPrivateProfileIntA("TeamSound", "Log", 0, ini);
}

#ifndef TS_TEST
/* ---------------------------------------------------------------- install */
static int patch_slot(void **slot, void *hook, exec_fn_t *orig)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return 0;
    *orig = (exec_fn_t)*slot;
    *slot = hook;
    VirtualProtect(slot, sizeof *slot, old, &old);
    return 1;
}

static void install(void)
{
    uint8_t *base = (uint8_t *)GetModuleHandleW(NULL);
    const char *err = find_all(base);
    if (err) { logf_("[GEM TeamSound %s] NOT installed: %s", TS_VERSION, err); return; }
    InitializeCriticalSection(&g_cs);
    if (!patch_slot(&vt_play[1], (void *)hk_play, &orig_play) ||
        !patch_slot(&vt_spawn[1], (void *)hk_spawn, &orig_spawn)) {
        logf_("[GEM TeamSound] VirtualProtect failed (%lu)", GetLastError());
        return;
    }
    const char *aerr = find_army(base);
    if (aerr) logf_("[GEM TeamSound %s] army from the engine: NOT available (%s); script hints / ArmyDefault are used", TS_VERSION, aerr);
    else logf_("[GEM TeamSound %s] army from the engine: session ptr exe+0x%llX, lookup exe+0x%llX, army string +0x%X",
               TS_VERSION, (unsigned long long)((uint8_t *)g_session - base),
               (unsigned long long)((uint8_t *)f_look - base), o_army);
    logf_("[GEM TeamSound %s] installed | play exe+0x%llX spawn exe+0x%llX, actor flag +0x%X&%u player +0x%X, "
          "local exe+0x%llX, relation exe+0x%llX table exe+0x%llX | Enabled=%d Token=%s Own=%s Ally=%s Enemy=%s "
          "Neutral=%s UseEffector=%d Log=%d | army: hint \"%s<word>\", tokens %s %s, default %s",
          TS_VERSION, (unsigned long long)((uint8_t *)orig_play - base),
          (unsigned long long)((uint8_t *)orig_spawn - base), o_flag, m_flag, o_player,
          (unsigned long long)((uint8_t *)g_local - base), (unsigned long long)((uint8_t *)f_rel - base),
          (unsigned long long)((uint8_t *)g_reltab - base), cfg_enabled, cfg_token, cfg_own, cfg_ally,
          cfg_enemy, cfg_neutral, cfg_effector, cfg_log, cfg_hint, cfg_tok_army, cfg_tok_who, cfg_army_def);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID res)
{
    (void)res;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        char path[MAX_PATH], ini[MAX_PATH], exe[MAX_PATH], *nm;
        DWORD n = GetModuleFileNameA(inst, path, MAX_PATH);
        if (n == 0 || n >= MAX_PATH - 16) return TRUE;
        char *dot = strrchr(path, '.');
        if (dot) *dot = 0;
        GetModuleFileNameA(NULL, exe, MAX_PATH);
        nm = strrchr(exe, '\\'); nm = nm ? nm + 1 : exe;
        snprintf(ini, sizeof ini, "%s.ini", path);
        snprintf(g_logpath, sizeof g_logpath, "%s%s.log", path,
                 strstr(nm, "_ed") ? "_editor" : (strstr(nm, "_server") ? "_server" : ""));
        load_ini(ini);
        if (strstr(nm, "_server")) { logf_("[GEM TeamSound] dedicated server: not installed (no sound)"); return TRUE; }
        install();
    }
    return TRUE;
}
#else
/* ---------------------------------------------------------------- offline test */
/* build.bat test -> ts_test.exe <exe>...: finders on each mapped exe, then the rewrite logic
 * against fake actions/entities using the exe's own Relation function. */
static uint8_t *last_action; static char last_name[600];
static void __attribute__((ms_abi)) fake_exec(void *a, void *c)
{
    (void)c; last_action = a;
    strcpy(last_name, ms_cstr((MsString *)((uint8_t *)a + 0x30)));
}

int main(int argc, char **argv)
{
    int bad = 0;
    InitializeCriticalSection(&g_cs);
    for (int i = 1; i < argc; i++) {
        HMODULE m = LoadLibraryExA(argv[i], NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (!m) { printf("%s: load failed %lu\n", argv[i], GetLastError()); bad = 1; continue; }
        uint8_t *base = (uint8_t *)m;
        const char *err = find_all(base);
        if (err) { printf("%s: FAIL %s\n", argv[i], err); bad = 1; continue; }
        printf("%s:\n  play exec exe+0x%llX spawn exec exe+0x%llX\n  actor flag +0x%X & %u, player +0x%X, "
               "local exe+0x%llX, relation exe+0x%llX, table exe+0x%llX\n",
               argv[i], (unsigned long long)((uint8_t *)vt_play[1] - base),
               (unsigned long long)((uint8_t *)vt_spawn[1] - base), o_flag, m_flag, o_player,
               (unsigned long long)((uint8_t *)g_local - base), (unsigned long long)((uint8_t *)f_rel - base),
               (unsigned long long)((uint8_t *)g_reltab - base));

        const char *aerr = find_army(base);
        if (aerr) printf("  army from the engine: NOT FOUND (%s)\n", aerr);
        else printf("  army from the engine: session ptr exe+0x%llX, lookup exe+0x%llX, army string +0x%X\n",
                    (unsigned long long)((uint8_t *)g_session - base), (unsigned long long)((uint8_t *)f_look - base), o_army);
        look_fn_t real_look = f_look; uint32_t real_off = o_army;
        f_look = NULL;      /* the script-hint cases below run without the engine source */

        /* logic test with a fake relation table: players 0,1 = team A, 2,3 = team B */
        static int tbl[18 * 18];
        for (int a = 0; a < 18; a++) for (int b = 0; b < 18; b++)
            tbl[b * 18 + a] = (a < 2) == (b < 2) ? 2 : 1;
        static struct { int *t; void *pad; int stride; } fake = { tbl, 0, 18 };
        g_reltab = &fake;
        static uint8_t local; g_local = &local;
        static uint8_t ent[0x4000], eff[0x4000], action[0x60];
        uint8_t *ctx[2] = { ent, eff };
        struct { uint8_t lp, owner; int actor; const char *in, *want; } cases[] = {
            { 0, 0, 1, "world/call-ins/support_incoming_@side", "world/call-ins/support_incoming_ally" },
            { 1, 0, 1, "support_@side",                         "support_ally" },
            { 2, 0, 1, "support_@side",                         "support_enemy" },
            { 3, 1, 1, "x/@side/y",                             "x/enemy/y" },
            { 2, 0xFF, 1, "support_@side",                      "support_neutral" },
            { 0xFF, 0, 1, "support_@side",                      "support_neutral" },
            { 2, 0, 0, "support_@side",                         "support_enemy" },   /* via effector */
            { 2, 0, 1, "plain/sound",                           "plain/sound" },
            { 2, 0, 1, "vo_w_@who",                             "vo_w_enemy_ig" },   /* no hint yet: ArmyDefault */
            { 2, 2, 1, "@army=sm",                              "<not played>" },    /* own unit: hint taken */
            { 2, 0, 1, "vo_w_@who",                             "vo_w_enemy_sm" },
            { 2, 3, 1, "vo_w_@who",                             "vo_w_ally" },
            { 2, 0, 1, "@army=tg",                              "<not played>" },    /* enemy unit: hint ignored */
            { 2, 0, 1, "x_@side_@army",                         "x_enemy_sm" },
            { 0, 2, 1, "vo_w_@who",                             "vo_w_enemy_ig" },   /* other local player: default again */
            { 0xFF, 0, 1, "vo_w_@who",                          "vo_w_neutral" },
            { 2, 0, 1, "@army!tg",                              "<not played>" },    /* forced by the mission, sender not ours */
            { 2, 0, 1, "vo_w_@who",                             "vo_w_enemy_tg" },
            { 2, 2, 1, "@army=sm",                              "<not played>" },    /* own unit hint ignored while forced */
            { 2, 0, 1, "vo_w_@who",                             "vo_w_enemy_tg" },
        };
        for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
            local = cases[c].lp;
            memset(ent, 0, sizeof ent); memset(eff, 0, sizeof eff);
            ent[o_flag] = cases[c].actor ? m_flag : 0; ent[o_player] = cases[c].actor ? cases[c].owner : 0xFF;
            eff[o_flag] = m_flag; eff[o_player] = cases[c].owner;
            MsString *s = (MsString *)(action + 0x30);
            char heap[300]; strcpy(heap, cases[c].in);
            size_t L = strlen(heap);
            memset(s, 0, sizeof *s);
            if (L < 16) { memcpy(s->u.buf, heap, L + 1); s->cap = 15; } else { s->u.ptr = heap; s->cap = L; }
            s->size = L;
            MsString before = *s;
            strcpy(last_name, "<not played>");
            run(fake_exec, "test", action, ctx);
            int ok = !strcmp(last_name, cases[c].want) && !memcmp(&before, s, sizeof before);
            printf("  case %zu: \"%s\" -> \"%s\" %s\n", c, cases[c].in, last_name, ok ? "OK" : "FAIL");
            if (!ok) bad = 1;
        }
        /* the exe's own record lookup on a fake session: player 2 = "TG", player 5 = "sm" */
        if (real_look) {
            static uint8_t sess[0x200], rec2[0x400], rec5[0x400];
            static void *list1[1], *list2[1];
            static void *sessp;
            memset(sess, 0, sizeof sess); memset(rec2, 0, sizeof rec2); memset(rec5, 0, sizeof rec5);
            *(int *)rec2 = 2; *(int *)rec5 = 5;
            MsString *a2 = (MsString *)(rec2 + real_off), *a5 = (MsString *)(rec5 + real_off);
            strcpy(a2->u.buf, "TG"); a2->size = 2; a2->cap = 15;
            strcpy(a5->u.buf, "sm"); a5->size = 2; a5->cap = 15;
            list1[0] = rec2; list2[0] = rec5;
            *(void **)(sess + 0x160) = &list1[0]; *(void **)(sess + 0x168) = &list1[1];
            *(void **)(sess + 0x178) = &list2[0]; *(void **)(sess + 0x180) = &list2[1];
            sessp = sess; g_session = &sessp; f_look = real_look; o_army = real_off;
            struct { uint8_t lp, owner; const char *in, *want; } ac[] = {
                { 2, 0, "vo_w_@who", "vo_w_enemy_tg" },      /* engine record, lower-cased */
                { 5, 0, "vo_w_@who", "vo_w_enemy_sm" },      /* second list */
                { 3, 0, "vo_w_@who", "vo_w_enemy_tg" },      /* no record for player 3: falls back (script word is for player 2 -> default... see below) */
                { 5, 4, "x_@army",   "x_sm" },
            };
            for (size_t c = 0; c < sizeof ac / sizeof *ac; c++) {
                local = ac[c].lp;
                memset(ent, 0, sizeof ent); memset(eff, 0, sizeof eff);
                ent[o_flag] = m_flag; ent[o_player] = ac[c].owner;
                MsString *s = (MsString *)(action + 0x30);
                memset(s, 0, sizeof *s);
                size_t L = strlen(ac[c].in);
                memcpy(s->u.buf, ac[c].in, L + 1); s->cap = 15; s->size = L;
                strcpy(last_name, "<not played>");
                run(fake_exec, "test", action, ctx);
                const char *want = ac[c].want;
                if (c == 2) want = "vo_w_enemy_ig";       /* player 3: no record, script word belongs to player 2 -> ArmyDefault */
                int ok = !strcmp(last_name, want);
                printf("  engine case %zu: local %d \"%s\" -> \"%s\" %s\n", c, ac[c].lp, ac[c].in, last_name, ok ? "OK" : "FAIL");
                if (!ok) bad = 1;
            }
        }
        /* the exe's own Relation on the fake table */
        uint8_t a = 0, b = 2, a2 = 1;
        int r1 = f_rel(&fake, &a, &b), r2 = f_rel(&fake, &a, &a2), r3 = f_rel(&fake, &a, &a);
        printf("  engine Relation: 0v2=%d 0v1=%d 0v0=%d %s\n", r1, r2, r3, (r1 == 1 && r2 == 2 && r3 == 2) ? "OK" : "FAIL");
        if (!(r1 == 1 && r2 == 2 && r3 == 2)) bad = 1;
    }
    printf(bad ? "SOME FAILED\n" : "ALL OK\n");
    return bad;
}
#endif
