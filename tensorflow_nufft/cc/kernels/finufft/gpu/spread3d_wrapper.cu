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

#include <tensorflow_nufft/third_party/cuda_samples/helper_cuda.h>
#include <iostream>
#include <iomanip>
#include <assert.h>

#include <thrust/device_ptr.h>
#include <thrust/scan.h>

#include <cuComplex.h>
#include "tensorflow_nufft/cc/kernels/finufft/gpu/cuspreadinterp.h"
#include "tensorflow_nufft/cc/kernels/finufft/gpu/memtransfer.h"
#include "tensorflow_nufft/cc/kernels/finufft/gpu/precision_independent.h"

using namespace std;
using namespace tensorflow;
using namespace tensorflow::nufft;


int CUSPREAD3D(Plan<GPUDevice, FLT>* d_plan, int blksize)
/*
  A wrapper for different spreading methods. 

  Methods available:
  (1) Non-uniform points driven
  (2) Subproblem
  (4) Block gather

  Melody Shih 07/25/19
*/
{
  int nf1 = d_plan->nf1;
  int nf2 = d_plan->nf2;
  int nf3 = d_plan->nf3;
  int M = d_plan->M;

  cudaEvent_t start, stop;
  cudaEventCreate(&start);
  cudaEventCreate(&stop);

  int ier = 0;
  switch(d_plan->options_.spread_method)
  {
    case SpreadMethod::NUPTS_DRIVEN:
      {
        cudaEventRecord(start);
        ier = CUSPREAD2D_NUPTSDRIVEN(d_plan, blksize);
        if (ier != 0 ) {
          cout<<"error: cnufftspread3d_gpu_subprob"<<endl;
          return 1;
        }
      }
      break;
    case SpreadMethod::SUBPROBLEM:
      {
        cudaEventRecord(start);
        ier = CUSPREAD2D_SUBPROB(d_plan, blksize);
        if (ier != 0 ) {
          cout<<"error: cnufftspread3d_gpu_subprob"<<endl;
          return 1;
        }
      }
      break;
    case SpreadMethod::BLOCK_GATHER:
      {
        cudaEventRecord(start);
        ier = CUSPREAD3D_BLOCKGATHER(nf1, nf2, nf3, M, d_plan, blksize);
        if (ier != 0 ) {
          cout<<"error: cnufftspread3d_gpu_subprob"<<endl;
          return 1;
        }
      }
      break;
    default:
      cerr<<"error: incorrect method, should be 1,2,4"<<endl;
      return 2;
  }
  return ier;
}

