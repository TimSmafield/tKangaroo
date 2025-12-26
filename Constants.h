/*
* This file is part of the BSGS distribution (https://github.com/JeanLucPons/Kangaroo).
* Copyright (c) 2020 Jean Luc PONS.
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, version 3.
*
* This program is distributed in the hope that it will be useful, but
* WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
* General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef CONSTANTSH
#define CONSTANTSH

// Release number
#define RELEASE "2.2"

// Use symmetry - provides sqrt(2) speedup (~41% faster)
// Enabled for 150-bit support
#define USE_SYMMETRY

// Number of random jumps
// Max 512 for the GPU - increased to 64 for better distribution
// More jumps = more uniform random walk = closer to theoretical bounds
#define NB_JUMP 64

// GPU group size
// 128 is optimal balance between throughput and register pressure
// Higher values may cause register spills on older GPUs
#define GPU_GRP_SIZE 128

// GPU number of run per kernel call
// Increased for better GPU utilization and reduced kernel launch overhead
#define NB_RUN 128

// Kangaroo type
#define TAME 0  // Tame kangaroo
#define WILD 1  // Wild kangaroo

// SendDP Period in sec
#define SEND_PERIOD 2.0

// Timeout before closing connection idle client in sec
#define CLIENT_TIMEOUT 3600.0

// Number of merge partition
#define MERGE_PART 256

#endif //CONSTANTSH
