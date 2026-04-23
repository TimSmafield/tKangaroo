#include "PerfHarness.h"
#include "SECPK1/Random.h"
#include "Timer.h"
#include <algorithm>
#include <cuda_runtime.h>
#include <errno.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <math.h>
#include <stdio.h>
#include <sstream>
#include <sys/stat.h>
#include <vector>

#ifdef WIN64
#include <direct.h>
#endif

#ifndef PERF_GIT_COMMIT
#define PERF_GIT_COMMIT "unknown"
#endif

using namespace std;

namespace {

const char *kPerfBinaryName = "kangaroo-perf";
const char *kBenchmarkProfileName = "embedded-test-profile-v1";
const char *kBenchmarkRangeStart = "0000003fffffffffffffffffffffffffffffffffffffffe00000000000000000";
const char *kBenchmarkRangeEnd = "00000040000000000000000000000000000000000000001fffffffffffffffff";
const char *kBenchmarkPublicKey = "03f7aef8a7e38440238f9332906e48f6fd5adbd02d56b76a5ffa5aca58c56c3943";
const char *kTimingSource = "cuda_events";
const char *kDpMode = "suppressed_max_mask";
const char *kStabilizationMode = "min_launches_plus_kernel_floor";
const char *kSweepMode = "auto_band";
const char *kSweepRanking = "kernel_ns_per_step asc, steps_per_sec desc, grid_x asc, grid_y asc";
const char *kDefaultResultDir = "perf/results.local";
const char *kDefaultGitCommit = PERF_GIT_COMMIT;
const uint32_t kJumpTableSeed = 0x600DCAFEU;
const double kWarmupKernelFloorMs = 1000.0;

bool DirectoryExists(const string& path) {

  struct stat info;
  if(stat(path.c_str(),&info) != 0)
    return false;

  return (info.st_mode & S_IFDIR) != 0;

}

void EnsureDirectoryExists(const string& path) {

  if(DirectoryExists(path))
    return;

#ifdef WIN64
  int rc = _mkdir(path.c_str());
#else
  int rc = mkdir(path.c_str(),0755);
#endif
  if(rc != 0 && errno != EEXIST) {
    throw PerfError(PERF_EXIT_JSON_ERROR,string("Failed to create directory: ") + path);
  }

  if(!DirectoryExists(path)) {
    throw PerfError(PERF_EXIT_JSON_ERROR,string("Path is not a directory: ") + path);
  }

}

}

PerfHarness::PerfHarness(const PerfOptions& options) : options(options), rangePower(0) {
}