int CUSPREAD3D_BLOCKGATHER_PROP(int nf1, int nf2, int nf3, int M, 
  Plan<GPUDevice, FLT>* d_plan)
{
  cudaEvent_t start, stop;
  cudaEventCreate(&start);
  cudaEventCreate(&stop);

  dim3 threadsPerBlock;
  dim3 blocks;

  int pirange = d_plan->spread_params_.pirange;

  int maxsubprobsize=d_plan->options_.gpu_max_subproblem_size;
  int o_bin_size_x = d_plan->options_.gpu_obin_size.x;
  int o_bin_size_y = d_plan->options_.gpu_obin_size.y;
  int o_bin_size_z = d_plan->options_.gpu_obin_size.z;
  
  int numobins[3];
  if (nf1 % o_bin_size_x != 0 ||
    nf2 % o_bin_size_y != 0 ||
    nf3 % o_bin_size_z != 0) {
    cout<<"error: mod(nf1, options.gpu_obin_size.x) != 0"<<endl;
    cout<<"       mod(nf2, options.gpu_obin_size.y) != 0"<<endl;
    cout<<"       mod(nf3, options.gpu_obin_size.z) != 0"<<endl;
    cout<<"error: (nf1, nf2, nf3) = ("<<nf1<<", "<<nf2<<", "<<nf3<<")"<<endl;
    cout<<"error: (obinsizex, obinsizey, obinsizez) = ("
      <<o_bin_size_x<<", "<<o_bin_size_y<<", "<<o_bin_size_z<<")"<<endl;
    return 1;
  }

  numobins[0] = ceil((FLT) nf1/o_bin_size_x);
  numobins[1] = ceil((FLT) nf2/o_bin_size_y);
  numobins[2] = ceil((FLT) nf3/o_bin_size_z);

  int bin_size_x=d_plan->options_.gpu_bin_size.x;
  int bin_size_y=d_plan->options_.gpu_bin_size.y;
  int bin_size_z=d_plan->options_.gpu_bin_size.z;
  if (o_bin_size_x % bin_size_x != 0 ||
    o_bin_size_y % bin_size_y != 0 ||
    o_bin_size_z % bin_size_z != 0) {
    cout<<"error: mod(ops.gpu_obin_size.x, options.gpu_bin_size.x) != 0"<<endl;
    cout<<"       mod(ops.gpu_obin_size.y, options.gpu_bin_size.y) != 0"<<endl;
    cout<<"       mod(ops.gpu_obin_size.z, options.gpu_bin_size.z) != 0"<<endl;
    cout<<"error: (binsizex, binsizey, binsizez) = ("
      <<bin_size_x<<", "<<bin_size_y<<", "<<bin_size_z<<")"<<endl;
    cout<<"error: (obinsizex, obinsizey, obinsizez) = ("
      <<o_bin_size_x<<", "<<o_bin_size_y<<", "<<o_bin_size_z<<")"<<endl;
    return 1;
  }

  int binsperobinx, binsperobiny, binsperobinz;
  int numbins[3];
  binsperobinx = o_bin_size_x/bin_size_x+2;
  binsperobiny = o_bin_size_y/bin_size_y+2;
  binsperobinz = o_bin_size_z/bin_size_z+2;
  numbins[0] = numobins[0]*(binsperobinx);
  numbins[1] = numobins[1]*(binsperobiny);
  numbins[2] = numobins[2]*(binsperobinz);
#ifdef DEBUG
  cout<<"[debug ] Dividing the uniform grids to bin size["
    <<d_plan->options_.gpu_bin_size.x<<"x"<<d_plan->options_.gpu_bin_size.y<<"x"<<
    d_plan->options_.gpu_bin_size.z<<"]"<<endl;
  cout<<"[debug ] numobins = ["<<numobins[0]<<"x"<<numobins[1]<<"x"<<
    numobins[2]<<"]"<<endl;
  cout<<"[debug ] numbins = ["<<numbins[0]<<"x"<<numbins[1]<<"x"<<
    numbins[2]<<"]"<<endl;
#endif

  FLT*   d_kx = d_plan->kx;
  FLT*   d_ky = d_plan->ky;
  FLT*   d_kz = d_plan->kz;

#ifdef DEBUG
  FLT *h_kx, *h_ky, *h_kz;
  h_kx = (FLT*)malloc(M*sizeof(FLT));
  h_ky = (FLT*)malloc(M*sizeof(FLT));
  h_kz = (FLT*)malloc(M*sizeof(FLT));

  checkCudaErrors(cudaMemcpy(h_kx,d_kx,M*sizeof(FLT),cudaMemcpyDeviceToHost));
  checkCudaErrors(cudaMemcpy(h_ky,d_ky,M*sizeof(FLT),cudaMemcpyDeviceToHost));
  checkCudaErrors(cudaMemcpy(h_kz,d_kz,M*sizeof(FLT),cudaMemcpyDeviceToHost));
  for (int i=0; i<M; i++) {
    cout<<"[debug ] ";
    cout <<"("<<setw(3)<<h_kx[i]<<","<<setw(3)<<h_ky[i]<<","<<h_kz[i]<<")"
      <<endl;
  }
#endif
  int *d_binsize = d_plan->binsize;
  int *d_sortidx = d_plan->sortidx;
  int *d_binstartpts = d_plan->binstartpts;
  int *d_numsubprob = d_plan->numsubprob;
  void*d_temp_storage = NULL;
  int *d_idxnupts = NULL;
  int *d_subprobstartpts = d_plan->subprobstartpts;
  int *d_subprob_to_bin = NULL;

  // Synchronize device before we start. This is essential! Otherwise the
  // next kernel could read the wrong (kx, ky, kz) values.
  checkCudaErrors(cudaDeviceSynchronize());

  cudaEventRecord(start);
  checkCudaErrors(cudaMemset(d_binsize,0,numbins[0]*numbins[1]*numbins[2]*
    sizeof(int)));
  LocateNUptstoBins_ghost<<<(M+1024-1)/1024, 1024>>>(M,bin_size_x,
    bin_size_y,bin_size_z,numobins[0],numobins[1],numobins[2],binsperobinx,
    binsperobiny, binsperobinz,d_binsize,d_kx,
    d_ky,d_kz,d_sortidx,pirange,nf1,nf2,nf3);

#ifdef SPREADTIME
  float milliseconds = 0;
  cudaEventRecord(stop);
  cudaEventSynchronize(stop);
  cudaEventElapsedTime(&milliseconds, start, stop);
  printf("[time  ] \tKernel LocateNUptstoBins_ghost \t\t%.3g ms\n",
    milliseconds);
#endif
#ifdef DEBUG
  int *h_binsize;// For debug
  h_binsize     = (int*)malloc(numbins[0]*numbins[1]*numbins[2]*sizeof(int));
  checkCudaErrors(cudaMemcpy(h_binsize,d_binsize,numbins[0]*numbins[1]*
    numbins[2]*sizeof(int),cudaMemcpyDeviceToHost));
  cout<<"[debug ] bin size:"<<endl;
  for (int k=0; k<numbins[2]; k++) {
    cout<<"[debug ]"<<endl;
    for (int j=0; j<numbins[1]; j++) {
      if (j%binsperobinx == 0 && j!=0)
        cout<<"[debug ] -----------------"<<endl;
      cout<<"[debug ] ";
      for (int i=0; i<numbins[0]; i++) {
        if (i%binsperobinx == 0 && i!=0)
          cout<<"|";
        if (i!=0) cout<<" ";
        int binidx = CalcGlobalIdx(i,j,k,numobins[0],numobins[1],
          numobins[2],binsperobinx,binsperobiny,binsperobinz);
        cout<<h_binsize[binidx];
      }
      cout<<endl;
    }
  }
  cout<<"[debug ] ---------------------------------------------------"<<endl;
#endif
#ifdef DEBUG
  int *h_sortidx;
  h_sortidx = (int*)malloc(M*sizeof(int));

  checkCudaErrors(cudaMemcpy(h_sortidx,d_sortidx,M*sizeof(int),
    cudaMemcpyDeviceToHost));
  for (int i=0; i<M; i++) {
    cout <<"[debug ] point["<<setw(3)<<i<<"]="<<setw(3)<<h_sortidx[i]<<endl;
  }
#endif
  cudaEventRecord(start);
  threadsPerBlock.x=8;
  threadsPerBlock.y=8;
  threadsPerBlock.z=8;

  blocks.x = (threadsPerBlock.x+numbins[0]-1)/threadsPerBlock.x;
  blocks.y = (threadsPerBlock.y+numbins[1]-1)/threadsPerBlock.y;
  blocks.z = (threadsPerBlock.z+numbins[2]-1)/threadsPerBlock.z;

  FillGhostBins<<<blocks, threadsPerBlock>>>(binsperobinx, binsperobiny,
    binsperobinz, numobins[0], numobins[1], numobins[2], d_binsize);
#ifdef SPREADTIME
  cudaEventRecord(stop);
  cudaEventSynchronize(stop);
  cudaEventElapsedTime(&milliseconds, start, stop);
  printf("[time  ] \tKernel FillGhostBins \t\t\t%.3g ms\n",
    milliseconds);
#endif
#ifdef DEBUG
  checkCudaErrors(cudaMemcpy(h_binsize,d_binsize,numbins[0]*numbins[1]*
    numbins[2]*sizeof(int),cudaMemcpyDeviceToHost));
  cout<<"[debug ] Filled ghost bins:"<<endl;
  for (int k=0; k<numbins[2]; k++) {
    cout<<"[debug ] "<<endl;
    cout<<"[debug ] "<<endl;
    for (int j=0; j<numbins[1]; j++) {
      if (j%binsperobinx == 0 && j!=0)
        cout<<"[debug ] -----------------"<<endl;
      cout<<"[debug ] ";
      for (int i=0; i<numbins[0]; i++) {
        if (i%binsperobinx == 0 && i!=0)
        cout<<"|";
        int binidx = CalcGlobalIdx(i,j,k,numobins[0],numobins[1],
          numobins[2],binsperobinx,binsperobiny,binsperobinz);
        if (i!=0) cout<<" ";
        cout<<h_binsize[binidx];
      }
      cout<<endl;
    }
  }
  cout<<"[debug ] ---------------------------------------------------"<<endl;
#endif
  cudaEventRecord(start);
  int n=numbins[0]*numbins[1]*numbins[2];
  thrust::device_ptr<int> d_ptr(d_binsize);
  thrust::device_ptr<int> d_result(d_binstartpts+1);
  thrust::inclusive_scan(d_ptr, d_ptr + n, d_result);
  checkCudaErrors(cudaMemset(d_binstartpts,0,sizeof(int)));
#ifdef SPREADTIME
  cudaEventRecord(stop);
  cudaEventSynchronize(stop);
  cudaEventElapsedTime(&milliseconds, start, stop);
  printf("[time  ] \tKernel BinStartPts_3d \t\t\t%.3g ms\n", milliseconds);
#endif
#ifdef DEBUG
  int *h_binstartpts;
  h_binstartpts = (int*)malloc((numbins[0]*numbins[1]*numbins[2])*sizeof(int));
  checkCudaErrors(cudaMemcpy(h_binstartpts,d_binstartpts,(numbins[0]*
    numbins[1]*numbins[2])*sizeof(int),cudaMemcpyDeviceToHost));
  cout<<"[debug ] Result of scan bin_size array:"<<endl;
  for (int k=0; k<numbins[2]; k++) {
    cout<<"[debug ] "<<endl;
    for (int j=0; j<numbins[1]; j++) {
      cout<<"[debug ] ";
      for (int i=0; i<numbins[0]; i++) {
        if (i!=0) cout<<" ";
        int binidx = CalcGlobalIdx(i,j,k,numobins[0],numobins[1],
          numobins[2],binsperobinx,binsperobiny,binsperobinz);
        cout<<h_binstartpts[binidx];
      }
      cout<<endl;
    }
  }
  cout<<"[debug ] ----------------------------------------------------"<<endl;
#endif
  cudaEventRecord(start);
  int totalNUpts;
  checkCudaErrors(cudaMemcpy(&totalNUpts,&d_binstartpts[n],
    sizeof(int),cudaMemcpyDeviceToHost));
  checkCudaErrors(cudaMalloc(&d_idxnupts,totalNUpts*sizeof(int)));
#ifdef DEBUG
  checkCudaErrors(cudaMemset(d_idxnupts,-1,totalNUpts*sizeof(int)));
#endif
  cudaEventRecord(start);
  CalcInvertofGlobalSortIdx_ghost<<<(M+1024-1)/1024,1024>>>(M,bin_size_x,
    bin_size_y,bin_size_z,numobins[0],numobins[1],numobins[2],binsperobinx,
    binsperobiny,binsperobinz,d_binstartpts,d_sortidx,d_kx,d_ky,d_kz,
    d_idxnupts,pirange,nf1,nf2,nf3);
#ifdef SPREADTIME
  cudaEventRecord(stop);
  cudaEventSynchronize(stop);
  cudaEventElapsedTime(&milliseconds, start, stop);
  printf("[time  ] \tKernel CalcInvertofGlobalIdx_ghost \t%.3g ms\n",
    milliseconds);
#endif
#ifdef DEBUG
  int *h_idxnupts;
  h_idxnupts = (int*)malloc(totalNUpts*sizeof(int));
  checkCudaErrors(cudaMemcpy(h_idxnupts,d_idxnupts,totalNUpts*sizeof(int),
    cudaMemcpyDeviceToHost));
  int pts = 0;
  for (int b=0; b<numbins[0]*numbins[1]*numbins[1]; b++) {
    if (h_binsize[b] > 0)
      cout <<"[debug ] Bin "<<b<<endl;
    for (int i=h_binstartpts[b]; i<h_binstartpts[b]+h_binsize[b]; i++) {
      cout <<"[debug ] NUpts-index= "<< h_idxnupts[i]<<endl;
      pts++;
    }
  }
  cout<<"[debug ] totalpts = "<<pts<<endl;
#endif
  cudaEventRecord(start);
  threadsPerBlock.x=2;
  threadsPerBlock.y=2;
  threadsPerBlock.z=2;

  blocks.x = (threadsPerBlock.x+numbins[0]-1)/threadsPerBlock.x;
  blocks.y = (threadsPerBlock.y+numbins[1]-1)/threadsPerBlock.y;
  blocks.z = (threadsPerBlock.z+numbins[2]-1)/threadsPerBlock.z;

  GhostBinPtsIdx<<<blocks, threadsPerBlock>>>(binsperobinx, binsperobiny,
    binsperobinz, numobins[0], numobins[1], numobins[2], d_binsize,
    d_idxnupts, d_binstartpts, M);
        if (d_plan->idxnupts != NULL) cudaFree(d_plan->idxnupts);
  d_plan->idxnupts = d_idxnupts;
#ifdef SPREADTIME
  cudaEventRecord(stop);
  cudaEventSynchronize(stop);
  cudaEventElapsedTime(&milliseconds, start, stop);
  printf("[time  ] \tKernel GhostBinPtsIdx \t\t\t%.3g ms\n",
    milliseconds);
#endif
#ifdef DEBUG
  checkCudaErrors(cudaMemcpy(h_idxnupts,d_idxnupts,totalNUpts*sizeof(int),
    cudaMemcpyDeviceToHost));
  pts = 0;
  for (int b=0; b<numbins[0]*numbins[1]*numbins[1]; b++) {
    if (h_binsize[b] > 0)
      cout <<"[debug ] Bin "<<b<<endl;
    for (int i=h_binstartpts[b]; i<h_binstartpts[b]+h_binsize[b]; i++) {
      cout <<"[debug ] NUpts-index= "<< h_idxnupts[i]<<endl;
      pts++;
    }
  }
  cout<<"[debug ] totalpts = "<<pts<<endl;
  free(h_idxnupts);
  free(h_binstartpts);
  free(h_binsize);
#endif

  /* --------------------------------------------- */
  //        Determining Subproblem properties      //
  /* --------------------------------------------- */
  cudaEventRecord(start);
  n = numobins[0]*numobins[1]*numobins[2];
  cudaEventRecord(start);
  CalcSubProb_3d_v1<<<(n+1024-1)/1024, 1024>>>(binsperobinx, binsperobiny,
    binsperobinz, d_binsize, d_numsubprob, maxsubprobsize, numobins[0]*
    numobins[1]*numobins[2]);
#ifdef SPREADTIME
  cudaEventRecord(stop);
  cudaEventSynchronize(stop);
  cudaEventElapsedTime(&milliseconds, start, stop);
  printf("[time  ] \tKernel CalcSubProb_3d_v1\t\t%.3g ms\n",
    milliseconds);
#endif
#ifdef DEBUG
  int* h_numsubprob;
  h_numsubprob = (int*) malloc(n*sizeof(int));
  checkCudaErrors(cudaMemcpy(h_numsubprob,d_numsubprob,numobins[0]*numobins[1]*
    numobins[2]*sizeof(int),cudaMemcpyDeviceToHost));
  for (int k=0; k<numobins[2]; k++) {
    cout<<"[debug ] "<<endl;
    for (int j=0; j<numobins[1]; j++) {
      cout<<"[debug ] ";
      for (int i=0; i<numobins[0]; i++) {
        if (i!=0) cout<<" ";
        cout <<"s["<<setw(1)<<i<<","<<setw(1)<<j<<","<<setw(1)<<k
          <<"]= "<<setw(3)<<h_numsubprob[i+j*numobins[0]+k*
          numobins[1]*numobins[2]];
      }
      cout<<endl;
    }
  }
  free(h_numsubprob);
#endif
  cudaEventRecord(start);
  n = numobins[0]*numobins[1]*numobins[2];
  d_ptr    = thrust::device_pointer_cast(d_numsubprob);
  d_result = thrust::device_pointer_cast(d_subprobstartpts+1);
  thrust::inclusive_scan(d_ptr, d_ptr + n, d_result);
  checkCudaErrors(cudaMemset(d_subprobstartpts,0,sizeof(int)));
#ifdef SPREADTIME
  cudaEventRecord(stop);
  cudaEventSynchronize(stop);
  cudaEventElapsedTime(&milliseconds, start, stop);
  printf("[time  ] \tScan  numsubprob\t\t\t%.3g ms\n", milliseconds);
#endif
#ifdef DEBUG
  printf("[debug ] Subproblem start points\n");
  int* h_subprobstartpts;
  h_subprobstartpts = (int*) malloc((n+1)*sizeof(int));
  checkCudaErrors(cudaMemcpy(h_subprobstartpts,d_subprobstartpts,(numobins[0]*
    numobins[1]*numobins[2]+1)*sizeof(int),cudaMemcpyDeviceToHost));
  for (int k=0; k<numobins[2]; k++) {
    if (k!=0)
      cout<<"[debug ] "<<endl;
    for (int j=0; j<numobins[1]; j++) {
      cout<<"[debug ] ";
      for (int i=0; i<numobins[0]; i++) {
        if (i!=0) cout<<" ";
        cout <<"s["<<setw(1)<<i<<","<<setw(1)<<j<<","<<setw(1)<<k
          <<"]= "<<setw(3)<<h_subprobstartpts[i+j*numobins[0]+k*
          numobins[1]*numobins[2]];
      }
      cout<<endl;
    }
  }
  printf("[debug ] Total number of subproblems (%d) = %d\n", n,
    h_subprobstartpts[n]);
  free(h_subprobstartpts);
  cout<<"[debug ] ---------------------------------------------------"<<endl;
#endif
  cudaEventRecord(start);
  int totalnumsubprob;
  checkCudaErrors(cudaMemcpy(&totalnumsubprob,&d_subprobstartpts[n],
    sizeof(int),cudaMemcpyDeviceToHost));
  checkCudaErrors(cudaMalloc(&d_subprob_to_bin,totalnumsubprob*sizeof(int)));
  MapBintoSubProb_3d_v1<<<(n+1024-1)/1024, 1024>>>(d_subprob_to_bin,
    d_subprobstartpts,d_numsubprob,n);
  assert(d_subprob_to_bin != NULL);
        if (d_plan->subprob_to_bin != NULL) cudaFree(d_plan->subprob_to_bin);
  d_plan->subprob_to_bin   = d_subprob_to_bin;
  d_plan->totalnumsubprob  = totalnumsubprob;
#ifdef SPREADTIME
  cudaEventRecord(stop);
  cudaEventSynchronize(stop);
  cudaEventElapsedTime(&milliseconds, start, stop);
  printf("[time  ] \tKernel Subproblem to Bin map\t\t%.3g ms\n", milliseconds);
#endif
#ifdef DEBUG
  printf("[debug ] Map Subproblem to Bins\n");
  int* h_subprob_to_bin;
  h_subprob_to_bin   = (int*) malloc((totalnumsubprob)*sizeof(int));
  checkCudaErrors(cudaMemcpy(h_subprob_to_bin,d_subprob_to_bin,
    (totalnumsubprob)*sizeof(int),cudaMemcpyDeviceToHost));
  for (int j=0; j<totalnumsubprob; j++) {
    cout<<"[debug ] ";
    cout <<"s["<<j<<"] = "<<setw(2)<<"b["<<h_subprob_to_bin[j]<<"]";
    cout<<endl;
  }
  free(h_subprob_to_bin);
#endif
  cudaFree(d_temp_storage);

  return 0;
}

