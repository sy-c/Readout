// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

#include "ReadoutEquipment.h"
#include "ReadoutStats.h"
#include "readoutInfoLogger.h"
#include <inttypes.h>

extern tRunNumber occRunNumber;

ReadoutEquipment::ReadoutEquipment(ConfigFile& cfg, std::string cfgEntryPoint)
{

  // example: browse config keys
  // for (auto cfgKey : ConfigFileBrowser (&cfg,"",cfgEntryPoint)) {
  //   std::string cfgValue=cfg.getValue<std::string>(cfgEntryPoint + "." + cfgKey);
  //   printf("%s.%s = %s\n",cfgEntryPoint.c_str(),cfgKey.c_str(),cfgValue.c_str());
  //}

  // by default, name the equipment as the config node entry point
  // configuration parameter: | equipment-* | name | string| | Name used to identify this equipment (in logs). By default, it takes the name of the configuration section, equipment-xxx |
  cfg.getOptionalValue<std::string>(cfgEntryPoint + ".name", name, cfgEntryPoint);

  // configuration parameter: | equipment-* | id | int| | Optional. Number used to identify equipment (used e.g. in file recording). Range 1-65535.|
  int cfgEquipmentId = undefinedEquipmentId;
  cfg.getOptionalValue<int>(cfgEntryPoint + ".id", cfgEquipmentId);
  id = (uint16_t)cfgEquipmentId; // int to 16-bit value

  // configuration parameter: | readout | rate | double | -1 | Data rate limit, per equipment, in Hertz. -1 for unlimited. |
  cfg.getOptionalValue<double>("readout.rate", readoutRate, -1.0);

  // configuration parameter: | equipment-* | idleSleepTime | int | 200 | Thread idle sleep time, in microseconds. |
  int cfgIdleSleepTime = 200;
  cfg.getOptionalValue<int>(cfgEntryPoint + ".idleSleepTime", cfgIdleSleepTime);

  // size of equipment output FIFO
  // configuration parameter: | equipment-* | outputFifoSize | int | -1 | Size of output fifo (number of pages). If -1, set to the same value as memoryPoolNumberOfPages (this ensures that nothing can block the equipment while there are free pages). |
  int cfgOutputFifoSize = -1;
  cfg.getOptionalValue<int>(cfgEntryPoint + ".outputFifoSize", cfgOutputFifoSize);

  // get memory bank parameters
  // configuration parameter: | equipment-* | memoryBankName | string | | Name of bank to be used. By default, it uses the first available bank declared. |
  cfg.getOptionalValue<std::string>(cfgEntryPoint + ".memoryBankName", memoryBankName);
  std::string cfgMemoryPoolPageSize = "";
  // configuration parameter: | equipment-* | memoryPoolPageSize | bytes | | Size of each memory page to be created. Some space might be kept in each page for internal readout usage. |
  cfg.getOptionalValue<std::string>(cfgEntryPoint + ".memoryPoolPageSize", cfgMemoryPoolPageSize);
  // configuration parameter: | equipment-* | memoryPoolNumberOfPages | int | | Number of pages to be created for this equipment, taken from the chosen memory bank. The bank should have enough free space to accomodate (memoryPoolNumberOfPages + 1) * memoryPoolPageSize bytes. |
  memoryPoolPageSize = (int)ReadoutUtils::getNumberOfBytesFromString(cfgMemoryPoolPageSize.c_str());
  cfg.getOptionalValue<int>(cfgEntryPoint + ".memoryPoolNumberOfPages", memoryPoolNumberOfPages);
  if (cfgOutputFifoSize == -1) {
    cfgOutputFifoSize = memoryPoolNumberOfPages;
  }

  // disable output?
  // configuration parameter: | equipment-* | disableOutput | int | 0 | If non-zero, data generated by this equipment is discarded immediately and is not pushed to output fifo of readout thread. Used for testing. |
  cfg.getOptionalValue<int>(cfgEntryPoint + ".disableOutput", disableOutput);

  // memory alignment
  // configuration parameter: | equipment-* | firstPageOffset | bytes | | Offset of the first page, in bytes from the beginning of the memory pool. If not set (recommended), will start at memoryPoolPageSize (one free page is kept before the first usable page for readout internal use). |
  std::string cfgStringFirstPageOffset = "0";
  cfg.getOptionalValue<std::string>(cfgEntryPoint + ".firstPageOffset", cfgStringFirstPageOffset);
  size_t cfgFirstPageOffset = (size_t)ReadoutUtils::getNumberOfBytesFromString(cfgStringFirstPageOffset.c_str());
  // configuration parameter: | equipment-* | blockAlign | bytes | 2M | Alignment of the beginning of the big memory block from which the pool is created. Pool will start at a multiple of this value. Each page will then begin at a multiple of memoryPoolPageSize from the beginning of big block. |
  std::string cfgStringBlockAlign = "2M";
  cfg.getOptionalValue<std::string>(cfgEntryPoint + ".blockAlign", cfgStringBlockAlign);
  size_t cfgBlockAlign = (size_t)ReadoutUtils::getNumberOfBytesFromString(cfgStringBlockAlign.c_str());

  // output periodic statistics on console
  // configuration parameter: | equipment-* | consoleStatsUpdateTime | double | 0 | If set, number of seconds between printing statistics on console. |
  cfg.getOptionalValue<double>(cfgEntryPoint + ".consoleStatsUpdateTime", cfgConsoleStatsUpdateTime);

  // configuration parameter: | equipment-* | stopOnError | int | 0 | If 1, readout will stop automatically on equipment error. |
  int cfgStopOnError = 0;
  cfg.getOptionalValue<int>(cfgEntryPoint + ".stopOnError", cfgStopOnError);
  if (cfgStopOnError) {
    this->stopOnError = 1;
  }
  // configuration parameter: | equipment-* | debugFirstPages | int | 0 | If set, print debug information for first (given number of) data pages readout. |
  int cfgDebugFirstPages = 0;
  cfg.getOptionalValue<int>(cfgEntryPoint + ".debugFirstPages", cfgDebugFirstPages);
  if (cfgDebugFirstPages >= 0) {
    this->debugFirstPages = cfgDebugFirstPages;
  }

  // log config summary
  theLog.log(LogInfoDevel_(3002), "Equipment %s: from config [%s], max rate=%lf Hz, idleSleepTime=%d us, outputFifoSize=%d", name.c_str(), cfgEntryPoint.c_str(), readoutRate, cfgIdleSleepTime, cfgOutputFifoSize);
  theLog.log(LogInfoDevel_(3008), "Equipment %s: requesting memory pool %d pages x %d bytes from bank '%s', block aligned @ 0x%X, 1st page offset @ 0x%X", name.c_str(), (int)memoryPoolNumberOfPages, (int)memoryPoolPageSize, memoryBankName.c_str(), (int)cfgBlockAlign, (int)cfgFirstPageOffset);
  if (disableOutput) {
    theLog.log(LogWarningDevel_(3002), "Equipment %s: output DISABLED ! Data will be readout and dropped immediately", name.c_str());
  }

  // RDH-related extra configuration parameters
  // configuration parameter: | equipment-* | rdhCheckEnabled | int | 0 | If set, data pages are parsed and RDH headers checked. Errors are reported in logs. |
  cfg.getOptionalValue<int>(cfgEntryPoint + ".rdhCheckEnabled", cfgRdhCheckEnabled);
  // configuration parameter: | equipment-* | rdhDumpEnabled | int | 0 | If set, data pages are parsed and RDH headers summary printed. Setting a negative number will print only the first N RDH.|
  cfg.getOptionalValue<int>(cfgEntryPoint + ".rdhDumpEnabled", cfgRdhDumpEnabled);
  // configuration parameter: | equipment-* | rdhDumpErrorEnabled | int | 1 | If set, a log message is printed for each RDH header error found.|
  cfg.getOptionalValue<int>(cfgEntryPoint + ".rdhDumpErrorEnabled", cfgRdhDumpErrorEnabled);
  // configuration parameter: | equipment-* | rdhDumpWarningEnabled | int | 0 | If set, a log message is printed for each RDH header warning found.|
  cfg.getOptionalValue<int>(cfgEntryPoint + ".rdhDumpWarningEnabled", cfgRdhDumpWarningEnabled);
  // configuration parameter: | equipment-* | rdhUseFirstInPageEnabled | int | 0 | If set, the first RDH in each data page is used to populate readout headers (e.g. linkId).|
  cfg.getOptionalValue<int>(cfgEntryPoint + ".rdhUseFirstInPageEnabled", cfgRdhUseFirstInPageEnabled);
  theLog.log(LogInfoDevel_(3002), "RDH settings: rdhCheckEnabled=%d rdhDumpEnabled=%d rdhDumpErrorEnabled=%d rdhDumpWarningEnabled=%d rdhUseFirstInPageEnabled=%d", cfgRdhCheckEnabled, cfgRdhDumpEnabled, cfgRdhDumpErrorEnabled, cfgRdhDumpWarningEnabled, cfgRdhUseFirstInPageEnabled);

  // configuration parameter: | equipment-* | TFperiod | int | 256 | Duration of a timeframe, in number of LHC orbits. |
  int cfgTFperiod = 256;
  cfg.getOptionalValue<int>(cfgEntryPoint + ".TFperiod", cfgTFperiod);
  timeframePeriodOrbits = cfgTFperiod;

  if (!cfgRdhUseFirstInPageEnabled) {
    usingSoftwareClock = true; // if RDH disabled, use internal clock for TF id
  }
  theLog.log(LogInfoDevel_(3002), "Timeframe length = %d orbits", (int)timeframePeriodOrbits);
  if (usingSoftwareClock) {
    timeframeRate = LHCOrbitRate * 1.0 / timeframePeriodOrbits; // timeframe rate, in Hz
    theLog.log(LogInfoDevel_(3002), "Timeframe IDs generated by software, %.2lf Hz", timeframeRate);
  } else {
    theLog.log(LogInfoDevel_(3002), "Timeframe IDs generated from RDH trigger counters");
  }

  // init stats
  equipmentStats.resize(EquipmentStatsIndexes::maxIndex);
  equipmentStatsLast.resize(EquipmentStatsIndexes::maxIndex);

  // creation of memory pool for data pages
  // todo: also allocate pool of DataBlockContainers? at the same time? reserve space at start of pages?
  if ((memoryPoolPageSize <= 0) || (memoryPoolNumberOfPages <= 0)) {
    theLog.log(LogErrorSupport_(3103), "Equipment %s: wrong memory pool settings", name.c_str());
    throw __LINE__;
  }
  pageSpaceReserved = sizeof(DataBlock); // reserve some data at beginning of each page for header,
                                         // keep beginning of payload aligned as requested in config
  size_t firstPageOffset = 0;            // alignment of 1st page of memory pool
  if (pageSpaceReserved) {
    // auto-align
    firstPageOffset = memoryPoolPageSize - pageSpaceReserved;
  }
  if (cfgFirstPageOffset) {
    firstPageOffset = cfgFirstPageOffset - pageSpaceReserved;
  }
  theLog.log(LogInfoDevel_(3008), "pageSpaceReserved = %d, aligning 1st page @ 0x%X", (int)pageSpaceReserved, (int)firstPageOffset);
  mp = nullptr;
  try {
    mp = theMemoryBankManager.getPagedPool(memoryPoolPageSize, memoryPoolNumberOfPages, memoryBankName, firstPageOffset, cfgBlockAlign);
  } catch (...) {
  }
  if (mp == nullptr) {
    theLog.log(LogErrorSupport_(3230), "Failed to create pool of memory pages");
    throw __LINE__;
  }
  // todo: move page align to MemoryPool class
  assert(pageSpaceReserved == mp->getPageSize() - mp->getDataBlockMaxSize());

  // create output fifo
  dataOut = std::make_shared<AliceO2::Common::Fifo<DataBlockContainerReference>>(cfgOutputFifoSize);
  if (dataOut == nullptr) {
    throw __LINE__;
  }

  // create thread
  readoutThread = std::make_unique<Thread>(ReadoutEquipment::threadCallback, this, name, cfgIdleSleepTime);
  if (readoutThread == nullptr) {
    throw __LINE__;
  }
}