void PerfHarness::PrintUsage() {

  printf("kangaroo-perf [--gpu-id id] [--grid X,Y] [--grid-sweep auto] [--warmup n]\n");
  printf("              [--iterations n | --seconds s] [--json-out path] [--seed value]\n");
  printf("              [--help|-h]\n");
  printf(" --gpu-id <int>: GPU id to benchmark, default is 0\n");
  printf(" --grid <X,Y>: Fixed grid to benchmark, default is the solver auto grid\n");
  printf(" --grid-sweep auto: Sweep a deterministic 3x3 grid band around the auto grid or the --grid center\n");
  printf(" --warmup <int>: Minimum number of warmup launches to discard, default is 5\n");
  printf(" --iterations <int>: Number of measured launches to run, default is 50 when no run mode is set\n");
  printf(" --seconds <double>: Run measured launches until wall-clock seconds elapse\n");
  printf(" --json-out <path>: Override the default JSON output path\n");
  printf(" --seed <uint32>: Seed for initial herd generation, default is 0x600DCAFE\n");
  printf(" Benchmark warmup also discards at least %.1f ms of kernel time for stabilization\n",kWarmupKernelFloorMs);
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

int PerfHarness::RoundToNearestInt(double value) {
  return (int)floor(value + 0.5);
}

int PerfHarness::RoundToNearestMultiple(double value,int multiple) {

  if(multiple <= 0)
    return RoundToNearestInt(value);

  return RoundToNearestInt(value / (double)multiple) * multiple;

}

vector<int> PerfHarness::BuildSweepAxis(int center,int step) {

  vector<int> axis;
  axis.push_back(center - step);
  axis.push_back(center);
  axis.push_back(center + step);

  vector<int> filtered;
  filtered.reserve(axis.size());
  for(size_t i = 0; i < axis.size(); i++) {
    if(axis[i] > 0)
      filtered.push_back(axis[i]);
  }

  sort(filtered.begin(),filtered.end());
  filtered.erase(unique(filtered.begin(),filtered.end()),filtered.end());
  return filtered;

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

PerfResult PerfHarness::BenchmarkGrid(int gridSizeX,int gridSizeY) {

  double setupStart = Timer::get_tick();
  secp.Init();

  LoadBenchmarkProfile();
  InitRange();
  InitSearchKey();
  CreateJumpTable();

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
  found.reserve(1);

  for(uint64_t i = 0; i < nbThread; i++) {
    CreateHerd(GPU_GRP_SIZE,&px[i * GPU_GRP_SIZE],&py[i * GPU_GRP_SIZE],&distance[i * GPU_GRP_SIZE],TAME);
  }
  double setupMs = (Timer::get_tick() - setupStart) * 1000.0;

#ifdef USE_SYMMETRY
  gpu.SetWildOffset(&rangeWidthDiv4);
#else
  gpu.SetWildOffset(&rangeWidthDiv2);
#endif
  double uploadStart = Timer::get_tick();
  gpu.SetParams(UINT64_MAX,jumpDistance,jumpPointx,jumpPointy);
  gpu.SetKangaroos(&px[0],&py[0],&distance[0]);
  double uploadMs = (Timer::get_tick() - uploadStart) * 1000.0;

  if(!gpu.callKernel()) {
    throw PerfError(PERF_EXIT_RUNTIME_ERROR,"Failed to launch initial benchmark kernel");
  }

  GPULaunchTimings launchTimings;
  uint64_t actualWarmupLaunches = 0;
  double warmupKernelElapsedMs = 0.0;
  while(actualWarmupLaunches < (uint64_t)options.warmupIterations || warmupKernelElapsedMs < kWarmupKernelFloorMs) {
    if(!gpu.Launch(found,false,&launchTimings)) {
      throw PerfError(PERF_EXIT_RUNTIME_ERROR,"Warmup kernel launch failed");
    }
    warmupKernelElapsedMs += launchTimings.kernelMs;
    actualWarmupLaunches++;
  }

  uint64_t measuredLaunches = 0;
  double totalKernelElapsedMs = 0.0;
  double totalWaitMs = 0.0;
  double totalCopyMs = 0.0;
  double totalPostMs = 0.0;

  if(options.useSeconds) {
    double measureStart = Timer::get_tick();
    while(measuredLaunches == 0 || (Timer::get_tick() - measureStart) < options.requestedSeconds) {
      if(!gpu.Launch(found,false,&launchTimings)) {
        throw PerfError(PERF_EXIT_RUNTIME_ERROR,"Measured kernel launch failed");
      }
      totalKernelElapsedMs += launchTimings.kernelMs;
      totalWaitMs += launchTimings.waitMs;
      totalCopyMs += launchTimings.copyMs;
      totalPostMs += launchTimings.postMs;
      measuredLaunches++;
    }
  } else {
    for(int i = 0; i < options.requestedIterations; i++) {
      if(!gpu.Launch(found,false,&launchTimings)) {
        throw PerfError(PERF_EXIT_RUNTIME_ERROR,"Measured kernel launch failed");
      }
      totalKernelElapsedMs += launchTimings.kernelMs;
      totalWaitMs += launchTimings.waitMs;
      totalCopyMs += launchTimings.copyMs;
      totalPostMs += launchTimings.postMs;
      measuredLaunches++;
    }
  }

  cudaError_t syncError = cudaDeviceSynchronize();
  if(syncError != cudaSuccess) {
    throw PerfError(PERF_EXIT_RUNTIME_ERROR,string("Failed to synchronize benchmark device: ") + cudaGetErrorString(syncError));
  }

  PerfResult result;
  GPUDeviceMetadata deviceMetadata = gpu.GetDeviceMetadata();
  result.binary = kPerfBinaryName;
  result.benchmarkProfile = kBenchmarkProfileName;
  result.gpuId = options.gpuId;
  result.deviceName = gpu.deviceName;
  result.gpuName = deviceMetadata.gpuName;
  result.computeCapabilityMajor = deviceMetadata.computeCapabilityMajor;
  result.computeCapabilityMinor = deviceMetadata.computeCapabilityMinor;
  result.gridSizeX = gridSizeX;
  result.gridSizeY = gridSizeY;
  result.totalThreads = deviceMetadata.totalThreads;
  result.gitCommit = string(kDefaultGitCommit);
  result.seed = options.seed;
  result.warmupIterations = options.warmupIterations;
  result.actualWarmupLaunches = actualWarmupLaunches;
  result.warmupKernelElapsedMs = warmupKernelElapsedMs;
  result.runMode = options.useSeconds ? "seconds" : "iterations";
  result.requestedIterations = options.useSeconds ? 0 : options.requestedIterations;
  result.requestedSeconds = options.useSeconds ? options.requestedSeconds : 0.0;
  result.measuredLaunches = measuredLaunches;
  result.walkersPerLaunch = walkersPerLaunch;
  result.stepsPerLaunch = stepsPerLaunch;
  result.totalSteps = stepsPerLaunch * measuredLaunches;
  result.setupMs = setupMs;
  result.uploadMs = uploadMs;
  result.totalKernelElapsedMs = totalKernelElapsedMs;
  result.avgKernelElapsedMs = measuredLaunches > 0 ? totalKernelElapsedMs / (double)measuredLaunches : 0.0;
  result.totalWaitMs = totalWaitMs;
  result.avgWaitMs = measuredLaunches > 0 ? totalWaitMs / (double)measuredLaunches : 0.0;
  result.totalCopyMs = totalCopyMs;
  result.avgCopyMs = measuredLaunches > 0 ? totalCopyMs / (double)measuredLaunches : 0.0;
  result.totalPostMs = totalPostMs;
  result.avgPostMs = measuredLaunches > 0 ? totalPostMs / (double)measuredLaunches : 0.0;

  GPUBenchmarkMetrics metrics = GPUEngine::ComputeBenchmarkMetrics(gridSizeX,gridSizeY,result.avgKernelElapsedMs);
  result.kernelNsPerStep = metrics.kernelNsPerStep;
  result.stepsPerSecond = metrics.stepsPerSecond;
  result.legacyMKeysPerSecond = metrics.legacyMKeysPerSecond;
  result.stabilizationMode = kStabilizationMode;
  result.warmupKernelFloorMs = kWarmupKernelFloorMs;
  result.jsonOutputPath = "";

  return result;

}

PerfSweepResult PerfHarness::RunSweep(int centerGridX,int centerGridY) {

  int gridStepX = max(1,RoundToNearestInt((double)centerGridX * 0.125));
  int gridStepY = max(32,RoundToNearestMultiple((double)centerGridY * 0.125,32));
  vector<int> xAxis = BuildSweepAxis(centerGridX,gridStepX);
  vector<int> yAxis = BuildSweepAxis(centerGridY,gridStepY);
  vector<PerfResult> rankedResults;

  for(size_t i = 0; i < xAxis.size(); i++) {
    for(size_t j = 0; j < yAxis.size(); j++) {
      rankedResults.push_back(BenchmarkGrid(xAxis[i],yAxis[j]));
    }
  }

  if(rankedResults.empty()) {
    throw PerfError(PERF_EXIT_RUNTIME_ERROR,"No valid sweep candidates generated");
  }

  sort(rankedResults.begin(),rankedResults.end(),[](const PerfResult& left,const PerfResult& right) {
    if(left.kernelNsPerStep != right.kernelNsPerStep)
      return left.kernelNsPerStep < right.kernelNsPerStep;
    if(left.stepsPerSecond != right.stepsPerSecond)
      return left.stepsPerSecond > right.stepsPerSecond;
    if(left.gridSizeX != right.gridSizeX)
      return left.gridSizeX < right.gridSizeX;
    return left.gridSizeY < right.gridSizeY;
  });

  PerfSweepResult result;
  result.binary = kPerfBinaryName;
  result.benchmarkProfile = kBenchmarkProfileName;
  result.gpuId = options.gpuId;
  result.deviceName = rankedResults[0].deviceName;
  result.gpuName = rankedResults[0].gpuName;
  result.computeCapabilityMajor = rankedResults[0].computeCapabilityMajor;
  result.computeCapabilityMinor = rankedResults[0].computeCapabilityMinor;
  result.gitCommit = rankedResults[0].gitCommit;
  result.seed = options.seed;
  result.runMode = options.useSeconds ? "seconds" : "iterations";
  result.requestedIterations = options.useSeconds ? 0 : options.requestedIterations;
  result.requestedSeconds = options.useSeconds ? options.requestedSeconds : 0.0;
  result.warmupIterations = options.warmupIterations;
  result.sweepMode = kSweepMode;
  result.centerGridX = centerGridX;
  result.centerGridY = centerGridY;
  result.gridStepX = gridStepX;
  result.gridStepY = gridStepY;
  result.candidateCount = (int)rankedResults.size();
  result.ranking = kSweepRanking;
  result.winner = rankedResults[0];
  for(size_t i = 0; i < rankedResults.size(); i++) {
    PerfSweepCandidate candidate;
    candidate.rank = (int)i + 1;
    candidate.result = rankedResults[i];
    result.candidates.push_back(candidate);
  }
  result.jsonOutputPath = ResolveSweepJsonOutputPath(result);

  WriteSweepJson(result);
  PrintSweepSummary(result);

  return result;

}

PerfResult PerfHarness::Run() {

  Timer::Init();

  if(options.gridSweepAuto) {
    int centerGridX = 0;
    int centerGridY = 0;
    ResolveGrid(&centerGridX,&centerGridY);
    PerfSweepResult sweepResult = RunSweep(centerGridX,centerGridY);
    return sweepResult.winner;
  }

  int gridSizeX = 0;
  int gridSizeY = 0;
  ResolveGrid(&gridSizeX,&gridSizeY);

  PerfResult result = BenchmarkGrid(gridSizeX,gridSizeY);
  result.jsonOutputPath = ResolveJsonOutputPath(result);

  WriteJson(result);
  PrintSummary(result);

  return result;

}

string PerfHarness::ResolveJsonOutputPath(const PerfResult& result) {

  if(options.jsonOutPath.length() > 0)
    return options.jsonOutPath;

  EnsureDirectoryExists("perf");
  EnsureDirectoryExists(kDefaultResultDir);

  ostringstream fileName;
  fileName << kDefaultResultDir
           << "/"
           << Timer::getTS()
           << "_"
           << result.gitCommit
           << "_gpu"
           << result.gpuId
           << "_cc"
           << result.computeCapabilityMajor
           << result.computeCapabilityMinor
           << "_g"
           << result.gridSizeX
           << "x"
           << result.gridSizeY
           << "_"
           << result.runMode
           << ".json";

  return fileName.str();

}

string PerfHarness::ResolveSweepJsonOutputPath(const PerfSweepResult& result) {

  if(options.jsonOutPath.length() > 0)
    return options.jsonOutPath;

  EnsureDirectoryExists("perf");
  EnsureDirectoryExists(kDefaultResultDir);

  ostringstream fileName;
  fileName << kDefaultResultDir
           << "/"
           << Timer::getTS()
           << "_"
           << result.gitCommit
           << "_gpu"
           << result.gpuId
           << "_cc"
           << result.computeCapabilityMajor
           << result.computeCapabilityMinor
           << "_sweep.json";

  return fileName.str();

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
  cout << "Requested warmup launches: " << result.warmupIterations << endl;
  cout << "Actual warmup launches: " << result.actualWarmupLaunches << endl;
  cout << "Warmup kernel elapsed ms: " << FormatDouble(result.warmupKernelElapsedMs) << endl;
  cout << "Warmup stabilization: " << result.stabilizationMode << " (" << FormatDouble(result.warmupKernelFloorMs) << " ms floor)" << endl;
  cout << "Measured launches: " << result.measuredLaunches << endl;
  cout << "Walkers per launch: " << result.walkersPerLaunch << endl;
  cout << "Steps per launch: " << result.stepsPerLaunch << endl;
  cout << "Total steps: " << result.totalSteps << endl;
  cout << "Setup ms: " << FormatDouble(result.setupMs) << endl;
  cout << "Upload ms: " << FormatDouble(result.uploadMs) << endl;
  cout << "Total kernel elapsed ms: " << FormatDouble(result.totalKernelElapsedMs) << endl;
  cout << "Average kernel elapsed ms: " << FormatDouble(result.avgKernelElapsedMs) << endl;
  cout << "Total wait ms: " << FormatDouble(result.totalWaitMs) << endl;
  cout << "Average wait ms: " << FormatDouble(result.avgWaitMs) << endl;
  cout << "Total copy ms: " << FormatDouble(result.totalCopyMs) << endl;
  cout << "Average copy ms: " << FormatDouble(result.avgCopyMs) << endl;
  cout << "Total post ms: " << FormatDouble(result.totalPostMs) << endl;
  cout << "Average post ms: " << FormatDouble(result.avgPostMs) << endl;
  cout << "kernel_ns_per_step: " << FormatDouble(result.kernelNsPerStep) << endl;
  cout << "steps_per_sec: " << FormatDouble(result.stepsPerSecond) << endl;
  cout << "legacy_mkeys_per_sec: " << FormatDouble(result.legacyMKeysPerSecond) << endl;
  cout << "Git commit: " << result.gitCommit << endl;
  cout << "Timing source: " << kTimingSource << endl;
  cout << "JSON output: " << result.jsonOutputPath << endl;

}

void PerfHarness::PrintSweepSummary(const PerfSweepResult& result) {

#ifdef USE_SYMMETRY
  cout << kPerfBinaryName << " v" << RELEASE << " (with symmetry)" << endl;
#else
  cout << kPerfBinaryName << " v" << RELEASE << endl;
#endif
  cout << "Profile: " << result.benchmarkProfile << endl;
  cout << "GPU id: " << result.gpuId << endl;
  cout << "Device: " << result.deviceName << endl;
  cout << "Sweep mode: " << result.sweepMode << endl;
  cout << "Center grid: " << result.centerGridX << "x" << result.centerGridY << endl;
  cout << "Grid steps: " << result.gridStepX << "x" << result.gridStepY << endl;
  cout << "Candidate count: " << result.candidateCount << endl;
  cout << "Ranking: " << result.ranking << endl;
  cout << "Winner grid: " << result.winner.gridSizeX << "x" << result.winner.gridSizeY << endl;
  cout << "Winner kernel_ns_per_step: " << FormatDouble(result.winner.kernelNsPerStep) << endl;
  cout << "Winner steps_per_sec: " << FormatDouble(result.winner.stepsPerSecond) << endl;
  cout << "Rank  Grid      kernel_ns_per_step  steps_per_sec" << endl;
  for(size_t i = 0; i < result.candidates.size(); i++) {
    const PerfSweepCandidate& candidate = result.candidates[i];
    cout << setw(4) << candidate.rank
         << "  "
         << setw(3) << candidate.result.gridSizeX
         << "x"
         << left << setw(4) << candidate.result.gridSizeY
         << right << "  "
         << setw(18) << FormatDouble(candidate.result.kernelNsPerStep)
         << "  "
         << setw(13) << FormatDouble(candidate.result.stepsPerSecond)
         << endl;
  }
  cout << "Git commit: " << result.gitCommit << endl;
  cout << "Timing source: " << kTimingSource << endl;
  cout << "JSON output: " << result.jsonOutputPath << endl;

}

void PerfHarness::WriteJson(const PerfResult& result) {

  ofstream out(result.jsonOutputPath.c_str(),ios::out | ios::trunc);
  if(!out.is_open()) {
    throw PerfError(PERF_EXIT_JSON_ERROR,"Failed to open JSON output path");
  }

  WritePerfResultJsonObject(out,result,0);
  out << "\n";

  if(!out.good()) {
    throw PerfError(PERF_EXIT_JSON_ERROR,"Failed while writing JSON output");
  }

}

void PerfHarness::WriteSweepJson(const PerfSweepResult& result) {

  ofstream out(result.jsonOutputPath.c_str(),ios::out | ios::trunc);
  if(!out.is_open()) {
    throw PerfError(PERF_EXIT_JSON_ERROR,"Failed to open JSON output path");
  }

  out << "{\n";
  out << "  \"binary\": \"" << JsonEscape(result.binary) << "\",\n";
  out << "  \"benchmark_profile\": \"" << JsonEscape(result.benchmarkProfile) << "\",\n";
  out << "  \"gpu_id\": " << result.gpuId << ",\n";
  out << "  \"device_name\": \"" << JsonEscape(result.deviceName) << "\",\n";
  out << "  \"gpu_name\": \"" << JsonEscape(result.gpuName) << "\",\n";
  out << "  \"compute_capability_major\": " << result.computeCapabilityMajor << ",\n";
  out << "  \"compute_capability_minor\": " << result.computeCapabilityMinor << ",\n";
  out << "  \"git_commit\": \"" << JsonEscape(result.gitCommit) << "\",\n";
  out << "  \"seed\": " << result.seed << ",\n";
  out << "  \"run_mode\": \"" << result.runMode << "\",\n";
  out << "  \"requested_iterations\": " << result.requestedIterations << ",\n";
  out << "  \"requested_seconds\": " << FormatDouble(result.requestedSeconds) << ",\n";
  out << "  \"warmup_iterations\": " << result.warmupIterations << ",\n";
  out << "  \"sweep_mode\": \"" << JsonEscape(result.sweepMode) << "\",\n";
  out << "  \"center_grid_x\": " << result.centerGridX << ",\n";
  out << "  \"center_grid_y\": " << result.centerGridY << ",\n";
  out << "  \"grid_step_x\": " << result.gridStepX << ",\n";
  out << "  \"grid_step_y\": " << result.gridStepY << ",\n";
  out << "  \"candidate_count\": " << result.candidateCount << ",\n";
  out << "  \"ranking\": \"" << JsonEscape(result.ranking) << "\",\n";
  out << "  \"winner\":\n";
  WritePerfResultJsonObject(out,result.winner,1);
  out << ",\n";
  out << "  \"candidates\": [\n";
  for(size_t i = 0; i < result.candidates.size(); i++) {
    WritePerfResultJsonObject(out,result.candidates[i].result,2,true,result.candidates[i].rank);
    if(i + 1 < result.candidates.size())
      out << ",";
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";

  if(!out.good()) {
    throw PerfError(PERF_EXIT_JSON_ERROR,"Failed while writing JSON output");
  }

}

void PerfHarness::WritePerfResultJsonObject(ostream& out,const PerfResult& result,int indentLevel,bool includeRank,int rank) {

  WriteIndent(out,indentLevel);
  out << "{\n";
  if(includeRank) {
    WriteIndent(out,indentLevel + 1);
    out << "\"rank\": " << rank << ",\n";
  }
  WriteIndent(out,indentLevel + 1);
  out << "\"binary\": \"" << JsonEscape(result.binary) << "\",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"benchmark_profile\": \"" << JsonEscape(result.benchmarkProfile) << "\",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"gpu_id\": " << result.gpuId << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"device_name\": \"" << JsonEscape(result.deviceName) << "\",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"gpu_name\": \"" << JsonEscape(result.gpuName) << "\",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"compute_capability_major\": " << result.computeCapabilityMajor << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"compute_capability_minor\": " << result.computeCapabilityMinor << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"grid_x\": " << result.gridSizeX << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"grid_y\": " << result.gridSizeY << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"total_threads\": " << result.totalThreads << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"git_commit\": \"" << JsonEscape(result.gitCommit) << "\",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"seed\": " << result.seed << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"warmup_iterations\": " << result.warmupIterations << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"actual_warmup_launches\": " << result.actualWarmupLaunches << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"warmup_kernel_elapsed_ms\": " << FormatDouble(result.warmupKernelElapsedMs) << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"stabilization_mode\": \"" << JsonEscape(result.stabilizationMode) << "\",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"warmup_kernel_floor_ms\": " << FormatDouble(result.warmupKernelFloorMs) << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"run_mode\": \"" << result.runMode << "\",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"requested_iterations\": " << result.requestedIterations << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"requested_seconds\": " << FormatDouble(result.requestedSeconds) << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"measured_launches\": " << result.measuredLaunches << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"walkers_per_launch\": " << result.walkersPerLaunch << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"steps_per_launch\": " << result.stepsPerLaunch << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"total_steps\": " << result.totalSteps << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"setup_ms\": " << FormatDouble(result.setupMs) << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"upload_ms\": " << FormatDouble(result.uploadMs) << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"total_kernel_elapsed_ms\": " << FormatDouble(result.totalKernelElapsedMs) << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"avg_kernel_elapsed_ms\": " << FormatDouble(result.avgKernelElapsedMs) << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"total_wait_ms\": " << FormatDouble(result.totalWaitMs) << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"avg_wait_ms\": " << FormatDouble(result.avgWaitMs) << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"total_copy_ms\": " << FormatDouble(result.totalCopyMs) << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"avg_copy_ms\": " << FormatDouble(result.avgCopyMs) << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"total_post_ms\": " << FormatDouble(result.totalPostMs) << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"avg_post_ms\": " << FormatDouble(result.avgPostMs) << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"kernel_ns_per_step\": " << FormatDouble(result.kernelNsPerStep) << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"steps_per_sec\": " << FormatDouble(result.stepsPerSecond) << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"legacy_mkeys_per_sec\": " << FormatDouble(result.legacyMKeysPerSecond) << ",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"timing_source\": \"" << kTimingSource << "\",\n";
  WriteIndent(out,indentLevel + 1);
  out << "\"dp_mode\": \"" << kDpMode << "\"\n";
  WriteIndent(out,indentLevel);
  out << "}";

}

void PerfHarness::WriteIndent(ostream& out,int indentLevel) {

  for(int i = 0; i < indentLevel; i++) {
    out << "  ";
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
