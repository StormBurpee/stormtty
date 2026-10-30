/* PORTABLE TEST DOUBLE ONLY. Never an include path in a native MSYS build. */
#ifndef ST_COMBINED_TEST_WINDOWS
#define ST_COMBINED_TEST_WINDOWS
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#define __declspec(x)
#define TRUE 1
#define FALSE 0
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xffffffffu
#define FILE_MAP_READ 4u
#define FILE_MAP_WRITE 2u
#define MOUSE_EVENT 2u
#define ERROR_INVALID_PARAMETER 87u
#define ZeroMemory(p,n) memset(p,0,n)
typedef void *HANDLE;
typedef void *HMODULE;
typedef void (*FARPROC)(void);
typedef uint32_t DWORD;
typedef int BOOL;
typedef int16_t SHORT;
typedef struct {SHORT X,Y;} COORD;
typedef struct {SHORT Left,Top,Right,Bottom;} SMALL_RECT;
typedef struct {COORD dwSize,dwCursorPosition;unsigned short wAttributes;SMALL_RECT srWindow;COORD dwMaximumWindowSize;} CONSOLE_SCREEN_BUFFER_INFO;
typedef struct {DWORD nLength;void *lpSecurityDescriptor;BOOL bInheritHandle;} SECURITY_ATTRIBUTES;
#ifdef __cplusplus
extern "C" {
#endif
DWORD GetLastError(void);
void SetLastError(DWORD);
DWORD GetCurrentProcessId(void);
DWORD GetCurrentThreadId(void);
void *MapViewOfFile(HANDLE,DWORD,DWORD,DWORD,size_t);
BOOL UnmapViewOfFile(const void *);
DWORD WaitForSingleObject(HANDLE,DWORD);
DWORD WaitForMultipleObjects(DWORD,const HANDLE *,BOOL,DWORD);
BOOL ReleaseMutex(HANDLE);
BOOL SetEvent(HANDLE);
HANDLE CreateEventW(SECURITY_ATTRIBUTES *,BOOL,BOOL,const wchar_t *);
BOOL CloseHandle(HANDLE);
BOOL GetConsoleScreenBufferInfo(HANDLE,CONSOLE_SCREEN_BUFFER_INFO *);
BOOL SetConsoleWindowInfo(HANDLE,BOOL,const SMALL_RECT *);
HMODULE GetModuleHandleW(const wchar_t *);
FARPROC GetProcAddress(HMODULE,const char *);
#ifdef __cplusplus
}
#endif
#endif
