/* GEM UserControl 0.1 - hand "user_control" tagged units to their player (Call to Arms: Gates of Hell, GEM 2, 1.067)
 *
 * Replaces the mission trigger 40k_triggers/user_control. A unit that carries the entity tag
 * "user_control" (ini Tag=) gets, at once:
 *   - brain control = user  (what the trigger action  actor_state {control user}  does)
 *   - tags "player" and "_user" added (ini AddTags=)
 *   - "user_control" removed
 * No mission trigger, no polling: the dll acts when the tag is added and when the owner is set.
 *
 * Engine (all found at runtime, nothing hard-coded; game 1.067 addresses for reference):
 *   actor_state {control user}: eHelperCommandActor.cpp apply exe+0x99F940 does nothing but
 *     flags = [[entity+0xDC8]+0x398]; flags = (flags & ~3) | control   (AI 0, user 1)
 *     for entities that have a brain. No engine function behind it, so the dll writes the same bits.
 *     Offsets come from eActor::IsUserControl (exe+0x7EF670), reached through the RTTI vtable of
 *     eCondition_UserControl ({if user_control}): slot 1 tests the actor flag [e+0x30]&2, slot 3
 *     jumps to  mov rax,[rcx+BRAIN]; test; je; mov eax,[rax+CTL]; and al,3; cmp al,1.
 *   Tag added: the {tags add} action (eAction "tags", table entry in .data) Execute(action, ctx),
 *     vtable slot 1 exe+0x74E040: entity = [ctx], container action+0x28, add flag action+0x358.
 *     The slot is patched: run the original, then, only when this action adds the watched tag,
 *     look at the entity. Patched late (on the first SetPlayer call), because other payload dlls
 *     find the tag functions by reading this slot's code at start-up.
 *   Owner set: eActor::SetPlayer (vtable slot +0x1D0 of eActor and eHuman, exe+0x7EEEC0, same
 *     slots GEM TeamTags patches; the hooks chain). A unit tagged before it had an owner is done here.
 *   Human or bot: mp_player.cpp. One object per MP player: player number byte +4, bot byte +0x1E1
 *     (set from the lobby slot kind == 2 "bot", exe+0xD0E315). The engine's own "bless actor to
 *     player" (exe+0xD02740, string "An attempt of bless brainless actor") gives bots control AI and
 *     humans control user. Lookup = engine GetPlayerByNumber(game, uint8_t*) exe+0xD0C3D0 on the
 *     game singleton [exe+0x15A83C0] (getter exe+0xD0F070). Game and server exes only: the editor
 *     has no mp_player code, every owner is "unknown" there.
 *   Tags are changed through the {tags add} action's own functions (same derivation as TeamTags).
 * Simulation state: must run on every MP machine and on the dedicated server.
 *
 * Build: build.bat   Offline finder + logic test: build.bat test
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#define UC_VERSION "0.1"
#define UC_SECT    "UserControl"
#define MAXPL      0x12                    /* the engine's player numbers are 0..17, 0xFF = none */

typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;

/* ---------------------------------------------------------------- config */
static int      cfg_enabled = 1, cfg_skipbots = 1, cfg_unknown = 1, cfg_unowned = 0, cfg_late = 1, cfg_log = 1;
static uint32_t cfg_skipmask;
static char     cfg_tag[48] = "user_control", cfg_add[96] = "player _user";
static size_t   cfg_taglen = 12;
static int      cfg_addn = 2;

/* ---------------------------------------------------------------- log */
static char  g_logpath[MAX_PATH];
static FILE *g_log;
static int   g_lines;
static void logf_(const char *fmt, ...)
{
    if (g_lines > 5000) return;
#ifndef UC_TEST
    if (!g_log && g_logpath[0]) g_log = fopen(g_logpath, "w");
    if (!g_log) return;
    FILE *out = g_log;
#else
    FILE *out = stdout;
#endif
    va_list ap; va_start(ap, fmt);
    vfprintf(out, fmt, ap); fputc('\n', out); fflush(out);
    va_end(ap);
    g_lines++;
}

/* ---------------------------------------------------------------- PE helpers */
typedef struct { uint8_t *p; size_t n; } Sec;
static Sec g_text, g_rdata, g_data;
static uint8_t *g_base;

static int get_sections(uint8_t *base)
{
    g_base = base; g_text.p = g_rdata.p = g_data.p = NULL;
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        Sec s = { base + sh[i].VirtualAddress, sh[i].Misc.VirtualSize };
        if (!strncmp((char *)sh[i].Name, ".text", 8)) g_text = s;
        else if (!strncmp((char *)sh[i].Name, ".rdata", 8)) g_rdata = s;
        else if (!strncmp((char *)sh[i].Name, ".data", 8)) g_data = s;
    }
    return g_text.p && g_rdata.p && g_data.p;
}
static int in_text(const void *a)  { return (uint8_t *)a >= g_text.p  && (uint8_t *)a < g_text.p + g_text.n; }
static int in_rdata(const void *a) { return (uint8_t *)a >= g_rdata.p && (uint8_t *)a < g_rdata.p + g_rdata.n; }
static int in_data(const void *a)  { return (uint8_t *)a >= g_data.p  && (uint8_t *)a < g_data.p + g_data.n; }
#define OFF(a) ((unsigned long long)((uint8_t *)(a) - g_base))

static uint8_t *find_bytes(uint8_t *p, size_t n, const void *pat, size_t m)
{
    const uint8_t *q = pat;
    if (m > n) return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (p[i] == q[0] && !memcmp(p + i, q, m)) return p + i;
    return NULL;
}
static uint8_t *rel32(uint8_t *at, int len) { return at + len + *(int32_t *)(at + len - 4); }

static uint8_t *next_call(uint8_t *p, size_t window, uint8_t **site)
{
    for (size_t i = 0; i < window; i++)
        if (p[i] == 0xE8) {
            uint8_t *t = rel32(p + i, 5);
            if (in_text(t)) { if (site) *site = p + i; return t; }
        }
    return NULL;
}

