#include "PerfHarness.h"
#include "SECPK1/Random.h"
#include "Timer.h"
#include <cuda_runtime.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <math.h>
#include <stdio.h>
#include <sstream>
#include <vector>

using namespace std;

namespace {

const char *kPerfBinaryName = "kangaroo-perf";
const char *kBenchmarkProfileName = "embedded-test-profile-v1";
const char *kBenchmarkRangeStart = "0000003fffffffffffffffffffffffffffffffffffffffe00000000000000000";
const char *kBenchmarkRangeEnd = "00000040000000000000000000000000000000000000001fffffffffffffffff";
const char *kBenchmarkPublicKey = "03f7aef8a7e38440238f9332906e48f6fd5adbd02d56b76a5ffa5aca58c56c3943";
const char *kTimingSource = "cuda_events";
const char *kDpMode = "suppressed_max_mask";
const uint32_t kJumpTableSeed = 0x600DCAFEU;

}

PerfHarness::PerfHarness(const PerfOptions& options) : options(options), rangePower(0) {
}

void PerfHarness::PrintUsage() {

  printf("kangaroo-perf [--gpu-id id] [--grid X,Y] [--warmup n]\n");
  printf("              [--iterations n | --seconds s] [--json-out path] [--seed value]\n");
  printf("              [--help|-h]\n");
  printf(" --gpu-id <int>: GPU id to benchmark, default is 0\n");
  printf(" --grid <X,Y>: Fixed grid to benchmark, default is the solver auto grid\n");
  printf(" --warmup <int>: Number of warmup launches to discard, default is 5\n");
  printf(" --iterations <int>: Number of measured launches to run, default is 50 when no run mode is set\n");
  printf(" --seconds <double>: Run measured launches until wall-clock seconds elapse\n");
  printf(" --json-out <path>: Write JSON benchmark output to path\n");
  printf(" --seed <uint32>: Seed for initial herd generation, default is 0x600DCAFE\n");
  printf(" --help, -h: Show this help text\n");

}

void PerfHarness::LoadBenchmarkProfile() {

  bool isCompressed = false;

  rangeStart.SetBase16((char *)kBenchmarkRangeStart);
  rangeEnd.SetBase16((char *)kBenchmarkRangeEnd);

  if(!secp.ParsePublicKeyHex(string(kBenchmarkPublicKey),benchmarkPublicKey,isCompressed)) {
    throw PerfError(PERF_EXIT_RUNTIME_ERROR,"Failed to parse embedded benchmark public key");
  }

}

void PerfHarness::InitRange() {

  rangeWidth.Set(&rangeEnd);
  rangeWidth.Sub(&rangeStart);
  rangePower = rangeWidth.GetBitLength();
  rangeWidthDiv2.Set(&rangeWidth);
  rangeWidthDiv2.ShiftR(1);
  rangeWidthDiv4.Set(&rangeWidthDiv2);
  rangeWidthDiv4.ShiftR(1);

}

void PerfHarness::InitSearchKey() {

  Int shiftStart;
  shiftStart.Set(&rangeStart);
#ifdef USE_SYMMETRY
  shiftStart.ModAddK1order(&rangeWidthDiv2);
#endif

  if(!shiftStart.IsZero()) {
    Point rangeStartPoint = secp.ComputePublicKey(&shiftStart);
    rangeStartPoint.y.ModNeg();
    keyToSearch = secp.AddDirect(benchmarkPublicKey,rangeStartPoint);
  } else {
    keyToSearch = benchmarkPublicKey;
  }

}

