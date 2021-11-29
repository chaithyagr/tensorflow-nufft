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

// Switchable-precision interface template for cufinufft. Used by cufinufft.h
// Internal use only: users should link to cufinufft.h

#if (!defined(__CUFINUFFT_H__) && !defined(SINGLE)) || \
  (!defined(__CUFINUFFTF_H__) && defined(SINGLE))
// (note we entered one level of conditional until the end of this header)
// Make sure we don't include double or single headers more than once...

#include <cstdlib>
#include <cufft.h>
#include <assert.h>
#include <cuda_runtime.h>
#include "tensorflow_nufft/cc/kernels/finufft/gpu/precision_independent.h"
#include "cufinufft_errors.h"

#include "tensorflow_nufft/cc/kernels/finufft/gpu/contrib/utils.h"
#include "tensorflow_nufft/cc/kernels/finufft/gpu/contrib/dataTypes.h"
#include "tensorflow_nufft/cc/kernels/finufft/gpu/contrib/spreadinterp.h"
#include "tensorflow_nufft/cc/kernels/finufft/gpu/contrib/utils_fp.h"
#include "tensorflow_nufft/cc/kernels/nufft_options.h"
#include "tensorflow_nufft/cc/kernels/nufft_plan.h"


#ifndef SINGLE
#define __CUFINUFFT_H__
#else
#define __CUFINUFFTF_H__
#endif


/* Undefine things so we don't get warnings/errors later */
#undef CUFINUFFT_MAKEPLAN
#undef CUFINUFFT_SETPTS
#undef CUFINUFFT_EXECUTE
#undef CUFINUFFT_INTERP
#undef CUFINUFFT_SPREAD
#undef CUFINUFFT_DESTROY
#undef CUFINUFFT2D1_EXEC
#undef CUFINUFFT2D2_EXEC
#undef CUFINUFFT3D1_EXEC
#undef CUFINUFFT3D2_EXEC
#undef CUFINUFFT2D_INTERP
#undef CUFINUFFT2D_SPREAD
#undef CUFINUFFT3D_INTERP
#undef CUFINUFFT3D_SPREAD
#undef SETUP_BINSIZE
/* memtransfer.h */
#undef ALLOCGPUMEM1D_PLAN
#undef ALLOCGPUMEM1D_NUPTS
#undef FREEGPUMEMORY1D
#undef ALLOCGPUMEM2D_PLAN
#undef ALLOCGPUMEM2D_NUPTS
#undef FREEGPUMEMORY2D
#undef ALLOCGPUMEM3D_PLAN
#undef ALLOCGPUMEM3D_NUPTS
#undef FREEGPUMEMORY3D
/* spreading and interp only*/
#undef CUFINUFFT_SPREAD2D
#undef CUFINUFFT_SPREAD3D
#undef CUFINUFFT_INTERP2D
#undef CUFINUFFT_INTERP3D
/* spreading 2D */
#undef CUSPREAD2D
#undef CUSPREAD2D_NUPTSDRIVEN_PROP
#undef CUSPREAD2D_NUPTSDRIVEN
#undef CUSPREAD2D_SUBPROB_PROP
#undef CUSPREAD2D_SUBPROB
#undef CUSPREAD2D_PAUL
#undef CUSPREAD2D_PAUL_PROP
/* spreading 3d */
#undef CUSPREAD3D
#undef CUSPREAD3D_NUPTSDRIVEN_PROP
#undef CUSPREAD3D_NUPTSDRIVEN
#undef CUSPREAD3D_BLOCKGATHER_PROP
#undef CUSPREAD3D_BLOCKGATHER
#undef CUSPREAD3D_SUBPROB_PROP
#undef CUSPREAD3D_SUBPROB
/* interp */
#undef CUINTERP2D
#undef CUINTERP3D
#undef CUINTERP2D_NUPTSDRIVEN
#undef CUINTERP2D_SUBPROB
#undef CUINTERP3D_NUPTSDRIVEN
#undef CUINTERP3D_SUBPROB
/* deconvolve */
#undef CUDECONVOLVE2D
#undef CUDECONVOLVE3D
/* structs */


#ifdef SINGLE