/* k-th 'lea reg,[rip+disp]' in .text whose target == want */
static uint8_t *find_lea_to(const void *want, int which)
{
    int k = 0;
    for (size_t i = 0; i + 7 <= g_text.n; i++) {
        uint8_t *p = g_text.p + i;
        if ((p[0] & 0xF8) != 0x48 || p[1] != 0x8D || (p[2] & 0xC7) != 0x05) continue;
        if (rel32(p, 7) == want && k++ == which) return p;
    }
    return NULL;
}

/* string in .rdata starting at a string boundary (prefix match when !whole) */
static uint8_t *find_str(const char *s, int whole)
{
    size_t m = strlen(s) + (whole ? 1 : 0);
    for (uint8_t *p = g_rdata.p; (p = find_bytes(p, g_rdata.p + g_rdata.n - p, s, m)); p++)
        if (p == g_rdata.p || p[-1] == 0) return p;
    return NULL;
}

/* byte signature with wildcards (mask 0 = any) in .text: first match, *count = number of matches */
static uint8_t *find_sig(const uint8_t *pat, const uint8_t *mask, size_t m, int *count)
{
    uint8_t *first = NULL; int n = 0;
    for (size_t i = 0; i + m <= g_text.n; i++) {
        const uint8_t *p = g_text.p + i;
        if (p[0] != pat[0]) continue;
        size_t k = 1;
        while (k < m && (!mask[k] || p[k] == pat[k])) k++;
        if (k == m) { if (!first) first = (uint8_t *)p; n++; }
    }
    *count = n;
    return first;
}

/* vtable of a class by its RTTI name (complete object locator, offset 0) */
static void **find_vtable(const char *rtti)
{
    size_t n = strlen(rtti) + 1;
    uint8_t *td = NULL;
    for (uint8_t *p = g_data.p; p + n <= g_data.p + g_data.n; p++)
        if (*p == '.' && !memcmp(p, rtti, n)) { td = p - 0x10; break; }
    if (!td) return NULL;
    uint32_t tdr = (uint32_t)(td - g_base);
    for (uint8_t *p = g_rdata.p; p + 24 <= g_rdata.p + g_rdata.n; p += 4) {
        uint32_t *col = (uint32_t *)p;
        if (col[0] != 1 || col[1] != 0 || col[3] != tdr || col[5] != (uint32_t)(p - g_base)) continue;
        uint64_t va = (uint64_t)(uintptr_t)p;
        for (uint8_t *q = g_rdata.p; q + 16 <= g_rdata.p + g_rdata.n; q += 8)
            if (*(uint64_t *)q == va) return (void **)(q + 8);
    }
    return NULL;
}

/* ---------------------------------------------------------------- engine bindings */
typedef void     (*SetPlayerFn)(void *e, uint8_t *player);
typedef uint64_t (*TagsExecFn)(void *action, void *ctx, void *c, void *d);
typedef void    *(*TcCtorFn)(void *c);
typedef void    *(*TcCopyFn)(void *dst, const void *src);
typedef void     (*TcAllFn)(void *dst, const void *src);
typedef void     (*TcFromStrFn)(void *c, const MsString *s, char sep);
typedef void     (*SetTagsFn)(void *e, const void *c);
typedef void     (*TcDtorFn)(void *c);
typedef uint8_t *(*GetPlayerFn)(void *game, uint8_t *player);

#define TC_SELF_A  0x308
#define TC_SELF_B  0x310
#define TC_VEC_BEG 0x318
#define TC_VEC_END 0x320
#define TC_SIZE    0x330
#define TAG_SIZE   0x60
#define TAG_STR    0x40

static uint32_t o_player, o_setslot, o_tags, o_actcont, o_addflag, o_brain, o_ctl, o_eflags, o_bot;
static uint8_t  m_actor;
static void   **vt_actor, **vt_human, **vt_tags;
static TagsExecFn  f_tagsexec;
static TcCtorFn    f_tcctor;
static TcCopyFn    f_tccopy;
static TcAllFn     f_addall, f_rmall;
static TcFromStrFn f_fromstr;
static SetTagsFn   f_settags;
static TcDtorFn    f_tcdtor;
static GetPlayerFn f_getplayer;
static void      **g_gameslot;
static uint8_t    *f_isuser, *f_bless;
static const char *g_mp_why;

/* {player N}: eAction_Player Execute tail-jumps to its own slot, which does
   movzx eax,[action+..] ; cmp al,[r8+PLAYER] ; je ; ... ; call [rax+SETSLOT].
   The eActor / eHuman slots themselves may already hold another dll's hook (GEM TeamTags): fine. */
static const char *find_player(void)
{
    void **vt = find_vtable(".?AUeAction_Player@Interaction@@");
    if (!vt || !in_text(vt[1])) return "eAction_Player vtable not found";
    uint8_t *ex = vt[1], *f = NULL;
    for (uint8_t *p = ex; p < ex + 0x60 && !f; p++)       /* game: jmp [rax+0x48] (tail call), editor: call [rax+0x48] */
        if (p[0] == 0xFF && (p[1] == 0x60 || p[1] == 0x50) && !(p[2] & 7) && p[2] >= 0x18) f = vt[p[2] / 8];
    if (!f) f = ex;
    if (!in_text(f)) return "eAction_Player apply not in .text";
    for (uint8_t *p = f; p < f + 0x60; p++) {
        int r = (p[0] & 0xF0) == 0x40;                                   /* optional REX */
        uint8_t op = p[r], m = p[r + 1];
        if (!o_player && (op == 0x3A || op == 0x38) && (m & 0xC0) == 0x80 && (m & 7) != 4) {
            uint32_t d = *(uint32_t *)(p + r + 2);
            if (d > 0x100 && d < 0x4000) o_player = d;
        }
        if (o_player && !o_setslot && p[0] == 0xFF && (p[1] & 0xF8) == 0x90 && (p[1] & 7) != 4) {
            uint32_t d = *(uint32_t *)(p + 2);
            if (d >= 0x40 && d < 0x800 && !(d & 7)) o_setslot = d;
        }
    }
    if (!o_player) return "player byte offset not found";
    if (!o_setslot) return "SetPlayer slot not found";
    vt_actor = find_vtable(".?AVeActor@@");
    vt_human = find_vtable(".?AVeHuman@@");
    if (!vt_actor || !vt_human) return "eActor/eHuman vtables not found";
    uint8_t *sp = vt_actor[o_setslot / 8];
    if (in_text(sp)) {                                                   /* not hooked yet: it must store the player byte */
        int stores = 0;
        for (uint8_t *p = sp; p < sp + 0x140; p++) {
            int r = (p[0] & 0xF0) == 0x40;
            if (p[r] == 0x88 && (p[r + 1] & 0xC0) == 0x80 && (p[r + 1] & 7) != 4 && *(uint32_t *)(p + r + 2) == o_player) stores++;
        }
        if (!stores) return "SetPlayer does not write the player byte";
    }
    return NULL;
}

