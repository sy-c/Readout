// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

#include "Consumer.h"
#include "MemoryBank.h"
#include "MemoryBankManager.h"
#include "MemoryPagesPool.h"
#include "ReadoutStats.h"
#include "ReadoutUtils.h"
#include "CounterStats.h"
#include <atomic>
#include <chrono>
#include <inttypes.h>

#ifdef WITH_FAIRMQ

#include <fairmq/FairMQDevice.h>
#include <fairmq/FairMQMessage.h>
#include <fairmq/FairMQTransportFactory.h>
#include <fairmq/tools/Unique.h>

#include "RAWDataHeader.h"
#include "SubTimeframe.h"
#include <Common/Fifo.h>

// cleanup function
// defined with the callback footprint expected in the 3rd argument of FairMQTransportFactory.CreateMessage()
// when object not null, it should be a (DataBlockContainerReference *), which will be destroyed
void msgcleanupCallback(void* data, void* object)
{
  if ((object != nullptr) && (data != nullptr)) {
    DataBlockContainerReference* ptr = (DataBlockContainerReference*)object;
    // printf("ptr %p: use_count=%d\n",ptr,(int)ptr->use_count());
    delete ptr;
  }
}

// a structure to be stored in DataBlock.userSpace at runtime
// to monitor usage of memory pages passed to FMQ
struct DataBlockFMQStats {
  uint8_t magic;
  std::atomic<int> countRef;
  uint64_t t0;
  uint64_t dataSizeAccounted;
  uint64_t memorySizeAccounted;
};
static_assert(std::is_pod<DataBlockFMQStats>::value, "DataBlockFMQStats is not a POD");
static_assert(sizeof(DataBlockFMQStats) <= DataBlockHeaderUserSpace, "DataBlockFMQStats does not fit in DataBlock.userSpace");

#define timeNowMicrosec (std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())).count


//uint64_t ddsizepayload=0;
//uint64_t ddsizemem=0;


void initDataBlockStats(DataBlock* b, uint64_t v_memorySizeAccounted = 0)
{
  DataBlockFMQStats* s = (DataBlockFMQStats*)&(b->header.userSpace);
  s->magic = 0xAA;
  s->countRef = 0;
  s->dataSizeAccounted = 0;
  s->memorySizeAccounted = v_memorySizeAccounted;
  //printf ("TF %d adding mem sz %d\n", (int)b->header.timeframeId, (int) v_memorySizeAccounted);
}

void incDataBlockStats(DataBlock* b, uint64_t dataSizeAccounted = 0)
{
  //printf("inc %p\n",b);
  DataBlockFMQStats* s = (DataBlockFMQStats*)&(b->header.userSpace);
  if (s->magic != 0xAA)
    return;
  if ((s->countRef++) == 0) {
    s->t0 = timeNowMicrosec();
    gReadoutStats.counters.pagesPendingFairMQ++;
    gReadoutStats.counters.notify++;
    // printf("init %p -> pages locked = %lu\n",b,(unsigned long)gReadoutStats.counters.pagesPendingFairMQ);
    gReadoutStats.counters.ddMemoryPendingBytes += s->memorySizeAccounted;
    //printf("adding %d / %d\n", (int)dataSizeAccounted, (int)s->memorySizeAccounted);
    //ddsizemem+=s->memorySizeAccounted;
  }
  s->dataSizeAccounted += dataSizeAccounted;
  gReadoutStats.counters.ddPayloadPendingBytes += dataSizeAccounted;
  //ddsizepayload += dataSizeAccounted;
}

void decDataBlockStats(DataBlock* b)
{
  DataBlockFMQStats* s = (DataBlockFMQStats*)&(b->header.userSpace);
  //printf("dec %p\n",b);
  if (s->magic != 0xAA)
    return;
  if ((--s->countRef) == 0) {
    // printf("done with %p\n",b);
    gReadoutStats.counters.pagesPendingFairMQ--;
    gReadoutStats.counters.pagesPendingFairMQreleased++;
    uint64_t timeUsed = (timeNowMicrosec() - s->t0);
    gReadoutStats.counters.pagesPendingFairMQtime += timeUsed;
    gReadoutStats.counters.ddPayloadPendingBytes -= s->dataSizeAccounted;
    gReadoutStats.counters.ddMemoryPendingBytes -= s->memorySizeAccounted;
    // if (b->header.timeframeId % 100 >= 0) { printf("%p releasing TF %d: %u / %u bytes run %ld\n", b, (int)b->header.timeframeId, b->header.dataSize, b->header.memorySize, b->header.runNumber); }
    gReadoutStats.counters.notify++;
    //printf("ack %p after %.6lfs (pending: %lu)\n", b, timeUsed/1000000.0, gReadoutStats.counters.pagesPendingFairMQ.load());
    s->magic = 0x00;
  }
}

class ConsumerFMQchannel : public Consumer
{
 private:
  std::unique_ptr<FairMQChannel> sendingChannel;
  std::shared_ptr<FairMQTransportFactory> transportFactory;
  FairMQUnmanagedRegionPtr memoryBuffer = nullptr;
  bool disableSending = 0;
  bool enableRawFormat = false;
  bool enableStfSuperpage = false; // optimized stf transport: minimize STF packets
  bool enableRawFormatDatablock = false;
  int enablePackedCopy = 1; // default mode for repacking of page overlapping HBF. 0 = one page per copy, 1 = change page on TF only

  std::shared_ptr<MemoryBank> memBank; // a dedicated memory bank allocated by FMQ mechanism
  std::shared_ptr<MemoryPagesPool> mp; // a memory pool from which to allocate data pages

  int memoryPoolPageSize;
  int memoryPoolNumberOfPages;

  CounterStats repackSizeStats; // keep track of page size used when repacking
  uint64_t nPagesUsedForRepack = 0; // count pages used for repack
  uint64_t nPagesUsedInput = 0; // count pages received

  // custom log function for memory pool
  void mplog(const std::string &msg) {
    static InfoLogger::AutoMuteToken logMPToken(LogWarningSupport_(3230), 10, 60);
    theLog.log(logMPToken, "Consumer %s : %s", name.c_str(), msg.c_str());
  }
  
  // pool of threads for the processing
  int nwThreads = 0;
  int wThreadFifoSize = 0;

