/* nca_inspect.c: host diagnostic on a REAL NSP + prod.keys.
 *
 * Uses the exact loader code that ships in the console build (emucore/crypto,
 * emucore/loader) to answer, with evidence, whether a retail NSP is genuine and
 * whether the supplied keys actually decrypt it - as opposed to "the parser saw
 * 215 key names, so it must be fine". It then loads the extracted NSO into guest
 * memory and runs the AArch64 interpreter, printing entry PC, instruction count,
 * SVC counters and halt reason.
 *
 * Usage: nca_inspect <prod.keys> <title.nsp> [max_insns]
 * Exit: 0 = NSO extracted and loaded; 1 = extraction failed (reason printed).
 *
 * This is a developer tool: it reads whole files from the host filesystem, which
 * the console frontend never does. Only diagnostics live here.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "keys.h"
#include "nca.h"
#include "pfs0.h"
#include "nso.h"
#include "loader.h"
#include "a64.h"
#include "a64_mem.h"
#include "hle.h"

static uint8_t *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *b;
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    b = (uint8_t *)malloc((size_t)n);
    if (!b) { fclose(f); fprintf(stderr, "oom reading %s\n", path); return NULL; }
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    *out_len = (size_t)n;
    return b;
}

static void print_rights(const uint8_t r[0x10]) {
    int i; for (i = 0; i < 0x10; ++i) printf("%02x", r[i]);
}

int main(int argc, char **argv) {
    size_t klen = 0, nsp_len = 0;
    uint8_t *kbuf, *nsp;
    ProdKeys keys;
    Pfs0 pfs;
    uint32_t i;
    int prog_idx = -1;
    Nca nca;
    const uint8_t *prog_data = NULL;
    uint64_t prog_size = 0;
    unsigned long max_insns = 5000000ul;
    NcaResult r;

    if (argc < 3) { fprintf(stderr, "usage: %s <prod.keys> <title.nsp> [max_insns]\n", argv[0]); return 2; }
    if (argc >= 4) max_insns = strtoul(argv[3], NULL, 0);

    kbuf = read_file(argv[1], &klen);
    nsp = read_file(argv[2], &nsp_len);
    if (!kbuf || !nsp) return 2;
    printf("== files ==\n  prod.keys: %zu bytes\n  nsp:       %zu bytes\n", klen, nsp_len);

    /* ---- keys ---- */
    Keys_Init(&keys);
    printf("\n== prod.keys ==\n  parse: %s (%u recognised)\n",
           Keys_ResultName(Keys_Parse(&keys, kbuf, klen)), keys.count);
    {
        uint32_t hl = 0; const uint8_t *hk = Keys_HeaderKey(&keys, &hl);
        printf("  header_key: %s", hk ? "present" : "MISSING");
        if (hk) printf(" (%u bytes)%s", hl, hl == 32 ? "" : "  <-- XTS needs 32");
        printf("\n");
    }
    {
        int n = 0, rev;
        for (rev = 0; rev < KEYS_MAX_REVISION; ++rev) if (keys.have_titlekek[rev]) ++n;
        printf("  titlekek_*: %d revision(s) present\n", n);
        n = 0;
        for (rev = 0; rev < KEYS_MAX_REVISION; ++rev)
            if (keys.have_kaek[KEYS_KAEK_APPLICATION][rev]) ++n;
        printf("  key_area_key_application_*: %d revision(s)\n", n);
    }

    /* ---- NSP container ---- */
    printf("\n== NSP (PFS0) ==\n");
    if (Pfs0_Parse(&pfs, nsp, nsp_len) != PFS0_OK) {
        printf("  Pfs0_Parse FAILED (not a PFS0 / bad table)\n");
        return 1;
    }
    printf("  %u file(s)\n", pfs.count);
    for (i = 0; i < pfs.count; ++i) {
        uint64_t sz = 0;
        const char *nm = Pfs0_Name(&pfs, i);
        const uint8_t *d = Pfs0_Data(&pfs, i, &sz);
        printf("    [%u] %-42s off=%#llx size=%llu %s\n", i, nm,
               (unsigned long long)(d ? (uint64_t)(d - nsp) : 0),
               (unsigned long long)sz, d ? "" : "(OUT OF RANGE)");
        if (strstr(nm, ".nca")) {
            Nca cand;
            NcaResult cr = Nca_Parse(&cand, d, sz, &keys);
            printf("         Nca_Parse: %s", Nca_ResultName(cr));
            if (cr == NCA_OK) {
                printf("  type=%u rights=%d", cand.content_type, cand.has_rights_id);
                if (cand.content_type == NCA_CONTENT_PROGRAM) {
                    printf("  <-- PROGRAM NCA");
                    if (prog_idx < 0) { prog_idx = (int)i; nca = cand; prog_data = d; prog_size = sz; }
                }
            }
            printf("\n");
        }
    }
    if (prog_idx < 0) { printf("\n  NO PROGRAM NCA FOUND\n"); return 1; }

    /* ---- program NCA ---- */
    printf("\n== program NCA [%d] ==\n", prog_idx);
    printf("  magic=%c%c%c%c content_type=%u kaek_ind=%u keygen=%u\n",
           (nca.magic) & 0xFF, (nca.magic >> 8) & 0xFF, (nca.magic >> 16) & 0xFF,
           (nca.magic >> 24) & 0xFF, nca.content_type, nca.kaek_ind, nca.key_generation);
    printf("  nca_size=%llu title_id=%016llx rights_id=",
           (unsigned long long)nca.nca_size, (unsigned long long)nca.title_id);
    if (nca.has_rights_id) print_rights(nca.rights_id); else printf("(none)");
    printf("\n");
    for (i = 0; i < 4; ++i) {
        const NcaSection *s = &nca.section[i];
        if (!s->present) continue;
        printf("  sec%u: part=%u fs=%u crypt=%u off=%#llx size=%llu pfs0_off=%#llx pfs0_size=%llu\n",
               i, s->partition_type, s->fs_type, s->crypt_type,
               (unsigned long long)s->offset, (unsigned long long)s->size,
               (unsigned long long)s->pfs0_offset, (unsigned long long)s->pfs0_size);
    }

    /* ---- ticket (rights-id titles) ---- */
    uint8_t titlekey[16];
    const uint8_t *tk = NULL;
    if (nca.has_rights_id) {
        printf("\n== ticket ==\n");
        for (i = 0; i < pfs.count; ++i) {
            const char *nm = Pfs0_Name(&pfs, i);
            uint64_t sz = 0;
            const uint8_t *d;
            if (!strstr(nm, ".tik")) continue;
            d = Pfs0_Data(&pfs, i, &sz);
            uint8_t rid[16]; uint8_t rev = 0;
            r = Nca_ParseTicket(d, sz, NULL, &rev, rid);
            printf("  %s: parse=%s rev=%u rights=", nm, Nca_ResultName(r), rev);
            print_rights(rid); printf("\n");
            if (r != NCA_OK) continue;
            r = Nca_TitleKey(d, sz, &keys, titlekey, NULL);
            printf("  title key: %s\n", Nca_ResultName(r));
            if (r != NCA_OK) { printf("  -> titlekek_%02x is REQUIRED and %s\n", rev,
                                      Keys_Titlekek(&keys, rev) ? "present" : "MISSING"); return 1; }
            tk = titlekey;
            break;
        }
        if (!tk) { printf("  no usable .tik\n"); return 1; }
    }

    /* ---- ExeFS extraction ---- */
    int exefs_i = -1;
    for (i = 0; i < 4; ++i) {
        const NcaSection *s = &nca.section[i];
        if (s->present && s->partition_type == NCA_PARTITION_PFS0 && s->fs_type == NCA_FS_PFS0)
            { exefs_i = (int)i; break; }
    }
    if (exefs_i < 0) { printf("\n  no ExeFS PFS0 section\n"); return 1; }
    {
        uint64_t sec_size = nca.section[exefs_i].size;
        uint8_t *scratch = (uint8_t *)malloc((size_t)sec_size);
        const uint8_t *nso = NULL; uint64_t nso_len = 0;
        if (!scratch) { printf("  oom scratch %llu\n", (unsigned long long)sec_size); return 1; }
        printf("\n== ExeFS (section %d, %llu bytes) ==\n", exefs_i, (unsigned long long)sec_size);
        r = Nca_ExtractExefs(&nca, exefs_i, prog_data, prog_size, &keys, tk,
                             scratch, (size_t)sec_size, &nso, &nso_len);
        printf("  Nca_ExtractExefs: %s\n", Nca_ResultName(r));
        if (r != NCA_OK) { free(scratch); return 1; }

        {
            Pfs0 ex; uint32_t j;
            if (Pfs0_Parse(&ex, scratch + nca.section[exefs_i].pfs0_offset,
                           nca.section[exefs_i].pfs0_size) != PFS0_OK) {
                printf("  ExeFS PFS0 parse failed\n"); free(scratch); return 1;
            }
            printf("  ExeFS files: %u\n", ex.count);
            for (j = 0; j < ex.count; ++j) {
                uint64_t sz = 0; Pfs0_Data(&ex, j, &sz);
                printf("    %-16s %llu bytes\n", Pfs0_Name(&ex, j), (unsigned long long)sz);
            }
        }

        {
            Nso nso_h;
            A64GuestMem mem; A64 cpu; HleState hle; LoadedImage img;
            static uint8_t ram[256u * 1024u * 1024u];
            NsoResult nr = Nso_Parse(&nso_h, nso, nso_len);
            printf("\n== NSO ('main', %llu bytes) ==\n", (unsigned long long)nso_len);
            printf("  Nso_Parse: %s\n", nr == NSO_OK ? "ok" : "FAILED");
            if (nr != NSO_OK) { free(scratch); return 1; }
            for (i = 0; i < 3; ++i) {
                const NsoSegment *s = &nso_h.seg[i];
                printf("  seg%u loc=%#x size=%#x comp=%#x (stored %#x)%s\n", i,
                       s->location, s->size, s->compressed, s->compressed_size,
                       s->compressed ? " LZ4" : "");
            }
            printf("  entry=%#llx\n", (unsigned long long)nso_h.entry);

            A64GuestMem_Init(&mem, ram, sizeof(ram), 0);
            Hle_Init(&hle);
            A64GuestMem_Attach(&mem, &cpu);
            A64_SetSvcHandler(&cpu, Hle_HandleSvc, &hle);
            {
                LoadResult lr = Nso_LoadIntoGuest(&nso_h, nso, nso_len, &mem, &img);
                printf("\n== interpreter ==\n  Nso_LoadIntoGuest: %s\n", Load_ResultName(lr));
                if (lr != LOAD_OK) { free(scratch); return 1; }
                printf("  entry=%#llx mapped=%llu end=%#llx\n",
                       (unsigned long long)img.entry, (unsigned long long)img.mapped,
                       (unsigned long long)img.image_end);
                A64_Reset(&cpu, img.entry, 1);
                {
                    int stopped = A64_Run(&cpu, max_insns);
                    printf("  ran: insns=%llu stopped=%d halt=%s\n",
                           (unsigned long long)cpu.icount, stopped,
                           A64_HaltName(cpu.halt));
                    printf("  SVC: calls=%llu handled=%llu unknown=%llu badimm=%llu last_unknown=%llu\n",
                           (unsigned long long)hle.calls, (unsigned long long)hle.handled,
                           (unsigned long long)hle.unknown_calls, (unsigned long long)hle.bad_imm,
                           (unsigned long long)hle.last_unknown);
                    printf("  PC=%#llx fault_size=%llu fault_write=%d\n",
                           (unsigned long long)cpu.pc, (unsigned long long)cpu.fault_size,
                           cpu.fault_write);
                }
            }
        }
        free(scratch);
    }

    printf("\n== result: NSO extracted and executed (see counters above) ==\n");
    free(kbuf); free(nsp);
    return 0;
}