/* the {tags add|remove} action: table entry {"tags", ctor} in .data */
static const char *find_tags(void)
{
    uint8_t *ctor = NULL; int hits = 0;
    for (size_t i = 0; i + 16 <= g_data.n; i += 8) {
        uint8_t *str = *(uint8_t **)(g_data.p + i), *fn = *(uint8_t **)(g_data.p + i + 8);
        if (!in_rdata(str) || !in_text(fn)) continue;
        if (memcmp(str, "tags", 5) || str[-1] != 0) continue;
        ctor = fn; hits++;
    }
    if (hits != 1) return hits ? "tags action entry ambiguous" : "tags action entry not found";
    /* ctor: ... lea rax,[rip+vtable] ; mov [rbx],rax ; call TagContainer::ctor(rbx+0x28) */
    uint8_t *vt = NULL, *after = NULL;
    for (uint8_t *p = ctor; p < ctor + 0x80; p++)
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05 && in_rdata(rel32(p, 7))) { vt = rel32(p, 7); after = p + 7; break; }
    if (!vt) return "tags action vtable not found";
    vt_tags = (void **)vt;
    static const uint8_t ctor_sig[] = { 0xC6, 0x81, 0x00, 0x03, 0x00, 0x00, 0x00 };            /* mov byte [rcx+0x300],0 */
    uint8_t *cc = next_call(after, 0x20, NULL);
    if (!cc || !find_bytes(cc, 0x20, ctor_sig, sizeof ctor_sig)) return "TagContainer ctor not recognised";
    f_tcctor = (TcCtorFn)cc;

    /* Execute (slot 1): mov rdx,[ctx] ; add rdx,TAGS ; call copy ; cmp byte [action+ADD],0 ; lea rdx,[action+CONT] ;
       je REMOVE ; call add_all ; ... call remove_all ; lea rdx,[rsp+0x20] ; call SetTags ; lea rcx,[rsp+0x20] ; call dtor */
    uint8_t *ev = ((uint8_t **)vt)[1], *p = NULL, *site = NULL, *addp = NULL;
    if (!in_text(ev)) return "tags action execute not in .text (already hooked?)";
    f_tagsexec = (TagsExecFn)ev;
    for (uint8_t *q = ev; q < ev + 0x100; q++)
        if (q[0] == 0x48 && q[1] == 0x81 && q[2] == 0xC2) { o_tags = *(uint32_t *)(q + 3); addp = q; p = q + 7; break; }
    if (!p || o_tags < 0x100 || o_tags > 0x400) return "entity tag container offset not found";
    int deref = 0;                                                       /* entity = [ctx]: mov rdx,[rdx] / mov rdx,[rbx] */
    for (uint8_t *q = ev; q < addp; q++)
        if (q[0] == 0x48 && q[1] == 0x8B && (q[2] == 0x12 || q[2] == 0x13)) deref = 1;
    if (!deref) return "tags action: entity is not [ctx]";
    uint8_t *cp = next_call(p, 0x20, &site);
    if (!cp || !find_bytes(cp, 0x40, ctor_sig, sizeof ctor_sig)) return "TagContainer copy not recognised";
    f_tccopy = (TcCopyFn)cp;
    uint8_t *rmv = NULL; p = NULL;
    for (uint8_t *q = site; q < ev + 0x200 && !p; q++)
        if (q[0] == 0x80 && (q[1] & 0xF8) == 0xB8 && q[6] == 0x00) {
            o_addflag = *(uint32_t *)(q + 2);
            for (uint8_t *j = q + 7; j < q + 0x20; j++)
                if (j[0] == 0x74) { p = j + 2; rmv = j + 2 + (int8_t)j[1]; break; }
        }
    if (!p) return "tags add/remove switch not found";
    for (uint8_t *q = site; q < p; q++)                                  /* lea rdx,[rbx+d8] / [rdi+d8] */
        if (q[0] == 0x48 && q[1] == 0x8D && (q[2] == 0x53 || q[2] == 0x57)) o_actcont = q[3];
    if (!o_actcont || o_addflag != o_actcont + TC_SIZE) return "tags action layout not recognised";
    static const uint8_t aa_sig[] = { 0x48, 0x8B, 0x9A, 0x18, 0x03, 0x00, 0x00 };             /* mov rbx,[rdx+0x318] */
    uint8_t *aa = next_call(p, 0x10, &site);
    if (!aa || !find_bytes(aa, 0x20, aa_sig, sizeof aa_sig)) return "add_all not recognised";
    f_addall = (TcAllFn)aa;
    uint8_t *ra = (rmv && in_text(rmv) && rmv[0] == 0xE8) ? rel32(rmv, 5) : NULL;
    if (!ra || ra == aa || !in_text(ra) || !find_bytes(ra, 0x20, aa_sig, sizeof aa_sig)) return "remove_all not recognised";
    f_rmall = (TcAllFn)ra;
    static const uint8_t lea_rdx[] = { 0x48, 0x8D, 0x54, 0x24, 0x20 }, lea_rcx[] = { 0x48, 0x8D, 0x4C, 0x24, 0x20 };
    p = find_bytes(site + 5, 0x80, lea_rdx, sizeof lea_rdx);
    uint8_t *st = p ? next_call(p + 5, 0x10, &site) : NULL;
    static const uint8_t st_sig[] = { 0xE8, 0x04, 0x00, 0x00 };                                /* [rcx+0x4e8] */
    if (!st || !find_bytes(st, 0x30, st_sig, sizeof st_sig)) return "SetTags not recognised";
    f_settags = (SetTagsFn)st;
    p = find_bytes(site + 5, 0x40, lea_rcx, sizeof lea_rcx);
    uint8_t *dt = p ? next_call(p + 5, 0x10, NULL) : NULL;
    static const uint8_t dt_sig[] = { 0x48, 0x81, 0xC1, 0x10, 0x03, 0x00, 0x00 };             /* add rcx,0x310 */
    if (!dt || memcmp(dt, dt_sig, sizeof dt_sig)) return "TagContainer dtor not recognised";
    f_tcdtor = (TcDtorFn)dt;

    /* parse (slot 5): ... mov r8b,0x20 ; call FromString(container, std::string*, ' ') */
    uint8_t *ps = ((uint8_t **)vt)[5], *fs = NULL;
    if (!in_text(ps)) return "tags action parse not in .text";
    for (uint8_t *q = ps; q < ps + 0x180 && !fs; q++)
        if (q[0] == 0x41 && q[1] == 0xB0 && q[2] == 0x20) fs = next_call(q + 3, 0x10, NULL);
    if (!fs) return "FromString not found";
    int clears = 0;                                                       /* mov reg,[rcx+0x318] early on */
    for (uint8_t *q = fs; q < fs + 0x40; q++)
        if ((q[0] & 0xFB) == 0x48 && q[1] == 0x8B && (q[2] & 0xC7) == 0x81 && *(uint32_t *)(q + 3) == TC_VEC_BEG) clears = 1;
    if (!clears) return "FromString not recognised";
    f_fromstr = (TcFromStrFn)fs;
    return NULL;
}