const std::string& ReadoutEquipment::getName() { return name; }

void ReadoutEquipment::start()
{
  // reset counters
  for (int i = 0; i < (int)EquipmentStatsIndexes::maxIndex; i++) {
    equipmentStats[i].reset();
    equipmentStatsLast[i] = 0;
  }
  isError = 0;
  currentBlockId = 0;
  isDataOn = false;

  // reset equipment counters
  ReadoutEquipment::initCounters();
  this->initCounters();

  // reset block rate clock
  if (readoutRate > 0) {
    clk.reset(1000000.0 / readoutRate);
  }
  clk0.reset();

  // reset stats timer
  consoleStatsTimer.reset(cfgConsoleStatsUpdateTime * 1000000);

  readoutThread->start();
}

void ReadoutEquipment::stop()
{

  // just in case this was not done yet
  isDataOn = false;

  double runningTime = clk0.getTime();
  readoutThread->stop();
  // printf("%llu blocks in %.3lf seconds => %.1lf block/s\n",nBlocksOut,clk0.getTimer(),nBlocksOut/clk0.getTime());
  readoutThread->join();

  this->finalCounters();
  ReadoutEquipment::finalCounters();

  for (int i = 0; i < (int)EquipmentStatsIndexes::maxIndex; i++) {
    if (equipmentStats[i].getCount()) {
      theLog.log(LogInfoDevel_(3003), "%s.%s = %llu  (avg=%.2lf  min=%llu  max=%llu  count=%llu)", name.c_str(), EquipmentStatsNames[i], (unsigned long long)equipmentStats[i].get(), equipmentStats[i].getAverage(), (unsigned long long)equipmentStats[i].getMinimum(), (unsigned long long)equipmentStats[i].getMaximum(), (unsigned long long)equipmentStats[i].getCount());
    } else {
      theLog.log(LogInfoDevel_(3003), "%s.%s = %llu", name.c_str(), EquipmentStatsNames[i], (unsigned long long)equipmentStats[i].get());
    }
  }

  theLog.log(LogInfoDevel_(3003), "Average pages pushed per iteration: %.1f", equipmentStats[EquipmentStatsIndexes::nBlocksOut].get() * 1.0 / (equipmentStats[EquipmentStatsIndexes::nLoop].get() - equipmentStats[EquipmentStatsIndexes::nIdle].get()));
  theLog.log(LogInfoDevel_(3003), "Average fifoready occupancy: %.1f", equipmentStats[EquipmentStatsIndexes::fifoOccupancyFreeBlocks].get() * 1.0 / (equipmentStats[EquipmentStatsIndexes::nLoop].get() - equipmentStats[EquipmentStatsIndexes::nIdle].get()));
  theLog.log(LogInfoDevel_(3003), "Average data throughput: %s", ReadoutUtils::NumberOfBytesToString(equipmentStats[EquipmentStatsIndexes::nBytesOut].get() / runningTime, "B/s").c_str());
}