  struct DDMessage {
    std::vector<FairMQMessagePtr> messagesToSend;  // FMQ message parts to be sent
    SubTimeframe* stfHeader;                       // pointer to DD STF header
    uint64_t subTimeframeDataSize;                 // size of data (superpages payload, no STF header)
    uint64_t subTimeframeTotalSize;                // size of data (superpages payload) + STF header = what is sent by FMQ
    uint64_t subTimeframeMemorySize;               // total size in memory (allocated, accounting for unused superpages part)
    uint64_t subTimeframeFMQSize;                  // sum of FMQ message sizes
  };
 
  using wThreadInput = std::shared_ptr<std::vector<DataSetReference>>;
  using wThreadOutput = std::shared_ptr<std::vector<DDMessage>>;

  uint64_t currentTimeframeId = undefinedTimeframeId; // current timeframe being processed
  wThreadInput currentTimeframeBuffer; // all data sets for current TF
  
  struct wThread {
    std::unique_ptr<AliceO2::Common::Fifo<wThreadInput>> input;
    std::unique_ptr<AliceO2::Common::Fifo<wThreadOutput>> output;
    std::unique_ptr<std::thread> thread;
  };
  std::vector<wThread> wThreads;
  int wThreadShutdown = 0;
  const int wThreadSleepTime = 1000; // sleep time in microseconds.   
  std::unique_ptr<std::thread> senderThread; // this one empties the output FIFOs of the wThreads
  int wThreadIxWrite = 0; // push data round-robin in wThreads
  int wThreadIxRead = 0; // read data round-robin in wThreads
  void cleanupThreads() {
    if (nwThreads) {
      wThreadShutdown = 1;
      for (auto& w : wThreads) {
        if (w.thread != nullptr) {
          w.thread->join();
	}
      }
      if (senderThread) {
        senderThread->join();
	senderThread = nullptr;
      }
      // clear the FIFOs only here
      wThreads.clear();
      nwThreads = 0;
    }
  }
  
  int processForDataDistribution(DataSetReference& bc);
 
 public:

  ConsumerFMQchannel(ConfigFile& cfg, std::string cfgEntryPoint) : Consumer(cfg, cfgEntryPoint)
  {

    // configuration parameter: | consumer-FairMQChannel-* | disableSending | int | 0 | If set, no data is output to FMQ channel. Used for performance test to create FMQ shared memory segment without pushing the data. |
    int cfgDisableSending = 0;
    cfg.getOptionalValue<int>(cfgEntryPoint + ".disableSending", cfgDisableSending);
    if (cfgDisableSending) {
      theLog.log(LogInfoDevel_(3002), "FMQ message sending disabled");
      disableSending = true;
    } else {
      gReadoutStats.isFairMQ = 1; // enable FMQ stats
    }

    // configuration parameter: | consumer-FairMQChannel-* | enableRawFormat | int | 0 | If 0, data is pushed 1 STF header + 1 part per HBF. If 1, data is pushed in raw format without STF headers, 1 FMQ message per data page. If 2, format is 1 STF header + 1 part per data page.|
    int cfgEnableRawFormat = 0;
    cfg.getOptionalValue<int>(cfgEntryPoint + ".enableRawFormat", cfgEnableRawFormat);
    if (cfgEnableRawFormat == 1) {
      theLog.log(LogInfoDevel_(3002), "FMQ message output in raw format - mode 1 : 1 message per data page");
      enableRawFormat = true;
    } else if (cfgEnableRawFormat == 2) {
      theLog.log(LogInfoDevel_(3002), "FMQ message output in raw format - mode 2 : 1 message = 1 STF header + 1 part per data page");
      enableStfSuperpage = true;
    } else if (cfgEnableRawFormat == 3) {
      theLog.log(LogInfoDevel_(3002), "FMQ message output in raw format - mode 3 : 1 message = 1 DataBlock header + 1 data page");
      enableRawFormatDatablock = true;
    }

    // configuration parameter: | consumer-FairMQChannel-* | sessionName | string | default | Name of the FMQ session. c.f. FairMQ::FairMQChannel.h |
    std::string cfgSessionName = "default";
    cfg.getOptionalValue<std::string>(cfgEntryPoint + ".sessionName", cfgSessionName);

    // configuration parameter: | consumer-FairMQChannel-* | fmq-transport | string | shmem | Name of the FMQ transport. Typically: zeromq or shmem. c.f. FairMQ::FairMQChannel.h |
    std::string cfgTransportType = "shmem";
    cfg.getOptionalValue<std::string>(cfgEntryPoint + ".fmq-transport", cfgTransportType);

    // configuration parameter: | consumer-FairMQChannel-* | fmq-name | string | readout | Name of the FMQ channel. c.f. FairMQ::FairMQChannel.h |
    std::string cfgChannelName = "readout";
    cfg.getOptionalValue<std::string>(cfgEntryPoint + ".fmq-name", cfgChannelName);

    // configuration parameter: | consumer-FairMQChannel-* | fmq-type | string | pair | Type of the FMQ channel. Typically: pair. c.f. FairMQ::FairMQChannel.h |
    std::string cfgChannelType = "pair";
    cfg.getOptionalValue<std::string>(cfgEntryPoint + ".fmq-type", cfgChannelType);

    // configuration parameter: | consumer-FairMQChannel-* | fmq-address | string | ipc:///tmp/pipe-readout | Address of the FMQ channel. Depends on transportType. c.f. FairMQ::FairMQChannel.h |
    std::string cfgChannelAddress = "ipc:///tmp/pipe-readout";
    cfg.getOptionalValue<std::string>(cfgEntryPoint + ".fmq-address", cfgChannelAddress);

    theLog.log(LogInfoDevel_(3002), "Creating FMQ (session %s) TX channel %s type %s:%s @ %s", cfgSessionName.c_str(), cfgChannelName.c_str(), cfgTransportType.c_str(), cfgChannelType.c_str(), cfgChannelAddress.c_str());

    FairMQProgOptions fmqOptions;
    fmqOptions.SetValue<std::string>("session", cfgSessionName);

    // configuration parameter: | consumer-FairMQChannel-* | fmq-progOptions | string |  | Additional FMQ program options parameters, as a comma-separated list of key=value pairs. |
    std::string cfgFmqOptions = "";
    cfg.getOptionalValue<std::string>(cfgEntryPoint + ".fmq-progOptions", cfgFmqOptions);
    std::map<std::string, std::string> mapOptions;
    if (getKeyValuePairsFromString(cfgFmqOptions, mapOptions)) {
      throw("Can not parse configuration item fmqProgOptions");
    }
    for (auto& it : mapOptions) {
      fmqOptions.SetValue<std::string>(it.first, it.second);
      theLog.log(LogInfoDevel_(3002), "Setting FMQ option %s = %s", it.first.c_str(), it.second.c_str());
    }

    transportFactory = FairMQTransportFactory::CreateTransportFactory(cfgTransportType, fair::mq::tools::Uuid(), &fmqOptions);
    sendingChannel = std::make_unique<FairMQChannel>(cfgChannelName, cfgChannelType, transportFactory);

    // configuration parameter: | consumer-FairMQChannel-* | memoryBankName | string |  | Name of the memory bank to crete (if any) and use. This consumer has the special property of being able to provide memory banks to readout, as the ones defined in bank-*. It creates a memory region optimized for selected transport and to be used for readout device DMA. |
    std::string memoryBankName = ""; // name of memory bank to create (if any) and use.
    cfg.getOptionalValue<std::string>(cfgEntryPoint + ".memoryBankName", memoryBankName);

    // configuration parameter: | consumer-FairMQChannel-* | unmanagedMemorySize | bytes |  | Size of the memory region to be created. c.f. FairMQ::FairMQUnmanagedRegion.h. If not set, no special FMQ memory region is created. |
    std::string cfgUnmanagedMemorySize = "";
    cfg.getOptionalValue<std::string>(cfgEntryPoint + ".unmanagedMemorySize", cfgUnmanagedMemorySize);
    long long mMemorySize = ReadoutUtils::getNumberOfBytesFromString(cfgUnmanagedMemorySize.c_str());
    if (mMemorySize > 0) {
    
      // check system resources first, as region creation does not check available memory, so bad crash could occur later      
      theLog.log(LogInfoDevel_(3002), "Configuring memory buffer %lld MB", (long long)(mMemorySize/1048576LL));

      // configuration parameter: | consumer-FairMQChannel-* | checkResources | string | | Check beforehand if unmanaged region would fit in given list of resources. Comma-separated list of items to be checked: eg /dev/shm, MemFree, MemAvailable. (any filesystem path, and any /proc/meminfo entry).|
      std::string cfgCheckResources;
      cfg.getOptionalValue<std::string>(cfgEntryPoint + ".checkResources", cfgCheckResources);
      bool isResourceError = 0;
      std::vector<std::string> resources;
      
      if (getListFromString(cfgCheckResources, resources)) {
        throw("Can not parse configuration item checkResources");
      }
      
      for(auto r : resources) {
        unsigned long long freeBytes;

	int getStatsErr = 0;
        if (r.find_first_of("/")!=std::string::npos) {
	  // this is a path
	  getStatsErr = getStatsFilesystem(freeBytes, r);
	} else {
	  // look in /proc/meminfo
	  getStatsErr = getStatsMemory(freeBytes, r);
	  r = "/proc/meminfo " + r;
	}
	
	if (getStatsErr) {
            theLog.log(LogWarningSupport_(3230), "Can not get stats for %s", r.c_str());
	} else {
          theLog.log(LogInfoSupport_(3230), "Stats for %s : %lld MB available", r.c_str(), (long long)(freeBytes/1048576LL));
          if ((long long)freeBytes < mMemorySize) {
            theLog.log(LogErrorSupport_(3230), "Not enough space on %s", r.c_str());
            isResourceError = 1;
	  }
	}
      }

      if (isResourceError) {
        throw "ConsumerFMQ: can not allocate shared memory region, system resources check failed";
      }      
            
      theLog.log(LogInfoDevel_(3008), "Creating FMQ unmanaged memory region");
      memoryBuffer = sendingChannel->Transport()->CreateUnmanagedRegion(mMemorySize, [](void* /*data*/, size_t /*size*/, void* hint) { // cleanup callback
        if (hint != nullptr) {
          DataBlockContainerReference* blockRef = (DataBlockContainerReference*)hint;
          //printf("ack hint=%p page %p\n",hint,(*blockRef)->getData());
	  //printf("ptr %p: use_count=%d\n",blockRef, (int)blockRef->use_count());
          decDataBlockStats((*blockRef)->getData());
          delete blockRef;
        }
      },fair::mq::RegionConfig{false,false});  // lock / zero - done later

      theLog.log(LogInfoDevel_(3008), "Got FMQ unmanaged memory buffer size %lu @ %p", memoryBuffer->GetSize(), memoryBuffer->GetData());
    }

    // complete channel bind/validate before proceeding with memory bank
    if (!sendingChannel->Bind(cfgChannelAddress)) {
      throw "ConsumerFMQ: channel bind failed";
    }

    if (!sendingChannel->Validate()) {
      throw "ConsumerFMQ: channel validation failed";
    }

    // create of a readout memory bank if unmanaged region defined
    if (memoryBuffer != nullptr) {
      memBank = std::make_shared<MemoryBank>(memoryBuffer->GetData(), memoryBuffer->GetSize(), nullptr, "FMQ unmanaged memory buffer from " + cfgEntryPoint);
      if (memoryBankName.length() == 0) {
        memoryBankName = cfgEntryPoint; // if no bank name defined, create one with the name of the consumer
      }
      theMemoryBankManager.addBank(memBank, memoryBankName);
      theLog.log(LogInfoDevel_(3008), "Bank %s added", memoryBankName.c_str());
    }

    // allocate a pool of pages for headers and data frame copies
    // configuration parameter: | consumer-FairMQChannel-* | memoryPoolPageSize | bytes | 128k | c.f. same parameter in bank-*. |
    // configuration parameter: | consumer-FairMQChannel-* | memoryPoolNumberOfPages | int | 100 | c.f. same parameter in bank-*. |
    memoryPoolPageSize = 0;
    memoryPoolNumberOfPages = 100;
    std::string cfgMemoryPoolPageSize = "128k";
    cfg.getOptionalValue<std::string>(cfgEntryPoint + ".memoryPoolPageSize", cfgMemoryPoolPageSize);
    memoryPoolPageSize = (int)ReadoutUtils::getNumberOfBytesFromString(cfgMemoryPoolPageSize.c_str());
    cfg.getOptionalValue<int>(cfgEntryPoint + ".memoryPoolNumberOfPages", memoryPoolNumberOfPages);
    mp = theMemoryBankManager.getPagedPool(memoryPoolPageSize, memoryPoolNumberOfPages, memoryBankName);
    if (mp == nullptr) {
      throw "ConsumerFMQ: failed to get memory pool from " + memoryBankName + " for " + std::to_string(memoryPoolNumberOfPages) + " pages x " + std::to_string(memoryPoolPageSize) + " bytes";
    } else {
      mp -> setWarningCallback(std::bind(&ConsumerFMQchannel::mplog, this, std::placeholders::_1));
      if ((mp->getId() >= 0) && (mp->getId() < ReadoutStatsMaxItems)) {
	mp -> setBufferStateVariable(&gReadoutStats.counters.bufferUsage[mp->getId()]);
      }
    }
    theLog.log(LogInfoDevel_(3008), "Using memory pool [%d]: %d pages x %d bytes", mp->getId(), memoryPoolNumberOfPages, memoryPoolPageSize);

    // configuration parameter: | consumer-FairMQChannel-* | enablePackedCopy | int | 1 | If set, the same superpage may be reused (space allowing) for the copy of multiple HBF (instead of a separate one for each copy). This allows a reduced memoryPoolNumberOfPages. |
    cfg.getOptionalValue<int>(cfgEntryPoint + ".enablePackedCopy", enablePackedCopy);
    theLog.log(LogInfoDevel_(3008), "Packed copy enabled = %d", enablePackedCopy);

    // configuration parameter: | consumer-FairMQChannel-* | threads | int | 0 | If set, a pool of thread is created for the data processing. |
    cfg.getOptionalValue<int>(cfgEntryPoint + ".threads", nwThreads);
    if (nwThreads) {
      wThreadFifoSize = 88 / nwThreads; // 1s of buffer
      wThreads.resize(nwThreads);
      wThreadShutdown = 0;
      int isError = 0;
      for (int i=0; i<nwThreads; i++) {
        wThreads[i].input = std::make_unique<AliceO2::Common::Fifo<wThreadInput>>(wThreadFifoSize);
        wThreads[i].output = std::make_unique<AliceO2::Common::Fifo<wThreadOutput>>(wThreadFifoSize);
        if (wThreads[i].input == nullptr) { isError = __LINE__; break; }
        if (wThreads[i].output == nullptr) { isError = __LINE__; break; }
        std::function<void(void)> wThreadLoop = std::bind(&ConsumerFMQchannel::wThreadLoop, this, i);
        wThreads[i].thread = std::make_unique<std::thread>(wThreadLoop);
        if (wThreads[i].thread == nullptr) { isError = __LINE__; break; }
      }
      std::function<void(void)> sThreadLoop = std::bind(&ConsumerFMQchannel::senderThreadLoop, this);
      senderThread = std::make_unique<std::thread>(sThreadLoop);
      if (senderThread == nullptr) { isError = __LINE__; }
      if (isError) {
        cleanupThreads();
        throw isError;
      }
    }

  }