int CUSPREAD3D_BLOCKGATHER(int nf1, int nf2, int nf3, int M, 
  Plan<GPUDevice, FLT>* d_plan, int blksize)
{
  cudaEvent_t start, stop;
  cudaEventCreate(&start);
  cudaEventCreate(&stop);

  int ns=d_plan->spread_params_.nspread; 
  FLT es_c=d_plan->spread_params_.ES_c;
  FLT es_beta=d_plan->spread_params_.ES_beta;
  FLT sigma=d_plan->spread_params_.upsampling_factor;
  int pirange=d_plan->spread_params_.pirange;
  int maxsubprobsize=d_plan->options_.gpu_max_subproblem_size;

  int obin_size_x=d_plan->options_.gpu_obin_size.x;
  int obin_size_y=d_plan->options_.gpu_obin_size.y;
  int obin_size_z=d_plan->options_.gpu_obin_size.z;
  int bin_size_x=d_plan->options_.gpu_bin_size.x;
  int bin_size_y=d_plan->options_.gpu_bin_size.y;
  int bin_size_z=d_plan->options_.gpu_bin_size.z;
  int numobins[3];
  numobins[0] = ceil((FLT) nf1/obin_size_x);
  numobins[1] = ceil((FLT) nf2/obin_size_y);
  numobins[2] = ceil((FLT) nf3/obin_size_z);

  int binsperobinx, binsperobiny, binsperobinz;
  binsperobinx = obin_size_x/bin_size_x+2;
  binsperobiny = obin_size_y/bin_size_y+2;
  binsperobinz = obin_size_z/bin_size_z+2;
#ifdef INFO
  cout<<"[info  ] Dividing the uniform grids to bin size["
    <<obin_size_x<<"x"<<obin_size_y<<"x"<<obin_size_z<<"]"<<endl;
  cout<<"[info  ] numbins = ["<<numobins[0]<<"x"<<numobins[1]<<"x"<<
    numobins[2]<<"]"<<endl;
  cout<<"[info  ] ns = "<< ns<<endl;
#endif

  FLT* d_kx = d_plan->kx;
  FLT* d_ky = d_plan->ky;
  FLT* d_kz = d_plan->kz;
  CUCPX* d_c = d_plan->c;
  CUCPX* d_fw = d_plan->fine_grid_data_;

  int *d_binstartpts = d_plan->binstartpts;
  int *d_subprobstartpts = d_plan->subprobstartpts;
  int *d_idxnupts = d_plan->idxnupts;

  int totalnumsubprob=d_plan->totalnumsubprob;
  int *d_subprob_to_bin = d_plan->subprob_to_bin;

  cudaEventRecord(start);
  for (int t=0; t<blksize; t++) {
    if (d_plan->options_.kernel_evaluation_method == KernelEvaluationMethod::HORNER) {
      size_t sharedplanorysize = obin_size_x*obin_size_y*obin_size_z
        *sizeof(CUCPX);
      if (sharedplanorysize > 49152) {
        cout<<"error: not enough shared memory"<<endl;
        return 1;
      }
      Spread_3d_BlockGather_Horner<<<totalnumsubprob, 64, sharedplanorysize
        >>>(d_kx, d_ky, d_kz, d_c+t*M, d_fw+t*nf1*nf2*nf3, M, ns,
          nf1, nf2, nf3, es_c, es_beta, sigma, d_binstartpts,
          obin_size_x, obin_size_y, obin_size_z,
          binsperobinx*binsperobiny*binsperobinz,d_subprob_to_bin,
          d_subprobstartpts, maxsubprobsize, numobins[0], numobins[1],
          numobins[2], d_idxnupts,pirange);
    }else{
      size_t sharedplanorysize = obin_size_x*obin_size_y*obin_size_z
        *sizeof(CUCPX);
      if (sharedplanorysize > 49152) {
        cout<<"error: not enough shared memory"<<endl;
        return 1;
      }
      Spread_3d_BlockGather<<<totalnumsubprob, 64, sharedplanorysize>>>(
          d_kx, d_ky, d_kz, d_c+t*M, d_fw+t*nf1*nf2*nf3, M, ns,
          nf1, nf2, nf3, es_c, es_beta, sigma, d_binstartpts,
          obin_size_x, obin_size_y, obin_size_z,
          binsperobinx*binsperobiny*binsperobinz,d_subprob_to_bin,
          d_subprobstartpts, maxsubprobsize, numobins[0], numobins[1],
          numobins[2], d_idxnupts,pirange);
    }
  }
#ifdef SPREADTIME
      float milliseconds = 0;
      cudaEventRecord(stop);
      cudaEventSynchronize(stop);
      cudaEventElapsedTime(&milliseconds, start, stop);
      printf("[time  ] \tKernel Spread_3d_BlockGather (%d)\t%.3g ms\n",
        milliseconds, d_plan->options_.kernel_evaluation_method);
#endif
  return 0;
}

