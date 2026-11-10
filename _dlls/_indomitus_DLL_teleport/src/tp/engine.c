/* GEM Teleport - engine bindings and finders
 * Part of the unity build: #included by gem_teleport.c, not compiled on its own.
 *
 * Engine facts (GoH 1.067, all re-derived at startup; game exe addresses in the comments are for reading
 * the disassembly, nothing below depends on them):
 *
 * - Custom action instructions (eInterfaceActionInstruction.cpp): the {instructions ...} block parser walks a
 *   singly linked registry of nodes {next, entry*}, entry = {const char* name, Instruction* (*ctor)(void)}, whose
 *   head is a function-local static (accessor 0x140b5fc50: head 0x1415a0480, MSVC guard 0x1415a0488). Match =
 *   strcmp(word, entry->name) == 0 -> ctor() ; optional word "nocheck" -> byte [obj+8] = 1 ; vt[1](obj, reader) ;
 *   push into the action's vector. No match -> "Unknown instruction" (log 0xBAD0001, line 0x61).
 *   Instruction vtable: [0] deleting dtor(this, flags) [1] parse(this, reader) [2] bool ready(this)
 *   [3] bool execute(this, action, order, entity, Target*, uint8_t* flagF0).
 * - The action (Interface::Action::eCustom, RTTI) keeps its instructions in a std::vector<Instruction*> at
 *   action+0x4A8..0x4B0. Its vtable slot 15 = bool IsApplicable(this, uint8_t* flag, entity, Target*) (used for
 *   the cursor and re-checked every tick by the running order), slot 17 = Execute (creates the order, GiveOrder).
 * - Target (0x90 bytes): int type at +0 (1 = ground, 2 = entity), float pos[3] at +4, entity ref at +0x20.
 * - Order::eCustomActionOrder (0x108, RTTI) runs the instructions one by one (vtable slot 19 = Update(this, phase)).
 *   eSingleOrder base (0x38): +0x08 actor (= entity+0xAF0, the eOrdered sub-object), +0x10 link, +0x18 flags
 *   (bit16 started, bit17 finished, bit18 no-cancel, bit20 waiting; low word = retries), +0x20 result,
 *   +0x24 wait countdown in ms (eOrder::Tick, slot 3, subtracts the simulation quant each tick while bit20 is set),
 *   +0x30 sub-order. Update returns 0 = running, 1 = done, 2 = failed; Tick calls Finish (slot 4) when it ends, and
 *   Finish calls Update(this, 2) for a started order. Slot 16 = Start(this) after +0x08 was set by the parent,
 *   slot 17 = Describe, slot 5 = name, 6/7 = save/load, 19 = Update (base: return 1).
 *   A running instruction waits by installing a sub-order at order+0x30: the custom order ticks it (slot 3) and
 *   steps to the next instruction when it returns 1 (2 = the whole action fails).
 * - Entity: flags +0x30 (bit1 alive, bit2 human, bit3 vehicle), world matrix +0x50 (axes) with the position at
 *   +0x74 (x, y, z). Entity::SetPos(entity, const float pos[3], int flag) 0x1406fdd20 (used by the editor's
 *   e_teleport action, vt slot 17): rebuilds the matrix at pos (vt 25) and notifies the world (vt 33, reason 3).
 * - Entity::GiveOrder(entity, order, ctx) 0x1407efa10: brain = [entity+0xDC8], refuses when the vitality flags
 *   (+0x174 & 3) say the unit is out of action, else brain->vt[18](order); a refused order is Finished (vt 4) and
 *   deleted (vt 0, flag 1), result 0.
 * - Damage: Interaction::eHealthDamage{,Pierce,Blast,Blow,Count} share execute 0x140741710 (vt slot 9,
 *   (this, ctx, entity)); eHealthDamageCrush has its own (0x140742cd0). All human health loss goes through them.
 * - World scale: float 20.0 (world units per metre) in .data, followed by 1000, 1000, 3600.
 * - Named interaction events: Tag(cstr), Ctx(entity), FireNamed(ctx, tag) as in GEM ScriptExt (copied finder).
 *
 * Coexistence: the launcher loads GEM Teleport after the other payload DLLs, so every finder below avoids code they
 * patch. ScriptExt swaps eHealthDamageCount slots 5/10/12 and eHealthDamagePierce/Blast slots 5/10/12 (the 0.1.0
 * ReadFloat finder read slot 5 and failed in every launcher run); Fauna redirects the move action's
 * 'call MovementChange' and 'call GiveOrder' rel32s (squad pace). Vtable slots this DLL hooks itself (eCustom 15,
 * damage 9) are accepted when another DLL already replaced them: the hook then chains to that DLL.
 */

