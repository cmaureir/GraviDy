#ifndef COMMON_HPP
#define COMMON_HPP
#include "common.hpp"
#endif

#include "dynamics_cpu.hpp"
#include "dynamics_gpu.cuh"
#include "equilibrium.hpp"

void integrate_gpu();
void integrate_cpu();