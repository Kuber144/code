/*
 hCRF-light Library 3.0 (full version http://hcrf.sf.net)
 Copyright (C) Yale Song (yalesong@mit.edu)
 
 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <exception>

#include "hcrf/gradient.h"

Gradient::Gradient(InferenceEngine* infEngine, FeatureGenerator* featureGen)
:pInfEngine(infEngine), pFeatureGen(featureGen)
{
  nbThreadsMP = 1;
}

Gradient::Gradient(const Gradient& other)
:pInfEngine(other.pInfEngine), pFeatureGen(other.pFeatureGen)
{
  nbThreadsMP = other.nbThreadsMP;
}

Gradient& Gradient::operator=(const Gradient& other)
{
  pInfEngine = other.pInfEngine;
  pFeatureGen = other.pFeatureGen;
  nbThreadsMP = other.nbThreadsMP;
  return *this;
}

Gradient::~Gradient()
{
  nbThreadsMP = 0;
}

void Gradient::setMaxNumberThreads(int maxThreads)
{
  if (nbThreadsMP < maxThreads)
  {
    nbThreadsMP = maxThreads;
  }
}

void Gradient::viterbiDecoding(Beliefs bel, iVector& ystar, dMatrix& pystar)
{
  int nbNodes, nbStates, xi, yi;
  double max_val;
  
  nbNodes = (int)bel.belStates.size();
  nbStates = 0;
  if(nbNodes > 0)
    nbStates = bel.belStates[0].getLength();
  
  ystar.create(nbNodes);
  pystar.create(nbNodes,nbStates);
  
  // Viterbi decoding
  for( xi=0; xi<nbNodes; xi++) {
    ystar.setValue(xi, 0);
    max_val = bel.belStates[xi][0];
    pystar.setValue(0, xi, bel.belStates[xi][0]);
    for( yi=1; yi<nbStates; yi++) {
      pystar.setValue(yi, xi, bel.belStates[xi][yi]);
      if(max_val < bel.belStates[xi][yi]) {
        ystar.setValue(xi,yi);
        max_val = bel.belStates[xi][yi];
      }
    }
  }
}

double Gradient::computeGradient(dVector& vecGradient, Model* m, DataSet* X)
{
  double ans = 0.0;
  
#ifdef _OPENMP
  if( nbThreadsMP < 1 )
    nbThreadsMP = omp_get_max_threads();
  setMaxNumberThreads(nbThreadsMP);
  pInfEngine->setMaxNumberThreads(nbThreadsMP);
  pFeatureGen->setMaxNumberThreads(nbThreadsMP);
#endif
  
  //Check the size of vecGradient
  int nbFeatures = pFeatureGen->getNumberOfFeatures();
  if(vecGradient.getLength() != nbFeatures)
    vecGradient.create(nbFeatures);
  else
    vecGradient.set(0);
  
  ////////////////////////////////////////////////////////////
  // Start of parallel Region
  // Some weird stuff in gcc 4.1, with openmp 2.5 support
  //
  // Note 1: In OpenMP 2.5, the iteration variable in "for" must be
  // a signed integer variable type. In OpenMP 3.0 (_OPENMP>=200805),
  // it may  also be an unsigned integer variable type, a pointer type,
  // or a constant-time random access iterator type.
  //
  // Note 2: schedule(static | dynamic): In the dynamic schedule, there
  // is no predictable order in which the loop items are assigned to
  // different threads. Each thread asks the OpenMP runtime library for
  // an iteration number, then handles it, then asks for the next one.
  // It is thus useful when different iterations in the loop may take
  // different time to execute.
#pragma omp parallel default(none) \
  shared(vecGradient, X, m, ans, nbFeatures, std::cout)
  {
    // code inside this region runs in parallel
    dVector g(nbFeatures, COLVECTOR, 0.0);
    
#pragma omp for schedule(dynamic) reduction(+:ans)
    for(int i=0; (int)i<X->size(); i++) {
      DataSequence* x = X->at(i);
      if( m->isWeightSequence() && x->getWeightSequence() != 1.0) {
        dVector tmp(nbFeatures, COLVECTOR, 0.0);
        ans += computeGradient(tmp, m, x) * x->getWeightSequence();
        tmp.multiply(x->getWeightSequence());
        g.add(tmp);
      }
      else {
        ans += computeGradient(g, m, x);
      }
    }
    
    // We now put togheter the gradients
    // No two threads can execute a critical directive of the same name at the same time
#pragma omp critical (reduce_sum)
    {
      vecGradient.add(g);
    }
  }
  // End of parallel Region
  ////////////////////////////////////////////////////////////
  vecGradient.negate();
  
  // MaxMargin objective: min L = 0.5*\L2sigma*W*W + Loss()
  // MLE objective: min L = 0.5*1/(\L2sigma*\L2sigma)*W*W - log p(y|x)
  
  // Add the regularization term
  double scale = (m->isMaxMargin())
		? m->getRegL2Sigma()
		: 1/(double)(m->getRegL2Sigma()*m->getRegL2Sigma());
  
  if( m->isMaxMargin() )
    ans = (1/(double)X->size()) * ans;
  
  if(m->getRegL2Sigma()!=0.0f)
  {
    for(int f=0; f<nbFeatures; f++)
      vecGradient[f] += (*m->getWeights())[f]*scale;
    ans += 0.5*scale*m->getWeights()->l2Norm(false);
  }
  
  return ans;
}