/* ------------------------------------------------------------------------------------ */
/* engine bindings                                                                      */
/* ------------------------------------------------------------------------------------ */
typedef uint8_t   (*RdPeekFn)(void *reader, int kind, int z);     /* non-consuming: 1 word, 3 quoted, 4 '{', 9 number */
typedef void      (*RdEnterFn)(void *reader);                     /* consume '{' (reader error otherwise)             */
typedef void      (*RdLeaveFn)(void *reader);                     /* consume '}'                                      */
typedef void     *(*RdWordFn)(void *reader, int flag);            /* -> MSVC std::string*                             */
typedef float     (*RdFloatFn)(void *reader);
typedef void     *(*EngineNewFn)(size_t n);
typedef void      (*EngineDeleteFn)(void *p, size_t n);
typedef void      (*SetPosFn)(void *entity, const float *pos, int flag);
typedef uint64_t  (*GiveOrderFn)(void *entity, void *order, void *ctx);
typedef uint8_t   (*IsApplicableFn)(void *action, uint8_t *flag, void *entity, void *target);
typedef void      (*DamageExecFn)(void *action, void *ctx, void *entity);
typedef void      (*SetSubOrderFn)(void *order, void *sub);
typedef void      (*TagCtorCStrFn)(void *tag, const char *s);
typedef void      (*TagDtorFn)(void *tag);
typedef void     *(*CtxCtorFn)(void *ctx, void *entity);
typedef void      (*CtxDtorFn)(void *ctx);
typedef void      (*FireNamedFn)(void *ctx, const void *tag);

static RdPeekFn g_rdPeek; static RdEnterFn g_rdEnter; static RdLeaveFn g_rdLeave; static RdWordFn g_rdWord; static RdFloatFn g_rdFloat;
static EngineNewFn g_engineNew; static EngineDeleteFn g_engineDelete;
static void **g_instrHead; static int32_t *g_instrGuard; static uint8_t *g_instrParser;
static void **g_vtCustomAction; static uint32_t g_instrBegOff, g_instrEndOff;     /* action+beg..end = vector<Instruction*> */
static void **g_vtCustomOrder, **g_vtSingleOrder;
static SetPosFn g_setPos; static GiveOrderFn g_giveOrder; static SetSubOrderFn g_setSubOrder;
static float *g_worldScale;
static TagCtorCStrFn g_tagCtorCStr; static TagDtorFn g_tagDtor; static CtxCtorFn g_ctxCtor; static CtxDtorFn g_ctxDtor; static FireNamedFn g_fireNamed;
static uint32_t g_entDefOff, g_defSetOff;
#define TP_NDMG 6
static void **g_vtDamage[TP_NDMG]; static int g_nDamage;                         /* damage action vtables (slot 9 = execute) */
static DamageExecFn g_damageExec[TP_NDMG]; static int g_nDamageExec;               /* distinct execute functions among them */
static int g_dmgExecIdx[TP_NDMG];

/* fixed engine layout (checked by the finders where a pattern shows it) */
#define ENT_FLAGS   0x30
#define ENT_POS     0x74
#define ORD_ACTOR   0x08
#define ORD_FLAGS   0x18
#define ORD_RESULT  0x20
#define ORD_WAIT    0x24
#define ORD_SUB     0x30
#define ORD_SIZE    0x38
#define ORD_WAITING (1u << 20)
#define TGT_TYPE    0x00
#define TGT_POS     0x04
#define TGT_GROUND  1
#define INSTR_NOCHECK 0x08
/* MSVC std::string (SSO) */
typedef struct { union { char buf[16]; char *ptr; } u; size_t size, cap; } MsString;
static const char *ms_cstr(const MsString *s) { return s->cap > 15 ? s->u.ptr : s->u.buf; }

/* ------------------------------------------------------------------------------------ */
/* finders                                                                              */
/* ------------------------------------------------------------------------------------ */
static uint8_t *func_start_of(const void *a) { uint32_t r = func_of(a); return r ? g_base + r : NULL; }

/* {instructions} parser: ... call Registry() ; mov rbx,[rax] ; test rbx,rbx ; ... ; lea r8,"Unknown instruction".
   The parser's first four calls are Peek(reader,4,0), Enter(reader), ReadWord(reader,1), Registry(). */
