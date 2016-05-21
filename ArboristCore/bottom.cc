// This file is part of ArboristCore.

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/**
   @file bottom.cc

   @brief Methods involving the most recently trained tree levels.

   @author Mark Seligman
 */

#include "bottom.h"
#include "bv.h"
#include "index.h"
#include "splitpred.h"
#include "samplepred.h"
#include "sample.h"
#include "splitsig.h"
#include "predblock.h"
#include "runset.h"

// Testing only:
//#include <iostream>
using namespace std;

/**
   @brief Static entry for regression.
 */
Bottom *Bottom::FactoryReg(SamplePred *_samplePred, unsigned int bagCount) {
  return new Bottom(_samplePred, new SPReg(_samplePred, bagCount), bagCount, PBTrain::NPred(), PBTrain::NPredFac());
}


/**
   @brief Static entry for classification.
 */
Bottom *Bottom::FactoryCtg(SamplePred *_samplePred, SampleNode *_sampleCtg, unsigned int bagCount) {
  return new Bottom(_samplePred, new SPCtg(_samplePred, _sampleCtg, bagCount), bagCount, PBTrain::NPred(), PBTrain::NPredFac());
}


/**
   @brief Class constructor.

   @param bagCount enables sizing of predicate bit vectors.

   @param splitCount specifies the number of splits to map.
 */
Bottom::Bottom(SamplePred *_samplePred, SplitPred *_splitPred, unsigned int bagCount, unsigned int _nPred, unsigned int _nPredFac) : samplePath(new SamplePath[bagCount]), nPred(_nPred), nPredFac(_nPredFac), ancTot(0), levelCount(1), samplePred(_samplePred), splitPred(_splitPred), splitSig(new SplitSig()), bottomNode(new BottomNode[nPred]) {
  for (unsigned int predIdx = 0; predIdx < nPred; predIdx++) {
    bottomNode[predIdx].Init(PBTrain::FacCard(predIdx));
  }

    // 'nPred'-many source bits for level zero initialized to zero.
  bufferLevel.push_front(new BitMatrix(1, nPred));

  // 'bagCount'-many indices in staged predictors.
  std::vector<MRRA> mrraZero;
  MRRA mrra;
  mrra.Init(0, bagCount);
  mrraZero.push_back(mrra);
  mrraLevel.push_front(mrraZero);
}


/**
   @brief Class finalizer.
 */
Bottom::~Bottom() {
  for (BitMatrix *bitLevel : bufferLevel) {
    delete bitLevel;
  }
  bufferLevel.clear();
  mrraLevel.clear();

  delete [] samplePath;
  delete [] bottomNode; // Always one dangling.
  delete splitPred;
  delete splitSig;
}


Run *Bottom::Runs() {
  return splitPred->Runs();
}


/**
   @brief Entry to spltting and restaging.

   @return vector of splitting signatures, possibly empty, for each node passed.
 */
const std::vector<class SSNode*> Bottom::LevelSplit(class Index *index, class IndexNode indexNode[]) {
  Run *run;
  bool *splitFlags = splitPred->LevelInit(index, indexNode, this, levelCount, run);
  Level(run, splitFlags, indexNode);
  std::vector<SSNode*> ssNode(levelCount);
  for (unsigned int levelIdx = 0; levelIdx < levelCount; levelIdx++) {
    ssNode[levelIdx] = splitSig->ArgMax(levelIdx, indexNode[levelIdx].MinInfo());
  }

  return ssNode;
}


