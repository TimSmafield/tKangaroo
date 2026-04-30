#---------------------------------------------------------------------
# Makefile for BSGS
#
# Author : Jean-Luc PONS

ifdef gpu

SRC = SECPK1/IntGroup.cpp main.cpp SECPK1/Random.cpp \
      Timer.cpp SECPK1/Int.cpp SECPK1/IntMod.cpp \
      SECPK1/Point.cpp SECPK1/SECP256K1.cpp \
      GPU/GPUEngine.o Kangaroo.cpp HashTable.cpp \
      Backup.cpp Thread.cpp Check.cpp Network.cpp Merge.cpp PartMerge.cpp

OBJDIR = obj

OBJET = $(addprefix $(OBJDIR)/, \
      SECPK1/IntGroup.o main.o SECPK1/Random.o \
      Timer.o SECPK1/Int.o SECPK1/IntMod.o \
      SECPK1/Point.o SECPK1/SECP256K1.o \
      GPU/GPUEngine.o Kangaroo.o HashTable.o Thread.o \
      Backup.o Check.o Network.o Merge.o PartMerge.o)

PERF_OBJET = $(addprefix $(OBJDIR)/, \
      PerfMain.o PerfHarness.o SECPK1/IntGroup.o SECPK1/Random.o \
      Timer.o SECPK1/Int.o SECPK1/IntMod.o \
      SECPK1/Point.o SECPK1/SECP256K1.o \
      GPU/GPUEngine.o)

else

SRC = SECPK1/IntGroup.cpp main.cpp SECPK1/Random.cpp \
      Timer.cpp SECPK1/Int.cpp SECPK1/IntMod.cpp \
      SECPK1/Point.cpp SECPK1/SECP256K1.cpp \
      Kangaroo.cpp HashTable.cpp Thread.cpp Check.cpp \
      Backup.cpp Network.cpp Merge.cpp PartMerge.cpp

OBJDIR = obj

OBJET = $(addprefix $(OBJDIR)/, \
      SECPK1/IntGroup.o main.o SECPK1/Random.o \
      Timer.o SECPK1/Int.o SECPK1/IntMod.o \
      SECPK1/Point.o SECPK1/SECP256K1.o \
      Kangaroo.o HashTable.o Thread.o Check.o Backup.o \
      Network.o Merge.o PartMerge.o)

endif

CXX        = g++
CUDA       = /usr/local/cuda-8.0
CXXCUDA    = /usr/bin/g++-4.8
NVCC       = $(CUDA)/bin/nvcc
GIT_COMMIT ?= $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)

profile ?=

ifeq ($(profile),gtx1060)
ccap ?= 61
gpu_grp_size ?= 128
nb_run ?= 64
nb_jump ?= 32
gpu_launch_bounds ?= 256
RELEASE_CXX_OPT ?= -O2
RELEASE_NVCC_OPT ?= -O2
else
RELEASE_CXX_OPT ?= -O2
RELEASE_NVCC_OPT ?= -O2
endif

ifneq ($(gpu_grp_size),)
TUNE_DEFINES += -DGPU_GRP_SIZE=$(gpu_grp_size)
endif
ifneq ($(nb_run),)
TUNE_DEFINES += -DNB_RUN=$(nb_run)
endif
ifneq ($(nb_jump),)
TUNE_DEFINES += -DNB_JUMP=$(nb_jump)
endif
ifneq ($(gpu_launch_bounds),)
TUNE_DEFINES += -DGPU_LAUNCH_BOUNDS_THREADS=$(gpu_launch_bounds)
endif

PERF_CXXFLAGS = $(CXXFLAGS) -DPERF_GIT_COMMIT=\"$(GIT_COMMIT)\"

ifdef gpu

ifdef debug
CXXFLAGS   = -DWITHGPU $(TUNE_DEFINES) -m64  -mssse3 -Wno-unused-result -Wno-write-strings -g -I. -I$(CUDA)/include
else
CXXFLAGS   = -DWITHGPU $(TUNE_DEFINES) -m64 -mssse3 -Wno-unused-result -Wno-write-strings $(RELEASE_CXX_OPT) -I. -I$(CUDA)/include
endif
LFLAGS     = -lpthread -L$(CUDA)/lib64 -lcudart -lcrypto

else

ifdef debug
CXXFLAGS   = $(TUNE_DEFINES) -m64 -mssse3 -Wno-unused-result -Wno-write-strings -g -I. -I$(CUDA)/include
else
CXXFLAGS   =  $(TUNE_DEFINES) -m64 -mssse3 -Wno-unused-result -Wno-write-strings $(RELEASE_CXX_OPT) -I. -I$(CUDA)/include
endif
LFLAGS     = -lpthread -lcrypto

endif

#--------------------------------------------------------------------

ifdef gpu
ifdef debug
$(OBJDIR)/GPU/GPUEngine.o: GPU/GPUEngine.cu
	$(NVCC) $(TUNE_DEFINES) -G -maxrregcount=0 --ptxas-options=-v --compile --compiler-options -fPIC -ccbin $(CXXCUDA) -m64 -g -I$(CUDA)/include -gencode=arch=compute_$(ccap),code=sm_$(ccap) -o $(OBJDIR)/GPU/GPUEngine.o -c GPU/GPUEngine.cu
else
$(OBJDIR)/GPU/GPUEngine.o: GPU/GPUEngine.cu
	$(NVCC) $(TUNE_DEFINES) -maxrregcount=0 --ptxas-options=-v --compile --compiler-options -fPIC -ccbin $(CXXCUDA) -m64 $(RELEASE_NVCC_OPT) -I$(CUDA)/include -gencode=arch=compute_$(ccap),code=sm_$(ccap) -o $(OBJDIR)/GPU/GPUEngine.o -c GPU/GPUEngine.cu
endif
endif

$(OBJDIR)/%.o : %.cpp
	$(CXX) $(CXXFLAGS) -o $@ -c $<

$(OBJDIR)/PerfMain.o: PerfMain.cpp
	$(CXX) $(PERF_CXXFLAGS) -o $@ -c $<

$(OBJDIR)/PerfHarness.o: PerfHarness.cpp
	$(CXX) $(PERF_CXXFLAGS) -o $@ -c $<

all: bsgs

bsgs: $(OBJET)
	@echo Making Kangaroo...
	$(CXX) $(OBJET) $(LFLAGS) -o kangaroo

ifdef gpu
perf: $(PERF_OBJET)
	@echo Making kangaroo-perf...
	$(CXX) $(PERF_OBJET) $(LFLAGS) -o kangaroo-perf
else
perf:
	@echo perf target requires gpu=1
	@false
endif

$(OBJET) $(PERF_OBJET): | $(OBJDIR) $(OBJDIR)/SECPK1 $(OBJDIR)/GPU

$(OBJDIR):
	mkdir -p $(OBJDIR)

$(OBJDIR)/GPU: $(OBJDIR)
	cd $(OBJDIR) && mkdir -p GPU

$(OBJDIR)/SECPK1: $(OBJDIR)
	cd $(OBJDIR) &&	mkdir -p SECPK1

clean:
	@echo Cleaning...
	@rm -f obj/*.o
	@rm -f obj/GPU/*.o
	@rm -f obj/SECPK1/*.o