ReadoutEquipment::~ReadoutEquipment()
{
  // check if mempool still referenced
  if (!mp.unique()) {
    theLog.log(LogInfoDevel_(3008), "Equipment %s :  mempool still has %d references\n", name.c_str(), (int)mp.use_count());
  }
}

DataBlockContainerReference ReadoutEquipment::getBlock()
{
  DataBlockContainerReference b = nullptr;
  dataOut->pop(b);
  return b;
}

Thread::CallbackResult ReadoutEquipment::threadCallback(void* arg)
{
  ReadoutEquipment* ptr = static_cast<ReadoutEquipment*>(arg);

  // flag to identify if something was done in this iteration
  bool isActive = false;

  // in software clock mode, set timeframe id based on current timestamp
  if (ptr->usingSoftwareClock) {
    if (ptr->timeframeClock.isTimeout()) {
      ptr->currentTimeframe++;
      ptr->statsNumberOfTimeframes++;
      ptr->timeframeClock.increment();
    }
  }

  for (;;) {
    ptr->equipmentStats[EquipmentStatsIndexes::nLoop].increment();

    // max number of blocks to read in this iteration.
    // this is a finite value to ensure all readout steps are done regularly.
    int maxBlocksToRead = 1024;

    // check throughput
    if (ptr->readoutRate > 0) {
      uint64_t nBlocksOut = ptr->equipmentStats[(int)EquipmentStatsIndexes::nBlocksOut].get(); // number of blocks we have already readout until now
      maxBlocksToRead = ptr->readoutRate * ptr->clk0.getTime() - nBlocksOut;
      if ((!ptr->clk.isTimeout()) && (nBlocksOut != 0) && (maxBlocksToRead <= 0)) {
        // target block rate exceeded, wait a bit
        ptr->equipmentStats[EquipmentStatsIndexes::nThrottle].increment();
        break;
      }
    }

    // check status of output FIFO
    ptr->equipmentStats[EquipmentStatsIndexes::fifoOccupancyOutBlocks].set(ptr->dataOut->getNumberOfUsedSlots());

    // check status of memory pool
    {
      size_t nPagesTotal = 0, nPagesFree = 0, nPagesUsed = 0;
      if (ptr->getMemoryUsage(nPagesFree, nPagesTotal) == 0) {
        nPagesUsed = nPagesTotal - nPagesFree;
      }
      ptr->equipmentStats[EquipmentStatsIndexes::nPagesUsed].set(nPagesUsed);
      ptr->equipmentStats[EquipmentStatsIndexes::nPagesFree].set(nPagesFree);
    }

    // try to get new blocks
    int nPushedOut = 0;
    for (int i = 0; i < maxBlocksToRead; i++) {

      // check output FIFO status so that we are sure we can push next block, if any
      if (ptr->dataOut->isFull()) {
        ptr->equipmentStats[EquipmentStatsIndexes::nOutputFull].increment();
        break;
      }

      // get next block
      DataBlockContainerReference nextBlock = nullptr;
      try {
        nextBlock = ptr->getNextBlock();
      } catch (...) {
        theLog.log(LogWarningDevel_(3230), "getNextBlock() exception");
        break;
      }
      // printf("getNextBlock=%p\n",nextBlock);
      if (nextBlock == nullptr) {
        break;
      }

      // handle RDH-formatted data
      if (ptr->isRdhEquipment) {
        ptr->processRdh(nextBlock);
      }

      // tag data with equipment Id, if set (will overwrite field if was already set by equipment)
      if (ptr->id != undefinedEquipmentId) {
        nextBlock->getData()->header.equipmentId = ptr->id;
      }

      // tag data with block id
      ptr->currentBlockId++; // don't start from 0
      nextBlock->getData()->header.blockId = ptr->currentBlockId;

      // tag data with (dummy) timeframeid, if none set
      if (nextBlock->getData()->header.timeframeId == undefinedTimeframeId) {
        nextBlock->getData()->header.timeframeId = ptr->getCurrentTimeframe();
      }

      // tag data with run number
      nextBlock->getData()->header.runNumber = occRunNumber;

      // update rate-limit clock
      if (ptr->readoutRate > 0) {
        ptr->clk.increment();
      }

      // update stats
      nPushedOut++;
      ptr->equipmentStats[EquipmentStatsIndexes::nBytesOut].increment(nextBlock->getData()->header.dataSize);
      gReadoutStats.counters.bytesReadout += nextBlock->getData()->header.dataSize;
      isActive = true;

      // print block debug info
      if (ptr->debugFirstPages > 0) {
        DataBlockHeader* h = &(nextBlock->getData()->header);
        theLog.log(LogDebugDevel_(3009), "Equipment %s (%d) page %" PRIu64 " link %d tf %" PRIu64 " size %d", ptr->name.c_str(), h->equipmentId, h->blockId, h->linkId, h->timeframeId, h->dataSize);
        ptr->debugFirstPages--;
      }

      if (!ptr->disableOutput) {
        // push new page to output fifo
        ptr->dataOut->push(nextBlock);
      }
    }
    ptr->equipmentStats[EquipmentStatsIndexes::nBlocksOut].increment(nPushedOut);

    // prepare next blocks
    if (ptr->isDataOn) {
      Thread::CallbackResult statusPrepare = ptr->prepareBlocks();
      switch (statusPrepare) {
        case (Thread::CallbackResult::Ok):
          isActive = true;
          break;
        case (Thread::CallbackResult::Idle):
          break;
        default:
          // this is an abnormal situation, return corresponding status
          return statusPrepare;
      }
    }

    // consider inactive if we have not pushed much compared to free space in output fifo
    // todo: instead, have dynamic 'inactive sleep time' as function of actual outgoing page rate to optimize polling interval
    if (nPushedOut < ptr->dataOut->getNumberOfFreeSlots() / 4) {
      // disabled, should not depend on output fifo size
      // isActive=0;
    }

    // todo: add SLICER to aggregate together time-range data
    // todo: get other FIFO status

    // print statistics on console, if configured
    if (ptr->cfgConsoleStatsUpdateTime > 0) {
      if (ptr->consoleStatsTimer.isTimeout()) {
        for (int i = 0; i < (int)EquipmentStatsIndexes::maxIndex; i++) {
          CounterValue vNew = ptr->equipmentStats[i].getCount();
          CounterValue vDiff = vNew - ptr->equipmentStatsLast[i];
          ptr->equipmentStatsLast[i] = vNew;
          theLog.log(LogInfoDevel_(3003), "%s.%s : diff=%llu total=%llu", ptr->name.c_str(), ptr->EquipmentStatsNames[i], (unsigned long long)vDiff, (unsigned long long)vNew);
        }
        ptr->consoleStatsTimer.increment();
      }
    }

    break;
  }

  if (!isActive) {
    ptr->equipmentStats[EquipmentStatsIndexes::nIdle].increment();
    return Thread::CallbackResult::Idle;
  }
  return Thread::CallbackResult::Ok;
}

