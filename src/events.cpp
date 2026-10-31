#include "events.h"
#include <stdarg.h>
#include "log.h"

namespace events {

void emit(const char* type, const char* detailFmt, ...) {
    char detail[192] = "{}";
    if (detailFmt) {
        va_list args;
        va_start(args, detailFmt);
        vsnprintf(detail, sizeof(detail), detailFmt, args);
        va_end(args);
    }
    LOG("EVENT %s %s", type, detail);
}

}  // namespace events
