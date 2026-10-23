#include <stdio.h>
#include <stdlib.h>
__attribute__((constructor)) static void c(void) { const char* d = getenv("HM_MARK"); if (d) { FILE* f = fopen(d, "w"); if (f) { fputs("loaded", f); fclose(f); } } }
const char* hm_bundle_id(void) { return "tiny-test"; }