static int find_instr_registry(void)
{
    uint8_t *s = find_cstr("Unknown instruction");
    if (!s) { logf_("instructions: 'Unknown instruction' string not found"); return 0; }
    uint8_t *lea = NULL, *acc = NULL;
    static const uint8_t sig[] = { 0x48,0x8B,0x18, 0x48,0x85,0xDB };               /* mov rbx,[rax] ; test rbx,rbx */
    for (int k = 0; k < 8 && !acc; k++) {
        lea = find_lea_to(s, k);
        if (!lea) break;
        for (uint8_t *p = lea - 5; p > lea - 0x100; p--)
            if (p[0] == 0xE8 && !memcmp(p + 5, sig, sizeof sig)) { acc = p + 5 + *(int32_t *)(p + 1); break; }
    }
    if (!acc || !in_text(acc)) { logf_("instructions: registry accessor not found"); return 0; }
    uint8_t *cmp = NULL, *la = NULL;
    for (uint8_t *p = acc; p < acc + 0x40; p++) {
        if (p[0] == 0x39 && p[1] == 0x05 && !cmp) cmp = p;
        if (p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x05 && cmp && !la) { la = p; break; }
    }
    if (!cmp || !la) { logf_("instructions: registry statics not recognised"); return 0; }
    g_instrGuard = (int32_t *)(cmp + 6 + *(int32_t *)(cmp + 2));
    g_instrHead  = (void **)(la + 7 + *(int32_t *)(la + 3));
    if (!in_data(g_instrGuard) || !in_data(g_instrHead)) { logf_("instructions: registry statics not in .data"); return 0; }
    g_instrParser = func_start_of(lea);
    if (!g_instrParser) { logf_("instructions: parser function start unknown"); return 0; }
    /* reader API from the parser's call sequence */
    uint8_t *calls[4] = { 0 }, *site, *p = g_instrParser;
    for (int i = 0; i < 4; i++) { calls[i] = next_call(p, 0x60, &site); if (!calls[i]) break; p = site + 5; }
    if (!calls[3] || calls[3] != acc) { logf_("instructions: parser call sequence not recognised"); return 0; }
    g_rdPeek = (RdPeekFn)calls[0]; g_rdEnter = (RdEnterFn)calls[1]; g_rdWord = (RdWordFn)calls[2];
    /* Enter checks the token kind 4 ('{'), Leave kind 5 ('}'): 'cmp dword [rdx],N' in their first 0x30 bytes */
    static const uint8_t enterSig[] = { 0x83,0x3A,0x04 }, leaveSig[] = { 0x83,0x3A,0x05 };
    if (!find_bytes((uint8_t *)g_rdEnter, 0x30, enterSig, 3)) { logf_("instructions: reader Enter not recognised"); return 0; }
    for (uint8_t *q = (uint8_t *)site; q < g_instrParser + 0x300 && !g_rdLeave; q++)
        if (q[0] == 0xE8) { uint8_t *t = q + 5 + *(int32_t *)(q + 1); if (in_text(t) && find_bytes(t, 0x30, leaveSig, 3)) g_rdLeave = (RdLeaveFn)t; }
    if (!g_rdLeave) { logf_("instructions: reader Leave not found"); return 0; }
    /* engine operator new: a registration routine 'call Registry() ; mov ecx,0x10 ; mov rbx,rax ; call NEW' */
    g_engineNew = NULL; g_engineDelete = NULL;
    for (size_t i = 0; i + 5 <= g_text.n && !g_engineNew; i++) {
        uint8_t *c = g_text.p + i;
        if (c[0] != 0xE8 || c + 5 + *(int32_t *)(c + 1) != acc) continue;
        uint8_t *q = NULL;
        for (uint8_t *r = c + 5; r < c + 0x28; r++) { if (r[0] == 0xE8) break; if (r[0] == 0xB9 && *(uint32_t *)(r + 1) == 0x10) { q = r; break; } }
        if (!q) continue;
        uint8_t *n = next_call(q + 5, 0x10, NULL);
        if (n) g_engineNew = (EngineNewFn)n;
    }
    /* sized delete: the registry's atexit destructor (lea rcx,[rip+dtor] in the accessor) frees every node with
       'mov edx,0x10 ; call DELETE' */
    for (uint8_t *q = acc; q < acc + 0x80 && !g_engineDelete; q++) {
        if (q[0] != 0x48 || q[1] != 0x8D || q[2] != 0x0D) continue;
        uint8_t *d = q + 7 + *(int32_t *)(q + 3);
        if (!in_text(d)) continue;
        for (uint8_t *r = d; r < d + 0x40; r++)
            if (r[0] == 0xBA && *(uint32_t *)(r + 1) == 0x10 && r[5] == 0xE8) {
                uint8_t *t = r + 10 + *(int32_t *)(r + 6);
                if (in_text(t) && t[0] == 0xE9) t = t + 5 + *(int32_t *)(t + 1);           /* thunk */
                if (in_text(t)) { g_engineDelete = (EngineDeleteFn)t; break; }
            }
    }
    if (!g_engineNew || !g_engineDelete) { logf_("instructions: engine operator new/delete not found"); return 0; }
    logf_("instructions: registry head exe+0x%llX guard exe+0x%llX (accessor exe+0x%llX, parser exe+0x%llX); reader Peek exe+0x%llX Enter exe+0x%llX Word exe+0x%llX Leave exe+0x%llX; new exe+0x%llX delete exe+0x%llX",
          EXE_OFF(g_instrHead), EXE_OFF(g_instrGuard), EXE_OFF(acc), EXE_OFF(g_instrParser), EXE_OFF(g_rdPeek), EXE_OFF(g_rdEnter),
          EXE_OFF(g_rdWord), EXE_OFF(g_rdLeave), EXE_OFF(g_engineNew), EXE_OFF(g_engineDelete));
    return 1;
}

