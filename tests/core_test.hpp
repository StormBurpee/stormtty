#pragma once
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

inline std::size_t core_checks{};
#define CHECK(...) do { ++core_checks; if (!(__VA_ARGS__)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + ": " + #__VA_ARGS__); } while (false)
inline bool near(double a, double b, double tolerance = 1e-9) { return std::abs(a - b) <= tolerance; }
template<class F> int run_core_tests(const char* name, F&& f) {
    try { f(); std::cout << name << ": " << core_checks << " checks passed\n"; return EXIT_SUCCESS; }
    catch (const std::exception& e) { std::cerr << name << ": " << e.what() << '\n'; return EXIT_FAILURE; }
}