/* {if user_control}: eCondition_UserControl slot 1 = test byte [e+FLAGS],ACTOR ; slot 3 = mov rcx,r8 ; jmp IsUserControl
   IsUserControl: mov rax,[rcx+BRAIN] ; test rax,rax ; je ; mov eax,[rax+CTL] ; and al,3 ; cmp al,1 */
static const char *find_control(void)
{
    void **vt = find_vtable(".?AUeCondition_UserControl@Interaction@@");
    if (!vt || !in_text(vt[1]) || !in_text(vt[3])) return "eCondition_UserControl vtable not found";
    uint8_t *s1 = vt[1], *s3 = vt[3];
    for (uint8_t *q = s1; q < s1 + 0x60 && !o_eflags; q++)
        if (q[0] == 0x41 && q[1] == 0xF6 && q[2] == 0x40 && q[4] && !(q[4] & (q[4] - 1))) { o_eflags = q[3]; m_actor = q[4]; }
    if (!o_eflags) return "actor flag test not found";
    if (s3[0] != 0x49 || s3[1] != 0x8B || s3[2] != 0xC8 || s3[3] != 0xE9) return "user_control check not recognised";
    uint8_t *f = rel32(s3 + 3, 5);
    static const uint8_t pat[]  = { 0x48, 0x8B, 0x81, 0, 0, 0, 0, 0x48, 0x85, 0xC0, 0x74, 0, 0x8B, 0x80, 0, 0, 0, 0, 0x24, 0x03, 0x3C, 0x01 };
    static const uint8_t mask[] = { 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1 };
    if (!in_text(f)) return "IsUserControl not in .text";
    for (size_t k = 0; k < sizeof pat; k++)
        if (mask[k] && f[k] != pat[k]) return "IsUserControl not recognised";
    o_brain = *(uint32_t *)(f + 3);
    o_ctl   = *(uint32_t *)(f + 14);
    if (o_brain < 0x400 || o_brain > 0x4000 || o_ctl < 0x40 || o_ctl > 0x2000) return "brain / control offsets out of range";
    f_isuser = f;
    return NULL;
}

/* mp_player.cpp "bless actor to player": cmp byte [mp+BOT],0 ; and dword [brain+CTL],~3 (bot = AI) ... call GetGame.
   Optional: the editor exe has no mp_player code. */
static const char *find_mp(void)
{
    uint8_t *s = find_str("ERROR: An attempt of bless brainless actor", 0);
    if (!s) return "no mp_player code in this exe";
    uint8_t *lea = find_lea_to(s, 0);
    if (!lea) return "bless string not referenced";
    uint8_t *w = lea - 0x180, *andp = NULL;
    for (uint8_t *q = w; q < lea && !andp; q++)                           /* and dword [reg+CTL],0xFFFFFFFC */
        if (q[0] == 0x83 && (q[1] & 0xF8) == 0xA0 && (q[1] & 7) != 4 && *(uint32_t *)(q + 2) == o_ctl && q[6] == 0xFC) andp = q;
    if (!andp) return "bless: control write not found";
    for (uint8_t *q = andp - 0x20; q < andp && !o_bot; q++)               /* cmp byte [reg+BOT],0 right before it */
        if (q[0] == 0x80 && (q[1] & 0xF8) == 0xB8 && (q[1] & 7) != 4 && q[6] == 0) {
            uint32_t d = *(uint32_t *)(q + 2);
            if (d >= 0x20 && d < 0x1000) o_bot = d;
        }
    if (!o_bot) return "bless: bot byte not found";
    for (uint8_t *q = andp; q < lea && !g_gameslot; q++)                  /* call GetGame: mov rax,[rip+g] ; ret */
        if (q[0] == 0xE8) {
            uint8_t *t = rel32(q, 5);
            if (in_text(t) && t[0] == 0x48 && t[1] == 0x8B && t[2] == 0x05 && t[7] == 0xC3 && in_data(rel32(t, 7)))
                g_gameslot = (void **)rel32(t, 7);
        }
    if (!g_gameslot) return "bless: game singleton not found";
    for (uint8_t *q = andp; q > w; q--)                                   /* function start, for the log only */
        if (q[-1] == 0xCC && !((uintptr_t)q & 15)) { f_bless = q; break; }
    /* GetPlayerByNumber(game, uint8_t*): movzx r8d,[rdx] ; mov r9,rcx ; cmp r8b,0x12 ; jae ; mov rax,[rcx+A] ;
       mov rdx,[rcx+A+8] ; cmp rax,rdx ; je ; mov rcx,[rax] ; cmp [rcx+4],r8b */
    static const uint8_t pat[]  = { 0x44, 0x0F, 0xB6, 0x02, 0x4C, 0x8B, 0xC9, 0x41, 0x80, 0xF8, MAXPL, 0x73, 0,
                                    0x48, 0x8B, 0x81, 0, 0, 0, 0, 0x48, 0x8B, 0x91, 0, 0, 0, 0, 0x48, 0x3B, 0xC2, 0x74, 0,
                                    0x48, 0x8B, 0x08, 0x44, 0x38, 0x41, 0x04 };
    static const uint8_t mask[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0,
                                    1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1, 1, 0,
                                    1, 1, 1, 1, 1, 1, 1 };
    int n = 0;
    uint8_t *gp = find_sig(pat, mask, sizeof pat, &n);
    if (n != 1) { g_gameslot = NULL; return n ? "GetPlayerByNumber ambiguous" : "GetPlayerByNumber not found"; }
    f_getplayer = (GetPlayerFn)gp;
    return NULL;
}