/* ReadFloat, by its own code: push rbx ; sub rsp,0x40 ; mov rbx,rcx ; call SYNC ; mov rax,[rbx+0x20] ; mov rdx,[rax-8] ;
   mov eax,[rdx] ; cmp eax,0xa ; je ; cmp eax,0xb  (token kinds 0xa/0xb = numbers), with SYNC = the helper Enter calls
   first. Unique in game/editor/server 1.067. (0.1.0 took the first call of eHealthDamageCount::parse - a vtable slot
   GEM ScriptExt replaces.) */
static int find_read_float(void)
{
    uint8_t *sync = (uint8_t *)g_rdEnter + 14 + *(int32_t *)((uint8_t *)g_rdEnter + 10);
    if (((uint8_t *)g_rdEnter)[9] != 0xE8 || !in_text(sync)) { logf_("reader: Enter layout not recognised"); return 0; }
    static const uint8_t pro[] = { 0x40,0x53,0x48,0x83,0xEC,0x40,0x48,0x8B,0xD9,0xE8 };
    static const uint8_t body[] = { 0x48,0x8B,0x43,0x20,0x48,0x8B,0x50,0xF8,0x8B,0x02,0x83,0xF8,0x0A,0x0F,0x84 };
    uint8_t *found = NULL; int n = 0;
    for (uint8_t *q = g_text.p; (q = find_bytes(q, g_text.p + g_text.n - q - 40, pro, sizeof pro)); q++) {
        if (q + 14 + *(int32_t *)(q + 10) != sync || memcmp(q + 14, body, sizeof body) || q[33] != 0x83 || q[34] != 0xF8 || q[35] != 0x0B) continue;
        found = q; n++;
    }
    if (n != 1) { logf_("reader: ReadFloat not recognised (%d candidates)", n); return 0; }
    g_rdFloat = (RdFloatFn)found;
    logf_("reader: ReadFloat exe+0x%llX", EXE_OFF(found));
    return 1;
}

/* Interface::Action::eCustom: vtable (RTTI), slot 15 IsApplicable, instruction vector offsets (from the custom
   order's Update: mov rdx,[rax+BEG] ; mov rax,[rax+END] ; sub rax,rdx ; sar rax,3), entity position offset (from
   slot 31: movss xmm,[entity+0x74/0x78/0x7C]) */
