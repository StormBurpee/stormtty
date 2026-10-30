/* Compile-only C ABI/signal-helper surface; not a native import check. */
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#include "windows.h"
#include "../reader-protocol.h"
int main(void){(void)st_reader_resolve;(void)st_reader_install_handlers;return 0;}
