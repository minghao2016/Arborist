// This file is part of ArboristCore.

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

/**
   @file splitsig.h

   @brief Class definitions for split signatures, which transmit splitting results to the index-tree splitting methods.

   @author Mark Seligman
 */

#ifndef ARBORIST_SPLITSIG_H
#define ARBORIST_SPLITSIG_H


/**
   @brief Holds the information actually computed by a splitting method.
 */
class NuxLH {
  double info; // Information content of split.
  unsigned int idxStart; // Not derivable from index node alone.
  unsigned int lhExtent; // Index count of split LHS.
  unsigned int sCount; // # samples subsumed by split LHS.
  unsigned int rankLH; // Numeric only.
  unsigned int rankRH; // Numeric only.
  unsigned int lhImplicit; // Numeric only.
 public:

  void inline Init(unsigned int _idxStart, unsigned int _lhExtent, unsigned int _sCount, double _info) {
    idxStart = _idxStart;
    lhExtent = _lhExtent;
    sCount = _sCount;
    info = _info;
    rankLH = rankRH = 0; // TODO:  Default should be 'noRank'.
  }

  
  /**
     @brief With introduction of dense ranks, splitting ranks can no longer be
     inferred by position alone so are passed explicitly.
  */
  void inline InitNum(unsigned int _idxStart, unsigned int _lhExtent, unsigned int _sCount, double _info, unsigned int _rankLH, unsigned int _rankRH, unsigned int _lhImplicit = 0) {
    Init(_idxStart, _lhExtent, _sCount, _info);
    rankLH = _rankLH;
    rankRH = _rankRH;
    lhImplicit = _lhImplicit;
  }

  
  void Ref(unsigned int &_idxStart, unsigned int &_lhExtent, unsigned int &_sCount, double &_info, double &_rankMean, unsigned int &_lhImplicit) const {
    _idxStart = idxStart;
    _lhExtent = lhExtent;
    _sCount = sCount;
    _info = info;
    _rankMean = 0.5 * (double(rankLH) + double(rankRH));
    _lhImplicit = lhImplicit;
  }
};


/**
   @brief SSNode records sample, index and information content for a
   potential split at a given split/predictor pair.

 */
class SSNode {
  bool leftExpl;  // Whether LH or RH split indices explicit.

  double NonTerminalRun(class Bottom *bottom, class PreTree *preTree, class Run *run, unsigned int extent, double sum, unsigned int ptId);
  double ReplayRun(class Bottom *bottom, class PreTree *preTree, double sum, unsigned int ptId, const class Run *run);
  double NonTerminalNum(class Bottom *bottom, class PreTree *preTree, unsigned int extent, double sum, unsigned int ptId);
  double ReplayNum(class Bottom *bottom, double sum, unsigned int idxCount);

 public:
  SSNode();
  unsigned int setIdx; // Index into RunSet workspace.
  unsigned int predIdx; // Rederivable, but convenient to cache.
  unsigned int sCount; // # samples subsumed by split LHS.
  unsigned int idxStart; // Dense packing causes value to vary.
  unsigned int lhExtent; // Index count of split LHS.
  double info; // Information content of split.
  double rankMean; // Numeric only.
  unsigned int lhImplicit; // LHS implicit index count:  numeric only.
  unsigned char bufIdx;
  
  static double minRatio;
  
  // Ideally, there would be SplitSigFac and SplitSigNum subclasses, with
  // Replay() and NonTerminal() methods implemented virtually.  Coprocessor
  // may not support virtual invocation, however, so we opt for a less
  // elegant solution.


  inline bool LeftExpl() const {
    return leftExpl;
  }

  
  /**
   @brief Derives an information threshold.

   @return information threshold
  */
  double inline MinInfo() {
    return minRatio * info;
  }

  
  /**
     @brief Accessor for bipartitioning.

     @param _sCount outputs the number of samples in LHS.

     @param _lhExtent outputs the number of indices in LHS.

     @return void, with output reference parameters.
   */  
  void inline LHSizes(unsigned int &_sCount, unsigned int &_lhExtent) const {
    _sCount = sCount;
    _lhExtent = lhExtent;
  }

  double NonTerminal(class Bottom *bottom, class PreTree *preTree, class Run *run, unsigned int extent, double sum, unsigned int ptId);
};


/**
  @brief SplitSigs manage the SSNodes for a given level instantation.
*/
class SplitSig {
  const unsigned int nPred;
  unsigned int splitCount;
  SSNode *levelSS; // Workspace records for the current level.

  
  /**
     @brief Looks up the SplitSig associated with a given pair.

     SplitSigs are stored with split number as the fastest-varying
     index.  The likelihood of false sharing during splitting is
     fairly low, given that predictor selection is probabalistic
     and splitting workloads themselves are nonuniform.  Nonetheless,
     predictor-specific references are kept fairly far apart.

     @param splitIdx is the split index.

     @param predIdx is the predictor index.

     @return pointer to looked-up SplitSig.
   */
  inline SSNode &Lookup(unsigned int splitIdx, unsigned int predIdx = 0) {
    return levelSS[predIdx * splitCount + splitIdx];
  }


 public:
 SplitSig(unsigned int _nPred) : nPred(_nPred), splitCount(0), levelSS(0) {
  }

  static void Immutables(double _minRatio);
  static void DeImmutables();

  SSNode *ArgMax(unsigned int levelIdx, double gainMax) const;
  void LevelInit(unsigned int _splitCount);
  void LevelClear();
  void Write(unsigned int _splitIdx, unsigned int _predIdx, unsigned int _setPos, unsigned int _bufIdx, const NuxLH &nux);
};

#endif