static const char *find_all(uint8_t *base)
{
    o_player = o_setslot = o_tags = o_actcont = o_addflag = o_brain = o_ctl = o_eflags = o_bot = 0;
    g_gameslot = NULL; f_getplayer = NULL; f_bless = NULL; g_mp_why = NULL;
    if (!get_sections(base)) return "sections not found";
    const char *e;
    if ((e = find_player())) return e;
    if ((e = find_tags())) return e;
    if ((e = find_control())) return e;
    if ((e = find_mp())) { g_mp_why = e; g_gameslot = NULL; f_getplayer = NULL; }
    return NULL;
}

/* ---------------------------------------------------------------- tags */
static const char *ms_cstr(const MsString *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }
static void ms_make(MsString *s, char *text)
{
    memset(s, 0, sizeof *s);
    s->size = strlen(text);
    if (s->size < 16) { memcpy(s->u.buf, text, s->size + 1); s->cap = 15; }
    else { s->u.ptr = text; s->cap = s->size; }                          /* read only: the engine copies it */
}
static int container_ok(const uint8_t *c) { return *(const uint8_t *const *)(c + TC_SELF_A) == c && *(const uint8_t *const *)(c + TC_SELF_B) == c; }
static size_t tag_count(const uint8_t *c) { return (size_t)(*(uint8_t *const *)(c + TC_VEC_END) - *(uint8_t *const *)(c + TC_VEC_BEG)) / TAG_SIZE; }

/* the container holds the watched tag (exact name) */
static int has_tag(const uint8_t *c)
{
    const uint8_t *b = *(uint8_t *const *)(c + TC_VEC_BEG), *e = *(uint8_t *const *)(c + TC_VEC_END);
    if (!b || e < b || (size_t)(e - b) > 4096 * TAG_SIZE) return 0;
    for (const uint8_t *t = b; t < e; t += TAG_SIZE) {
        const MsString *s = (const MsString *)(t + TAG_STR);
        if (s->size == cfg_taglen && !memcmp(ms_cstr(s), cfg_tag, cfg_taglen)) return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------- the rule */
enum { K_UNKNOWN, K_HUMAN, K_BOT };
enum { D_WAIT, D_SKIP, D_GIVE };
static const char *const k_name[] = { "unknown", "human", "bot" };

/* What happens to a tagged unit.
   D_GIVE: control user + AddTags, tag removed.  D_SKIP: tag removed, nothing else.  D_WAIT: left as it is
   (no owner yet: the SetPlayer hook comes back to it). */
static int decide(int actor, int owner, int kind)
{
    if (!actor) return D_GIVE;                                           /* no owner to ask: tags only, as the trigger did */
    if (owner >= MAXPL) return cfg_unowned ? D_GIVE : D_WAIT;
    if (cfg_skipmask >> owner & 1) return D_SKIP;
    if (kind == K_BOT) return cfg_skipbots ? D_SKIP : D_GIVE;
    if (kind == K_UNKNOWN) return cfg_unknown ? D_GIVE : D_SKIP;
    return D_GIVE;
}

static uint32_t control_user(uint32_t flags) { return (flags & ~3u) | 1u; }

#ifndef UC_TEST
static int readable(const void *p, size_t n)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!p || !VirtualQuery(p, &mi, sizeof mi)) return 0;
    if (mi.State != MEM_COMMIT || (mi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mi.Protect & 0xEE)) return 0;
    return (const uint8_t *)p + n <= (const uint8_t *)mi.BaseAddress + mi.RegionSize;
}

/* owner kind from the engine's MP player list (rare call: once per handed-over unit) */
static int player_kind(int p)
{
    if (!f_getplayer || !g_gameslot) return K_UNKNOWN;
    void *g = *g_gameslot;
    if (!readable(g, 0x200)) return K_UNKNOWN;
    uint8_t b = (uint8_t)p, *mp = f_getplayer(g, &b);
    if (!readable(mp, o_bot + 1)) return K_UNKNOWN;
    return mp[o_bot] ? K_BOT : K_HUMAN;
}

static __attribute__((aligned(16))) uint8_t g_store[2][TC_SIZE + 16];
static uint8_t *g_ctag, *g_cadd;
static volatile LONG g_broken, g_done[3];
static CRITICAL_SECTION g_cs;

/* containers built on first use (game thread, engine fully up), kept for the process lifetime */
static int build(uint8_t *slot, char *text, size_t want)
{
    MsString s; ms_make(&s, text);
    f_tcctor(slot);
    f_fromstr(slot, &s, ' ');
    if (!container_ok(slot) || tag_count(slot) != want) {
        logf_("[GEM UserControl] built \"%.60s\" has %zu tags, expected %zu: disabled", text, tag_count(slot), want);
        return 0;
    }
    return 1;
}
static int ensure(void)
{
    if (g_broken) return 0;
    if (g_ctag && g_cadd) return 1;
    int ok = 1;
    EnterCriticalSection(&g_cs);
    if (!g_ctag) { if (build(g_store[0], cfg_tag, 1)) g_ctag = g_store[0]; else ok = 0; }
    if (ok && !g_cadd) { if (build(g_store[1], cfg_add, (size_t)cfg_addn)) g_cadd = g_store[1]; else ok = 0; }
    if (!ok) g_broken = 1;
    LeaveCriticalSection(&g_cs);
    return ok;
}

