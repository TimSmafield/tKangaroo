#ifndef PERFHARNESSH
#define PERFHARNESSH

#include "Constants.h"
#include "GPU/GPUEngine.h"
#include "SECPK1/SECP256k1.h"
#include <iosfwd>
#include <stdint.h>
#include <stdexcept>
#include <string>
#include <vector>

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
  bool gridSweepAuto;
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

struct PerfSweepCandidate {
  int rank;
  PerfResult result;
};

struct PerfSweepResult {
  std::string binary;
  std::string benchmarkProfile;
  int gpuId;
  std::string deviceName;
  std::string gpuName;
  int computeCapabilityMajor;
  int computeCapabilityMinor;
  std::string gitCommit;
  uint32_t seed;
  std::string runMode;
  int requestedIterations;
  double requestedSeconds;
  int warmupIterations;
  std::string sweepMode;
  int centerGridX;
  int centerGridY;
  int gridStepX;
  int gridStepY;
  int candidateCount;
  std::string ranking;
  PerfResult winner;
  std::vector<PerfSweepCandidate> candidates;
  std::string jsonOutputPath;
};

class PerfHarness {

public:

  explicit PerfHarness(const PerfOptions& options);
  PerfResult Run();
  static void PrintUsage();

private:

  PerfResult BenchmarkGrid(int gridSizeX,int gridSizeY);
  PerfSweepResult RunSweep(int centerGridX,int centerGridY);
  void LoadBenchmarkProfile();
  void InitRange();
  void InitSearchKey();
  void CreateJumpTable();
  void CreateHerd(int nbKangaroo,Int *px,Int *py,Int *distance,int firstType);
  void ResolveGrid(int *gridSizeX,int *gridSizeY);
  std::vector<int> BuildSweepAxis(int center,int step);
  static int RoundToNearestInt(double value);
  static int RoundToNearestMultiple(double value,int multiple);
  std::string ResolveJsonOutputPath(const PerfResult& result);
  std::string ResolveSweepJsonOutputPath(const PerfSweepResult& result);
  void PrintSummary(const PerfResult& result);
  void PrintSweepSummary(const PerfSweepResult& result);
  void WriteJson(const PerfResult& result);
  void WriteSweepJson(const PerfSweepResult& result);
  static void WritePerfResultJsonObject(std::ostream& out,const PerfResult& result,int indentLevel,bool includeRank = false,int rank = 0);
  static void WriteIndent(std::ostream& out,int indentLevel);
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
