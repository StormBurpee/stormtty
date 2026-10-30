/* Linux syntax-only declarations. NOT ABI/layout or native execution evidence. */
#include "../../tests/windows.h"
#include <stdio.h>
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define ERROR_INVALID_HANDLE 6u
#define GENERIC_READ 1u
#define GENERIC_WRITE 2u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define OPEN_EXISTING 3u
#define CREATE_NEW 1u
#define FILE_ATTRIBUTE_NORMAL 128u
#define ENABLE_EXTENDED_FLAGS 128u
#define ENABLE_WINDOW_INPUT 8u
#define ENABLE_MOUSE_INPUT 16u
#define PAGE_READWRITE 4u
#define JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE 8192u
#define JobObjectExtendedLimitInformation 9
#define JobObjectAssociateCompletionPortInformation 7
#define JobObjectBasicAccountingInformation 1
#define PROC_THREAD_ATTRIBUTE_HANDLE_LIST 0x20002u
#define PROC_THREAD_ATTRIBUTE_JOB_LIST 0x2000du
#define STARTF_USESTDHANDLES 256u
#define CREATE_SUSPENDED 4u
#define EXTENDED_STARTUPINFO_PRESENT 0x80000u
#define STILL_ACTIVE 259u
#define KEY_EVENT 1u
#define FOCUS_EVENT 16u
#define WINDOW_BUFFER_SIZE_EVENT 4u
#define MOUSE_WHEELED 4u
#define WHEEL_DELTA 120u
#define STD_INPUT_HANDLE ((DWORD)-10)
#define STD_OUTPUT_HANDLE ((DWORD)-11)
#define _TRUNCATE ((size_t)-1)
typedef uint16_t WCHAR;
typedef uint64_t ULONGLONG;
typedef uintptr_t ULONG_PTR;
typedef size_t SIZE_T;
typedef void *LPOVERLAPPED;
typedef void *LPPROC_THREAD_ATTRIBUTE_LIST;
typedef struct {BOOL bKeyDown;uint16_t wRepeatCount,wVirtualKeyCode,wVirtualScanCode;union {WCHAR UnicodeChar;char AsciiChar;}uChar;DWORD dwControlKeyState;} KEY_EVENT_RECORD;
typedef struct {COORD dwMousePosition;DWORD dwButtonState,dwControlKeyState,dwEventFlags;} MOUSE_EVENT_RECORD;
typedef struct {uint16_t EventType;union {KEY_EVENT_RECORD KeyEvent;MOUSE_EVENT_RECORD MouseEvent;struct {BOOL bSetFocus;}FocusEvent;struct {COORD dwSize;}WindowBufferSizeEvent;}Event;} INPUT_RECORD;
typedef struct {DWORD cb,dwFlags;HANDLE hStdInput,hStdOutput,hStdError;} STARTUPINFOA;
typedef struct {STARTUPINFOA StartupInfo;LPPROC_THREAD_ATTRIBUTE_LIST lpAttributeList;} STARTUPINFOEXA;
typedef struct {HANDLE hProcess,hThread;DWORD dwProcessId,dwThreadId;} PROCESS_INFORMATION;
typedef struct {struct {DWORD LimitFlags;}BasicLimitInformation;} JOBOBJECT_EXTENDED_LIMIT_INFORMATION;
typedef struct {void *CompletionKey;HANDLE CompletionPort;} JOBOBJECT_ASSOCIATE_COMPLETION_PORT;
typedef struct {DWORD ActiveProcesses;} JOBOBJECT_BASIC_ACCOUNTING_INFORMATION;
BOOL FreeConsole(void);BOOL AllocConsole(void);
HANDLE CreateFileA(const char*,DWORD,DWORD,SECURITY_ATTRIBUTES*,DWORD,DWORD,HANDLE);
BOOL SetConsoleMode(HANDLE,DWORD);BOOL GetConsoleMode(HANDLE,DWORD*);HANDLE GetStdHandle(DWORD);
HANDLE CreateFileMappingW(HANDLE,SECURITY_ATTRIBUTES*,DWORD,DWORD,DWORD,const wchar_t*);
HANDLE CreateJobObjectW(SECURITY_ATTRIBUTES*,const wchar_t*);
HANDLE CreateIoCompletionPort(HANDLE,HANDLE,ULONG_PTR,DWORD);
BOOL SetInformationJobObject(HANDLE,int,void*,DWORD);
BOOL QueryInformationJobObject(HANDLE,int,void*,DWORD,DWORD*);
BOOL InitializeProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST,DWORD,DWORD,SIZE_T*);
BOOL UpdateProcThreadAttribute(LPPROC_THREAD_ATTRIBUTE_LIST,DWORD,uintptr_t,void*,SIZE_T,void*,SIZE_T*);
void DeleteProcThreadAttributeList(LPPROC_THREAD_ATTRIBUTE_LIST);
HANDLE GetProcessHeap(void);void *HeapAlloc(HANDLE,DWORD,SIZE_T);BOOL HeapFree(HANDLE,DWORD,void*);
BOOL CreateProcessA(const char*,char*,void*,void*,BOOL,DWORD,void*,const char*,STARTUPINFOA*,PROCESS_INFORMATION*);
DWORD ResumeThread(HANDLE);
BOOL GetNumberOfConsoleInputEvents(HANDLE,DWORD*);BOOL WriteConsoleInputW(HANDLE,const INPUT_RECORD*,DWORD,DWORD*);
BOOL GetExitCodeProcess(HANDLE,DWORD*);BOOL TerminateJobObject(HANDLE,unsigned);
ULONGLONG GetTickCount64(void);BOOL GetQueuedCompletionStatus(HANDLE,DWORD*,ULONG_PTR*,LPOVERLAPPED*,DWORD);
BOOL WriteFile(HANDLE,const void*,DWORD,DWORD*,void*);BOOL FlushFileBuffers(HANDLE);
int fopen_s(FILE**,const char*,const char*);int _snprintf_s(char*,size_t,size_t,const char*,...);