static int find_custom_action(void)
{
    g_vtCustomAction = find_vtable_rtti(".?AVeCustom@Action@Interface@@");
    g_vtCustomOrder  = find_vtable_rtti(".?AUeCustomActionOrder@Order@@");
    if (!g_vtCustomAction || !g_vtCustomOrder) { logf_("action: eCustom / eCustomActionOrder not found (RTTI)"); return 0; }
    /* slots 15 and 17 may already be hooked by another DLL (we chain); 19 and 31 are read for offsets and must be the engine's */
    if (!in_text(g_vtCustomAction[31]) || !in_text(g_vtCustomOrder[19])) { logf_("action: eCustom slot 31 / order slot 19 not engine code"); return 0; }
    uint8_t *isap = (uint8_t *)g_vtCustomAction[15];
    static const uint8_t a1[] = { 0xFF,0x50,0x70 }, a2[] = { 0xFF,0x50,0x58 }, a3[] = { 0xFF,0x50,0x60 };
    if (!isap) { logf_("action: IsApplicable (slot 15) empty"); return 0; }
    if (in_text(isap) && (!find_bytes(isap, 0x80, a1, 3) || !find_bytes(isap, 0x80, a2, 3) || !find_bytes(isap, 0x80, a3, 3))) { logf_("action: IsApplicable (slot 15) not recognised"); return 0; }
    if (!in_text(isap)) logf_("action: IsApplicable (slot 15) already hooked by another DLL - the radius check will chain to it");
    uint8_t *upd = (uint8_t *)g_vtCustomOrder[19]; uint32_t beg = 0, end = 0;
    for (uint8_t *q = upd; q < upd + 0x400; q++)
        if (q[0] == 0x48 && q[1] == 0x8B && (q[2] & 0xC7) == 0x80 && q[7] == 0x48 && q[8] == 0x8B && (q[9] & 0xC7) == 0x80 &&
            q[14] == 0x48 && q[15] == 0x2B && q[17] == 0x48 && q[18] == 0xC1 && q[20] == 0x03) {
            uint32_t a = *(uint32_t *)(q + 3), b = *(uint32_t *)(q + 10);
            if (b == a + 8 && a > 0x100 && a < 0x1000) { beg = a; end = b; break; }
        }
    if (!beg) { logf_("action: instruction vector offsets not recognised"); return 0; }
    static const uint8_t subSig[] = { 0x48,0x8B,0x4B,0x30 };                          /* mov rcx,[rbx+0x30] (sub-order) */
    if (!find_bytes(upd, 0x400, subSig, 4)) { logf_("action: sub-order offset not confirmed"); return 0; }
    uint8_t *slot31 = (uint8_t *)g_vtCustomAction[31]; int p74 = 0, p78 = 0, p7c = 0;
    for (uint8_t *q = slot31; q < slot31 + 0x200; q++)
        if (q[0] == 0xF3 && q[1] == 0x0F && q[2] == 0x10 && (q[3] & 0xC0) == 0x40) { if (q[4] == 0x74) p74 = 1; if (q[4] == 0x78) p78 = 1; if (q[4] == 0x7C) p7c = 1; }
    if (!p74 || !p78 || !p7c) { logf_("action: entity position offset not confirmed"); return 0; }
    g_instrBegOff = beg; g_instrEndOff = end;
    logf_("action: eCustom vt exe+0x%llX (IsApplicable exe+0x%llX), instructions at action+0x%X..0x%X, order vt exe+0x%llX (Update exe+0x%llX), entity pos +0x%X",
          EXE_OFF(g_vtCustomAction), EXE_OFF(isap), beg, end, EXE_OFF(g_vtCustomOrder), EXE_OFF(upd), ENT_POS);
    return 1;
}

/* eSingleOrder (RTTI): 20 slots; Tick (3) has 'bt ecx,0x14' + 'sub [rbx+0x24],eax', Finish (4) 'bt eax,0x10',
   Update (19) = 'mov eax,1 ; ret' */
static int find_order_base(void)
{
    g_vtSingleOrder = find_vtable_rtti(".?AVeSingleOrder@@");
    if (!g_vtSingleOrder) { logf_("order: eSingleOrder not found (RTTI)"); return 0; }
    for (int i = 0; i < 20; i++) if (!in_text(g_vtSingleOrder[i])) { logf_("order: eSingleOrder vtable too short"); return 0; }
    uint8_t *tick = (uint8_t *)g_vtSingleOrder[3], *fin = (uint8_t *)g_vtSingleOrder[4], *upd = (uint8_t *)g_vtSingleOrder[19];
    static const uint8_t bt20[] = { 0x0F,0xBA,0xE1,0x14 }, sub24[] = { 0x29,0x43,0x24 }, bt16[] = { 0x0F,0xBA,0xE0,0x10 }, ret1[] = { 0xB8,0x01,0x00,0x00,0x00,0xC3 };
    if (!find_bytes(tick, 0x40, bt20, 4) || !find_bytes(tick, 0x40, sub24, 3)) { logf_("order: eSingleOrder::Tick not recognised"); return 0; }
    if (!find_bytes(fin, 0x20, bt16, 4)) { logf_("order: eSingleOrder::Finish not recognised"); return 0; }
    if (memcmp(upd, ret1, 6)) { logf_("order: eSingleOrder::Update not recognised"); return 0; }
    /* SetSubOrder(order, sub): the {animations} instruction's execute installs its ePlayAnimations sub-order with
       call PlayAnimations ; mov rdx,rax ; mov rcx,rbp ; call SetSubOrder. Body: mov rcx,[rcx+0x30] ... (Finish + delete
       the old one) ... mov [rbx+0x30],rdi ... jmp (sub+8 = parent actor ; sub->vt[16] Start) */
    void **vtAnim = find_vtable_rtti(".?AUeInstruction_animations@Action@Interface@@");
    uint8_t *ex = vtAnim && in_text(vtAnim[3]) ? (uint8_t *)vtAnim[3] : NULL, *sso = NULL;
    static const uint8_t mid[] = { 0x48,0x8B,0xD0,0x48,0x8B,0xCD,0xE8 }, rd30[] = { 0x48,0x8B,0x49,0x30 }, wr30[] = { 0x48,0x89,0x7B,0x30 };
    uint8_t *m = ex ? find_bytes(ex, 0x200, mid, sizeof mid) : NULL;
    if (m && m[-5] == 0xE8) sso = m + 11 + *(int32_t *)(m + 7);
    if (!sso || !in_text(sso) || !find_bytes(sso, 0x20, rd30, 4) || !find_bytes(sso, 0x50, wr30, 4)) { logf_("order: SetSubOrder not recognised"); return 0; }
    g_setSubOrder = (SetSubOrderFn)sso;
    logf_("order: eSingleOrder vt exe+0x%llX (Tick exe+0x%llX, Finish exe+0x%llX), SetSubOrder exe+0x%llX", EXE_OFF(g_vtSingleOrder), EXE_OFF(tick), EXE_OFF(fin), EXE_OFF(sso));
    return 1;
}