  ~ConsumerFMQchannel()
  {
    // stop threads
    cleanupThreads();
  
    // log memory pool statistics
    if (mp!=nullptr) {
      theLog.log(LogInfoDevel_(3003), "Consumer %s - memory pool statistics ... %s", name.c_str(), mp->getStats().c_str());
      theLog.log(LogInfoDevel_(3003), "Consumer %s - STFB repacking statistics ... number: %" PRIu64 " average page size: %" PRIu64 " max page size: %" PRIu64 " repacked/received = %" PRIu64 "/%" PRIu64 " = %.1f%%", name.c_str(), repackSizeStats.getCount(), (uint64_t)repackSizeStats.getAverage(), repackSizeStats.getMaximum(), nPagesUsedForRepack, nPagesUsedInput, nPagesUsedForRepack * 100.0 / nPagesUsedInput);
    }
    
    // release in reverse order
    mp = nullptr;
    memoryBuffer = nullptr; // warning: data range may still be referenced in memory bank manager
    sendingChannel = nullptr;
    transportFactory = nullptr;
  }

  int pushData(DataBlockContainerReference&)
  {
    // this consumer does not accept a per-block push, it needs a set
    return -1;
  }

  int pushData(DataSetReference& bc)
  {

    nPagesUsedInput += bc->size();

    if (disableSending) {
      totalPushSuccess++;
      return 0;
    }

    // debug mode to send in simple raw format: 1 FMQ message per data page
    if (enableRawFormat) {
      // we just ship one FMQmessage per incoming data page
      for (auto& br : *bc) {
        DataBlock* b = br->getData();
        if (b == nullptr) {
          continue;
        }
        if (b->data == nullptr) {
          continue;
        }
        DataBlockContainerReference* blockRef = new DataBlockContainerReference(br);
        if (blockRef == nullptr) {
          totalPushError++;
          return -1;
        }
        void* hint = (void*)blockRef;
        void* blobPtr = b->data;
        size_t blobSize = (size_t)b->header.dataSize;
        // printf("send %p = %d bytes hint=%p\n",blobPtr,(int)blobSize,hint);
        if (memoryBuffer) {
          auto msg = sendingChannel->NewMessage(memoryBuffer, blobPtr, blobSize, hint);
          sendingChannel->Send(msg);
        } else {
          auto msg = sendingChannel->NewMessage(blobPtr, blobSize, msgcleanupCallback, hint);
          sendingChannel->Send(msg);
        }
        gReadoutStats.counters.bytesFairMQ += blobSize;
	gReadoutStats.counters.notify++;
      }
      totalPushSuccess++;
      return 0;
    }

    bool isRdhFormat = false;
    if (bc->size() > 0) {
      isRdhFormat = bc->at(0)->getData()->header.isRdhFormat;
    }

    // mode to send in simple raw format with Datablock header:
    // 1 FMQ message per data page, 1 part= header, 1 part= payload
    if (enableRawFormatDatablock) {
      for (auto& br : *bc) {
        // create a copy of the reference, in a newly allocated object, so that reference is kept alive until this new object is destroyed in the cleanupCallback
        DataBlockContainerReference* ptr = new DataBlockContainerReference(br);
        if (ptr == nullptr) {
          totalPushError++;
          return -1;
        }
        std::unique_ptr<FairMQMessage> msgHeader(transportFactory->CreateMessage((void*)&(br->getData()->header), (size_t)(br->getData()->header.headerSize), msgcleanupCallback, (void*)nullptr));
        std::unique_ptr<FairMQMessage> msgBody(transportFactory->CreateMessage((void*)(br->getData()->data), (size_t)(br->getData()->header.dataSize), msgcleanupCallback, (void*)(ptr)));

        FairMQParts message;
        message.AddPart(std::move(msgHeader));
        message.AddPart(std::move(msgBody));
        sendingChannel->Send(message);
      }
      totalPushSuccess++;
      return 0;
    }

    // StfSuperpage format
    // we just ship STFheader + one FMQ message part per incoming data page
    if ((enableStfSuperpage) || (!isRdhFormat)) {

      DataBlockContainerReference headerBlock = nullptr;
      headerBlock = mp->getNewDataBlockContainer();
      if (headerBlock == nullptr) {
        totalPushError++;
        return -1;
      }
      auto blockRef = new DataBlockContainerReference(headerBlock);
      if (blockRef == nullptr) {
        totalPushError++;
        return -1;
      }
      SubTimeframe* stfHeader = (SubTimeframe*)headerBlock->getData()->data;
      if (stfHeader == nullptr) {
        totalPushError++;
        return -1;
      }
      SubTimeframe stfhDefaults;
      *stfHeader = stfhDefaults;

      // set flag when this is last STF in timeframe
      if (bc->back()->getData()->header.flagEndOfTimeframe) {
        stfHeader->lastTFMessage = 1;
      }

      for (auto& br : *bc) {
        DataBlock* b = br->getData();
        stfHeader->timeframeId = b->header.timeframeId;
        stfHeader->runNumber = b->header.runNumber;
        stfHeader->systemId = b->header.systemId;
        stfHeader->feeId = b->header.feeId;
        stfHeader->equipmentId = b->header.equipmentId;
        stfHeader->linkId = b->header.linkId;
        stfHeader->timeframeOrbitFirst = b->header.timeframeOrbitFirst;
        stfHeader->timeframeOrbitLast = b->header.timeframeOrbitLast;
        break;
      }

      // printf("Sending TF %lu\n", stfHeader->timeframeId);

      std::vector<FairMQMessagePtr> msgs;
      msgs.reserve(bc->size() + 1);

      // header
      if (memoryBuffer) {
        msgs.emplace_back(sendingChannel->NewMessage(memoryBuffer, (void*)stfHeader, sizeof(SubTimeframe), (void*)(blockRef)));
      } else {
        msgs.emplace_back(sendingChannel->NewMessage((void*)stfHeader, sizeof(SubTimeframe), msgcleanupCallback, (void*)(blockRef)));
      }
      // one msg part per superpage
      for (auto& br : *bc) {
        DataBlock* b = br->getData();
        DataBlockContainerReference* blockRef = new DataBlockContainerReference(br);
        if (blockRef == nullptr) {
          totalPushError++;
          return -1;
        }
        void* hint = (void*)blockRef;
        void* blobPtr = b->data;
        size_t blobSize = (size_t)b->header.dataSize;
        if (memoryBuffer) {
          msgs.emplace_back(sendingChannel->NewMessage(memoryBuffer, blobPtr, blobSize, hint));
        } else {
          msgs.emplace_back(sendingChannel->NewMessage(blobPtr, blobSize, msgcleanupCallback, hint));
        }
      }
      sendingChannel->Send(msgs);

      totalPushSuccess++;
      return 0;
    }

    // send msg with WP5 format:
    // 1 FMQ message for header + 1 FMQ message per HBF (all belonging to same CRU/link id)
    return processForDataDistribution(bc);
  }

