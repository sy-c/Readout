#include "ReadoutEquipment.h"


#include <InfoLogger/InfoLogger.hxx>
using namespace AliceO2::InfoLogger;
extern InfoLogger theLog;


ReadoutEquipment::ReadoutEquipment(ConfigFile &cfg, std::string cfgEntryPoint) {
  
  // example: browse config keys
  //for (auto cfgKey : ConfigFileBrowser (&cfg,"",cfgEntryPoint)) {
  //  std::string cfgValue=cfg.getValue<std::string>(cfgEntryPoint + "." + cfgKey);
  //  printf("%s.%s = %s\n",cfgEntryPoint.c_str(),cfgKey.c_str(),cfgValue.c_str());
  //}

  // by default, name the equipment as the config node entry point
  // configuration parameter: | equipment-* | name | string| | Name used to identify this equipment (in logs). By default, it takes the name of the configuration section, equipment-xxx |
  cfg.getOptionalValue<std::string>(cfgEntryPoint + ".name", name, cfgEntryPoint);

  // A number to identify equipment (used e.g. to tag data produced by this equipment)
  // configuration parameter: | equipment-* | id | int| | Optional. Number used to identify equipment (used e.g. in file recording). Range 1-65535.|
  int cfgEquipmentId=undefinedEquipmentId;
  cfg.getOptionalValue<int>(cfgEntryPoint + ".id", cfgEquipmentId);
  id=(uint16_t)cfgEquipmentId; // int to 16-bit value

  // target readout rate in Hz, -1 for unlimited (default). Global parameter, same for all equipments.
  // configuration parameter: | readout | rate | double | -1 | Data rate limit, per equipment, in Hertz. -1 for unlimited. |
  cfg.getOptionalValue<double>("readout.rate", readoutRate, -1.0);

  // idle sleep time, in microseconds.
  // configuration parameter: | equipment-* | idleSleepTime | int | 200 | Thread idle sleep time, in microseconds. |
  int cfgIdleSleepTime=200;
  cfg.getOptionalValue<int>(cfgEntryPoint + ".idleSleepTime", cfgIdleSleepTime);

  // size of equipment output FIFO
  // configuration parameter: | equipment-* | outputFifoSize | int | -1 | Size of output fifo (number of pages). If -1, set to the same value as memoryPoolNumberOfPages (this ensures that nothing can block the equipment while there are free pages). |
  int cfgOutputFifoSize=-1;
  cfg.getOptionalValue<int>(cfgEntryPoint + ".outputFifoSize", cfgOutputFifoSize);

  // get memory bank parameters
  // configuration parameter: | equipment-* | memoryBankName | string | | Name of bank to be used. By default, it uses the first available bank declared. |
  cfg.getOptionalValue<std::string>(cfgEntryPoint + ".memoryBankName", memoryBankName);
  std::string cfgMemoryPoolPageSize="";
  // configuration parameter: | equipment-* | memoryPoolPageSize | bytes | | Size of each memory page to be created. Some space might be kept in each page for internal readout usage. |
  cfg.getOptionalValue<std::string>(cfgEntryPoint + ".memoryPoolPageSize",cfgMemoryPoolPageSize);
  // configuration parameter: | equipment-* | memoryPoolNumberOfPages | int | | Number of pages to be created for this equipment, taken from the chosen memory bank.|
  memoryPoolPageSize=(int)ReadoutUtils::getNumberOfBytesFromString(cfgMemoryPoolPageSize.c_str());
  cfg.getOptionalValue<int>(cfgEntryPoint + ".memoryPoolNumberOfPages", memoryPoolNumberOfPages);
  if (cfgOutputFifoSize==-1) {
    cfgOutputFifoSize=memoryPoolNumberOfPages;
  }

  // disable output?
  // configuration parameter: | equipment-* | disableOutput | int | 0 | If non-zero, data generated by this equipment is discarded immediately and is not pushed to output fifo of readout thread. Used for testing. |
  cfg.getOptionalValue<int>(cfgEntryPoint + ".disableOutput", disableOutput);

  // memory alignment
  // configuration parameter: | equipment-* | firstPageOffset | bytes | | Offset of the first page, in bytes from the beginning of the memory pool. If not set (recommended), will start at memoryPoolPageSize (one free page is kept before the first usable page for readout internal use). |
  std::string cfgStringFirstPageOffset="0";
  cfg.getOptionalValue<std::string>(cfgEntryPoint + ".firstPageOffset", cfgStringFirstPageOffset);
  size_t cfgFirstPageOffset=(size_t)ReadoutUtils::getNumberOfBytesFromString(cfgStringFirstPageOffset.c_str());
  // configuration parameter: | equipment-* | blockAlign | bytes | 2M | Alignment of the beginning of the big memory block from which the pool is created. Pool will start at a multiple of this value. Each page will then begin at a multiple of memoryPoolPageSize from the beginning of big block. |
  std::string cfgStringBlockAlign="2M";
  cfg.getOptionalValue<std::string>(cfgEntryPoint + ".blockAlign", cfgStringBlockAlign);
  size_t cfgBlockAlign=(size_t)ReadoutUtils::getNumberOfBytesFromString(cfgStringBlockAlign.c_str());
  
  // output periodic statistics on console
  // configuration parameter: | equipment-* | consoleStatsUpdateTime | double | 0 | If set, number of seconds between printing statistics on console. |
  cfg.getOptionalValue<double>(cfgEntryPoint + ".consoleStatsUpdateTime", cfgConsoleStatsUpdateTime);

    // configuration parameter: | equipment-* | stopOnError | int | 0 | If 1, readout will stop automatically on equipment error. |
    int cfgStopOnError=0;
    cfg.getOptionalValue<int>(cfgEntryPoint + ".stopOnError",cfgStopOnError);
    if (cfgStopOnError) {
      this->stopOnError=1;
    }
    
  // log config summary
  theLog.log("Equipment %s: from config [%s], max rate=%lf Hz, idleSleepTime=%d us, outputFifoSize=%d", name.c_str(), cfgEntryPoint.c_str(), readoutRate, cfgIdleSleepTime, cfgOutputFifoSize);
  theLog.log("Equipment %s: requesting memory pool %d pages x %d bytes from bank '%s', block aligned @ 0x%X, 1st page offset @ 0x%X", name.c_str(),(int) memoryPoolNumberOfPages, (int) memoryPoolPageSize, memoryBankName.c_str(),(int)cfgBlockAlign,(int)cfgFirstPageOffset);
  if (disableOutput) {
    theLog.log("Equipment %s: output DISABLED ! Data will be readout and dropped immediately",name.c_str());
  }
  
  // init stats
  equipmentStats.resize(EquipmentStatsIndexes::maxIndex);
  equipmentStatsLast.resize(EquipmentStatsIndexes::maxIndex);

  // creation of memory pool for data pages
  // todo: also allocate pool of DataBlockContainers? at the same time? reserve space at start of pages?
  if ((memoryPoolPageSize<=0)||(memoryPoolNumberOfPages<=0)) {
     theLog.log("Equipment %s: wrong memory pool settings", name.c_str());
     throw __LINE__;
  } 
  pageSpaceReserved=sizeof(DataBlock); // reserve some data at beginning of each page for header, keep beginning of payload aligned as requested in config
  size_t firstPageOffset=0; // alignment of 1st page of memory pool
  if (pageSpaceReserved) {
    // auto-align
    firstPageOffset=memoryPoolPageSize-pageSpaceReserved;  
  }
  if (cfgFirstPageOffset) {
    firstPageOffset=cfgFirstPageOffset-pageSpaceReserved;
  }
  theLog.log("pageSpaceReserved = %d, aligning 1st page @ 0x%X",(int)pageSpaceReserved,(int)firstPageOffset);
  mp=nullptr;
  try {
    mp=theMemoryBankManager.getPagedPool(memoryPoolPageSize, memoryPoolNumberOfPages, memoryBankName, firstPageOffset, cfgBlockAlign);
  }
  catch(...) {
  }
  if (mp==nullptr) {
    theLog.log(InfoLogger::Severity::Error,"Failed to create pool of memory pages");
    throw __LINE__;
  }

  // create output fifo
  dataOut=std::make_shared<AliceO2::Common::Fifo<DataBlockContainerReference>>(cfgOutputFifoSize);
  if (dataOut==nullptr) {
    throw __LINE__;
  }
  
  // create thread
  readoutThread=std::make_unique<Thread>(ReadoutEquipment::threadCallback, this, name, cfgIdleSleepTime);
  if (readoutThread==nullptr) {
    throw __LINE__;
  }
}

