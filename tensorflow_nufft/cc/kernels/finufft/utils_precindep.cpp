/* Copyright 2017-2021 The Simons Foundation. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

  http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// Low-level array manipulations, timer, and OMP helpers, that are precision-
// independent (no FLT allowed in argument lists). Others are in utils.cpp

// For self-test see ../test/testutils.cpp.      Barnett 2017-2020.


#include "tensorflow_nufft/cc/kernels/finufft/utils_precindep.h"
#include "tensorflow_nufft/cc/kernels/finufft/dataTypes.h"
#include "tensorflow_nufft/cc/kernels/finufft/finufft_definitions.h"

BIGINT next235even(BIGINT n)
// finds even integer not less than n, with prime factors no larger than 5
// (ie, "smooth"). Adapted from fortran in hellskitchen.  Barnett 2/9/17
// changed INT64 type 3/28/17. Runtime is around n*1e-11 sec for big n.
{
  if (n<=2) return 2;
  if (n%2 == 1) n+=1;   // even
  BIGINT nplus = n-2;   // to cancel out the +=2 at start of loop
  BIGINT numdiv = 2;    // a dummy that is >1
  while (numdiv>1) {
    nplus += 2;         // stays even
    numdiv = nplus;
    while (numdiv%2 == 0) numdiv /= 2;  // remove all factors of 2,3,5...
    while (numdiv%3 == 0) numdiv /= 3;
    while (numdiv%5 == 0) numdiv /= 5;
  }
  return nplus;
}

// ----------------------- helpers for timing (always stay double prec) ------
using namespace std;

void CNTime::start()
{
  gettimeofday(&initial, 0);
}

double CNTime::restart()
// Barnett changed to returning in sec
{
  double delta = this->elapsedsec();
  this->start();
  return delta;
}

double CNTime::elapsedsec()
// returns answers as double, in seconds, to microsec accuracy. Barnett 5/22/18
{
  struct timeval now;
  gettimeofday(&now, 0);
  double nowsec = (double)now.tv_sec + 1e-6*now.tv_usec;
  double initialsec = (double)initial.tv_sec + 1e-6*initial.tv_usec;
  return nowsec - initialsec;
}


// -------------------------- openmp helpers -------------------------------
int get_num_threads_parallel_block()
// return how many threads an omp parallel block would use.
// omp_get_max_threads() does not report this; consider case of NESTED=0.
// Why is there no such routine?   Barnett 5/22/20
{
  int nth_used;
#pragma omp parallel
  {
#pragma omp single
    nth_used = FINUFFT_GET_NUM_THREADS();
  }
  return nth_used;
}


// ---------- thread-safe rand number generator for Windows platform ---------
// (note this is used by macros in finufft_definitions.h, and supplied in linux/macosx)
#ifdef _WIN32
int rand_r(unsigned int *seedp)
// Libin Lu, 6/18/20
{
    std::random_device rd;
    std::default_random_engine generator(rd());
    std::uniform_int_distribution<int> distribution(0,RAND_MAX);
    return distribution(generator);
}
#endif