 private:
 
 int DDformatMessage(DataSetReference &bc, DDMessage &msg);
 int DDsendMessage(DDMessage &msg);
 
 void wThreadLoop(int thIx) {
   // arg thIx is the thread index  
   std::string thname = name + "-w-" + std::to_string(thIx);
   setThreadName(thname.c_str());
   for(;;) {
     if (wThreadShutdown) {
       break;
     }

     // wait that there is a slot in outgoing FIFO
     if (wThreads[thIx].output->isFull()) {
       usleep(wThreadSleepTime);
       continue;
     }
     
     // get a TF from FIFO
     wThreadInput tf;
     if (wThreads[thIx].input->pop(tf) != 0) {
       // nothing available yet, retry later
       usleep(wThreadSleepTime);
       continue;
     }
     
     if (tf == nullptr) {
       continue;
     }
     if (tf.get() == nullptr) {
       continue;
     }
     if (tf->size() == 0) {
       continue;
     }
     
     bool isError = 0;
     //printf("thread %d got TF %d parts\n", thIx, (int)tf->size());
	
     wThreadOutput msglist;
     msglist = std::make_shared<std::vector<DDMessage>>();
     if (msglist == nullptr) {
       isError = 1;
     } else {
       msglist->reserve(tf->size());  
       // process each dataset in TF
       for (auto &bc : *tf){
	 msglist->emplace_back();
	 if (DDformatMessage(bc, msglist->back())!=0) {
           isError = 1;
           break;
	 }
       }
       if (!isError) {
	 if (wThreads[thIx].output->push(std::move(msglist))) {
           isError = 1;
	 }
       }
     }
     if (isError) {
       totalPushError++;
     }
   }
   return;
 }

 void senderThreadLoop(void) {
   std::string thname = name + "-s";
   setThreadName(thname.c_str());
   
   int thIx = 0;  // index of next thread to read from
   for(;;) {
     if (wThreadShutdown) {
       break;
     }
     
     // get a TF from FIFO
     wThreadOutput msglist;
     if (wThreads[thIx].output->pop(msglist) != 0) {
       // nothing available yet, retry later
       usleep(wThreadSleepTime);
       continue;
     }
     thIx++; // next TF will be read from next thread
     if (thIx == nwThreads) {
       thIx = 0;
     }

     //printf("sender: got TF\n");

     bool isError = 0;
     for (auto &msg: *msglist) {
       //printf ("sending thread TF %d\n", (int) msg.stfHeader->timeframeId);
       if (DDsendMessage(msg)) {
         // sending failed
	 isError = 1;
      }
     }
     if (isError) {
       totalPushError++;
     }

   }
   return;
 }


};

