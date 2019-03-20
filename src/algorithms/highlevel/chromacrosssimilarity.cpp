/*
 * Copyright (C) 2006-2016  Music Technology Group - Universitat Pompeu Fabra
 *
 * This file is part of Essentia
 *
 * Essentia is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License as published by the Free
 * Software Foundation (FSF), either version 3 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the Affero GNU General Public License
 * version 3 along with this program.  If not, see http://www.gnu.org/licenses/
 */
#include "chromacrosssimilarity.h"
#include "essentia/utils/tnt/tnt2vector.h"
#include "essentiamath.h"
#include <vector>
#include <iostream>
#include <string>
#include <algorithm>
#include <functional>

using namespace essentia;

std::vector<Real> globalAverageChroma(std::vector<std::vector<Real> >& inputFeature);
int optimalTranspositionIndex(std::vector<std::vector<Real> >& chromaA, std::vector<std::vector<Real> >& chromaB, int nshifts);
std::vector<std::vector<Real> > stackChromaFrames(std::vector<std::vector<Real> >& frames, int frameStackSize, int frameStackStride);
std::vector<std::vector<Real> > chromaBinarySimMatrix(std::vector<std::vector<Real> >& chromaA, std::vector<std::vector<Real> >& chromaB, int nshifts, Real matchCoef, Real mismatchCoef);