void Bottom::Level(Run *run, const bool splitFlags[], const IndexNode indexNode[]) {
  std::vector<SplitPair> pairNode;
  pairNode.reserve(levelCount * nPred); // Very high limit.

  // Pulls in the reaching MRRAs from the various levels at which
  // they last restaged, allowing dense lookup:  start, extent, pathBase.
  std::vector<RestageNode> restageNode;
  restageNode.reserve(ancTot); // Safe upper limit.
  unsigned int targTot = PairInit(run, splitFlags, pairNode, restageNode);

  // None of the restaging work need be done at level zero.
  if (ancTot > 0) {
    std::vector<PathNode> pathNode(targTot); // Reaching-path nodes and offsets.
    PathNode node;
    node.Init();
    std::fill(pathNode.begin(), pathNode.end(), node);

    std::vector<RestagePair> restagePair;
    BV *restageSource = RestageInit(indexNode, pairNode, restageNode, restagePair, pathNode);
    Restage(restageNode, restagePair, pathNode, restageSource);
    delete restageSource;
  }
  ancTot += levelCount; // All nodes at this level are potential ancestors.

  Split(pairNode, indexNode);
}


/**
   @brief Initializes the vector of splitting pairs.  Flags pairs with extinct
   ancestors for restaging.

   @return total count of restageable targets.
 */
unsigned int Bottom::PairInit(Run *run, const bool splitFlags[], std::vector<SplitPair> &pairNode, std::vector<RestageNode> &restageNode) {
  unsigned int setCount = 0;  // <= nPredFac * levelCount.
  unsigned int pathAccum = 0; // Accumulated count of reaching paths.

  std::vector<unsigned int> safeCount(nPredFac * levelCount);
  for (unsigned int botIdx = 0; botIdx < levelCount * nPred; botIdx++) {
    int rl = bottomNode[botIdx].RunCount();
    if (rl != 1 && (splitFlags[botIdx] || Exhausted(botIdx))) { // Non-singletons
      SplitPair pair;
      unsigned int restageIdx = ancTot > 0 ? PathAccum(restageNode, botIdx, pathAccum) : 0;
      if (splitFlags[botIdx]) { // Splits.  Restages if ancestors present (level > 0).
	if (rl > 1) {
	  safeCount[setCount] = rl;
	  pair.SplitInit(botIdx, restageIdx, setCount++);
	}
	else {
          pair.SplitInit(botIdx, restageIdx);
	}
      }
      else
	pair.Init(botIdx, restageIdx); // Restages only.
      pairNode.push_back(pair);
    }
  }

  run->RunSets(setCount);
  for (unsigned int setIdx = 0; setIdx < setCount; setIdx++) {
    run->CountSafe(setIdx) = safeCount[setIdx];
  }

  // Every restageable target now references a dense index into the set of MRRAs
  // visible from the current level.
  // Every densely-indexed MRRA in the set now references a unique target area
  // for restaging, sufficiently wide to hold all paths reaching from it.

  return pathAccum;
}


