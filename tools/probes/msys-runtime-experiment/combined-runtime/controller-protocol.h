/* Diagnostic fixture component, not a launcher. Caller owns/reaps its job.
   Include after windows.h. Run only on a NEW, private synthetic console.
   The controller never reads the live trace, consumes/reorders input records,
   suspends a runtime thread, or acquires an MSYS mutex. */
#ifndef STORMTTY_COMBINED_CONTROLLER_PROTOCOL
#define STORMTTY_COMBINED_CONTROLLER_PROTOCOL

typedef struct {
 HANDLE arrived, resume, wait_notice;
} st_fixture_events;
typedef struct {
 DWORD wait_result, wait_error, geometry_error, release_error;
 unsigned arrived, queried, applied, released;
 SHORT width_before, width_after, height_before, height_after;
} st_fixture_gate_result;

static void st_fixture_close_events(st_fixture_events *events) {
 if(events->arrived)CloseHandle(events->arrived);
 if(events->resume)CloseHandle(events->resume);
 if(events->wait_notice)CloseHandle(events->wait_notice);
 ZeroMemory(events,sizeof *events);
}
static int st_fixture_create_events(st_fixture_events *events) {
 SECURITY_ATTRIBUTES security={sizeof security,NULL,TRUE};
 ZeroMemory(events,sizeof *events);
 /* Unnamed, inherited, AUTO-reset, initially nonsignalled. Explicit handle-list
    inheritance is still mandatory in the owning controller. */
 events->arrived=CreateEventW(&security,FALSE,FALSE,NULL);
 events->resume=CreateEventW(&security,FALSE,FALSE,NULL);
 events->wait_notice=CreateEventW(&security,FALSE,FALSE,NULL);
 if(!events->arrived||!events->resume||!events->wait_notice){st_fixture_close_events(events);return 0;}
 return 1;
}
static int st_fixture_service_gate(HANDLE child,HANDLE output,const st_fixture_events *events,
 int width_delta,st_fixture_gate_result *result) {
 HANDLE waits[2]={events->arrived,child};
 CONSOLE_SCREEN_BUFFER_INFO before,after;SMALL_RECT rectangle;
 ZeroMemory(result,sizeof *result);
 /* Controller timeout is longer than the runtime's 1500ms bound. A late
    rendezvous is NOT evidence: GATE_END must independently report success. */
 result->wait_result=WaitForMultipleObjects(2,waits,FALSE,2000);
 result->wait_error=result->wait_result==WAIT_FAILED?GetLastError():0;
 if(result->wait_result!=WAIT_OBJECT_0)return 0;
 result->arrived=1;
 if(width_delta < -1 || width_delta > 1){result->geometry_error=ERROR_INVALID_PARAMETER;goto release;}
 if(!GetConsoleScreenBufferInfo(output,&before)){result->geometry_error=GetLastError();goto release;}
 result->queried=1;
 result->width_before=(SHORT)(before.srWindow.Right-before.srWindow.Left+1);
 result->height_before=(SHORT)(before.srWindow.Bottom-before.srWindow.Top+1);
 rectangle=before.srWindow;
 if((int)rectangle.Right+width_delta<=rectangle.Left||
    (int)rectangle.Right+width_delta>=before.dwSize.X){result->geometry_error=ERROR_INVALID_PARAMETER;goto release;}
 rectangle.Right=(SHORT)(rectangle.Right+width_delta);
 if(width_delta&&!SetConsoleWindowInfo(output,TRUE,&rectangle)){result->geometry_error=GetLastError();goto release;}
 if(!GetConsoleScreenBufferInfo(output,&after)){result->geometry_error=GetLastError();goto release;}
 result->width_after=(SHORT)(after.srWindow.Right-after.srWindow.Left+1);
 result->height_after=(SHORT)(after.srWindow.Bottom-after.srWindow.Top+1);
 result->applied=result->width_after==result->width_before+width_delta&&
     result->height_after==result->height_before;
release:
 /* Even a geometry failure releases an arrived reader. Caller must then reap
    its job and report INCONCLUSIVE, not continue a partially applied plan. */
 result->released=!!SetEvent(events->resume);
 result->release_error=result->released?0:GetLastError();
 return result->arrived&&result->queried&&result->applied&&result->released&&!result->geometry_error;
}
#endif