namespace essentia {
namespace standard {

const char* ChromaCrossSimilarity::name = "ChromaCrossSimilarity";
const char* ChromaCrossSimilarity::category = "Music Similarity";
const char* ChromaCrossSimilarity::description = DOC("This algorithm computes a binary cross similarity matrix from two chromagam feature vectors of a query and reference song.\n\n"
"Use HPCP algorithm for computing the chromagram and the default parameters of this algorithm for the best results.\n\n"
"In addition, the algorithm also provides an option to use another binary cross-similarity computation method using optimal transposition index (OTI) of chroma features as mentioned in [3].\n\n"
"Use default parameter values for best results.\n\n"
"The input chromagram should be in the shape (x, numbins), where 'x' is number of frames and 'numbins' for the number of bins in the chromagram. An exception is thrown otherwise.\n\n"
"An exception is also thrown if either one of the input chromagrams are empty or if the cross-similarity matrix is empty.\n\n"
"NOTE: Streaming mode of this algorithm outputs the same as the output of standard mode 'ChromaCrossSimilarity' with parameter 'optimizeThreshold=True'\n\n"
"References:\n"
"[1] Serra, J., Gómez, E., & Herrera, P. (2008). Transposing chroma representations to a common key, IEEE Conference on The Use of Symbols to Represent Music and Multimedia Objects.\n\n"
"[2] Serra, J., Serra, X., & Andrzejak, R. G. (2009). Cross recurrence quantification for cover song identification.New Journal of Physics.\n\n"
"[3] Serra, Joan, et al. Chroma binary similarity and local alignment applied to cover song identification. IEEE Transactions on Audio, Speech, and Language Processing 16.6 (2008).\n");


void ChromaCrossSimilarity::configure() {
  // configure parameters
  _frameStackStride = parameter("frameStackStride").toInt();
  _frameStackSize = parameter("frameStackSize").toInt();
  _kappa = parameter("kappa").toReal();
  _noti = parameter("noti").toInt();
  _oti = parameter("oti").toBool();
  _otiBinary = parameter("otiBinary").toBool();
  _optimizeThreshold = parameter("optimizeThreshold").toBool();
  _mathcCoef = 1; // for chroma binary sim-matrix based on OTI similarity as in [3]. 
  _mismatchCoef = 0; // for chroma binary sim-matrix based on OTI similarity as in [3]. 
}

void ChromaCrossSimilarity::compute() {
  // get inputs and output
  std::vector<std::vector<Real> > queryFeature = _queryFeature.get();
  std::vector<std::vector<Real> > referenceFeature = _referenceFeature.get();
  std::vector<std::vector<Real> >& csm = _csm.get();

  if (queryFeature.empty())
    throw EssentiaException("CrossSimilarityMatrix: input queryFeature is empty.");

  if (referenceFeature.empty())
    throw EssentiaException("CrossSimilarityMatrix: input referenceFeature is empty.");

  // check whether to use oti-based binary similarity 
  if (_otiBinary == true) {
    std::vector<std::vector<Real> >  stackFramesA = stackChromaFrames(queryFeature, _frameStackSize, _frameStackStride);
    std::vector<std::vector<Real> >  stackFramesB = stackChromaFrames(referenceFeature, _frameStackSize, _frameStackStride);
    csm = chromaBinarySimMatrix(stackFramesA, stackFramesB, _noti, _mathcCoef, _mismatchCoef);;
  }
  // Otherwise use default cross-similarity computation method based on euclidean distances
  else {
    // check whether to transpose by oti
    if (_oti == true) {
      int otiIdx = optimalTranspositionIndex(queryFeature, referenceFeature, _noti);
      rotateChroma(referenceFeature, otiIdx);
      std::cout << "OTI: " << otiIdx << std::endl;
    }
    // construct stacked chroma feature matrices from specified 'frameStackSize' and 'frameStackStride'
    std::vector<std::vector<Real> >  queryFeatureStack = stackChromaFrames(queryFeature, _frameStackSize, _frameStackStride);
    std::vector<std::vector<Real> >  referenceFeatureStack= stackChromaFrames(referenceFeature, _frameStackSize, _frameStackStride);
    // pairwise euclidean distance
    std::vector<std::vector<Real> > pdistances = pairwiseDistance(queryFeatureStack, referenceFeatureStack);
    // transposing the array of pairwise distance
    std::vector<std::vector<Real> > tpDistances = transpose(pdistances);
    size_t queryRows = pdistances.size();
    size_t referenceRows = pdistances[0].size();
 
    std::vector<std::vector<Real> > outputSimMatrix;
    std::vector<Real> thresholdX(queryRows);
    std::vector<Real> thresholdY(referenceRows);

    if (_optimizeThreshold == true) {
      // optimise the threshold computation by iniatilizing it to a matrix of ones
      outputSimMatrix.assign(queryRows, std::vector<Real>(referenceRows, 1));
    }
    else if (_optimizeThreshold == false) {
      outputSimMatrix.assign(queryRows, std::vector<Real>(referenceRows));
      // construct the binary output similarity matrix using the thresholds computed along the queryFeature axis
      for (size_t k=0; k<queryRows; k++) {
        thresholdX[k] = percentile(pdistances[k], _kappa*100);
        for (size_t l=0; l<referenceRows; l++) {
          if ((thresholdX[k] - pdistances[k][l]) > 0) {
            outputSimMatrix[k][l] = 1;
          }
          else if ((thresholdX[k] - pdistances[k][l]) <= 0) {
            outputSimMatrix[k][l] = 0.;
          }
        }
      }
    }
    // update the binary output similarity matrix by multiplying with the thresholds computed along the referenceFeature axis
    for (size_t j=0; j<referenceRows; j++) {
      thresholdY[j] = percentile(tpDistances[j], _kappa*100);
      for (size_t i=0; i<queryRows; i++) {
        if ((thresholdY[j] - pdistances[i][j]) > 0) {
          outputSimMatrix[i][j] *= 1; 
        }
        else if ((thresholdY[j] - pdistances[i][j]) <= 0) {
          outputSimMatrix[i][j] *= 0;
        }
      }
    }
    csm = outputSimMatrix;
  }
}

} // namespace standard
} // namespace essentia


#include "algorithmfactory.h"

