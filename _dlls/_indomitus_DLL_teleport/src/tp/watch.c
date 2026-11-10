/* ------------------------------------------------------------------------------------ */
/* 0.2.11 MP watch (diagnostic, log only; ini watch=1)                                  */
/* ------------------------------------------------------------------------------------ */
/* Alex's Nov 9 2026 3v3 on a dedicated server: the jump order runs on the server only (orders never execute on a
   client), so teleport_begin / takeoff / jump / end - and every animation, view and sound they start - fire there
   only; clients see the unit slide or snap through position sync. Before a client can replay the visuals we need to
   know what a client actually receives. For every unit aimed with (client: the player's own units) or jumped (server)
   this logs, for WATCH_MS:
     pos:  position (x y z) whenever it moved > WATCH_STEP units from the last logged one, with ms since the watch began
     tags: every change of the unit's script tag set (+added -removed)
     ev:   every named interaction event the engine fires on the unit (FireNamed hook; our own events included)
   Same code on game / editor / server, so the client and server logs of one jump line up. */
static void tp_quant_register(void);
#define WATCH_N     16
#define WATCH_MS    12000
#define WATCH_STEP  4.0f
#define WATCH_LINES 1500
#define WATCH_TAGS  1024
static int g_watchOn = 1, g_watchLines, g_okWatchEv;
static struct { uint8_t *e; ULONGLONG t0, until; float pos[3]; char tags[WATCH_TAGS]; } g_watch[WATCH_N];
typedef void (*FireNamedHookFn)(void *ctx, const void *tag);
static FireNamedHookFn g_fireNamedOrig;

