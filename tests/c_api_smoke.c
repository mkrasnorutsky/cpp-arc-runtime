#include <cpp_arc/arc.h>
int c_api_smoke(void) {
    cpp_register_object(NULL, NULL);
    cpp_release(NULL);
    void* slot = NULL;
    cpp_storeStrong(&slot, NULL);
    return cpp_retain(NULL) != NULL || cpp_autorelease(NULL) != NULL || cpp_retainCount(NULL) != 0;
}
