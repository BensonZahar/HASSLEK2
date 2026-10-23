#!/bin/sh
# Эмуляция: макет libil2cpp.so + бандл из Mods/ (все моды вместе или один мод с ядром). Запуск: sh test/emu/run.sh [rec|near|nocol|route]
set -e
D="$(cd "$(dirname "$0")" && pwd)"; M="$D/../../Mods"; T=$(mktemp -d); cd "$T"
gcc -shared -fPIC -O1 -w -Wl,-soname,libil2cpp.so -o libil2cpp.so "$D/fake_il2cpp.c"
{ echo '#define KONSTT_BUNDLE 1'; echo '#include "hassle_core.c"'
  if [ -n "$1" ]; then echo "#include \"hassle_$1.c\""; else for m in near rec nocol route; do echo "#include \"hassle_$m.c\""; done; fi
  echo 'EXPORT const char* hm_test_diag(void) { return g_diag_mem; }'; } > bundle_test.c
gcc -shared -fPIC -O1 -Wno-unused-function -DKONSTT_SETTLE_S=1 -DHM_BUILD_ID='"t"' -I"$M" -o bundle_test.so bundle_test.c -lm
gcc -O1 -o drv "$D/drv.c" -ldl
LD_LIBRARY_PATH=. ./drv