static int w_readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!p || !VirtualQuery(p, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !mi.Protect) return 0;
    return (const uint8_t *)p + n <= (const uint8_t *)mi.BaseAddress + mi.RegionSize;
}
/* MSVC std::string at s: (ptr or inline buf, size +0x10, cap +0x18) */
static const char *w_str(const uint8_t *s, size_t *len)
{
    uint64_t size = *(const uint64_t *)(s + 0x10), cap = *(const uint64_t *)(s + 0x18);
    if (cap < size || cap > 0x10000 || size > 200) return NULL;
    const char *z = cap > 15 ? *(const char *const *)s : (const char *)s;
    if (cap > 15 && !w_readable(z, size)) return NULL;
    *len = (size_t)size;
    return z;
}
/* sorted-insensitive is not needed: the engine keeps the vector order; we compare the joined string */
static void w_tags(const uint8_t *e, char *out, size_t cap)
{
    out[0] = 0;
    if (!w_readable(e + g_entTagsOff, 16)) return;
    const uint8_t *b = *(const uint8_t *const *)(e + g_entTagsOff), *en = *(const uint8_t *const *)(e + g_entTagsOff + 8);
    if (!b || !en || en < b || (uintptr_t)(en - b) % 0x60 || (uintptr_t)(en - b) > 0x60 * 512 || !w_readable(b, (size_t)(en - b))) return;
    size_t o = 0;
    for (const uint8_t *t = b; t < en; t += 0x60) {
        size_t n; const char *z = w_str(t + 0x40, &n);
        if (!z || o + n + 2 >= cap) continue;
        memcpy(out + o, z, n); o += n; out[o++] = ' '; out[o] = 0;
    }
}
static int w_has_word(const char *list, const char *w, size_t n)
{
    for (const char *p = list; *p; ) {
        const char *q = strchr(p, ' '); size_t l = q ? (size_t)(q - p) : strlen(p);
        if (l == n && !memcmp(p, w, n)) return 1;
        if (!q) break; p = q + 1;
    }
    return 0;
}
static void w_tagdiff(int k, const char *now)
{
    char buf[WATCH_TAGS + 64]; size_t o = 0;
    for (int pass = 0; pass < 2; pass++) {
        const char *a = pass ? g_watch[k].tags : now, *b = pass ? now : g_watch[k].tags;
        for (const char *p = a; *p; ) {
            const char *q = strchr(p, ' '); size_t l = q ? (size_t)(q - p) : strlen(p);
            if (l && !w_has_word(b, p, l) && o + l + 3 < sizeof buf) { buf[o++] = pass ? '-' : '+'; memcpy(buf + o, p, l); o += l; buf[o++] = ' '; }
            if (!q) break; p = q + 1;
        }
    }
    buf[o] = 0;
    if (o && g_watchLines < WATCH_LINES) { g_watchLines++; logf_("watch %p tags [%llu ms]: %s", (void *)g_watch[k].e, GetTickCount64() - g_watch[k].t0, buf); }
}
static void watch_add(uint8_t *e, const char *why)
{
    if (!g_watchOn || !e) return;
    ULONGLONG now = GetTickCount64(); int slot = -1;
    for (int k = 0; k < WATCH_N; k++) if (g_watch[k].e == e) { slot = k; break; }
    if (slot < 0) for (int k = 0; k < WATCH_N; k++) if (!g_watch[k].e || now > g_watch[k].until) { slot = k; break; }
    if (slot < 0) return;
    if (g_watch[slot].e != e) {
        g_watch[slot].e = e; g_watch[slot].t0 = now;
        memcpy(g_watch[slot].pos, e + ENT_POS, sizeof g_watch[slot].pos);
        w_tags(e, g_watch[slot].tags, WATCH_TAGS);
        if (g_watchLines < WATCH_LINES) {
            g_watchLines++;
            logf_("watch %p start (%s) at %.1f %.1f %.1f, tags: %s", (void *)e, why, g_watch[slot].pos[0], g_watch[slot].pos[1], g_watch[slot].pos[2], g_watch[slot].tags);
        }
    }
    g_watch[slot].until = now + WATCH_MS;
}
static void watch_tick(void)
{
    if (!g_watchOn) return;
    ULONGLONG now = GetTickCount64();
    for (int k = 0; k < WATCH_N; k++) {
        uint8_t *e = g_watch[k].e;
        if (!e) continue;
        if (now > g_watch[k].until || !w_readable(e, 0x600) || !(e[ENT_FLAGS] & 2)) {
            if (g_watchLines < WATCH_LINES) { g_watchLines++; logf_("watch %p end [%llu ms]", (void *)e, now - g_watch[k].t0); }
            g_watch[k].e = NULL; continue;
        }
        const float *p = (const float *)(e + ENT_POS);
        float dx = p[0] - g_watch[k].pos[0], dy = p[1] - g_watch[k].pos[1], dz = p[2] - g_watch[k].pos[2];
        if (dx * dx + dy * dy + dz * dz > WATCH_STEP * WATCH_STEP) {
            if (g_watchLines < WATCH_LINES) {
                g_watchLines++;
                logf_("watch %p pos [%llu ms]: %.1f %.1f %.1f (step %.1f, dz %+.1f)", (void *)e, now - g_watch[k].t0, p[0], p[1], p[2],
                      (float)__builtin_sqrtf(dx * dx + dy * dy + dz * dz), dz);
            }
            memcpy(g_watch[k].pos, p, sizeof g_watch[k].pos);
        }
        char t[WATCH_TAGS];
        w_tags(e, t, sizeof t);
        if (strcmp(t, g_watch[k].tags)) { w_tagdiff(k, t); memcpy(g_watch[k].tags, t, sizeof t); }
    }
}
/* FireNamed(ctx, tag): ctx[0] = entity (FireNamed reads [rcx] -> +0x548 def); the tag's name = std::string at +0x40 */
static void hook_fire_named(void *ctx, const void *tag)
{
    if (g_watchOn && ctx && tag) {
        uint8_t *e = *(uint8_t **)ctx;
        for (int k = 0; k < WATCH_N; k++) {
            if (!e || g_watch[k].e != e) continue;
            size_t n; const char *z = w_readable((const uint8_t *)tag + 0x40, 0x20) ? w_str((const uint8_t *)tag + 0x40, &n) : NULL;
            if (g_watchLines < WATCH_LINES) {
                g_watchLines++;
                logf_("watch %p ev [%llu ms]: \"%.*s\" (thread %lu)", (void *)e, GetTickCount64() - g_watch[k].t0, z ? (int)n : 1, z ? z : "?", GetCurrentThreadId());
            }
            break;
        }
    }
    g_fireNamedOrig(ctx, tag);
}
static void watch_install(void)
{
    if (!g_watchOn) { logf_("watch (MP diagnostic)            off (ini)"); return; }
    if (g_okEvents && g_fireNamed)
        g_okWatchEv = install_jmp5((uint8_t *)g_fireNamed, (void *)hook_fire_named, (void **)&g_fireNamedOrig, "FireNamed (watch)");
    logf_("watch (MP diagnostic)            ON: aimed / jumped units logged %d s (pos, tags, events: %s)", WATCH_MS / 1000,
          g_okWatchEv ? "hooked" : "not hooked");
}