/* Entity::SetPos = first call of the editor's e_teleport action Execute (vt slot 17) */
static int find_set_pos(void)
{
    void **vt = find_vtable_rtti(".?AUe_teleport@Action@Interface@@");
    if (!vt || !in_text(vt[17])) { logf_("teleport: e_teleport action not found (RTTI)"); return 0; }
    uint8_t *f = next_call((uint8_t *)vt[17], 0x30, NULL);
    static const uint8_t pro[] = { 0x40,0x53,0x48,0x83,0xEC,0x50 }, c1[] = { 0xFF,0x90,0xC8,0x00,0x00,0x00 }, c2[] = { 0xFF,0x90,0x08,0x01,0x00,0x00 };
    if (!f || memcmp(f, pro, 6) || !find_bytes(f, 0x80, c1, 6) || !find_bytes(f, 0x80, c2, 6)) { logf_("teleport: Entity::SetPos not recognised"); return 0; }
    g_setPos = (SetPosFn)f;
    logf_("teleport: Entity::SetPos exe+0x%llX", EXE_OFF(f));
    return 1;
}

/* Entity::GiveOrder: it starts 'mov rcx,[rcx+0xDC8]' (brain) within 0x20 bytes. Candidates, first that checks out:
   1. the tail 'jmp GiveOrder' of eCustom::Execute (vt slot 17) - the call our actions go through;
   2. the move action's movement-change site (GEM Fauna's squad-pace pattern): lea rax,[rip+MODES] ; xor r8d,r8d ;
      mov edx,[rax+rdx*4] ; call MovementChange ; lea r8,[rbp+CTX] ; mov rdx,rax ; mov rcx,rdi ; call GiveOrder.
      Fauna redirects that call, so this only helps when Fauna is absent. */
static int is_give_order(const uint8_t *f)
{
    static const uint8_t brain[] = { 0x48,0x8B,0x89,0xC8,0x0D,0x00,0x00 };
    return f && in_text(f) && find_bytes((uint8_t *)f, 0x20, brain, 7);
}
static int find_give_order(void)
{
    uint8_t *give = NULL; const char *how = NULL;
    uint8_t *ex = (uint8_t *)g_vtCustomAction[17];
    if (in_text(ex))
        for (uint8_t *q = ex; q < ex + 0x100 && !give; q++)
            if (q[0] == 0xE9) { uint8_t *t = q + 5 + *(int32_t *)(q + 1); if (is_give_order(t)) { give = t; how = "eCustom::Execute tail jump"; } }
    if (!give) {
        int n = 0; uint8_t *s = NULL;
        for (uint8_t *q = g_text.p; q + 36 <= g_text.p + g_text.n; q++)
            if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x05 && q[7] == 0x45 && q[8] == 0x33 && q[9] == 0xC0 && q[10] == 0x8B &&
                q[11] == 0x14 && q[12] == 0x90 && q[13] == 0xE8 && q[18] == 0x4C && q[19] == 0x8D && q[20] == 0x85 && q[25] == 0x48 &&
                q[26] == 0x8B && q[27] == 0xD0 && q[28] == 0x48 && q[29] == 0x8B && q[30] == 0xCF && q[31] == 0xE8) { n++; s = q; }
        uint8_t *t = n == 1 ? s + 36 + *(int32_t *)(s + 32) : NULL;
        if (is_give_order(t)) { give = t; how = "move action site"; }
    }
    if (!give) { logf_("lock: Entity::GiveOrder not recognised"); return 0; }
    g_giveOrder = (GiveOrderFn)give;
    logf_("lock: Entity::GiveOrder exe+0x%llX (from the %s)", EXE_OFF(give), how);
    return 1;
}

