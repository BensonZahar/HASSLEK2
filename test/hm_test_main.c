/* Тестовая обвязка: поднимает настоящую JVM (OpenJDK) и запускает загрузчик через JNI HttpURLConnection, как в Android. */
#include <stdio.h>
#include <stdlib.h>
typedef struct { char* optionString; void* extraInfo; } JavaVMOption;
typedef struct { int version; int nOptions; JavaVMOption* options; unsigned char ignoreUnrecognized; } JavaVMInitArgs;
extern int JNI_CreateJavaVM(void** vm, void** env, void* args);
extern void hm_test_set_dir(const char*);
extern void hm_test_run(void*);
int main(int argc, char** argv) {
    void *vm = NULL, *env = NULL; JavaVMOption o[1]; JavaVMInitArgs a; int rc;
    if (argc < 2) return 2;
    o[0].optionString = "-Xcheck:jni"; o[0].extraInfo = NULL;
    a.version = 0x00010008; a.nOptions = 1; a.options = o; a.ignoreUnrecognized = 1;
    rc = JNI_CreateJavaVM(&vm, &env, &a);
    fprintf(stderr, "[test] JNI_CreateJavaVM rc=%d vm=%p\n", rc, vm);
    if (rc != 0) return 3;
    hm_test_set_dir(argv[1]);
    hm_test_run(vm);
    fprintf(stderr, "[test] hm_test_run вернулся\n");
    return 0;
}
