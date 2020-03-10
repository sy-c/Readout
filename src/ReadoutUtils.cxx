// Copyright CERN and copyright holders of ALICE O2. This software is
// distributed under the terms of the GNU General Public License v3 (GPL
// Version 3), copied verbatim in the file "COPYING".
//
// See http://alice-o2.web.cern.ch/license for full licensing information.
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

#include "ReadoutUtils.h"
#include <math.h>
#include <sstream>

#include "RAWDataHeader.h"

// function to convert a string to a 64-bit integer value
// allowing usual "base units" in suffix (k,M,G,T)
// input can be decimal (1.5M is valid, will give 1.5*1024*1024)
long long ReadoutUtils::getNumberOfBytesFromString(const char *inputString) {
  double v = 0;
  char c = '?';
  int n = sscanf(inputString, "%lf%c", &v, &c);

  if (n == 1) {
    return (long long)v;
  }
  if (n == 2) {
    if (c == 'k') {
      return (long long)(v * 1024LL);
    }
    if (c == 'M') {
      return (long long)(v * 1024LL * 1024LL);
    }
    if (c == 'G') {
      return (long long)(v * 1024LL * 1024LL * 1024LL);
    }
    if (c == 'T') {
      return (long long)(v * 1024LL * 1024LL * 1024LL * 1024LL);
    }
    if (c == 'P') {
      return (long long)(v * 1024LL * 1024LL * 1024LL * 1024LL * 1024LL);
    }
  }
  return 0;
}

// macro to get number of element in static array
#define STATIC_ARRAY_ELEMENT_COUNT(x) sizeof(x) / sizeof(x[0])

std::string ReadoutUtils::NumberOfBytesToString(double value,
                                                const char *suffix) {
  const char *prefixes[] = {"", "k", "M", "G", "T", "P"};
  int maxPrefixIndex = STATIC_ARRAY_ELEMENT_COUNT(prefixes) - 1;
  int prefixIndex = log(value) / log(1024);
  if (prefixIndex > maxPrefixIndex) {
    prefixIndex = maxPrefixIndex;
  }
  if (prefixIndex < 0) {
    prefixIndex = 0;
  }
  double scaledValue = value / pow(1024, prefixIndex);
  char bufStr[64];
  if (suffix == nullptr) {
    suffix = "";
  }
  // optimize number of digits displayed
  int l = (int)floor(log10(fabs(scaledValue)));
  if (l < 0) {
    l = 3;
  } else if (l <= 3) {
    l = 3 - l;
  } else {
    l = 0;
  }
  snprintf(bufStr, sizeof(bufStr) - 1, "%.*lf %s%s", l, scaledValue,
           prefixes[prefixIndex], suffix);
  return std::string(bufStr);
}

void dumpRDH(o2::Header::RAWDataHeader *rdh) {
  printf("RDH:\tversion=%d\theader size=%d\tblock length=%d\n",
         (int)rdh->version, (int)rdh->headerSize, (int)rdh->blockLength);
  printf("\tTRG orbit=%d bc=%d\n", (int)rdh->triggerOrbit, (int)rdh->triggerBC);
  printf("\tHB  orbit=%d bc=%d\n", (int)rdh->heartbeatOrbit,
         (int)rdh->heartbeatBC);
  printf("\tfeeId=%d\tlinkId=%d\n", (int)rdh->feeId, (int)rdh->linkId);
}

int getKeyValuePairsFromString(const std::string &input,
                               std::map<std::string, std::string> &output) {
  output.clear();
  std::size_t ix0=0;                 // begin of pair in string
  std::size_t ix1=std::string::npos; // end of pair in string
  std::size_t ix2=std::string::npos; // position of '='
  for(;;) {
    ix1 = input.find(",",ix0);
    ix2 = input.find("=",ix0);
    if (ix2>=ix1) { break; } // end of string
    if (ix1 == std::string::npos) {
      output.insert(
          std::pair<std::string, std::string>(input.substr(ix0, ix2-ix0), input.substr(ix2+1)));
      
      break;
    }
    output.insert(
          std::pair<std::string, std::string>(input.substr(ix0, ix2-ix0), input.substr(ix2+1,ix1-(ix2+1))));
    ix0 = ix1 + 1;
  }
  return 0;
}

std::string NumberOfBytesToString(double value, const char *suffix, int base) {
  const char *prefixes[] = {"", "k", "M", "G", "T", "P"};
  int maxPrefixIndex = STATIC_ARRAY_ELEMENT_COUNT(prefixes) - 1;
  int prefixIndex = log(value) / log(base);
  if (prefixIndex > maxPrefixIndex) {
    prefixIndex = maxPrefixIndex;
  }
  if (prefixIndex < 0) {
    prefixIndex = 0;
  }
  double scaledValue = value / pow(base, prefixIndex);
  char bufStr[64];
  if (suffix == nullptr) {
    suffix = "";
  }
  snprintf(bufStr, sizeof(bufStr) - 1, "%.03lf %s%s", scaledValue,
           prefixes[prefixIndex], suffix);
  return std::string(bufStr);
}