/* damage actions: every eHealthDamage* class (RTTI), their distinct slot-9 execute functions */
static int find_damage(void)
{
    static const char *names[] = { ".?AUeHealthDamage@Interaction@@", ".?AUeHealthDamagePierce@Interaction@@", ".?AUeHealthDamageBlast@Interaction@@",
                                   ".?AUeHealthDamageBlow@Interaction@@", ".?AUeHealthDamageCount@Interaction@@", ".?AUeHealthDamageCrush@Interaction@@" };
    g_nDamage = 0; g_nDamageExec = 0;
    for (int i = 0; i < 6; i++) {
        void **vt = find_vtable_rtti(names[i]);
        if (!vt || !vt[9]) { logf_("invulnerable: %s not found", names[i] + 4); return 0; }
        if (!in_text(vt[9])) logf_("invulnerable: %s execute already hooked by another DLL - chaining", names[i] + 4);
        int k; for (k = 0; k < g_nDamageExec; k++) if ((void *)g_damageExec[k] == vt[9]) break;
        if (k == g_nDamageExec) g_damageExec[g_nDamageExec++] = (DamageExecFn)vt[9];
        g_vtDamage[g_nDamage] = vt; g_dmgExecIdx[g_nDamage] = k; g_nDamage++;
    }
    logf_("invulnerable: %d damage classes, %d distinct execute functions", g_nDamage, g_nDamageExec);
    return 1;
}

/* world scale (units per metre): the .data float block 20.0, 1000.0, 1000.0, 3600.0 */
static int find_world_scale(void)
{
    static const uint8_t blk[] = { 0x00,0x00,0xA0,0x41, 0x00,0x00,0x7A,0x44, 0x00,0x00,0x7A,0x44, 0x00,0x00,0x61,0x45 };
    uint8_t *p = find_bytes(g_data.p, g_data.n, blk, sizeof blk);
    if (!p || find_bytes(p + 1, g_data.p + g_data.n - p - 1, blk, sizeof blk)) { logf_("scale: world scale constant not found"); return 0; }
    g_worldScale = (float *)p;
    logf_("scale: %g world units per metre (exe+0x%llX)", *g_worldScale, EXE_OFF(p));
    return 1;
}

/* named interaction events (copied from GEM ScriptExt): the engine's "start_healing" call site
   lea rdx,"start_healing" ; lea rcx,[tag] ; call Tag(const char*) ; ... call Ctx(entity) ; mov rcx,rax ; lea rdx,[tag] ;
   call FireNamed ; lea rcx,[ctx] ; call ~Ctx ; lea rcx,[tag] ; call ~Tag */