void ReadoutEquipment::setDataOn() { isDataOn = true; }

void ReadoutEquipment::setDataOff() { isDataOn = false; }

int ReadoutEquipment::getMemoryUsage(size_t& numberOfPagesAvailable, size_t& numberOfPagesInPool)
{
  numberOfPagesAvailable = 0;
  numberOfPagesInPool = 0;
  if (mp == nullptr) {
    return -1;
  }
  numberOfPagesInPool = mp->getTotalNumberOfPages();
  numberOfPagesAvailable = mp->getNumberOfPagesAvailable();
  return 0;
}

void ReadoutEquipment::initCounters()
{

  statsRdhCheckOk = 0;
  statsRdhCheckErr = 0;
  statsRdhCheckStreamErr = 0;

  statsNumberOfTimeframes = 0;

  // reset timeframe clock
  if (usingSoftwareClock) {
    timeframeClock.reset(1000000 / timeframeRate);
  }

  currentTimeframe = undefinedTimeframeId;
  firstTimeframeHbOrbitBegin = undefinedOrbit;
  isDefinedFirstTimeframeHbOrbitBegin = 0;
};

void ReadoutEquipment::finalCounters()
{
  if (cfgRdhCheckEnabled) {
    theLog.log(LogInfoDevel_(3003), "Equipment %s : %llu timeframes, RDH checks %llu ok, %llu errors, %llu stream inconsistencies", name.c_str(), statsNumberOfTimeframes, statsRdhCheckOk, statsRdhCheckErr, statsRdhCheckStreamErr);
  }
};