BV *Bottom::RestageInit(const IndexNode indexNode[], const std::vector<SplitPair> &pairNode, std::vector<RestageNode> &restageNode, std::vector<RestagePair> &restagePair, std::vector<PathNode> &pathNode) {
  // Pulls in buffer indices (0/1) of restaging sources, on a per-
  // predictor basis, using the dense ordering.  Only dense pairs
  // to be restaged at this level have corresponding bits set.
  BV *restageSource = new BV(restageNode.size() * nPred);

  // Notes dense pairs reaching this level, for ultimate per-restager
  // compression.
  BV *reaching = new BV(restageNode.size() * nPred);
  
  // Records buffer positions (0/1) of restaged targets, by level-
  // relative pair ordering.  Looked up as MRRA by subsequent levels.
  BitMatrix *bufTarg = new BitMatrix(levelCount, nPred);

  // Records this level's restaged cells, by level index.
  std::vector<MRRA> mrraTarg(levelCount);

  unsigned int idxPrev = levelCount; // Tracks node index for short-circuiting.
  for (unsigned pairIdx = 0; pairIdx < pairNode.size(); pairIdx++) {
    unsigned int restageIdx;
    unsigned int bottomIdx = pairNode[pairIdx].BottomIdx(restageIdx);
    unsigned int levelIdx, predIdx;
    SplitCoords(bottomIdx, levelIdx, predIdx);

    // Will hit same source/target pair many times if multiple
    // predictors reach along the path.
    unsigned int pathZero = restageNode[restageIdx].PathZero();
    unsigned int start, extent;
    unsigned int path;
    indexNode[levelIdx].PathCoords(start, extent, path);
    pathNode[pathZero + path].Init(levelIdx, start);

    // Source buffer looked up by node position at MRRA's level:
    unsigned int levelDel;
    unsigned int mrraIdx = MrraIdx(bottomIdx, levelIdx, levelDel, true);
    BitMatrix *bufMRRA = *(end(bufferLevel) - levelDel);
    bool sourceBit = bufMRRA->IsSet(mrraIdx, predIdx);

    // Records source bit for dense pair reaching this level.
    restageSource->SetBit(PairOffset(restageIdx, predIdx), sourceBit);
    reaching->SetBit(PairOffset(restageIdx, predIdx));

    // Target position indexed by node position at THIS level:
    bufTarg->SetBit(levelIdx, predIdx, !sourceBit);

    // Walking in node-major order, so can short-ciruit repeats at
    // different predictors.
    if (idxPrev != levelIdx) {
      mrraTarg[levelIdx].Init(start, extent);
    }
    idxPrev = levelIdx;
  }

  // Distributes reaching predictors to their respective nodes.
  //
  unsigned int idx = 0;
  for (unsigned int restageIdx = 0; restageIdx < restageNode.size(); restageIdx++) {
    for (unsigned int predIdx = 0; predIdx < nPred; predIdx++) {
      if (reaching->IsSet(idx)) {
	RestagePair rsPair;
	rsPair.Init(restageIdx, predIdx);
	restagePair.push_back(rsPair);
      }
      idx++;
    }
  }
      
  bufferLevel.push_back(bufTarg);
  mrraLevel.push_back(mrraTarg);

  delete reaching;
  // Deletes information beyond the reach of future levels.
  if (bufferLevel.size() > BottomNode::pathMax) {
    delete *(begin(bufferLevel));
    bufferLevel.erase(bufferLevel.begin());
  }
  if (mrraLevel.size() > BottomNode::pathMax) {
    ancTot -= mrraLevel[0].size(); // Extinct ancestors.
    mrraLevel.erase(mrraLevel.begin());
  }

  return restageSource;
}


/**
   @brief Reports source buffer.
 */
unsigned int Bottom::BufBit(unsigned int levelIdx, unsigned int predIdx) {
  return bufferLevel.back()->IsSet(levelIdx, predIdx) ? 1 : 0;
}


/**
  @brief Looks up MRRA and accumulates its dense index and target path base.

  @param restageNode accumulates newly-created RestageNodes.

  @param bottomIdx is the node/predictor pair index.

  @param pathAccum accumulates a count of targets reached from the MRRA.

  @return index into restageNode vector.
 */
unsigned int Bottom::PathAccum(std::vector<RestageNode> &restageNode, unsigned int bottomIdx, unsigned int &pathAccum) {
  unsigned int levelIdx, predIdx, levelDel;
  SplitCoords(bottomIdx, levelIdx, predIdx);
  unsigned int mrraIdx = MrraIdx(bottomIdx, levelIdx, levelDel);

  std::vector<MRRA> &mrraVec = *(end(mrraLevel) - levelDel);
  unsigned int restageIdx = mrraVec[mrraIdx].RestageIdx(levelDel, pathAccum, restageNode);
  pathAccum += (1 << levelDel); // 2^levelDel target paths.

  return restageIdx;
}

  /**
     @brief Assigns and updates dense index and target path offset.

     @param pathZero is a starting index into a vector of target positions.

     @param _restageIdx accumulates the high water mark for dense indexing.

     @return dense index.
   */
unsigned int MRRA::RestageIdx(unsigned int levelDel, unsigned int &pathAccum, std::vector<RestageNode> &restageNode) {
  if (restageIdx < 0) { // First encounter:  caches state and starting index.
    RestageNode rsNode;
    restageIdx = restageNode.size();
    rsNode.Init(start, extent, levelDel, pathAccum);
    restageNode.push_back(rsNode);
    pathAccum += (1 << levelDel); // 2^del potential reaching paths.
  }
  return restageIdx;
}