void PerfHarness::CreateJumpTable() {

#ifdef USE_SYMMETRY
  int jumpBit = rangePower / 2;
#else
  int jumpBit = rangePower / 2 + 1;
#endif

  if(jumpBit > 128) jumpBit = 128;
  int maxRetry = 100;
  bool ok = false;
  double distAvg = 0.0;
  double maxAvg = pow(2.0,(double)jumpBit - 0.95);
  double minAvg = pow(2.0,(double)jumpBit - 1.05);

  rseed(kJumpTableSeed);

#ifdef USE_SYMMETRY
  Int old;
  old.Set(Int::GetFieldCharacteristic());
  Int u;
  Int v;
  u.SetInt32(1);
  u.ShiftL(jumpBit / 2);
  u.AddOne();
  while(!u.IsProbablePrime()) {
    u.AddOne();
    u.AddOne();
  }
  v.Set(&u);
  v.AddOne();
  v.AddOne();
  while(!v.IsProbablePrime()) {
    v.AddOne();
    v.AddOne();
  }
  Int::SetupField(&old);
#endif

  while(!ok && maxRetry > 0) {
    Int totalDist;
    totalDist.SetInt32(0);
#ifdef USE_SYMMETRY
    for(int i = 0; i < NB_JUMP / 2; ++i) {
      jumpDistance[i].Rand(jumpBit / 2);
      jumpDistance[i].Mult(&u);
      if(jumpDistance[i].IsZero())
        jumpDistance[i].SetInt32(1);
      totalDist.Add(&jumpDistance[i]);
    }
    for(int i = NB_JUMP / 2; i < NB_JUMP; ++i) {
      jumpDistance[i].Rand(jumpBit / 2);
      jumpDistance[i].Mult(&v);
      if(jumpDistance[i].IsZero())
        jumpDistance[i].SetInt32(1);
      totalDist.Add(&jumpDistance[i]);
    }
#else
    for(int i = 0; i < NB_JUMP; ++i) {
      jumpDistance[i].Rand(jumpBit);
      if(jumpDistance[i].IsZero())
        jumpDistance[i].SetInt32(1);
      totalDist.Add(&jumpDistance[i]);
    }
#endif
    distAvg = totalDist.ToDouble() / (double)NB_JUMP;
    ok = distAvg > minAvg && distAvg < maxAvg;
    maxRetry--;
  }

  if(!ok) {
    throw PerfError(PERF_EXIT_RUNTIME_ERROR,"Failed to generate benchmark jump table");
  }

  for(int i = 0; i < NB_JUMP; ++i) {
    Point jumpPoint = secp.ComputePublicKey(&jumpDistance[i]);
    jumpPointx[i].Set(&jumpPoint.x);
    jumpPointy[i].Set(&jumpPoint.y);
  }

}

void PerfHarness::CreateHerd(int nbKangaroo,Int *px,Int *py,Int *distance,int firstType) {

  vector<Int> privKeys;
  vector<Point> points;
  vector<Point> startPoints;
  Point zeroPoint;
  zeroPoint.Clear();

  privKeys.reserve(nbKangaroo);
  points.reserve(nbKangaroo);
  startPoints.reserve(nbKangaroo);

  for(int i = 0; i < nbKangaroo; i++) {
#ifdef USE_SYMMETRY
    distance[i].Rand(rangePower - 1);
    if((i + firstType) % 2 == WILD) {
      distance[i].ModSubK1order(&rangeWidthDiv4);
    }
#else
    distance[i].Rand(rangePower);
    if((i + firstType) % 2 == WILD) {
      distance[i].ModSubK1order(&rangeWidthDiv2);
    }
#endif
    privKeys.push_back(distance[i]);
  }

  points = secp.ComputePublicKeys(privKeys);

  for(int i = 0; i < nbKangaroo; i++) {
    if((i + firstType) % 2 == TAME) {
      startPoints.push_back(zeroPoint);
    } else {
      startPoints.push_back(keyToSearch);
    }
  }

  points = secp.AddDirect(startPoints,points);

  for(int i = 0; i < nbKangaroo; i++) {
    px[i].Set(&points[i].x);
    py[i].Set(&points[i].y);
#ifdef USE_SYMMETRY
    if(py[i].ModPositiveK1())
      distance[i].ModNegK1order();
#endif
  }

}

void PerfHarness::ResolveGrid(int *gridSizeX,int *gridSizeY) {

  int defaultGridX = 0;
  int defaultGridY = 0;
  if(!GPUEngine::GetGridSize(options.gpuId,&defaultGridX,&defaultGridY)) {
    throw PerfError(PERF_EXIT_RUNTIME_ERROR,"Failed to query CUDA grid size");
  }

  if(options.gridSpecified) {
    *gridSizeX = options.gridSizeX;
    *gridSizeY = options.gridSizeY;
  } else {
    *gridSizeX = defaultGridX;
    *gridSizeY = defaultGridY;
  }

}