void ReadoutEquipment::initRdhEquipment() { isRdhEquipment = true; }

uint64_t ReadoutEquipment::getTimeframeFromOrbit(uint32_t hbOrbit)
{
  if (!isDefinedFirstTimeframeHbOrbitBegin) {
    firstTimeframeHbOrbitBegin = hbOrbit;
    isDefinedFirstTimeframeHbOrbitBegin = 1;
  }
  uint64_t tfId = 1 + (hbOrbit - firstTimeframeHbOrbitBegin) / getTimeframePeriodOrbits();
  if (tfId != currentTimeframe) {
    // theLog.log(LogDebugTrace, "TF %lu", tfId);
    statsNumberOfTimeframes++;

    // detect gaps in TF id continuity
    if (tfId != currentTimeframe + 1) {
      if (cfgRdhDumpWarningEnabled) {
        theLog.log(LogWarningSupport_(3004), "Non-contiguous timeframe IDs %llu ... %llu", (unsigned long long)currentTimeframe, (unsigned long long)tfId);
      }
    }
  }
  currentTimeframe = tfId;
  return tfId;
}

void ReadoutEquipment::getTimeframeOrbitRange(uint64_t tfId, uint32_t& hbOrbitMin, uint32_t& hbOrbitMax)
{
  hbOrbitMin = undefinedOrbit;
  hbOrbitMax = undefinedOrbit;
  if (tfId == undefinedTimeframeId)
    return;
  if (!isDefinedFirstTimeframeHbOrbitBegin)
    return;
  hbOrbitMin = firstTimeframeHbOrbitBegin + (tfId - 1) * getTimeframePeriodOrbits();
  hbOrbitMax = hbOrbitMin + getTimeframePeriodOrbits() - 1;
}