/**
   @brief Restages predictors and splits as pairs with equal priority.


   @return void, with side-effected restaging buffers.
 */
void Bottom::Restage(const std::vector<RestageNode> &restageNode, const std::vector<RestagePair> &restagePair, const std::vector<PathNode> &pathNode, const BV *restageSource) {
  unsigned int pairIdx, nodeIdx, predIdx;

#pragma omp parallel default(shared) private(pairIdx, nodeIdx, predIdx)
  {
#pragma omp for schedule(dynamic, 1)
    for (pairIdx = 0; pairIdx < restagePair.size(); pairIdx++) {
      restagePair[pairIdx].Coords(nodeIdx, predIdx);
      restageNode[nodeIdx].Restage(this, samplePred, pathNode, predIdx, restageSource->IsSet(PairOffset(nodeIdx, predIdx)) ? 1 : 0);
    }
  }
}


/**
   @brief General, multi-level restaging.
 */
void RestageNode::Restage(const Bottom *bottom, SamplePred *samplePred, const std::vector<PathNode> &pathNode, unsigned int predIdx, unsigned int sourceBit) const {
  int targOffset[1 << BottomNode::pathMax];
  unsigned int pathCount = 1 << levelDel;
  for (unsigned int path = 0; path < pathCount; path++) {
    targOffset[path] = pathNode[pathZero + path].Offset();
  }

  SPNode *source, *targ;
  unsigned int *sIdxSource, *sIdxTarg;
  samplePred->Buffers(predIdx, sourceBit, source, sIdxSource, targ, sIdxTarg);

  unsigned char pathMask = ~(0xff << levelDel);
  for (unsigned int idx = startIdx; idx < startIdx + extent; idx++) {
    unsigned int sIdx = sIdxSource[idx];
    unsigned char sIdxPath;
    if (bottom->IsLive(sIdx, sIdxPath)) {
      unsigned char path = sIdxPath & pathMask;
      int destIdx = targOffset[path]++;
      targ[destIdx] = source[idx];
      sIdxTarg[destIdx] = sIdx;
    }
  }
  // Target bit recorded during initialization.

  Singletons(bottom, pathNode, targOffset, targ, predIdx);
}


/**
   @brief Specialized for two-path case, bypasses stack array.

 */
void RestageNode::RestageTwo(const Bottom *bottom, SamplePred *samplePred, const std::vector<PathNode> &pathNode, unsigned int predIdx, unsigned int sourceBit) const {
  unsigned int leftOff = pathNode[pathZero].Offset();
  unsigned int rightOff = pathNode[pathZero+1].Offset();
  SPNode *source, *targ;
  unsigned int *sIdxSource, *sIdxTarg;
  samplePred->Buffers(predIdx, sourceBit, source, sIdxSource, targ, sIdxTarg);

  unsigned char pathMask = ~(0xff << levelDel);
  for (unsigned int idx = startIdx; idx < startIdx + extent; idx++) {
    unsigned int sIdx = sIdxSource[idx];
    unsigned char sIdxPath;
    if (bottom->IsLive(sIdx, sIdxPath)) {
      int destIdx = (sIdxPath & pathMask) == 0 ? leftOff++ : rightOff++;
      targ[destIdx] = source[idx];
      sIdxTarg[destIdx] = sIdx;
    }
  }

  // Target bit recorded during initialization.
  // Singletons() : pass <leftOff, rightOff> as vector.
}


/**
   @brief Notes any new singletons arising as a result of this restaging.

   @param bottom is the bottom environment.

   @param targ is the restaged data.

   @param predIdx is the predictor index.

   @return void, with side-effected bottom nodes.
 */
void RestageNode::Singletons(const Bottom *bottom, const std::vector<PathNode> &pathNode, const int targOffset[], const SPNode targ[], unsigned int predIdx) const {
  unsigned int pathTot = 1 << levelDel;
  for (unsigned int path = 0; path < pathTot; path++) {
    int levelIdx, offset;
    pathNode[pathZero + path].Coords(levelIdx, offset);
    if (levelIdx >= 0) {
      if (targ->IsRun(offset, targOffset[path]-1)) {
    	bottom->SetSingleton(levelIdx, predIdx);
      }
    }
  }
}


