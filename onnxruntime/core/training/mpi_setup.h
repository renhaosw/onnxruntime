#pragma once

#ifdef USE_HOROVOD
#include "core/graph/training/horovod_adapters.h"
#include <mpi.h>
#endif

namespace onnxruntime {
namespace training {

struct MPIContext {
  MPIContext(int world_rank = 0, int local_rank = 0, int world_size = 1);
  int world_rank;
  int local_rank;
  int world_size;
};

#ifdef USE_HOROVOD
MPIContext setup_horovod();
void shutdown_horovod();
#endif
}  // namespace training
}  // namespace onnxruntime