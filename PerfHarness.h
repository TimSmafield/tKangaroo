#ifndef PERFHARNESSH
#define PERFHARNESSH

#include "Constants.h"
#include "GPU/GPUEngine.h"
#include "SECPK1/SECP256k1.h"
#include <stdint.h>
#include <stdexcept>
#include <string>

enum PerfExitCode {
  PERF_EXIT_OK = 0,
  PERF_EXIT_INVALID_INPUT = 2,
  PERF_EXIT_RUNTIME_ERROR = 3,
  PERF_EXIT_JSON_ERROR = 4
};

class PerfError : public std::runtime_error {
public:
  PerfError(int exitCode,const std::string& message)
    : std::runtime_error(message), exitCode(exitCode) {
  }

  int exitCode;
};

struct PerfOptions {
  int gpuId;
  int gridSizeX;
  int gridSizeY;
  bool gridSpecified;
  int warmupIterations;
  int requestedIterations;
  double requestedSeconds;
  bool useSeconds;
  std::string jsonOutPath;
  uint32_t seed;
};

struct PerfResult {
  std::string binary;
  std::string benchmarkProfile;
  int gpuId;
  std::string deviceName;
  std::string gpuName;
  int computeCapabilityMajor;
  int computeCapabilityMinor;
  int gridSizeX;
  int gridSizeY;
  uint64_t totalThreads;
  std::string gitCommit;
  uint32_t seed;
  int warmupIterations;
  uint64_t actualWarmupLaunches;
  double warmupKernelElapsedMs;
  std::string runMode;
  int requestedIterations;
  double requestedSeconds;
  uint64_t measuredLaunches;
  uint64_t walkersPerLaunch;
  uint64_t stepsPerLaunch;
  uint64_t totalSteps;
  double setupMs;
  double uploadMs;
  double totalKernelElapsedMs;
  double avgKernelElapsedMs;
  double totalWaitMs;
  double avgWaitMs;
  double totalCopyMs;
  double avgCopyMs;
  double totalPostMs;
  double avgPostMs;
  double kernelNsPerStep;
  double stepsPerSecond;
  double legacyMKeysPerSecond;
  std::string stabilizationMode;
  double warmupKernelFloorMs;
  std::string jsonOutputPath;
};

class PerfHarness {

public:

  explicit PerfHarness(const PerfOptions& options);
  PerfResult Run();
  static void PrintUsage();

private:

  void LoadBenchmarkProfile();
  void InitRange();
  void InitSearchKey();
  void CreateJumpTable();
  void CreateHerd(int nbKangaroo,Int *px,Int *py,Int *distance,int firstType);
  void ResolveGrid(int *gridSizeX,int *gridSizeY);
  std::string ResolveJsonOutputPath(const PerfResult& result);
  void PrintSummary(const PerfResult& result);
  void WriteJson(const PerfResult& result);
  static std::string JsonEscape(const std::string& value);
  static std::string FormatDouble(double value);

  PerfOptions options;
  Secp256K1 secp;
  Point benchmarkPublicKey;
  Point keyToSearch;
  Int rangeStart;
  Int rangeEnd;
  Int rangeWidth;
  Int rangeWidthDiv2;
  Int rangeWidthDiv4;
  int rangePower;
  Int jumpDistance[NB_JUMP];
  Int jumpPointx[NB_JUMP];
  Int jumpPointy[NB_JUMP];

};

#endif // PERFHARNESSH
