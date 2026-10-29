// Cross/native compile and disassembly input only; never an evidence source.
#define STORMTTY_PASSIVE_CONSOLE_TRACE 1
#include "recorder.hpp"
extern "C" __declspec(dllexport) void stormtty_path_smoke_v1() {
 st_path::read_begin(2,0,2,0,0);st_path::copy(1,0,0,1);
 st_path::event(st_path::READ_MORE,1,2,2);st_path::peek(1,0,2,1,0,false);
 st_path::dispatch(2,0,2,0,0,0);st_path::geometry(80,24,79,24,true);
 st_path::process_return(0,0,false);st_path::read_end(2,2,0);
}