/* a unit that may carry the tag: decide and apply */
static void check(uint8_t *e, const char *why)
{
    if (!cfg_enabled || g_broken || !e) return;
    uint8_t *c = e + o_tags;
    if (!container_ok(c) || !has_tag(c)) return;
    int actor = (e[o_eflags] & m_actor) != 0;
    int owner = actor ? e[o_player] : 0xFF;
    int kind  = (actor && owner < MAXPL) ? player_kind(owner) : K_UNKNOWN;
    int d = decide(actor, owner, kind);
    if (d == D_WAIT) {
        if (cfg_log) logf_("%s: entity %p has no owner yet: waiting for it", why, (void *)e);
        return;
    }
    if (!ensure()) return;
    uint8_t *brain = actor ? *(uint8_t **)(e + o_brain) : NULL;
    uint32_t before = 0, after = 0;
    if (d == D_GIVE && brain) {
        uint32_t *fl = (uint32_t *)(brain + o_ctl);
        before = *fl; after = control_user(before);
        if (after != before) *fl = after;
    }
    uint8_t raw[TC_SIZE + 32], *tmp = (uint8_t *)(((uintptr_t)raw + 15) & ~(uintptr_t)15);
    f_tccopy(tmp, c);
    if (d == D_GIVE) f_addall(tmp, g_cadd);
    f_rmall(tmp, g_ctag);
    f_settags(e, tmp);
    f_tcdtor(tmp);
    InterlockedIncrement(&g_done[d]);
    if (!cfg_log) return;
    if (d == D_GIVE)
        logf_("%s: entity %p owner %d (%s)%s -> control user (brain flags %08X -> %08X), +\"%s\", -\"%s\"", why, (void *)e,
              owner, k_name[kind], actor ? (brain ? "" : " no brain") : " not an actor", before, after, cfg_add, cfg_tag);
    else
        logf_("%s: entity %p owner %d (%s) skipped (%s) -> -\"%s\" only", why, (void *)e, owner, k_name[kind],
              (cfg_skipmask >> owner & 1) ? "SkipPlayers" : kind == K_BOT ? "SkipBots" : "UnknownOwner=0", cfg_tag);
}

/* ---------------------------------------------------------------- hooks */
static SetPlayerFn orig_set_actor, orig_set_human;
static TagsExecFn  orig_tags;
static volatile LONG g_tagshook;

static int patch_slot(void **slot, void *hook)
{
    DWORD old;
    if (!VirtualProtect(slot, sizeof *slot, PAGE_READWRITE, &old)) return 0;
    InterlockedExchangePointer(slot, hook);
    VirtualProtect(slot, sizeof *slot, old, &old);
    return 1;
}

/* {tags add ...}: only an action that adds the watched tag costs more than one byte test and a short scan
   of the action's own tag list (1-3 tags) */
static uint64_t hk_tags(void *action, void *ctx, void *c, void *d)
{
    uint64_t r = orig_tags(action, ctx, c, d);
    if (((uint8_t *)action)[o_addflag] && has_tag((uint8_t *)action + o_actcont))
        check(*(uint8_t **)ctx, "tag");
    return r;
}
static void hook_tags(void)
{
    if (InterlockedCompareExchange(&g_tagshook, 1, 0)) return;
    void *cur = vt_tags[1];
    orig_tags = (TagsExecFn)cur;                                         /* whatever is there now: chains with a later hook */
    if (patch_slot(&vt_tags[1], (void *)hk_tags))
        logf_("[GEM UserControl] {tags} hook installed (slot was exe+0x%llX%s)", OFF(cur), cur == (void *)f_tagsexec ? "" : ", another hook: chained");
    else
        logf_("[GEM UserControl] {tags} hook FAILED (%lu): only units tagged before they get an owner are handled", GetLastError());
}

static void set_common(void *e)
{
    if (!g_tagshook) hook_tags();
    check((uint8_t *)e, "owner");
}
static void hk_set_actor(void *e, uint8_t *pl) { orig_set_actor(e, pl); set_common(e); }
static void hk_set_human(void *e, uint8_t *pl) { orig_set_human(e, pl); set_common(e); }

/* ---------------------------------------------------------------- config */
static void lower(char *s) { for (; *s; s++) *s = (char)tolower((unsigned char)*s); }
static void load_ini(const char *ini)
{
    char buf[128];
    cfg_enabled  = GetPrivateProfileIntA(UC_SECT, "Enabled", 1, ini);
    cfg_skipbots = GetPrivateProfileIntA(UC_SECT, "SkipBots", 1, ini);
    cfg_unknown  = GetPrivateProfileIntA(UC_SECT, "UnknownOwner", 1, ini);
    cfg_unowned  = GetPrivateProfileIntA(UC_SECT, "NoOwner", 0, ini);
    cfg_late     = GetPrivateProfileIntA(UC_SECT, "LateTagsHook", 1, ini);
    cfg_log      = GetPrivateProfileIntA(UC_SECT, "Log", 1, ini);
    GetPrivateProfileStringA(UC_SECT, "Tag", cfg_tag, buf, sizeof buf, ini);
    if (buf[0] && strlen(buf) < sizeof cfg_tag && !strchr(buf, ' ')) { lower(buf); strcpy(cfg_tag, buf); }
    cfg_taglen = strlen(cfg_tag);
    GetPrivateProfileStringA(UC_SECT, "AddTags", cfg_add, buf, sizeof buf, ini);
    if (buf[0] && strlen(buf) < sizeof cfg_add) {
        for (char *p = buf; *p; p++) if (*p == ',' || *p == '\t') *p = ' ';
        char out[sizeof cfg_add]; size_t n = 0; int words = 0;
        for (char *t = strtok(buf, " "); t; t = strtok(NULL, " ")) {     /* single spaces: the count must match the parser's */
            n += snprintf(out + n, sizeof out - n, "%s%s", words ? " " : "", t); words++;
        }
        if (words) { strcpy(cfg_add, out); cfg_addn = words; }           /* case kept: the trigger's tags are "player" and "_user" */
    }
    GetPrivateProfileStringA(UC_SECT, "SkipPlayers", "", buf, sizeof buf, ini);
    cfg_skipmask = 0;
    for (char *t = strtok(buf, ", "); t; t = strtok(NULL, ", ")) {
        int v = atoi(t);
        if (isdigit((unsigned char)t[0]) && v >= 0 && v < MAXPL) cfg_skipmask |= 1u << v;
    }
}

