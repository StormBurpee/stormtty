#include "stormtty/smoke_deadline.hpp"
#include <stdexcept>
#include <iostream>
using stormtty::SmokeDeadline;
void check(bool value) { if (!value) throw std::runtime_error("Smoke deadline invariant failed"); }
int main() {
    using P=SmokeDeadline::Phase;
    SmokeDeadline startup(100); check(!startup.expired(20099)); check(startup.expired(20100));
    check(!startup.advance(P::interaction,20100)); // Expired work cannot rescue itself.
    SmokeDeadline d(100); check(d.advance(P::interaction,15100));
    check(!d.advance(P::interaction,16000)); check(!d.advance(P::startup,17000));
    check(d.phase_elapsed(17000)==1900); check(!d.expired(35099)); check(d.expired(35100));
    SmokeDeadline exit(100); check(exit.advance(P::interaction,19100)); check(exit.advance(P::exit,38100));
    check(exit.phase_budget()==10000); check(!exit.expired(48099)); check(exit.expired(48100));
    SmokeDeadline hard(100); check(hard.advance(P::interaction,20099)); check(hard.advance(P::exit,40098));
    check(hard.expired(50100)); check(hard.expired(99));
    std::cout << "PASS monotonic bounded startup/interaction/exit smoke deadlines\n";
}
