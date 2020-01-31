//**************************************************************************\
//* This file is property of and copyright by the ALICE Project            *\
//* ALICE Experiment at CERN, All rights reserved.                         *\
//*                                                                        *\
//* Primary Authors: Matthias Richter <Matthias.Richter@ift.uib.no>        *\
//*                  for The ALICE HLT Project.                            *\
//*                                                                        *\
//* Permission to use, copy, modify and distribute this software and its   *\
//* documentation strictly for non-commercial purposes is hereby granted   *\
//* without fee, provided that the above copyright notice appears in all   *\
//* copies and that both the copyright notice and this permission notice   *\
//* appear in the supporting documentation. The authors make no claims     *\
//* about the suitability of this software for any purpose. It is          *\
//* provided "as is" without express or implied warranty.                  *\
//**************************************************************************

/// \file Deconvolution.cxx
/// \author Felix Weiglhofer

#include "Deconvolution.h"
#include "Array2D.h"
#include "CfConsts.h"
#include "CfUtils.h"

using namespace GPUCA_NAMESPACE::gpu;
using namespace GPUCA_NAMESPACE::gpu::deprecated;

GPUd() void Deconvolution::countPeaksImpl(int nBlocks, int nThreads, int iBlock, int iThread, GPUTPCClusterFinderKernels::GPUTPCSharedMemory& smem,
                                          GPUglobalref() const uchar* peakMap,
                                          GPUglobalref() PackedCharge* chargeMap,
                                          GPUglobalref() const Digit* digits,
                                          const uint digitnum)
{
  size_t idx = get_global_id(0);

  bool iamDummy = (idx >= digitnum);
  /* idx = select(idx, (size_t)(digitnum-1), (size_t)iamDummy); */
  idx = iamDummy ? digitnum - 1 : idx;

  Digit myDigit = digits[idx];

  GlobalPad gpad = Array2D::tpcGlobalPadIdx(myDigit.row, myDigit.pad);
  bool iamPeak = GET_IS_PEAK(IS_PEAK(peakMap, gpad, myDigit.time));

  char peakCount = (iamPeak) ? 1 : 0;

#if defined(BUILD_CLUSTER_SCRATCH_PAD)
  ushort ll = get_local_id(0);
  ushort partId = ll;

  ushort in3x3 = 0;
  partId = CfUtils::partition(smem, ll, iamPeak, SCRATCH_PAD_WORK_GROUP_SIZE, &in3x3);

  if (partId < in3x3) {
    smem.count.posBcast1[partId] = (ChargePos){gpad, myDigit.time};
  }
  GPUbarrier();

  CfUtils::fillScratchPad_uchar(
    peakMap,
    in3x3,
    SCRATCH_PAD_WORK_GROUP_SIZE,
    ll,
    0,
    8,
    CfConsts::InnerNeighbors,
    smem.count.posBcast1,
    smem.count.buf);

  uchar aboveThreshold = 0;
  if (partId < in3x3) {
    peakCount = countPeaksScratchpadInner(partId, smem.count.buf, &aboveThreshold);
  }

  ushort in5x5 = 0;
  partId = CfUtils::partition(smem, partId, peakCount > 0, in3x3, &in5x5);

  if (partId < in5x5) {
    smem.count.posBcast1[partId] = (ChargePos){gpad, myDigit.time};
    smem.count.aboveThresholdBcast[partId] = aboveThreshold;
  }
  GPUbarrier();

  CfUtils::fillScratchPadCondInv_uchar(
    peakMap,
    in5x5,
    SCRATCH_PAD_WORK_GROUP_SIZE,
    ll,
    0,
    16,
    CfConsts::OuterNeighbors,
    smem.count.posBcast1,
    smem.count.aboveThresholdBcast,
    smem.count.buf);

  if (partId < in5x5) {
    peakCount = countPeaksScratchpadOuter(partId, 0, aboveThreshold, smem.count.buf);
    peakCount *= -1;
  }

  /* fillScratchPadCondInv_uchar( */
  /*         peakMap, */
  /*         in5x5, */
  /*         SCRATCH_PAD_WORK_GROUP_SIZE, */
  /*         ll, */
  /*         8, */
  /*         N, */
  /*         OUTER_NEIGHBORS, */
  /*         smem.count.posBcast1, */
  /*         smem.count.aboveThresholdBcast, */
  /*         smem.count.buf); */
  /* if (partId < in5x5) */
  /* { */
  /*     peakCount += countPeaksScratchpadOuter(partId, 8, aboveThreshold, smem.count.buf); */
  /*     peakCount *= -1; */
  /* } */

#else
  peakCount = countPeaksAroundDigit(gpad, myDigit.time, peakMap);
  peakCount = iamPeak ? 1 : peakCount;
#endif

  if (iamDummy) {
    return;
  }

  bool has3x3 = (peakCount > 0);
  peakCount = abs(peakCount);
  bool split = (peakCount > 1);

  peakCount = (peakCount == 0) ? 1 : peakCount;

  PackedCharge p(myDigit.charge / peakCount, has3x3, split);

  CHARGE(chargeMap, gpad, myDigit.time) = p;
}

GPUd() char Deconvolution::countPeaksAroundDigit(
  const GlobalPad gpad,
  const Timestamp time,
  GPUglobalref() const uchar* peakMap)
{
  char peakCount = 0;

  uchar aboveThreshold = 0;
  for (uchar i = 0; i < 8; i++) {
    Delta2 d = CfConsts::InnerNeighbors[i];
    Delta dp = d.x;
    Delta dt = d.y;

    uchar p = IS_PEAK(peakMap, gpad + dp, time + dt);
    peakCount += GET_IS_PEAK(p);
    aboveThreshold |= GET_IS_ABOVE_THRESHOLD(p) << i;
  }

  if (peakCount > 0) {
    return peakCount;
  }

  for (uchar i = 0; i < 16; i++) {
    Delta2 d = CfConsts::OuterNeighbors[i];
    Delta dp = d.x;
    Delta dt = d.y;

    if (CfUtils::innerAboveThresholdInv(aboveThreshold, i)) {
      peakCount -= GET_IS_PEAK(IS_PEAK(peakMap, gpad + dp, time + dt));
    }
  }

  return peakCount;
}

GPUd() char Deconvolution::countPeaksScratchpadInner(
  ushort ll,
  GPUsharedref() const uchar* isPeak,
  uchar* aboveThreshold)
{
  char peaks = 0;
  for (uchar i = 0; i < 8; i++) {
    uchar p = isPeak[ll * 8 + i];
    peaks += GET_IS_PEAK(p);
    *aboveThreshold |= GET_IS_ABOVE_THRESHOLD(p) << i;
  }

  return peaks;
}

GPUd() char Deconvolution::countPeaksScratchpadOuter(
  ushort ll,
  ushort offset,
  uchar aboveThreshold,
  GPUsharedref() const uchar* isPeak)
{
  char peaks = 0;
  for (uchar i = 0; i < 16; i++) {
    uchar p = isPeak[ll * 16 + i];
    /* bool extend = innerAboveThresholdInv(aboveThreshold, i + offset); */
    /* p = (extend) ? p : 0; */
    peaks += GET_IS_PEAK(p);
  }

  return peaks;
}