#define CUFINUFFT_MAKEPLAN cufinufftf_makeplan
#define CUFINUFFT_SETPTS cufinufftf_setpts
#define CUFINUFFT_EXECUTE cufinufftf_execute
#define CUFINUFFT_INTERP cufinufftf_interp
#define CUFINUFFT_SPREAD cufinufftf_spread
#define CUFINUFFT_DESTROY cufinufftf_destroy
#define CUFINUFFT2D1_EXEC cufinufftf2d1_exec
#define CUFINUFFT2D2_EXEC cufinufftf2d2_exec
#define CUFINUFFT3D1_EXEC cufinufftf3d1_exec
#define CUFINUFFT3D2_EXEC cufinufftf3d2_exec
#define CUFINUFFT2D_INTERP cufinufftf2d_interp
#define SETUP_BINSIZE setup_binsizef
/* memtransfer.h */
#define ALLOCGPUMEM1D_PLAN allocgpumem1df_plan
#define ALLOCGPUMEM1D_NUPTS allocgpumem1df_nupts
#define FREEGPUMEMORY1D freegpumemory1df
#define ALLOCGPUMEM2D_PLAN allocgpumem2df_plan
#define ALLOCGPUMEM2D_NUPTS allocgpumem2df_nupts
#define FREEGPUMEMORY2D freegpumemory2df
#define ALLOCGPUMEM3D_PLAN allocgpumem3df_plan
#define ALLOCGPUMEM3D_NUPTS allocgpumem3df_nupts
#define FREEGPUMEMORY3D freegpumemory3df
/* spreading and interp only*/
#define CUFINUFFT_SPREAD2D cufinufft_spread2df
#define CUFINUFFT_SPREAD3D cufinufft_spread3df
#define CUFINUFFT_INTERP2D cufinufft_interp2df
#define CUFINUFFT_INTERP3D cufinufft_interp3df
/* spreading 2D */
#define CUSPREAD2D cuspread2df
#define CUSPREAD2D_NUPTSDRIVEN_PROP cuspread2df_nuptsdriven_prop
#define CUSPREAD2D_NUPTSDRIVEN cuspread2df_nuptsdriven
#define CUSPREAD2D_SUBPROB_PROP cuspread2df_subprob_prop
#define CUSPREAD2D_SUBPROB cuspread2df_subprob
#define CUSPREAD2D_PAUL cuspread2df_paul
#define CUSPREAD2D_PAUL_PROP cuspread2df_paul_prop
/* spreading 3d */
#define CUSPREAD3D cuspread3df
#define CUSPREAD3D_NUPTSDRIVEN_PROP cuspread3df_nuptsdriven_prop
#define CUSPREAD3D_NUPTSDRIVEN cuspread3df_nuptsdriven
#define CUSPREAD3D_BLOCKGATHER_PROP cuspread3df_blockgather_prop
#define CUSPREAD3D_BLOCKGATHER cuspread3df_blockgather
#define CUSPREAD3D_SUBPROB_PROP cuspread3df_subprob_prop
#define CUSPREAD3D_SUBPROB cuspread3df_subprob
/* interp */
#define CUINTERP2D cuinterp2df
#define CUINTERP3D cuinterp3df
#define CUINTERP2D_NUPTSDRIVEN cuinterp2df_nuptsdriven
#define CUINTERP2D_SUBPROB cuinterp2df_subprob
#define CUINTERP3D_NUPTSDRIVEN cuinterp3df_nuptsdriven
#define CUINTERP3D_SUBPROB cuinterp3df_subprob
/* deconvolve */
#define CUDECONVOLVE2D cudeconvolve2df
#define CUDECONVOLVE3D cudeconvolve3df
/* structs */

#else

#define CUFINUFFT_MAKEPLAN cufinufft_makeplan
#define CUFINUFFT_SETPTS cufinufft_setpts
#define CUFINUFFT_EXECUTE cufinufft_execute
#define CUFINUFFT_INTERP cufinufft_interp
#define CUFINUFFT_SPREAD cufinufft_spread
#define CUFINUFFT_DESTROY cufinufft_destroy
#define CUFINUFFT2D1_EXEC cufinufft2d1_exec
#define CUFINUFFT2D2_EXEC cufinufft2d2_exec
#define CUFINUFFT3D1_EXEC cufinufft3d1_exec
#define CUFINUFFT3D2_EXEC cufinufft3d2_exec
#define CUFINUFFT2D_INTERP cufinufft2d_interp
#define CUFINUFFT2D_SPREAD cufinufft2d_spread
#define CUFINUFFT3D_INTERP cufinufft3d_interp
#define CUFINUFFT3D_SPREAD cufinufft3d_spread
#define SETUP_BINSIZE setup_binsize
/* memtransfer.h */
#define ALLOCGPUMEM1D_PLAN allocgpumem1d_plan
#define ALLOCGPUMEM1D_NUPTS allocgpumem1d_nupts
#define FREEGPUMEMORY1D freegpumemory1d
#define ALLOCGPUMEM2D_PLAN allocgpumem2d_plan
#define ALLOCGPUMEM2D_NUPTS allocgpumem2d_nupts
#define FREEGPUMEMORY2D freegpumemory2d
#define ALLOCGPUMEM3D_PLAN allocgpumem3d_plan
#define ALLOCGPUMEM3D_NUPTS allocgpumem3d_nupts
#define FREEGPUMEMORY3D freegpumemory3d
/* spreading and interp only*/
#define CUFINUFFT_SPREAD2D cufinufft_spread2d
#define CUFINUFFT_SPREAD3D cufinufft_spread3d
#define CUFINUFFT_INTERP2D cufinufft_interp2d
#define CUFINUFFT_INTERP3D cufinufft_interp3d
/* spreading 2D */
#define CUSPREAD2D cuspread2d
#define CUSPREAD2D_NUPTSDRIVEN_PROP cuspread2d_nuptsdriven_prop
#define CUSPREAD2D_NUPTSDRIVEN cuspread2d_nuptsdriven
#define CUSPREAD2D_SUBPROB_PROP cuspread2d_subprob_prop
#define CUSPREAD2D_SUBPROB cuspread2d_subprob
#define CUSPREAD2D_PAUL cuspread2d_paul
#define CUSPREAD2D_PAUL_PROP cuspread2d_paul_prop
/* spreading 3d */
#define CUSPREAD3D cuspread3d
#define CUSPREAD3D_NUPTSDRIVEN_PROP cuspread3d_nuptsdriven_prop
#define CUSPREAD3D_NUPTSDRIVEN cuspread3d_nuptsdriven
#define CUSPREAD3D_BLOCKGATHER_PROP cuspread3d_blockgather_prop
#define CUSPREAD3D_BLOCKGATHER cuspread3d_blockgather
#define CUSPREAD3D_SUBPROB_PROP cuspread3d_subprob_prop
#define CUSPREAD3D_SUBPROB cuspread3d_subprob
/* interp */
#define CUINTERP2D cuinterp2d
#define CUINTERP3D cuinterp3d
#define CUINTERP2D_NUPTSDRIVEN cuinterp2d_nuptsdriven
#define CUINTERP2D_SUBPROB cuinterp2d_subprob
#define CUINTERP3D_NUPTSDRIVEN cuinterp3d_nuptsdriven
#define CUINTERP3D_SUBPROB cuinterp3d_subprob
/* deconvolve */
#define CUDECONVOLVE2D cudeconvolve2d
#define CUDECONVOLVE3D cudeconvolve3d


