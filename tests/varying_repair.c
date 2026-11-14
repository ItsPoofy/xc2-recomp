// Standalone driver for the shader-pair pass; all test shaders are synthetic.
#include "../runtime/vk_varying.h"
#include <stdio.h>
#include <stdlib.h>

static uint32_t *read_words(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long bytes = ftell(f); rewind(f);
    if (bytes < 20 || bytes % 4) { fclose(f); return NULL; }
    uint32_t *w = malloc((size_t)bytes);
    if (w && fread(w, 1, (size_t)bytes, f) != (size_t)bytes) { free(w); w = NULL; }
    fclose(f); *n = (size_t)bytes / 4; return w;
}
static int write_words(const char *path, const uint32_t *w, size_t n) {
    FILE *f = fopen(path, "wb"); if (!f) return 0;
    int ok = fwrite(w, 4, n, f) == n; return fclose(f) == 0 && ok;
}
int main(int argc, char **argv) {
    if (argc != 5 && argc != 6) return 2;
    size_t vn = 0, fn = 0, nv = 0, nf = 0;
    uint32_t *v = read_words(argv[1], &vn), *f = read_words(argv[2], &fn), *vo = NULL, *fo = NULL;
    if (!v || !f) { free(v); free(f); return 2; }
    int count = vk_varying_rewrite(v, vn, f, fn, argc == 6 ? (unsigned)atoi(argv[5]) : 32, &vo, &nv, &fo, &nf);
    int result = count ? write_words(argv[3], vo, nv) && write_words(argv[4], fo, nf) ? 0 : 2 : 3;
    printf("repaired inputs: %d\n", count);
    free(v); free(f); free(vo); free(fo); return result;
}