int ConsumerFMQchannel::DDformatMessage(DataSetReference &bc, DDMessage &ddm) {

  DataBlockContainerReference copyBlockBuffer = nullptr; // current buffer used for packed copy
  uint64_t lastTimeframeId = undefinedTimeframeId; // keep track of latest TF id received

  // allocate space for header
  if (memoryPoolPageSize < (int)sizeof(SubTimeframe)) {
    totalPushError++;
    return -1;
  }  
  DataBlockContainerReference headerBlock = nullptr;
  try {
    headerBlock = mp->getNewDataBlockContainer();
  } catch (...) {
  }
  if (headerBlock == nullptr) {
    totalPushError++;
    return -1;
  }
  // allocate a container
  auto blockRef = new DataBlockContainerReference(headerBlock);
  if (blockRef == nullptr) {
    totalPushError++;
    return -1;
  }
  SubTimeframe* stfHeader = (SubTimeframe*)headerBlock->getData()->data;
  if (stfHeader == nullptr) {
    totalPushError++;
    return -1;
  }
  ddm.stfHeader = stfHeader;
  SubTimeframe stfhDefaults;
  *stfHeader = stfhDefaults;
  ddm.subTimeframeMemorySize = headerBlock->getDataBufferSize();
  //printf("%d size 1 TF %d block %p mem size %d %d\n", __LINE__, (int)headerBlock->getData()->header.timeframeId, headerBlock->getData(), (int)headerBlock->getDataBufferSize(), headerBlock->getData()->header.memorySize);
  ddm.subTimeframeDataSize = 0;
  ddm.subTimeframeTotalSize = sizeof(SubTimeframe);
    
  // we iterate a first time to count number of HB
  unsigned int lastHBid = -1;
  int isFirst = true;
  int ix = 0;
  for (auto& br : *bc) {
    ix++;
    DataBlock* b = br->getData();
    ddm.subTimeframeMemorySize += br->getDataBufferSize();
    //printf("%d size 1 TF %d block %p mem size %d %d\n", __LINE__, (int)b->header.timeframeId, b, (int)br->getDataBufferSize(), b->header.memorySize);
    ddm.subTimeframeDataSize += b->header.dataSize;

    // set flag when this is last STF in timeframe
    if (b->header.flagEndOfTimeframe) {
      stfHeader->lastTFMessage = 1;
      //printf("end of TF %d eq %d link %d\n", (int) b->header.timeframeId, (int) b->header.equipmentId, (int)b->header.linkId);
      copyBlockBuffer = nullptr;
    }

    // detect changes of TF id
    if (b->header.timeframeId != lastTimeframeId) {
      lastTimeframeId = b->header.timeframeId;
      copyBlockBuffer = nullptr;
      //printf("TF %d start\n", (int) lastTimeframeId);
    }

    if (isFirst) {
      // fill STF header
      stfHeader->timeframeId = b->header.timeframeId;
      stfHeader->runNumber = b->header.runNumber;
      stfHeader->systemId = b->header.systemId;
      stfHeader->feeId = b->header.feeId;
      stfHeader->equipmentId = b->header.equipmentId;
      stfHeader->linkId = b->header.linkId;
      stfHeader->timeframeOrbitFirst = b->header.timeframeOrbitFirst;
      stfHeader->timeframeOrbitLast = b->header.timeframeOrbitLast;
      stfHeader->isRdhFormat = b->header.isRdhFormat;
      isFirst = false;
    } else {
      if (stfHeader->timeframeId != b->header.timeframeId) {
        theLog.log(LogWarningSupport_(3004), "mismatch tfId");
      }
      if (stfHeader->linkId != b->header.linkId) {
        theLog.log(LogWarningSupport_(3004), "mismatch linkId");
      }
    }
    // printf("block %d tf %d link %d\n",ix,b->header.timeframeId,b->header.linkId);

    for (int offset = 0; offset + sizeof(o2::Header::RAWDataHeader) <= b->header.dataSize;) {
      // printf("checking %p : %d\n",b,offset);
      o2::Header::RAWDataHeader* rdh = (o2::Header::RAWDataHeader*)&b->data[offset];
      if (rdh->heartbeatOrbit != lastHBid) {
        lastHBid = rdh->heartbeatOrbit;
        // printf("offset %d - HBid=%d\n",offset,lastHBid);
      }
      if (stfHeader->linkId != rdh->linkId) {
        static InfoLogger::AutoMuteToken token(LogWarningSupport_(3004));
        theLog.log(token, "TF%d equipment %d link Id mismatch %d != %d @ page offset %d", (int)stfHeader->timeframeId, (int)stfHeader->equipmentId, (int)stfHeader->linkId, (int)rdh->linkId, (int)offset);
        // dumpRDH(rdh);
        // printf("block %p : offset %d = %p\n",b,offset,rdh);
      }
      uint16_t offsetNextPacket = rdh->offsetNextPacket;
      if (offsetNextPacket == 0) {
        break;
      }
      offset += offsetNextPacket;
    }
  }
  headerBlock->getData()->header.timeframeId = stfHeader->timeframeId;
  headerBlock->getData()->header.dataSize=sizeof(SubTimeframe);
  ddm.subTimeframeTotalSize += ddm.subTimeframeDataSize;
  ddm.subTimeframeFMQSize = 0;
  
  // printf("TF %d link %d = %d blocks \n",(int)stfHeader->timeframeId,(int)stfHeader->linkId,(int)bc->size());

  // create a header message
  // std::unique_ptr<FairMQMessage> msgHeader(transportFactory->CreateMessage((void *)stfHeader, sizeof(SubTimeframe), cleanupCallback, (void *)(blockRef)));
  assert(ddm.messagesToSend.empty());
  if (memoryBuffer) {
    // printf("send H %p\n", blockRef);
    initDataBlockStats((*blockRef)->getData(), headerBlock->getDataBufferSize());
    incDataBlockStats((*blockRef)->getData(), sizeof(SubTimeframe));
    ddm.messagesToSend.emplace_back(sendingChannel->NewMessage(memoryBuffer, (void*)stfHeader, sizeof(SubTimeframe), (void*)(blockRef)));
  } else {
    ddm.messagesToSend.emplace_back(sendingChannel->NewMessage((void*)stfHeader, sizeof(SubTimeframe), msgcleanupCallback, (void*)(blockRef)));
  }
  ddm.subTimeframeFMQSize += sizeof(SubTimeframe);
  
  // printf("sent header %d bytes\n",(int)sizeof(SubTimeframe));

  // cut: one message per HBf
  lastHBid = -1;

  // this is for data not sent yet (from one loop to the next)
  struct pendingFrame {
    DataBlockContainerReference* blockRef;
    unsigned int HBstart;
    unsigned int HBlength;
    unsigned int HBid;
  };
  std::vector<pendingFrame> pendingFrames;

  auto pendingFramesAppend = [&](unsigned int ix, unsigned int l, unsigned int id, DataBlockContainerReference br) {
    pendingFrame pf;
    pf.HBstart = ix;
    pf.HBlength = l;
    pf.HBid = id;
    // create a copy of the reference, in a newly allocated object, so that reference is kept alive until this new object is destroyed in the cleanupCallback
    pf.blockRef = new DataBlockContainerReference(br);
    pendingFrames.push_back(pf);
  };

  auto pendingFramesCollect = [&]() {
    int nFrames = pendingFrames.size();

    if (nFrames == 0) {
      // printf("no pending frames\n");
      return;
    }

    if (nFrames == 1) {
      // single block, no need to repack
      auto br = *(pendingFrames[0].blockRef);
      DataBlock* b = br->getData();
      int ix = pendingFrames[0].HBstart;
      int l = pendingFrames[0].HBlength;
      // std::unique_ptr<FairMQMessage> msgBody(transportFactory->CreateMessage((void *)(&(b->data[ix])),(size_t)(l), cleanupCallback, (void *)(pendingFrames[0].blockRef)));
      void* hint = (void*)pendingFrames[0].blockRef;
      // printf("block %p ix = %d : %d hint=%p\n",(void *)(&(b->data[ix])),ix,l,hint);
      // std::cout << typeid(pendingFrames[0].blockRef).name() << std::endl;

      // create and queue a fmq message
      if (memoryBuffer) {
        // printf("send D %p\n", hint);
        incDataBlockStats((*(pendingFrames[0].blockRef))->getData(), l);
//        printf("mem1 sz = %d\n",(int)(*(pendingFrames[0].blockRef))->getData()->header.memorySize);
        ddm.messagesToSend.emplace_back(sendingChannel->NewMessage(memoryBuffer, (void*)(&(b->data[ix])), (size_t)(l), hint));
      } else {
        ddm.messagesToSend.emplace_back(sendingChannel->NewMessage((void*)(&(b->data[ix])), (size_t)(l), msgcleanupCallback, hint));
      }
      ddm.subTimeframeFMQSize += l;
      // printf("sent single HB %d = %d bytes\n",pendingFrames[0].HBid,l);
      // printf("left to FMQ: %p\n",pendingFrames[0].blockRef);

    } else {
      // todo : account number of repack-copies in this situation
      gReadoutStats.counters.ddHBFRepacked++;

      // multiple blocks, need to repack
      int totalSize = 0;
      for (auto& f : pendingFrames) {
        totalSize += f.HBlength;
      }

      // keep stats on repack page size
      repackSizeStats.set(totalSize);

      // allocate
      // todo: same code as for header -> create func/lambda
      // todo: send empty message if no page left in buffer
      if (memoryPoolPageSize < totalSize) {
	static InfoLogger::AutoMuteToken token(LogWarningSupport_(3230));
        theLog.log(token, "page size too small %d < %d", memoryPoolPageSize, totalSize);
        throw __LINE__;
      }
      DataBlockContainerReference copyBlock = nullptr;
      int isNewBlock = 0;
      int copyBlockMemSize = 0;
      try {
	if (enablePackedCopy) {
	  for (int i = 0; i<=2; i++) {
            // allocate new buffer for copies if needed
	    if (copyBlockBuffer == nullptr) {
	      copyBlockBuffer = mp->getNewDataBlockContainer();
	      isNewBlock = 1;
              if (copyBlockBuffer != nullptr) {
	        copyBlockMemSize = copyBlockBuffer->getDataBufferSize();
	      }
	      nPagesUsedForRepack++;
	      continue;
	    }
	    // try to allocate sub-block
	    copyBlock = DataBlockContainer::getChildBlock(copyBlockBuffer, totalSize);
	    if (copyBlock == nullptr) {
	      copyBlockBuffer = nullptr;
	      continue;
	    }
	    break;
	  }
	} else {
          copyBlock = mp->getNewDataBlockContainer();
	  isNewBlock = 1;
	  if (copyBlock != nullptr) {
	    copyBlockMemSize = copyBlock->getDataBufferSize();
	  }
	  nPagesUsedForRepack++;
	}
      } catch (...) {
        copyBlock = nullptr;
      }
      if (copyBlock == nullptr) {
	static InfoLogger::AutoMuteToken token(LogWarningSupport_(3230));
        theLog.log(token, "no page left");
        throw __LINE__;
      }
      if (isNewBlock) {
        ddm.subTimeframeMemorySize += copyBlockMemSize;
	//printf("%d size 1 TF %d block %p mem size %d %d\n", __LINE__, (int)copyBlock->getData()->header.timeframeId, copyBlock->getData(), (int)copyBlockBuffer->getDataBufferSize(), copyBlock->getData()->header.memorySize);
      }
      auto blockRef = new DataBlockContainerReference(copyBlock);
      char* newBlock = (char*)copyBlock->getData()->data;

      int newIx = 0;
      for (auto& f : pendingFrames) {
        auto br = *(f.blockRef);
        DataBlock* b = br->getData();
        int ix = f.HBstart;
        int l = f.HBlength;
        // printf("block %p @ %d : %d\n",b,ix,l);
        memcpy(&newBlock[newIx], &(b->data[ix]), l);
        gReadoutStats.counters.ddBytesCopied += l;
        // printf("release %p for %p\n",f.blockRef,br);
        delete f.blockRef;
        f.blockRef = nullptr;
        newIx += l;
      }

      // std::unique_ptr<FairMQMessage> msgBody(transportFactory->CreateMessage((void *)newBlock, totalSize, cleanupCallbackForMalloc, (void *)(newBlock))); sendingChannel->Send(msgBody);

      // create and queue a fmq message
      if (memoryBuffer) {
        // printf("send D2 %p\n", blockRef);
        initDataBlockStats((*blockRef)->getData(), copyBlockMemSize);
        incDataBlockStats((*blockRef)->getData(), totalSize);
        ddm.messagesToSend.emplace_back(sendingChannel->NewMessage(memoryBuffer, (void*)newBlock, totalSize, (void*)(blockRef)));
      } else {
        ddm.messagesToSend.emplace_back(sendingChannel->NewMessage((void*)newBlock, totalSize, msgcleanupCallback, (void*)(blockRef)));
      }
      ddm.subTimeframeFMQSize += totalSize;

      // printf("sent reallocated HB %d (originally %d blocks) = %d bytes\n",pendingFrames[0].HBid,nFrames,totalSize);
    }
    pendingFrames.clear();
  };

  try {
    for (auto& br : *bc) {
      DataBlock* b = br->getData();
      initDataBlockStats(b, br->getDataBufferSize());

      unsigned int HBstart = 0;
      for (int offset = 0; offset + sizeof(o2::Header::RAWDataHeader) <= b->header.dataSize;) {
        o2::Header::RAWDataHeader* rdh = (o2::Header::RAWDataHeader*)&b->data[offset];
        // printf("CRU block %p = HB %d link %d @ %d\n",b,(int)rdh->heartbeatOrbit,(int)rdh->linkId,offset);
        if (rdh->heartbeatOrbit != lastHBid) {
          // printf("new HBf detected\n");
          int HBlength = offset - HBstart;

          if (HBlength) {
            // add previous block to pending frames
            pendingFramesAppend(HBstart, HBlength, lastHBid, br);
          }
          // send pending frames, if any
          pendingFramesCollect();

          // update new HB frame
          HBstart = offset;
          lastHBid = rdh->heartbeatOrbit;
        }
        uint16_t offsetNextPacket = rdh->offsetNextPacket;
        if (offsetNextPacket == 0) {
          break;
        }
        offset += offsetNextPacket;
      }

      // keep last piece for later, HBframe may continue in next block(s)
      if (HBstart < b->header.dataSize) {
        pendingFramesAppend(HBstart, b->header.dataSize - HBstart, lastHBid, br);
      }
    }

    // purge pendingFrames
    pendingFramesCollect();

  } catch (int err) {
    // cleanup pending frames
    for (auto& f : pendingFrames) {
      if (f.blockRef != nullptr) {
        delete f.blockRef;
        f.blockRef = nullptr;
      }
    }
    pendingFrames.clear();
    static InfoLogger::AutoMuteToken token(LogErrorSupport_(3233));
    theLog.log(token, "ConsumerFMQ : error %d", err);
    // cleanup buffer, and start fresh (in particular: avoid sending empty message parts)
    ddm.messagesToSend.clear();
    totalPushError++;
    return -1;
  }

//  printf("%d TF %d size 1: %lu - %lu size 2: %lu - %lu\n", __LINE__, stfHeader->timeframeId, ddm.subTimeframeTotalSize, ddm.subTimeframeMemorySize, ddsizepayload, ddsizemem);
//  printf("%d TF %d sizes: data %d total %d fmq %d memory %d\n", __LINE__, stfHeader->timeframeId, (int) ddm.subTimeframeDataSize, (int) ddm.subTimeframeTotalSize, (int) ddm.subTimeframeFMQSize, (int) ddm.subTimeframeMemorySize);
//  ddsizepayload=0;
//  ddsizemem=0;
  return 0;
}

