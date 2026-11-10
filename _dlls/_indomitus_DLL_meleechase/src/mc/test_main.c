/* GEM MeleeChase - offline finder test
 * Part of the unity build: #included by gem_meleechase.c, not compiled on its own.
 * FX_TEST build only: maps an exe at its image base and runs every finder (tests/run_offline_check.sh).
 */

#ifdef FX_TEST
int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: mc_test <call_to_arms.exe>\n"); return 2; }
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
    int ok = 1;
    ok &= find_weapon_layout();
    ok &= find_weapons();
    ok &= find_vehicle();
    ok &= find_priority();
    ok &= find_run_mode();
    ok &= find_hold();
    printf("smooth re-path helper: %s\n", g_finishSub ? "found" : "not found");
    printf(ok ? "ALL FOUND\n" : "FAILED\n");
    return !ok;
}
#endif