int CUSPREAD3D_SUBPROB_PROP(int nf1, int nf2, int nf3, int M, 
  Plan<GPUDevice, FLT>* d_plan)
{

  int maxsubprobsize=d_plan->options_.gpu_max_subproblem_size;
  int bin_size_x=d_plan->options_.gpu_bin_size.x;
  int bin_size_y=d_plan->options_.gpu_bin_size.y;
  int bin_size_z=d_plan->options_.gpu_bin_size.z;
  if (bin_size_x < 0 || bin_size_y < 0 || bin_size_z < 0) {
    cout<<"error: invalid binsize (binsizex, binsizey, binsizez) = (";
    cout<<bin_size_x<<","<<bin_size_y<<","<<bin_size_z<<")"<<endl;
    return 1; 
  }

  int numbins[3];
  numbins[0] = ceil((FLT) nf1/bin_size_x);
  numbins[1] = ceil((FLT) nf2/bin_size_y);
  numbins[2] = ceil((FLT) nf3/bin_size_z);

  FLT*   d_kx = d_plan->kx;
  FLT*   d_ky = d_plan->ky;
  FLT*   d_kz = d_plan->kz;

  int *d_binsize = d_plan->binsize;
  int *d_binstartpts = d_plan->binstartpts;
  int *d_sortidx = d_plan->sortidx;
  int *d_numsubprob = d_plan->numsubprob;
  int *d_subprobstartpts = d_plan->subprobstartpts;
  int *d_idxnupts = d_plan->idxnupts;

  int *d_subprob_to_bin = NULL;
  void *d_temp_storage = NULL;
  int pirange = d_plan->spread_params_.pirange;

  // Synchronize device before we start. This is essential! Otherwise the
  // next kernel could read the wrong (kx, ky, kz) values.
  checkCudaErrors(cudaDeviceSynchronize());

  checkCudaErrors(cudaMemset(d_binsize,0,numbins[0]*numbins[1]*numbins[2]*
    sizeof(int)));
  CalcBinSizeNoGhost3DKernel<<<(M+1024-1)/1024, 1024>>>(M,nf1,nf2,nf3,bin_size_x,
    bin_size_y,bin_size_z,numbins[0],numbins[1],numbins[2],d_binsize,d_kx,
    d_ky,d_kz,d_sortidx,pirange);


  int n=numbins[0]*numbins[1]*numbins[2];
  thrust::device_ptr<int> d_ptr(d_binsize);
  thrust::device_ptr<int> d_result(d_binstartpts);
  thrust::exclusive_scan(d_ptr, d_ptr + n, d_result);


  CalcInvertofGlobalSortIdx3DKernel<<<(M+1024-1)/1024,1024>>>(M,bin_size_x,
    bin_size_y,bin_size_z,numbins[0],numbins[1],numbins[2], d_binstartpts,
    d_sortidx,d_kx,d_ky,d_kz,d_idxnupts,pirange,nf1,nf2,nf3);

  /* --------------------------------------------- */
  //        Determining Subproblem properties      //
  /* --------------------------------------------- */
  CalcSubProb_3d_v2<<<(M+1024-1)/1024, 1024>>>(d_binsize,d_numsubprob,
      maxsubprobsize,numbins[0]*numbins[1]*numbins[2]);

  d_ptr    = thrust::device_pointer_cast(d_numsubprob);
  d_result = thrust::device_pointer_cast(d_subprobstartpts+1);
  thrust::inclusive_scan(d_ptr, d_ptr + n, d_result);
  checkCudaErrors(cudaMemset(d_subprobstartpts,0,sizeof(int)));

  int totalnumsubprob;
  checkCudaErrors(cudaMemcpy(&totalnumsubprob,&d_subprobstartpts[n],
    sizeof(int),cudaMemcpyDeviceToHost));
  checkCudaErrors(cudaMalloc(&d_subprob_to_bin,totalnumsubprob*sizeof(int)));
  MapBintoSubProb_3d_v2<<<(numbins[0]*numbins[1]+1024-1)/1024, 1024>>>(
    d_subprob_to_bin,d_subprobstartpts,d_numsubprob,numbins[0]*numbins[1]*
    numbins[2]);
  assert(d_subprob_to_bin != NULL);
        if (d_plan->subprob_to_bin != NULL) cudaFree(d_plan->subprob_to_bin);
  d_plan->subprob_to_bin = d_subprob_to_bin;
  assert(d_plan->subprob_to_bin != NULL);
  d_plan->totalnumsubprob = totalnumsubprob;

  cudaFree(d_temp_storage);

  return 0;
}
