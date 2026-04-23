#include "PerfHarness.h"
#include <limits>
#include <stdio.h>
#include <string>
#include <string.h>

using namespace std;

namespace {

int ParseInt(const char *name,const char *value) {

  try {
    return stoi(string(value));
  } catch(...) {
    throw PerfError(PERF_EXIT_INVALID_INPUT,string("Invalid ") + name + " value");
  }

}

double ParseDouble(const char *name,const char *value) {

  try {
    return stod(string(value));
  } catch(...) {
    throw PerfError(PERF_EXIT_INVALID_INPUT,string("Invalid ") + name + " value");
  }

}

uint32_t ParseUInt32(const char *name,const char *value) {

  try {
    unsigned long parsed = stoul(string(value),NULL,0);
    if(parsed > numeric_limits<uint32_t>::max()) {
      throw PerfError(PERF_EXIT_INVALID_INPUT,string("Invalid ") + name + " value");
    }
    return (uint32_t)parsed;
  } catch(...) {
    throw PerfError(PERF_EXIT_INVALID_INPUT,string("Invalid ") + name + " value");
  }

}

void ParseGrid(const char *value,int *gridSizeX,int *gridSizeY) {

  string text(value);
  size_t comma = text.find(',');
  if(comma == string::npos || comma == 0 || comma == text.length() - 1 || text.find(',',comma + 1) != string::npos) {
    throw PerfError(PERF_EXIT_INVALID_INPUT,"Invalid --grid value, expected X,Y");
  }

  *gridSizeX = ParseInt("grid.x",text.substr(0,comma).c_str());
  *gridSizeY = ParseInt("grid.y",text.substr(comma + 1).c_str());
  if(*gridSizeX <= 0 || *gridSizeY <= 0) {
    throw PerfError(PERF_EXIT_INVALID_INPUT,"Invalid --grid value, expected positive X,Y");
  }

}

}

int main(int argc,char *argv[]) {

  try {

    PerfOptions options;
    options.gpuId = 0;
    options.gridSizeX = 0;
    options.gridSizeY = 0;
    options.gridSpecified = false;
    options.warmupIterations = 5;
    options.requestedIterations = 50;
    options.requestedSeconds = 0.0;
    options.useSeconds = false;
    options.seed = 0x600DCAFEU;
    bool hasIterations = false;
    bool hasSeconds = false;

    for(int i = 1; i < argc; i++) {
      if(strcmp(argv[i],"--help") == 0 || strcmp(argv[i],"-h") == 0) {
        PerfHarness::PrintUsage();
        return PERF_EXIT_OK;
      } else if(strcmp(argv[i],"--gpu-id") == 0) {
        if(i >= argc - 1)
          throw PerfError(PERF_EXIT_INVALID_INPUT,"--gpu-id requires a value");
        i++;
        options.gpuId = ParseInt("gpu-id",argv[i]);
      } else if(strcmp(argv[i],"--grid") == 0) {
        if(i >= argc - 1)
          throw PerfError(PERF_EXIT_INVALID_INPUT,"--grid requires a value");
        i++;
        ParseGrid(argv[i],&options.gridSizeX,&options.gridSizeY);
        options.gridSpecified = true;
      } else if(strcmp(argv[i],"--warmup") == 0) {
        if(i >= argc - 1)
          throw PerfError(PERF_EXIT_INVALID_INPUT,"--warmup requires a value");
        i++;
        options.warmupIterations = ParseInt("warmup",argv[i]);
      } else if(strcmp(argv[i],"--iterations") == 0) {
        if(i >= argc - 1)
          throw PerfError(PERF_EXIT_INVALID_INPUT,"--iterations requires a value");
        i++;
        if(hasSeconds)
          throw PerfError(PERF_EXIT_INVALID_INPUT,"--iterations and --seconds cannot be combined");
        options.requestedIterations = ParseInt("iterations",argv[i]);
        hasIterations = true;
      } else if(strcmp(argv[i],"--seconds") == 0) {
        if(i >= argc - 1)
          throw PerfError(PERF_EXIT_INVALID_INPUT,"--seconds requires a value");
        i++;
        if(hasIterations)
          throw PerfError(PERF_EXIT_INVALID_INPUT,"--iterations and --seconds cannot be combined");
        options.requestedSeconds = ParseDouble("seconds",argv[i]);
        options.useSeconds = true;
        hasSeconds = true;
      } else if(strcmp(argv[i],"--json-out") == 0) {
        if(i >= argc - 1)
          throw PerfError(PERF_EXIT_INVALID_INPUT,"--json-out requires a value");
        i++;
        options.jsonOutPath = string(argv[i]);
      } else if(strcmp(argv[i],"--seed") == 0) {
        if(i >= argc - 1)
          throw PerfError(PERF_EXIT_INVALID_INPUT,"--seed requires a value");
        i++;
        options.seed = ParseUInt32("seed",argv[i]);
      } else {
        throw PerfError(PERF_EXIT_INVALID_INPUT,string("Unexpected argument: ") + argv[i]);
      }
    }

    if(options.gpuId < 0)
      throw PerfError(PERF_EXIT_INVALID_INPUT,"--gpu-id must be >= 0");
    if(options.warmupIterations < 0)
      throw PerfError(PERF_EXIT_INVALID_INPUT,"--warmup must be >= 0");
    if(!options.useSeconds && options.requestedIterations <= 0)
      throw PerfError(PERF_EXIT_INVALID_INPUT,"--iterations must be > 0");
    if(options.useSeconds && options.requestedSeconds <= 0.0)
      throw PerfError(PERF_EXIT_INVALID_INPUT,"--seconds must be > 0");

    PerfHarness harness(options);
    harness.Run();
    return PERF_EXIT_OK;

  } catch(const PerfError& e) {
    fprintf(stderr,"%s\n",e.what());
    return e.exitCode;
  } catch(const std::exception& e) {
    fprintf(stderr,"%s\n",e.what());
    return PERF_EXIT_RUNTIME_ERROR;
  } catch(...) {
    fprintf(stderr,"Unexpected fatal error\n");
    return PERF_EXIT_RUNTIME_ERROR;
  }

}