/**
   @brief Dispatches splitting of staged pairs independently.

   @return void.
 */
void Bottom::Split(const std::vector<SplitPair> &pairNode, const IndexNode indexNode[]) {
  splitPred->RunOffsets();
  unsigned int stageIdx;
  
#pragma omp parallel default(shared) private(stageIdx)
  {
#pragma omp for schedule(dynamic, 1)
    for (stageIdx = 0; stageIdx < pairNode.size(); stageIdx++) {
      Split(indexNode, pairNode[stageIdx]);
    }
  }
}


/**
   @brief Dispatches the staged node to the appropriate splitting family.
 */
void Bottom::Split(const IndexNode indexNode[], const SplitPair &pairNode) {
   int setIdx;
   if (pairNode.Split(setIdx)) {
     unsigned int restageIdx; // Dense index:  zero if no restaging.
     unsigned int bottomIdx = pairNode.BottomIdx(restageIdx);
     unsigned int levelIdx, predIdx;
     SplitCoords(bottomIdx, levelIdx, predIdx);
     unsigned int bufBit = BufBit(levelIdx, predIdx);
     if (setIdx >= 0) {
       splitPred->SplitFac(bottomIdx, setIdx, &indexNode[levelIdx], samplePred->PredBase(predIdx, bufBit));
    }
    else {
      splitPred->SplitNum(bottomIdx, &indexNode[levelIdx], samplePred->PredBase(predIdx, bufBit));
    }
  }
}


void Bottom::SSWrite(unsigned int bottomIdx, int setIdx, unsigned int lhSampCount, unsigned int lhIdxCount, double info) {
  unsigned int levelIdx, predIdx;
  SplitCoords(bottomIdx, levelIdx, predIdx);
  splitSig->Write(levelIdx, predIdx, setIdx, lhSampCount, lhIdxCount, info);
}


void Bottom::LevelInit() {
  splitSig->LevelInit(levelCount);
}


/**
   @brief
 */
void Bottom::LevelClear() {
  splitPred->LevelClear();
  splitSig->LevelClear();
}


/**
   @brief Allocates storage for upcoming level.

   @param _splitNext is the number of nodes in the upcoming level.

   @return void.
 */
void Bottom::Overlap(unsigned int _splitNext) {
  levelCount = _splitNext;
  preStage = new BottomNode[levelCount * nPred];
}


/**
   @brief Consumes all fields in current NodeCache item relevant to restaging.

   @param _splitIdx is the split index.

   @param _lNext is the index node offset of the LHS in the next level.

   @param _rNext is the index node offset of the RHS in the next level.

   @param _lhIdxCount is the count of indices associated with the split's LHS.

   @param _rhIdxCount is the count of indices associated with the split's RHS.

   @return void.
*/
void Bottom::Inherit(unsigned int _levelIdx, int _lNext, int _rNext, unsigned int _lhIdxCount, unsigned int _rhIdxCount, unsigned int _startIdx, unsigned int _endIdx) {
  if (_lNext >= 0) {
    for (unsigned int predIdx = 0; predIdx < nPred; predIdx++) {
      preStage[PairOffset(_lNext, predIdx)].Inherit(bottomNode[PairOffset(_levelIdx, predIdx)]);
    }
  }
  if (_rNext >= 0) {
    for (unsigned int predIdx = 0; predIdx < nPred; predIdx++) {
      preStage[PairOffset(_rNext, predIdx)].Inherit(bottomNode[PairOffset(_levelIdx, predIdx)]);
    }
  }
}


/**
  @brief Finishes setting of map fields.

  @param index caches state information for the predicate bits.

  @param splitPrev is the number of splitable nodes in the previous level.

  @return void.
*/
void Bottom::DeOverlap(const Index *index, unsigned int splitPrev) {
  delete [] bottomNode;
  bottomNode = preStage;
  preStage = 0;
}


SamplePath::SamplePath() : extinct(0), path(0) {
}