PerfResult PerfHarness::Run() {

  Timer::Init();
  secp.Init();

  LoadBenchmarkProfile();
  InitRange();
  InitSearchKey();
  CreateJumpTable();

  int gridSizeX = 0;
  int gridSizeY = 0;
  ResolveGrid(&gridSizeX,&gridSizeY);

  rseed(options.seed);

  GPUEngine gpu(gridSizeX,gridSizeY,options.gpuId,1);
  if(!gpu.IsInitialized()) {
    throw PerfError(PERF_EXIT_RUNTIME_ERROR,"Failed to initialize CUDA benchmark engine");
  }

  uint64_t walkersPerLaunch = GPUEngine::GetWalkersPerLaunch(gridSizeX,gridSizeY);
  uint64_t stepsPerLaunch = GPUEngine::GetStepsPerLaunch(gridSizeX,gridSizeY);
  uint64_t nbThread = (uint64_t)gpu.GetNbThread();
  vector<Int> px(walkersPerLaunch);
  vector<Int> py(walkersPerLaunch);
  vector<Int> distance(walkersPerLaunch);
  vector<ITEM> found;

  for(uint64_t i = 0; i < nbThread; i++) {
    CreateHerd(GPU_GRP_SIZE,&px[i * GPU_GRP_SIZE],&py[i * GPU_GRP_SIZE],&distance[i * GPU_GRP_SIZE],TAME);
  }

#ifdef USE_SYMMETRY
  gpu.SetWildOffset(&rangeWidthDiv4);
#else
  gpu.SetWildOffset(&rangeWidthDiv2);
#endif
  gpu.SetParams(UINT64_MAX,jumpDistance,jumpPointx,jumpPointy);
  gpu.SetKangaroos(&px[0],&py[0],&distance[0]);

  if(!gpu.callKernel()) {
    throw PerfError(PERF_EXIT_RUNTIME_ERROR,"Failed to launch initial benchmark kernel");
  }

  double kernelElapsedMs = 0.0;
  for(int i = 0; i < options.warmupIterations; i++) {
    if(!gpu.Launch(found,false,&kernelElapsedMs)) {
      throw PerfError(PERF_EXIT_RUNTIME_ERROR,"Warmup kernel launch failed");
    }
  }

  uint64_t measuredLaunches = 0;
  double totalKernelElapsedMs = 0.0;

  if(options.useSeconds) {
    double measureStart = Timer::get_tick();
    while(measuredLaunches == 0 || (Timer::get_tick() - measureStart) < options.requestedSeconds) {
      if(!gpu.Launch(found,false,&kernelElapsedMs)) {
        throw PerfError(PERF_EXIT_RUNTIME_ERROR,"Measured kernel launch failed");
      }
      totalKernelElapsedMs += kernelElapsedMs;
      measuredLaunches++;
    }
  } else {
    for(int i = 0; i < options.requestedIterations; i++) {
      if(!gpu.Launch(found,false,&kernelElapsedMs)) {
        throw PerfError(PERF_EXIT_RUNTIME_ERROR,"Measured kernel launch failed");
      }
      totalKernelElapsedMs += kernelElapsedMs;
      measuredLaunches++;
    }
  }

  cudaError_t syncError = cudaDeviceSynchronize();
  if(syncError != cudaSuccess) {
    throw PerfError(PERF_EXIT_RUNTIME_ERROR,string("Failed to synchronize benchmark device: ") + cudaGetErrorString(syncError));
  }

  PerfResult result;
  result.binary = kPerfBinaryName;
  result.benchmarkProfile = kBenchmarkProfileName;
  result.gpuId = options.gpuId;
  result.deviceName = gpu.deviceName;
  result.gridSizeX = gridSizeX;
  result.gridSizeY = gridSizeY;
  result.seed = options.seed;
  result.warmupIterations = options.warmupIterations;
  result.runMode = options.useSeconds ? "seconds" : "iterations";
  result.requestedIterations = options.useSeconds ? 0 : options.requestedIterations;
  result.requestedSeconds = options.useSeconds ? options.requestedSeconds : 0.0;
  result.measuredLaunches = measuredLaunches;
  result.walkersPerLaunch = walkersPerLaunch;
  result.stepsPerLaunch = stepsPerLaunch;
  result.totalSteps = stepsPerLaunch * measuredLaunches;
  result.totalKernelElapsedMs = totalKernelElapsedMs;
  result.avgKernelElapsedMs = measuredLaunches > 0 ? totalKernelElapsedMs / (double)measuredLaunches : 0.0;

  GPUBenchmarkMetrics metrics = GPUEngine::ComputeBenchmarkMetrics(gridSizeX,gridSizeY,result.avgKernelElapsedMs);
  result.kernelNsPerStep = metrics.kernelNsPerStep;
  result.stepsPerSecond = metrics.stepsPerSecond;
  result.legacyMKeysPerSecond = metrics.legacyMKeysPerSecond;

  PrintSummary(result);
  if(options.jsonOutPath.length() > 0)
    WriteJson(result);

  return result;

}

