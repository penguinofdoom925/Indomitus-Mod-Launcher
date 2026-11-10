/* GEM Teleport - offline finder test
 * Part of the unity build: #included by gem_teleport.c, not compiled on its own.
 * TP_TEST build only: maps an exe at its image base and runs every finder twice - on the vanilla code, then after
 * simulating what the DLLs loaded before GEM Teleport patch (tests/run_offline_check.sh):
 *   ScriptExt  eHealthDamageCount / Pierce / Blast vtable slots 5, 10, 12 -> its own code
 *   Fauna      the move action's 'call MovementChange' and 'call GiveOrder' rel32 -> jump pads
 *   any DLL    eCustom slot 15 and the damage slot 9 already hooked (we must chain, not fail)
 */

#ifdef FX_TEST
typedef struct { void *readFloat, *give, *setPos, *setSub, *isap, *dmg0; uint32_t beg; int nDmgExec; } TpFound;

static int run_finders(TpFound *f)
{
    int ok = 1;
    printf("--- core finders (required)\n");
    ok &= find_instr_registry(); ok &= find_read_float(); ok &= find_custom_action(); ok &= find_order_base(); ok &= find_set_pos(); ok &= find_world_scale();
    printf("--- feature finders\n");
    ok &= find_fire_named(); ok &= find_net_effect(); ok &= find_give_order(); ok &= find_damage();
    if (ok && !prologue_len((const uint8_t *)g_giveOrder)) { printf("GiveOrder prologue NOT relocatable\n"); ok = 0; }
    f->readFloat = (void *)g_rdFloat; f->give = (void *)g_giveOrder; f->setPos = (void *)g_setPos; f->setSub = (void *)g_setSubOrder;
    f->isap = g_vtCustomAction ? g_vtCustomAction[15] : NULL; f->dmg0 = (void *)g_damageExec[0]; f->beg = g_instrBegOff; f->nDmgExec = g_nDamageExec;
    return ok;
}

/* a fake "other DLL" address: inside the mapping (readable) but outside .text */
static uint8_t *fake_code(void) { return g_data.p + 0x40; }

static void simulate_other_dlls(void)
{
    static const char *sx[] = { ".?AUeHealthDamageCount@Interaction@@", ".?AUeHealthDamagePierce@Interaction@@", ".?AUeHealthDamageBlast@Interaction@@" };
    for (int i = 0; i < 3; i++) { void **vt = find_vtable_rtti(sx[i]); if (vt) { vt[5] = fake_code(); vt[10] = fake_code(); vt[12] = fake_code(); } }
    int n = 0;
    for (uint8_t *q = g_text.p; q + 36 <= g_text.p + g_text.n; q++)
        if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x05 && q[7] == 0x45 && q[8] == 0x33 && q[9] == 0xC0 && q[10] == 0x8B &&
            q[11] == 0x14 && q[12] == 0x90 && q[13] == 0xE8 && q[18] == 0x4C && q[19] == 0x8D && q[20] == 0x85 && q[25] == 0x48 &&
            q[26] == 0x8B && q[27] == 0xD0 && q[28] == 0x48 && q[29] == 0x8B && q[30] == 0xCF && q[31] == 0xE8) {
            *(int32_t *)(q + 14) = (int32_t)(fake_code() - (q + 18)); *(int32_t *)(q + 32) = (int32_t)(fake_code() - (q + 36)); n++;
        }
    void **vtc = find_vtable_rtti(".?AVeCustom@Action@Interface@@"); if (vtc) vtc[15] = fake_code();
    void **vtp = find_vtable_rtti(".?AUeHealthDamagePierce@Interaction@@"); if (vtp) vtp[9] = fake_code();
    printf("simulated: ScriptExt vtable swaps, Fauna call redirection (%d site), eCustom slot 15 + Pierce slot 9 pre-hooked\n", n);
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: tp_test <call_to_arms.exe>\n"); return 2; }
    FILE *f = fopen(argv[1], "rb"); if (!f) { printf("cannot open %s\n", argv[1]); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(n); if (fread(file, 1, n, f) != (size_t)n) return 2; fclose(f);
    uint32_t pe = *(uint32_t *)(file + 0x3c);
    uint16_t nsec = *(uint16_t *)(file + pe + 6), opt = *(uint16_t *)(file + pe + 20);
    uint64_t base = *(uint64_t *)(file + pe + 24 + 24);
    uint32_t imgsz = *(uint32_t *)(file + pe + 24 + 56);
    g_base = mmap((void *)base, imgsz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (g_base != (uint8_t *)base) { printf("mmap at image base failed\n"); return 1; }
    for (int i = 0; i < nsec; i++) {
        uint8_t *sh = file + pe + 24 + opt + 40 * i;
        uint32_t vs = *(uint32_t *)(sh + 8), va = *(uint32_t *)(sh + 12), rs = *(uint32_t *)(sh + 16), raw = *(uint32_t *)(sh + 20);
        memcpy(g_base + va, file + raw, rs < vs ? rs : vs);
        Sec s = { g_base + va, vs };
        if (!strncmp((char *)sh, ".text", 8)) g_text = s; else if (!strncmp((char *)sh, ".rdata", 8)) g_rdata = s; else if (!strncmp((char *)sh, ".data", 8)) g_data = s; else if (!strncmp((char *)sh, ".pdata", 8)) g_pdata = s;
    }
    printf("%s timestamp %08X\n", argv[1], *(uint32_t *)(file + pe + 8));
    TpFound a = { 0 }, b = { 0 };
    printf("=== pass 1: vanilla exe\n");
    int ok1 = run_finders(&a);
    printf("=== pass 2: after the DLLs that load before GEM Teleport\n");
    simulate_other_dlls();
    int ok2 = run_finders(&b);
    int same = a.readFloat == b.readFloat && a.give == b.give && a.setPos == b.setPos && a.setSub == b.setSub && a.beg == b.beg;
    int chained = b.isap == fake_code() && b.nDmgExec == a.nDmgExec + 1;
    printf("pass 2 finds the same engine functions: %s; pre-hooked slots chained: %s\n", same ? "yes" : "NO", chained ? "yes" : "NO");
    int ok = ok1 && ok2 && same && chained;
    printf(ok ? "ALL FOUND\n" : "FAILED\n");
    return !ok;
}
#endif