uint64_t ReadoutEquipment::getCurrentTimeframe() { return currentTimeframe; }

int ReadoutEquipment::tagDatablockFromRdh(RdhHandle& h, DataBlockHeader& bh)
{

  uint64_t tfId = undefinedTimeframeId;
  uint8_t systemId = undefinedSystemId;
  uint16_t feeId = undefinedFeeId;
  uint16_t equipmentId = undefinedEquipmentId;
  uint8_t linkId = undefinedLinkId;
  uint32_t hbOrbit = undefinedOrbit;
  bool isError = 0;

  // check that it is a correct RDH
  std::string errorDescription;
  if (h.validateRdh(errorDescription) != 0) {
    theLog.log(LogWarningSupport_(3004), "First RDH in page is wrong: %s", errorDescription.c_str());
    isError = 1;
  } else {
    // timeframe ID
    hbOrbit = h.getHbOrbit();
    tfId = getTimeframeFromOrbit(hbOrbit);

    // system ID
    systemId = h.getSystemId();

    // fee ID - may not be valid for whole page
    feeId = h.getFeeId();

    // equipmentId - computed from CRU id + end-point
    equipmentId = (uint16_t)(h.getCruId() * 10 + h.getEndPointId());

    // discard value from CRU if this is the default one
    if (equipmentId == 0) {
      equipmentId = undefinedEquipmentId;
    }

    // linkId
    linkId = h.getLinkId();
  }

  bh.timeframeId = tfId;
  bh.systemId = systemId;
  bh.feeId = feeId;
  bh.equipmentId = equipmentId;
  bh.linkId = linkId;
  getTimeframeOrbitRange(tfId, bh.timeframeOrbitFirst, bh.timeframeOrbitLast);
  return isError;
}