namespace essentia {
namespace streaming {

const char* ChromaCrossSimilarity::name = standard::ChromaCrossSimilarity::name;
const char* ChromaCrossSimilarity::description = standard::ChromaCrossSimilarity::description;

void ChromaCrossSimilarity::configure() {
  // configure parameters
  _referenceFeature = parameter("referenceFeature").toVectorVectorReal();
  _frameStackStride = parameter("frameStackStride").toInt();
  _frameStackSize = parameter("frameStackSize").toInt();
  _kappa = parameter("kappa").toReal();
  _noti = parameter("noti").toInt();
  _oti = parameter("oti").toInt();
  _otiBinary = parameter("otiBinary").toBool();
  _mathcCoef = 1; // for chroma binary sim-matrix based on OTI similarity as in [3]. 
  _mismatchCoef = 0; // for chroma binary sim-matrix based on OTI similarity as in [3]. 
  _referenceFeatureStack = stackChromaFrames(_referenceFeature, _frameStackSize, _frameStackStride);
  _minFramesSize = _frameStackSize + 1; // min amount of frames needed to construct an frame of stacked vector
  
  input("queryFeature").setAcquireSize(_minFramesSize);
  input("queryFeature").setReleaseSize(1);

  output("csm").setAcquireSize(1);
  output("csm").setReleaseSize(1);
}

AlgorithmStatus ChromaCrossSimilarity::process() {
 
  EXEC_DEBUG("process()");
  AlgorithmStatus status = acquireData();
  EXEC_DEBUG("data acquired (in: " << _queryFeature.acquireSize()
             << " - out: " << _csm.acquireSize() << ")");

  if (status != OK) {
    if (!shouldStop()) return status;
    // if shouldStop is true, that means there is no more audio coming, so we need
    // to take what's left to fill in half-frames, instead of waiting for more
    // data to come in (which would have done by returning from this function)
    int available = input("queryFeature").available();
    if (available == 0) return NO_INPUT;

    input("queryFeature").setAcquireSize(available);
    input("queryFeature").setReleaseSize(available);

    return process();
  }

  const std::vector<std::vector<Real> >& inputQueryFrames = _queryFeature.tokens();
  std::vector<std::vector<Real> > inputFramesCopy = inputQueryFrames; 
  std::vector<std::vector<Real> >& csmOutput = _csm.tokens();
  _outputSimMatrix.clear();

  /* if we have less input frame streams than the required 'frameStackSize' in the last stream, 
   we append the already acquired frames of the current stream until it satisfies the 'frameStackSize' dimension */
  if (input("queryFeature").acquireSize() < _minFramesSize) {
    for (int i=0; i<(_minFramesSize - input("queryFeature").acquireSize()); i++) {
      inputFramesCopy.push_back(inputQueryFrames[i]);
    }
  }
  std::vector<std::vector<Real> > queryFeatureStack = stackChromaFrames(inputFramesCopy, _frameStackSize, _frameStackStride);
  // check whether to use oti-based binary similarity as mentioned in [3]
  if (_otiBinary == true) {
    _outputSimMatrix = chromaBinarySimMatrix(queryFeatureStack, _referenceFeatureStack, _noti, _mathcCoef, _mismatchCoef);
    csmOutput[0] = _outputSimMatrix[0];
    releaseData();
  }
  // otherwise we compute similarity matrix as mentioned in [2]
  else if (_otiBinary == false) {
    // here we compute the pairwsie euclidean distances between query and reference song time embedding and finally tranpose the resulting matrix.
    std::vector<std::vector<Real> > pdistances = pairwiseDistance(queryFeatureStack, _referenceFeatureStack);
    std::vector<std::vector<Real> > tpDistances = transpose(pdistances);
    size_t queryRows = pdistances.size();
    size_t referenceRows = pdistances[0].size();

    // optimise the threshold computation by iniatilizing it to a matrix of ones
    _outputSimMatrix.assign(queryRows, std::vector<Real>(referenceRows, 1));
    
    std::vector<Real> thresholdY(referenceRows);
    // update the binary output similarity matrix by multiplying with the thresholds computed along the referenceFeature axis
    for (size_t j=0; j<referenceRows; j++) {
      thresholdY[j] = percentile(tpDistances[j], _kappa*100);
      for (size_t i=0; i<queryRows; i++) {
        if (pdistances[i][j] > thresholdY[j]) {
          _outputSimMatrix[i][j] *= 1; 
        }
        else if (thresholdY[j] >= pdistances[i][j]) {
          _outputSimMatrix[i][j] *= 0;
        }
      }
    }
    csmOutput[0] = _outputSimMatrix[0];
    releaseData();
  }
  return OK;
}

} // namespace streaming
} // namespace essentia