const std::string & ReadoutEquipment::getName() {
  return name;
}

void ReadoutEquipment::start() {
  // reset counters
  for (int i=0; i<(int)EquipmentStatsIndexes::maxIndex; i++) {
    equipmentStats[i].reset();
    equipmentStatsLast[i]=0;
  }
  currentBlockId=0;
  
  readoutThread->start();
  if (readoutRate>0) {
    clk.reset(1000000.0/readoutRate);
  }
  clk0.reset();
  
  // reset stats timer
  consoleStatsTimer.reset(cfgConsoleStatsUpdateTime*1000000);
}

void ReadoutEquipment::stop() {
  
  double runningTime=clk0.getTime();
  readoutThread->stop();
  //printf("%llu blocks in %.3lf seconds => %.1lf block/s\n",nBlocksOut,clk0.getTimer(),nBlocksOut/clk0.getTime());
  readoutThread->join();
  
  for (int i=0; i<(int)EquipmentStatsIndexes::maxIndex; i++) {
    if (equipmentStats[i].getCount()) { 
      theLog.log("%s.%s = %lu  (avg=%.2lf  min=%lu  max=%lu  count=%lu)",name.c_str(),EquipmentStatsNames[i],equipmentStats[i].get(),equipmentStats[i].getAverage(),equipmentStats[i].getMinimum(),equipmentStats[i].getMaximum(),equipmentStats[i].getCount());
    } else {
      theLog.log("%s.%s = %lu",name.c_str(),EquipmentStatsNames[i],equipmentStats[i].get());    
    }
  }
  
  theLog.log("Average pages pushed per iteration: %.1f",equipmentStats[EquipmentStatsIndexes::nBlocksOut].get()*1.0/(equipmentStats[EquipmentStatsIndexes::nLoop].get()-equipmentStats[EquipmentStatsIndexes::nIdle].get()));
  theLog.log("Average fifoready occupancy: %.1f",equipmentStats[EquipmentStatsIndexes::fifoOccupancyFreeBlocks].get()*1.0/(equipmentStats[EquipmentStatsIndexes::nLoop].get()-equipmentStats[EquipmentStatsIndexes::nIdle].get()));
  theLog.log("Average data throughput: %s",ReadoutUtils::NumberOfBytesToString(equipmentStats[EquipmentStatsIndexes::nBytesOut].get()/runningTime,"B/s").c_str());
}