int ConsumerFMQchannel::DDsendMessage(DDMessage &ddm) {
  // send the messages
  if (sendingChannel->Send(ddm.messagesToSend) >= 0) {
    gReadoutStats.counters.bytesFairMQ += ddm.subTimeframeTotalSize;
    gReadoutStats.counters.timeframeIdFairMQ = ddm.stfHeader->timeframeId;
    gReadoutStats.counters.notify++;
  } else {
    theLog.log(LogErrorSupport_(3233), "Sending failed");
    return -1;
  }

  //gReadoutStats.counters.ddPayloadPendingBytes += ddm.subTimeframeDataSize;
  //gReadoutStats.counters.ddMemoryPendingBytes += ddm.subTimeframeMemorySize;

  totalPushSuccess++;
  return 0;
}


int ConsumerFMQchannel::processForDataDistribution(DataSetReference& bc) {

  // here we receive one DataSetReference per TF per CRU per link
  // TF data spans over several calls to this function

  // single-threaded, old style
  // process block now
  if (nwThreads == 0) {
    bool isError = 0;
    DDMessage msg;
    if (DDformatMessage(bc, msg)) {
      isError = 1;
    } else {
      if (DDsendMessage(msg)) {
        isError = 1;
      }
    }
    if (isError) {
      totalPushError++;
      return -1;
    }
    return 0;
  }


  // buffer all data of given TF together so that a single thread will process it
  // in order to keep good TF ordering on output

  auto pushCurrentTimeframe = [&]() {  
    if (currentTimeframeBuffer == nullptr) {
      return 0;
    }
    //printf( "push %d - %d datasets\n", (int)currentTimeframeId, (int) currentTimeframeBuffer->size());
    
    if (wThreads[wThreadIxWrite].input->push(currentTimeframeBuffer)) {
      static InfoLogger::AutoMuteToken token(LogWarningSupport_(3004));
      theLog.log(token, "%s - dropping TF %d, data distribution formatting thread pipeline full", name.c_str(), (int)currentTimeframeId);
      totalPushError++;
      return -1;
    }
    currentTimeframeBuffer = nullptr;
    
    // round-robin through available threads, 1 TF each
    wThreadIxWrite++;
    if (wThreadIxWrite == nwThreads) {
      wThreadIxWrite = 0;
    }
    return 0;
  };
 
  if (bc.get() == nullptr) {
    return 0;
  }
  if (bc->size()==0) {
    return 0;
  }  

  DataBlock* b1 = bc->front()->getData(); // first datablock
  DataBlock* b2 = bc->back()->getData(); // last datablock

  if (b1->header.timeframeId != b2->header.timeframeId) {
    // this is bad, all blocks of a dataset should belong to the same TF
    static InfoLogger::AutoMuteToken token(LogWarningSupport_(3004));
    theLog.log(token, "%s - found dataset with data from TF %d and TF %d", name.c_str(), (int)b1->header.timeframeId, (int)b2->header.timeframeId);
    totalPushError++;
    return -1;
  }
  
  uint64_t newTimeframeId = b1->header.timeframeId;
  if ( newTimeframeId != currentTimeframeId) {
    pushCurrentTimeframe();
    currentTimeframeBuffer = std::make_shared<std::vector<DataSetReference>>();
    if (currentTimeframeId != undefinedTimeframeId) {
      // keep track of out-of-order TF
      if (newTimeframeId != (currentTimeframeId + 1)) {
        static InfoLogger::AutoMuteToken token(LogWarningSupport_(3004));
        theLog.log(token, "%s - TF %d following TF %d: non-continuous ordering", name.c_str(), (int)newTimeframeId, (int)currentTimeframeId);
      }
    }
    currentTimeframeId = newTimeframeId;
  }
  if (currentTimeframeBuffer == nullptr) {
    totalPushError++;
    return -1;
  }
  currentTimeframeBuffer->push_back(bc);
  if (b2->header.flagEndOfTimeframe) {
    pushCurrentTimeframe(); // push immediately, we know this is the end
  }

  //printf("dataset: blocks %d TF %d eq %d link %d EOTF %d\n ", (int)bc->size(), (int) b2->header.timeframeId, (int) b2->header.equipmentId, (int)b2->header.linkId, (int)b2->header.flagEndOfTimeframe);


  return 0;
}


std::unique_ptr<Consumer> getUniqueConsumerFMQchannel(ConfigFile& cfg, std::string cfgEntryPoint) { return std::make_unique<ConsumerFMQchannel>(cfg, cfgEntryPoint); }

#endif