// computes global averaged chroma as described in [1]
std::vector<Real> globalAverageChroma(std::vector<std::vector<Real> >& inputFeature) {

  std::vector<Real> globalChroma = sumFrames(inputFeature);
  // divide the sum array by the max element to normalise it to 0-1 range
  normalize(globalChroma);
  return globalChroma;
}


// Compute the optimal transposition index for transposing reference song feature to the musical key of query song feature as described in [1].
int optimalTranspositionIndex(std::vector<std::vector<Real> >& chromaA, std::vector<std::vector<Real> >& chromaB, int nshifts) {
    
  std::vector<Real> globalChromaA = globalAverageChroma(chromaA);
  std::vector<Real> globalChromaB = globalAverageChroma(chromaB);
  std::vector<Real> valueAtShifts;
  int iterIdx = 0;
  for(int i=0; i<=nshifts; i++) {
    // circular rotate the input globalchroma by an index 'i'
    std::rotate(globalChromaB.begin(), globalChromaB.end() - (i - iterIdx), globalChromaB.end());
    // compute the dot product of the query global chroma and the shifted global chroma of reference song and append to an array
    valueAtShifts.push_back(dotProduct(globalChromaA, globalChromaB));
    if (i >= 1) iterIdx++;
  }
  // compute the optimal index by finding the index of maximum element in the array of value at various shifts
  return argmax(valueAtShifts);
}


// Construct a 'stacked-frames' feature vector from an input audio feature vector by given 'frameStackSize' and 'frameStackStride'
std::vector<std::vector<Real> > stackChromaFrames(std::vector<std::vector<Real> >& frames, int frameStackSize, int frameStackStride) {

  if (frameStackSize == 1) {
    return frames;
  }
  size_t stopIdx;
  int increment = frameStackSize * frameStackStride;
  std::vector<std::vector<Real> > stackedFrames;
  stackedFrames.reserve(frames.size() - increment);
  std::vector<Real> stack;
  for (size_t i=0; i<(frames.size() - increment); i+=frameStackStride) {
    stopIdx = i + increment;
    for (size_t startTime=i; startTime<stopIdx; startTime+=frameStackStride) {
      stack.insert(stack.end(), frames[startTime].begin(), frames[startTime].end());
    }
    stackedFrames.push_back(stack);
    stack.clear();
  }
  return stackedFrames;
}


// Computes a binary similarity matrix from two chroma vector inputs using OTI as described in [3]
std::vector<std::vector<Real> > chromaBinarySimMatrix(std::vector<std::vector<Real> >& chromaA, std::vector<std::vector<Real> >& chromaB, int nshifts, Real matchCoef, Real mismatchCoef) {

  int otiIndex;
  std::vector<Real> valueAtShifts;
  std::vector<Real> chromaBcopy;
  std::vector<std::vector<Real> > simMatrix(chromaA.size(), std::vector<Real>(chromaB.size()));
  for (size_t i=0; i<chromaA.size(); i++) {
    for (size_t j=0; j<chromaB.size(); j++) {
      // compute OTI-based similarity for each frame of chromaA and chromaB
      for(int k=0; k<=nshifts; k++) {
        chromaBcopy = chromaB[j];
        std::rotate(chromaBcopy.begin(), chromaBcopy.end() - k, chromaBcopy.end());
        valueAtShifts.push_back(dotProduct(chromaA[i], chromaBcopy));
      }
      otiIndex = argmax(valueAtShifts);
      valueAtShifts.clear();
      // assign matchCoef to similarity matrix if the OTI is 0 or 1 semitone
      if (otiIndex == 0 || otiIndex == 1) {
        simMatrix[i][j] = matchCoef;
      }
      else {
        simMatrix[i][j] = mismatchCoef;
      }
    }
  }
  return simMatrix;
};