int ReadoutEquipment::processRdh(DataBlockContainerReference& block)
{

  DataBlockHeader& blockHeader = block->getData()->header;
  void* blockData = block->getData()->data;
  if (blockData == nullptr) {
    return -1;
  }

  // retrieve metadata from RDH, if configured to do so
  if ((cfgRdhUseFirstInPageEnabled) || (cfgRdhCheckEnabled)) {
    RdhHandle h(blockData);
    if (tagDatablockFromRdh(h, blockHeader) == 0) {
      blockHeader.isRdhFormat = 1;
    }
  }

  // Dump RDH if configured to do so
  if (cfgRdhDumpEnabled) {
    RdhBlockHandle b(blockData, blockHeader.dataSize);
    if (b.printSummary()) {
      printf("errors detected, suspending RDH dump\n");
      cfgRdhDumpEnabled = 0;
    } else {
      cfgRdhDumpEnabled++; // if value positive, it continues... but negative, it stops on zero, to limit number of dumps
    }
  }

  // validate RDH structure, if configured to do so
  if (cfgRdhCheckEnabled) {
    std::string errorDescription;
    size_t blockSize = blockHeader.dataSize;
    uint8_t* baseAddress = (uint8_t*)(blockData);
    int rdhIndexInPage = 0;
    int linkId = undefinedLinkId;

    static InfoLogger::AutoMuteToken logRdhErrorsToken(LogWarningSupport_(3004), 30, 5);

    for (size_t pageOffset = 0; pageOffset < blockSize;) {
      RdhHandle h(baseAddress + pageOffset);
      rdhIndexInPage++;

      // printf("RDH #%d @ 0x%X : next block @ +%d bytes\n",rdhIndexInPage,(unsigned int)pageOffset,h.getOffsetNextPacket());

      if (h.validateRdh(errorDescription)) {
        if ((cfgRdhDumpEnabled) || (cfgRdhDumpErrorEnabled)) {
          for (int i = 0; i < 16; i++) {
            printf("%08X ", (int)(((uint32_t*)baseAddress)[i]));
          }
          printf("\n");
          printf("Page 0x%p + %ld\n%s", (void*)baseAddress, pageOffset, errorDescription.c_str());
          h.dumpRdh(pageOffset, 1);
          errorDescription.clear();
        }
        statsRdhCheckErr++;
        // stop on first RDH error (should distinguich valid/invalid block length)
        break;
      } else {
        statsRdhCheckOk++;

        if (cfgRdhDumpEnabled) {
          h.dumpRdh(pageOffset, 1);
          for (int i = 0; i < 16; i++) {
            printf("%08X ", (int)(((uint32_t*)baseAddress + pageOffset)[i]));
          }
          printf("\n");
        }
      }

      // linkId should be same everywhere in page
      if (pageOffset == 0) {
        linkId = h.getLinkId(); // keep link of 1st RDH
      }
      if (linkId != h.getLinkId()) {
        if (cfgRdhDumpWarningEnabled) {
          theLog.log(logRdhErrorsToken, "Equipment %d RDH #%d @ 0x%X : inconsistent link ids: %d != %d", id, rdhIndexInPage, (unsigned int)pageOffset, linkId, h.getLinkId());
        }
        statsRdhCheckStreamErr++;
        break; // stop checking this page
      }

      // check no timeframe overlap in page
      if (((blockHeader.timeframeOrbitFirst < blockHeader.timeframeOrbitLast) && ((h.getTriggerOrbit() < blockHeader.timeframeOrbitFirst) || (h.getTriggerOrbit() > blockHeader.timeframeOrbitLast))) || ((blockHeader.timeframeOrbitFirst > blockHeader.timeframeOrbitLast) && ((h.getTriggerOrbit() < blockHeader.timeframeOrbitFirst) && (h.getTriggerOrbit() > blockHeader.timeframeOrbitLast)))) {
        if (cfgRdhDumpErrorEnabled) {
          theLog.log(logRdhErrorsToken, "Equipment %d RDH #%d @ 0x%X : TimeFrame ID change in page not allowed : orbit 0x%08X not in range [0x%08X,0x%08X]", id, rdhIndexInPage, (unsigned int)pageOffset, (int)h.getTriggerOrbit(), (int)blockHeader.timeframeOrbitFirst, (int)blockHeader.timeframeOrbitLast);
        }
        statsRdhCheckStreamErr++;
        break; // stop checking this page
      }

      /*
      // check packetCounter is contiguous
      if (cfgRdhCheckPacketCounterContiguous) {
        uint8_t newCount = h.getPacketCounter();
        // no boundary check necessary to verify linkId<=RdhMaxLinkId, this was done in validateRDH()
        if (newCount != RdhLastPacketCounter[linkId]) {
          if (newCount !=
              (uint8_t)(RdhLastPacketCounter[linkId] + (uint8_t)1)) {
            theLog.log(LogDebugTrace,
                       "RDH #%d @ 0x%X : possible packets dropped for link %d, packetCounter jump from %d to %d",
                       rdhIndexInPage, (unsigned int)pageOffset,
                       (int)linkId, (int)RdhLastPacketCounter[linkId],
                       (int)newCount);
          }
          RdhLastPacketCounter[linkId] = newCount;
        }
      }
      */

      // todo: check counter increasing all have same TF id

      uint16_t offsetNextPacket = h.getOffsetNextPacket();
      if (offsetNextPacket == 0) {
        break;
      }
      pageOffset += offsetNextPacket;
    }
  }
  return 0;
}