ReadoutEquipment::~ReadoutEquipment() {
   // check if mempool still referenced
  if (!mp.unique()) {
    theLog.log("Equipment %s :  mempool still has %d references\n",name.c_str(),(int)mp.use_count());
  }
}




DataBlockContainerReference ReadoutEquipment::getBlock() {
  DataBlockContainerReference b=nullptr;
  dataOut->pop(b);
  return b;
}

Thread::CallbackResult  ReadoutEquipment::threadCallback(void *arg) {
  ReadoutEquipment *ptr=static_cast<ReadoutEquipment *>(arg);

  // flag to identify if something was done in this iteration
  bool isActive=false;
  
  for (;;) {
    ptr->equipmentStats[EquipmentStatsIndexes::nLoop].increment();

    // max number of blocks to read in this iteration.
    // this is a finite value to ensure all readout steps are done regularly.
    int maxBlocksToRead=1024;

    // check throughput
    if (ptr->readoutRate>0) {
      uint64_t nBlocksOut=ptr->equipmentStats[(int)EquipmentStatsIndexes::nBlocksOut].get(); // number of blocks we have already readout until now
      maxBlocksToRead=ptr->readoutRate*ptr->clk0.getTime()-nBlocksOut;
      if ((!ptr->clk.isTimeout()) && (nBlocksOut!=0) && (maxBlocksToRead<=0)) {
        // target block rate exceeded, wait a bit
        ptr->equipmentStats[EquipmentStatsIndexes::nThrottle].increment();
        break;
      }
    }

    // check status of output FIFO
    ptr->equipmentStats[EquipmentStatsIndexes::fifoOccupancyOutBlocks].set(ptr->dataOut->getNumberOfUsedSlots());

    // try to get new blocks
    int nPushedOut=0;
    for (int i=0;i<maxBlocksToRead;i++) {

      // check output FIFO status so that we are sure we can push next block, if any
      if (ptr->dataOut->isFull()) {
        ptr->equipmentStats[EquipmentStatsIndexes::nOutputFull].increment();
        break;
      }

      // get next block
      DataBlockContainerReference nextBlock=ptr->getNextBlock();
      if (nextBlock==nullptr) {
        break;
      }
      // tag data with equipment Id
      nextBlock->getData()->header.equipmentId=ptr->id;

      // tag data with block id
      ptr->currentBlockId++;  // don't start from 0
      nextBlock->getData()->header.blockId=ptr->currentBlockId;
      
      // tag data with (dummy) timeframeid, if none set
      if (nextBlock->getData()->header.timeframeId==undefinedTimeframeId) {
        nextBlock->getData()->header.timeframeId=nextBlock->getData()->header.blockId;
	// this should be done by something smarter, e.g. looking into the payload to set timeframeid
	// the code from ROC equipment extracting timestamps from RDH (and software clock if no RDH) should be moved here and common to all
      }
      
      if (!ptr->disableOutput) {
        // push new page to output fifo
        ptr->dataOut->push(nextBlock);
      }
      
      // update rate-limit clock
      if (ptr->readoutRate>0) {
        ptr->clk.increment();
      }

      // update stats
      nPushedOut++;
      ptr->equipmentStats[EquipmentStatsIndexes::nBytesOut].increment(nextBlock->getData()->header.dataSize);   
      isActive=true;
    }
    ptr->equipmentStats[EquipmentStatsIndexes::nBlocksOut].increment(nPushedOut);
    

    // prepare next blocks
    Thread::CallbackResult statusPrepare=ptr->prepareBlocks();
    switch (statusPrepare) {
      case (Thread::CallbackResult::Ok):
        isActive=true;
        break;
      case (Thread::CallbackResult::Idle):
        break;      
      default:
        // this is an abnormal situation, return corresponding status
        return statusPrepare;
    }


    // consider inactive if we have not pushed much compared to free space in output fifo
    // todo: instead, have dynamic 'inactive sleep time' as function of actual outgoing page rate to optimize polling interval
    if (nPushedOut<ptr->dataOut->getNumberOfFreeSlots()/4) {
// disabled, should not depend on output fifo size
//      isActive=0;
    }
      
    // todo: add SLICER to aggregate together time-range data
    // todo: get other FIFO status

    // print statistics on console, if configured
    if (ptr->cfgConsoleStatsUpdateTime>0) {
      if (ptr->consoleStatsTimer.isTimeout()) {         
        for (int i=0; i<(int)EquipmentStatsIndexes::maxIndex; i++) {
          CounterValue vNew=ptr->equipmentStats[i].getCount();
          CounterValue vDiff=vNew-ptr->equipmentStatsLast[i];
          ptr->equipmentStatsLast[i]=vNew;
          theLog.log("%s.%s : diff=%lu total=%lu",ptr->name.c_str(),ptr->EquipmentStatsNames[i],vDiff,vNew);
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


void ReadoutEquipment::setDataOn() {
  isDataOn=true;
}

void ReadoutEquipment::setDataOff() {
  isDataOn=false;
}