static void install(void)
{
    const char *err = find_all((uint8_t *)GetModuleHandleW(NULL));
    if (err) { logf_("[GEM UserControl %s] NOT installed: %s", UC_VERSION, err); return; }
    if (!cfg_enabled) { logf_("[GEM UserControl %s] Enabled=0: not installed", UC_VERSION); return; }
    InitializeCriticalSection(&g_cs);
    void *sa = vt_actor[o_setslot / 8], *sh = vt_human[o_setslot / 8];
    orig_set_actor = (SetPlayerFn)sa; orig_set_human = (SetPlayerFn)sh;
    int ok = patch_slot(&vt_actor[o_setslot / 8], (void *)hk_set_actor) && patch_slot(&vt_human[o_setslot / 8], (void *)hk_set_human);
    if (!ok) { logf_("[GEM UserControl] VirtualProtect failed (%lu)", GetLastError()); return; }
    logf_("[GEM UserControl %s] installed | SetPlayer slot +0x%X (actor was exe+0x%llX%s, human exe+0x%llX), player +0x%X | "
          "{tags} execute exe+0x%llX (container +0x%X, add flag +0x%X), entity tags +0x%X | IsUserControl exe+0x%llX: "
          "actor flag [+0x%X]&%u, brain +0x%X, control +0x%X | tag functions ctor exe+0x%llX copy exe+0x%llX add exe+0x%llX "
          "remove exe+0x%llX SetTags exe+0x%llX dtor exe+0x%llX FromString exe+0x%llX",
          UC_VERSION, o_setslot, OFF(sa), in_text(sa) ? "" : " = another dll's hook, chained", OFF(sh), o_player,
          OFF(f_tagsexec), o_actcont, o_addflag, o_tags, OFF(f_isuser), o_eflags, m_actor, o_brain, o_ctl,
          OFF(f_tcctor), OFF(f_tccopy), OFF(f_addall), OFF(f_rmall), OFF(f_settags), OFF(f_tcdtor), OFF(f_fromstr));
    if (f_getplayer)
        logf_("[GEM UserControl] bot check ON | bless exe+0x%llX, bot byte +0x%X, game singleton exe+0x%llX, GetPlayerByNumber exe+0x%llX",
              f_bless ? OFF(f_bless) : 0ULL, o_bot, OFF(g_gameslot), OFF(f_getplayer));
    else
        logf_("[GEM UserControl] bot check OFF (%s): every owner is \"unknown\" (UnknownOwner=%d)", g_mp_why ? g_mp_why : "?", cfg_unknown);
    logf_("[GEM UserControl] Tag=%s AddTags=\"%s\" SkipBots=%d SkipPlayers mask 0x%X UnknownOwner=%d NoOwner=%d LateTagsHook=%d Log=%d",
          cfg_tag, cfg_add, cfg_skipbots, cfg_skipmask, cfg_unknown, cfg_unowned, cfg_late, cfg_log);
    if (!cfg_late) hook_tags();
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
        install();
    }
    return TRUE;
}
#else
/* ---------------------------------------------------------------- offline test */
/* build.bat test -> uc_test.exe <exe>: the finders on one mapped exe (one exe per process), then the
   decision table, the control-bit write and the tag scan on fake containers. Engine functions are not called. */
static uint8_t g_fake[TC_SIZE + 16], g_tags[8][TAG_SIZE];
static void fake_set(int n, const char **names)
{
    uint8_t *c = g_fake;
    *(uint8_t **)(c + TC_SELF_A) = c; *(uint8_t **)(c + TC_SELF_B) = c;
    for (int i = 0; i < n; i++) ms_make((MsString *)(g_tags[i] + TAG_STR), (char *)names[i]);
    *(uint8_t **)(c + TC_VEC_BEG) = g_tags[0]; *(uint8_t **)(c + TC_VEC_END) = g_tags[n];
}

