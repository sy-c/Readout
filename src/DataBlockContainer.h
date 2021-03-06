#ifndef DATAFORMAT_DATABLOCKCONTAINER
#define DATAFORMAT_DATABLOCKCONTAINER

#include <Common/MemPool.h>
#include <functional>
#include <memory>
#include <stdint.h>
#include <stdlib.h>

#include "DataBlock.h"

// A container class for data blocks.
// In particular, allows to take care of the block release after use.

class DataBlockContainer
{

 public:
  using ReleaseCallback = std::function<void(void)>;
  // NB: may use std::bind to add extra arguments

  // default constructor
  DataBlockContainer(DataBlock* v_data = nullptr, uint64_t v_dataBufferSize = 0) : data(v_data), dataBufferSize(v_dataBufferSize), releaseCallback(nullptr){};

  // this constructor allows to specify a callback which is invoked when container is destroyed
  DataBlockContainer(ReleaseCallback v_callback = nullptr, DataBlock* v_data = nullptr, uint64_t v_dataBufferSize = 0) : data(v_data), dataBufferSize(v_dataBufferSize), releaseCallback(v_callback){};

  // destructor
  virtual ~DataBlockContainer()
  {
    if (releaseCallback != nullptr) {
      releaseCallback();
    }
  };

  DataBlock* getData()
  {
    return data;
  };

  uint64_t getDataBufferSize()
  {
    return dataBufferSize;
  };

 protected:
  DataBlock* data;                 // The DataBlock in use
  uint64_t dataBufferSize = 0;     // Usable memory size pointed by data. Unspecified if zero.
  ReleaseCallback releaseCallback; // Function called on object destroy, to release dataBlock.
};

#endif