static int find_fire_named(void)
{
    uint8_t *s = find_cstr("start_healing");
    if (!s) { logf_("events: 'start_healing' not found"); return 0; }
    uint8_t *ev = find_cstr("EVENT: ");
    static const uint8_t tagdtor_sig[] = { 0x48,0x83,0xC1,0x38,0xE9 };
    static const uint8_t ctxctor_sig[] = { 0x48,0x89,0x11 };
    uint8_t *fn[5] = { 0 }; int ok = 0;
    for (int k = 0; k < 8 && !ok; k++) {
        uint8_t *lea = find_lea_to(s, k);
        if (!lea) break;
        uint8_t *p = lea, *site; int i;
        for (i = 0; i < 5; i++) { fn[i] = next_call(p, 0x40, &site); if (!fn[i]) break; p = site + 5; }
        if (i < 5 || memcmp(fn[4], tagdtor_sig, 5) || !find_bytes(fn[1], 0x20, ctxctor_sig, 3)) continue;
        for (uint8_t *q = fn[2]; q < fn[2] + 0x100 && !ok; q++)
            if (q[0] == 0xE8) { uint8_t *t = q + 5 + *(int32_t *)(q + 1); if (in_text(t) && ev && find_rip_ref(t, 0x400, ev)) ok = 1; }
    }
    if (!ok) { logf_("events: FireNamedEvent not recognised"); return 0; }
    for (uint8_t *q = fn[2]; q < fn[2] + 0x80; q++)
        if ((q[0] & 0xF8) == 0x48 && q[1] == 0x8B && (q[2] & 0xC7) == 0x80 && (q[7] & 0xF8) == 0x48 && q[8] == 0x8B && (q[9] & 0xC0) == 0x40) {
            uint32_t off = *(uint32_t *)(q + 3);
            if (off > 0x100 && off < 0x1000) { g_entDefOff = off; g_defSetOff = q[10]; break; }
        }
    if (!g_entDefOff || !g_defSetOff) { logf_("events: entity->def / def->set offsets not recognised"); return 0; }
    g_tagCtorCStr = (TagCtorCStrFn)fn[0]; g_ctxCtor = (CtxCtorFn)fn[1]; g_fireNamed = (FireNamedFn)fn[2];
    g_ctxDtor = (CtxDtorFn)fn[3]; g_tagDtor = (TagDtorFn)fn[4];
    logf_("events: Tag(cstr)=exe+0x%llX Ctx=exe+0x%llX FireNamed=exe+0x%llX ~Ctx=exe+0x%llX ~Tag=exe+0x%llX, entity->def +0x%X, def->set +0x%X",
          EXE_OFF(fn[0]), EXE_OFF(fn[1]), EXE_OFF(fn[2]), EXE_OFF(fn[3]), EXE_OFF(fn[4]), g_entDefOff, g_defSetOff);
    return 1;
}

/* 0.2.12 network events. On a dedicated server the jump order runs on the server only, and FireNamed runs an event on
   this machine only: the clients never played the jump (Alex's Nov 9 2026 3v3; the 0.2.11 watch log showed script tags
   are NOT replicated, but the action's own {effect "jetpack_start"} did reach the client). The engine's {effect}
   instruction (eInstruction_effect, vtable slot 3 = execute) does it like this:
       Ctx(ctx, entity) ; if (IsNetGame()) { m = MakeNetEffect(buf, entity, &tag) ; m->vt[1](m) ; ~Ctx(buf + 0x10) }
       FireNamed(ctx, &tag) ; ~Ctx(ctx)
   MakeNetEffect looks the name up in the entity's interaction set and stores its index (0xFFFF = no such {on}); the
   receiver runs that event itself. We take IsNetGame and MakeNetEffect from that function (calls 2 and 3; calls 1, 4,
   5, 6 must be the Ctx / ~Ctx / FireNamed we already found, else the feature stays off). */
typedef uint8_t (*IsNetGameFn)(void);
typedef void   *(*MakeNetEffectFn)(void *buf, void *entity, const void *tag);
static IsNetGameFn g_isNetGame; static MakeNetEffectFn g_makeNetEffect; static int g_okNetEvents;
static int find_net_effect(void)
{
    if (!g_ctxCtor || !g_ctxDtor || !g_fireNamed) { logf_("net events: FireNamed not found, off"); return 0; }
    void **vt = find_vtable_rtti(".?AUeInstruction_effect@Action@Interface@@");
    if (!vt || !in_text(vt[3])) { logf_("net events: eInstruction_effect vtable not found, off"); return 0; }
    uint8_t *p = (uint8_t *)vt[3], *site, *c[6];
    for (int i = 0; i < 6; i++) { c[i] = next_call(p, 0x40, &site); if (!c[i]) { logf_("net events: effect execute not recognised (call %d), off", i + 1); return 0; } p = site + 5; }
    if (c[0] != (uint8_t *)g_ctxCtor || c[3] != (uint8_t *)g_ctxDtor || c[4] != (uint8_t *)g_fireNamed || c[5] != (uint8_t *)g_ctxDtor) {
        logf_("net events: effect execute has another shape (exe+0x%llX), off", EXE_OFF(vt[3])); return 0;
    }
    static const uint8_t ctx_at_10[] = { 0x48,0x8D,0x4E,0x10 };          /* lea rcx,[rsi+10h] ; call Ctx : the message holds a Ctx at +0x10 */
    if (!find_bytes(c[2], 0x90, ctx_at_10, 4)) { logf_("net events: message layout not recognised (exe+0x%llX), off", EXE_OFF(c[2])); return 0; }
    g_isNetGame = (IsNetGameFn)c[1]; g_makeNetEffect = (MakeNetEffectFn)c[2];
    logf_("net events: effect execute exe+0x%llX, IsNetGame exe+0x%llX, MakeNetEffect exe+0x%llX", EXE_OFF(vt[3]), EXE_OFF(c[1]), EXE_OFF(c[2]));
    return 1;
}