int main(int argc, char **argv)
{
    int bad = 0;
    if (argc > 1) {
        HMODULE m = LoadLibraryExA(argv[1], NULL, DONT_RESOLVE_DLL_REFERENCES);
        if (!m) { printf("%s: load failed %lu\n", argv[1], GetLastError()); return 1; }
        const char *err = find_all((uint8_t *)m);
        if (err) { printf("%s: FAIL %s\n", argv[1], err); return 1; }
        const char *nm = strrchr(argv[1], '\\'); nm = nm ? nm + 1 : argv[1];
        int editor = strstr(nm, "_ed") != NULL;
        printf("%s:\n  SetPlayer slot +0x%X = exe+0x%llX (human exe+0x%llX), player +0x%X\n"
               "  {tags} execute exe+0x%llX container +0x%X add flag +0x%X entity tags +0x%X\n"
               "  IsUserControl exe+0x%llX actor flag [+0x%X]&%u brain +0x%X control +0x%X\n"
               "  tags ctor exe+0x%llX copy exe+0x%llX add exe+0x%llX remove exe+0x%llX SetTags exe+0x%llX dtor exe+0x%llX FromString exe+0x%llX\n",
               nm, o_setslot, OFF(vt_actor[o_setslot / 8]), OFF(vt_human[o_setslot / 8]), o_player,
               OFF(f_tagsexec), o_actcont, o_addflag, o_tags, OFF(f_isuser), o_eflags, m_actor, o_brain, o_ctl,
               OFF(f_tcctor), OFF(f_tccopy), OFF(f_addall), OFF(f_rmall), OFF(f_settags), OFF(f_tcdtor), OFF(f_fromstr));
        if (f_getplayer)
            printf("  bot check: bless exe+0x%llX bot byte +0x%X game singleton exe+0x%llX GetPlayerByNumber exe+0x%llX\n",
                   f_bless ? OFF(f_bless) : 0ULL, o_bot, OFF(g_gameslot), OFF(f_getplayer));
        else if (editor)
            printf("  bot check: not available (%s) - expected in the editor exe\n", g_mp_why);
        else { printf("  FAIL bot check: %s\n", g_mp_why); bad = 1; }
        if (f_getplayer && editor) printf("  note: editor exe has mp_player code after all\n");
        /* chain: GEM TeamTags loads first and patches the same SetPlayer slots; the finder must still pass */
        void **sa = &vt_actor[o_setslot / 8], **sh = &vt_human[o_setslot / 8], *keep_a = *sa, *keep_h = *sh;
        DWORD old; uint32_t slot = o_setslot, pl = o_player;
        if (VirtualProtect(sa, 8, PAGE_READWRITE, &old) && VirtualProtect(sh, 8, PAGE_READWRITE, &old)) {
            *sa = *sh = (void *)main;                                    /* any address outside the exe's .text */
            o_player = o_setslot = 0;
            const char *e2 = find_player();
            int ok = !e2 && o_setslot == slot && o_player == pl && vt_actor[slot / 8] == (void *)main;
            printf("  SetPlayer slots already hooked by another dll: %s %s\n", e2 ? e2 : "finder passes, slot kept for chaining", ok ? "OK" : "FAIL");
            if (!ok) bad = 1;
            *sa = keep_a; *sh = keep_h;
        } else { printf("  FAIL chain test: VirtualProtect %lu\n", GetLastError()); bad = 1; }
    }
    /* decision table: actor, owner, kind, SkipBots, UnknownOwner, NoOwner, SkipPlayers mask -> decision */
    struct { int actor, owner, kind, sb, unk, noown; uint32_t mask; int want; const char *what; } dc[] = {
        { 1, 1, K_HUMAN,   1, 1, 0, 0,     D_GIVE, "human owner" },
        { 1, 0, K_BOT,     1, 1, 0, 0,     D_SKIP, "bot owner, SkipBots=1" },
        { 1, 0, K_BOT,     0, 1, 0, 0,     D_GIVE, "bot owner, SkipBots=0 (the MP trigger)" },
        { 1, 3, K_UNKNOWN, 1, 1, 0, 0,     D_GIVE, "unknown owner (editor)" },
        { 1, 3, K_UNKNOWN, 1, 0, 0, 0,     D_SKIP, "unknown owner, UnknownOwner=0" },
        { 1, 0, K_HUMAN,   1, 1, 0, 1,     D_SKIP, "SkipPlayers=0 (the conquest trigger)" },
        { 1, 2, K_HUMAN,   1, 1, 0, 1,     D_GIVE, "SkipPlayers=0, owner 2" },
        { 1, 0xFF, K_UNKNOWN, 1, 1, 0, 0,  D_WAIT, "no owner yet" },
        { 1, 0xFF, K_UNKNOWN, 1, 1, 1, 0,  D_GIVE, "no owner, NoOwner=1" },
        { 0, 0xFF, K_UNKNOWN, 1, 0, 0, 0x3FFFF, D_GIVE, "not an actor: tags only" },
        { 1, 17, K_BOT,    1, 1, 0, 0,     D_SKIP, "player 17 bot" },
    };
    for (size_t i = 0; i < sizeof dc / sizeof *dc; i++) {
        cfg_skipbots = dc[i].sb; cfg_unknown = dc[i].unk; cfg_unowned = dc[i].noown; cfg_skipmask = dc[i].mask;
        int d = decide(dc[i].actor, dc[i].owner, dc[i].kind), ok = d == dc[i].want;
        printf("  decide %-40s -> %s %s\n", dc[i].what, d == D_GIVE ? "give" : d == D_SKIP ? "skip" : "wait", ok ? "OK" : "FAIL");
        if (!ok) bad = 1;
    }
    /* the trigger's write: low two bits := 1, the rest untouched */
    struct { uint32_t in, out; } bc[] = { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 }, { 0x00000C40, 0x00000C41 }, { 0xFFFFFFFE, 0xFFFFFFFD } };
    for (size_t i = 0; i < sizeof bc / sizeof *bc; i++) {
        int ok = control_user(bc[i].in) == bc[i].out;
        printf("  control %08X -> %08X %s\n", bc[i].in, control_user(bc[i].in), ok ? "OK" : "FAIL");
        if (!ok) bad = 1;
    }
    struct { int n; const char *t[5]; int has; } tc[] = {
        { 2, { "soldier", "user_control" }, 1 },
        { 3, { "user_control_x", "user_contro", "_user" }, 0 },
        { 1, { "User_Control" }, 0 },
        { 0, { 0 }, 0 },
        { 4, { "team3", "sensor_off", "a_tag_longer_than_15_chars", "user_control" }, 1 },
    };
    for (size_t i = 0; i < sizeof tc / sizeof *tc; i++) {
        fake_set(tc[i].n, tc[i].t);
        int h = has_tag(g_fake), ok = h == tc[i].has;
        printf("  tag scan case %zu: has=%d %s\n", i, h, ok ? "OK" : "FAIL");
        if (!ok) bad = 1;
    }
    /* cost of the {tags} hook's pre-check on an action that adds 3 other tags (the common case) */
    const char *three[] = { "bot_combat_cd", "bot_in_combat", "sensor_off" };
    fake_set(3, three);
    LARGE_INTEGER f, a, b; volatile int sink = 0;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    for (int i = 0; i < 20000000; i++) sink += has_tag(g_fake);
    QueryPerformanceCounter(&b);
    printf("  {tags} hook pre-check: %.1f ns per call (3-tag action, no match)\n",
           (double)(b.QuadPart - a.QuadPart) * 1e9 / (double)f.QuadPart / 20000000.0);
    printf(bad ? "SOME FAILED\n" : "ALL OK\n");
    return bad;
}
#endif