void PerfHarness::PrintSummary(const PerfResult& result) {

#ifdef USE_SYMMETRY
  cout << kPerfBinaryName << " v" << RELEASE << " (with symmetry)" << endl;
#else
  cout << kPerfBinaryName << " v" << RELEASE << endl;
#endif
  cout << "Profile: " << result.benchmarkProfile << endl;
  cout << "GPU id: " << result.gpuId << endl;
  cout << "Device: " << result.deviceName << endl;
  cout << "Grid: " << result.gridSizeX << "x" << result.gridSizeY << endl;
  cout << "Warmup launches: " << result.warmupIterations << endl;
  cout << "Measured launches: " << result.measuredLaunches << endl;
  cout << "Walkers per launch: " << result.walkersPerLaunch << endl;
  cout << "Steps per launch: " << result.stepsPerLaunch << endl;
  cout << "Total steps: " << result.totalSteps << endl;
  cout << "Total kernel elapsed ms: " << FormatDouble(result.totalKernelElapsedMs) << endl;
  cout << "Average kernel elapsed ms: " << FormatDouble(result.avgKernelElapsedMs) << endl;
  cout << "kernel_ns_per_step: " << FormatDouble(result.kernelNsPerStep) << endl;
  cout << "steps_per_sec: " << FormatDouble(result.stepsPerSecond) << endl;
  cout << "legacy_mkeys_per_sec: " << FormatDouble(result.legacyMKeysPerSecond) << endl;
  cout << "Timing source: " << kTimingSource << endl;

}

void PerfHarness::WriteJson(const PerfResult& result) {

  ofstream out(options.jsonOutPath.c_str(),ios::out | ios::trunc);
  if(!out.is_open()) {
    throw PerfError(PERF_EXIT_JSON_ERROR,"Failed to open JSON output path");
  }

  out << "{\n";
  out << "  \"binary\": \"" << JsonEscape(result.binary) << "\",\n";
  out << "  \"benchmark_profile\": \"" << JsonEscape(result.benchmarkProfile) << "\",\n";
  out << "  \"gpu_id\": " << result.gpuId << ",\n";
  out << "  \"device_name\": \"" << JsonEscape(result.deviceName) << "\",\n";
  out << "  \"grid_x\": " << result.gridSizeX << ",\n";
  out << "  \"grid_y\": " << result.gridSizeY << ",\n";
  out << "  \"seed\": " << result.seed << ",\n";
  out << "  \"warmup_iterations\": " << result.warmupIterations << ",\n";
  out << "  \"run_mode\": \"" << result.runMode << "\",\n";
  out << "  \"requested_iterations\": " << result.requestedIterations << ",\n";
  out << "  \"requested_seconds\": " << FormatDouble(result.requestedSeconds) << ",\n";
  out << "  \"measured_launches\": " << result.measuredLaunches << ",\n";
  out << "  \"walkers_per_launch\": " << result.walkersPerLaunch << ",\n";
  out << "  \"steps_per_launch\": " << result.stepsPerLaunch << ",\n";
  out << "  \"total_steps\": " << result.totalSteps << ",\n";
  out << "  \"total_kernel_elapsed_ms\": " << FormatDouble(result.totalKernelElapsedMs) << ",\n";
  out << "  \"avg_kernel_elapsed_ms\": " << FormatDouble(result.avgKernelElapsedMs) << ",\n";
  out << "  \"kernel_ns_per_step\": " << FormatDouble(result.kernelNsPerStep) << ",\n";
  out << "  \"steps_per_sec\": " << FormatDouble(result.stepsPerSecond) << ",\n";
  out << "  \"legacy_mkeys_per_sec\": " << FormatDouble(result.legacyMKeysPerSecond) << ",\n";
  out << "  \"timing_source\": \"" << kTimingSource << "\",\n";
  out << "  \"dp_mode\": \"" << kDpMode << "\"\n";
  out << "}\n";

  if(!out.good()) {
    throw PerfError(PERF_EXIT_JSON_ERROR,"Failed while writing JSON output");
  }

}

string PerfHarness::JsonEscape(const string& value) {

  string escaped;
  escaped.reserve(value.size());

  for(size_t i = 0; i < value.size(); i++) {
    unsigned char c = (unsigned char)value[i];
    switch(c) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if(c < 0x20) {
          char tmp[7];
          sprintf(tmp,"\\u%04x",c);
          escaped += tmp;
        } else {
          escaped.push_back((char)c);
        }
        break;
    }
  }

  return escaped;

}

string PerfHarness::FormatDouble(double value) {
  ostringstream stream;
  stream << fixed << setprecision(6) << value;
  return stream.str();
}