#endif

/* We include common.h here because it depends on SpreadOptions<FLT> and
   tensorflow::nufft::Plan<tensorflow::GPUDevice, FLT> structs being completely defined first. */
#include "tensorflow_nufft/cc/kernels/finufft/gpu/contrib/common.h"

#define checkCufftErrors(call)

#ifdef __cplusplus
extern "C" {
#endif
int CUFINUFFT_MAKEPLAN(int type, int dim, int *n_modes, int iflag,
		       int ntransf, FLT tol, int maxbatchsize,
		       tensorflow::nufft::Plan<tensorflow::GPUDevice, FLT>* *d_plan_ptr,
			   const tensorflow::nufft::Options& options);
int CUFINUFFT_SETPTS(int M, FLT* h_kx, FLT* h_ky, FLT* h_kz, int N, FLT *h_s,
	FLT *h_t, FLT *h_u, tensorflow::nufft::Plan<tensorflow::GPUDevice, FLT>* d_plan);
int CUFINUFFT_EXECUTE(CUCPX* h_c, CUCPX* h_fk, tensorflow::nufft::Plan<tensorflow::GPUDevice, FLT>* d_plan);
int CUFINUFFT_INTERP(CUCPX* h_c, CUCPX* h_fk, tensorflow::nufft::Plan<tensorflow::GPUDevice, FLT>* d_plan);
int CUFINUFFT_SPREAD(CUCPX* h_c, CUCPX* h_fk, tensorflow::nufft::Plan<tensorflow::GPUDevice, FLT>* d_plan);
int CUFINUFFT_DESTROY(tensorflow::nufft::Plan<tensorflow::GPUDevice, FLT>* d_plan);
#ifdef __cplusplus
}
#endif


// 2d
int CUFINUFFT2D1_EXEC(CUCPX* d_c, CUCPX* d_fk, tensorflow::nufft::Plan<tensorflow::GPUDevice, FLT>* d_plan);
int CUFINUFFT2D2_EXEC(CUCPX* d_c, CUCPX* d_fk, tensorflow::nufft::Plan<tensorflow::GPUDevice, FLT>* d_plan);

// 3d
int CUFINUFFT3D1_EXEC(CUCPX* d_c, CUCPX* d_fk, tensorflow::nufft::Plan<tensorflow::GPUDevice, FLT>* d_plan);
int CUFINUFFT3D2_EXEC(CUCPX* d_c, CUCPX* d_fk, tensorflow::nufft::Plan<tensorflow::GPUDevice, FLT>* d_plan);

// 2d
int CUFINUFFT2D_INTERP(CUCPX* d_c, CUCPX* d_fk, tensorflow::nufft::Plan<tensorflow::GPUDevice, FLT>* d_plan);
int CUFINUFFT2D_SPREAD(CUCPX* d_c, CUCPX* d_fk, tensorflow::nufft::Plan<tensorflow::GPUDevice, FLT>* d_plan);

// 3d
int CUFINUFFT3D_INTERP(CUCPX* d_c, CUCPX* d_fk, tensorflow::nufft::Plan<tensorflow::GPUDevice, FLT>* d_plan);
int CUFINUFFT3D_SPREAD(CUCPX* d_c, CUCPX* d_fk, tensorflow::nufft::Plan<tensorflow::GPUDevice, FLT>* d_plan);

#endif
