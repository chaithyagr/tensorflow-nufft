/* Copyright 2021 The TensorFlow NUFFT Authors. All Rights Reserved.

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

#include "tensorflow_nufft/cc/kernels/fftw_api.h"
#include "tensorflow_nufft/cc/kernels/nufft_plan.h"
#include "tensorflow_nufft/cc/kernels/nufft_util.h"
#include "tensorflow_nufft/cc/kernels/omp_api.h"


// Largest possible kernel spread width per dimension, in fine grid points.
#define MAX_KERNEL_WIDTH 16


namespace tensorflow {
namespace nufft {

// local NU coord fold+rescale macro: does the following affine transform to x:
//   when p=true:   map [-3pi,-pi) and [-pi,pi) and [pi,3pi)    each to [0,N)
//   otherwise,     map [-N,0) and [0,N) and [N,2N)             each to [0,N)
// Thus, only one period either side of the principal domain is folded.
// (It is *so* much faster than slow std::fmod that we stick to it.)
// This explains FINUFFT's allowed input domain of [-3pi,3pi).
// Speed comparisons of this macro vs a function are in devel/foldrescale*.
// The macro wins hands-down on i7, even for modern GCC9.
#define FOLD_AND_RESCALE(x, N, p) (p ?                                         \
         (x + (x >= -kPi<FloatType> ? (x < kPi<FloatType> ? kPi<FloatType> : -kPi<FloatType>) : 3 * kPi<FloatType>)) * (kOneOverTwoPi<FloatType> * N) : \
                          (x >= 0.0 ? (x < (FloatType)N ? x : x - (FloatType)N) : x + (FloatType)N))

namespace {

// Forward declarations. Defined below.
template<typename FloatType>
Status set_grid_size(int ms,
                     const InternalOptions& options,
                     SpreadParameters<FloatType> spread_params,
                     int* grid_size);

template<typename FloatType>
Status setup_spreader(int rank, FloatType eps, double upsampling_factor,
                      int kerevalmeth, bool show_warnings,
                      SpreadParameters<FloatType> &spread_params);

template<typename FloatType>
Status setup_spreader_for_nufft(int rank, FloatType eps,
                                const InternalOptions& options,
                                SpreadParameters<FloatType> &spread_params);

static int get_transform_rank(int64_t n1, int64_t n2, int64_t n3);

template<typename FloatType>
Status check_spread_inputs(int64_t n1, int64_t n2, int64_t n3,
                           int64_t num_points, FloatType *kx, FloatType *ky,
                           FloatType *kz, SpreadParameters<FloatType> opts);

template<typename FloatType>
bool bin_sort_points(int64_t* sort_indices, int64_t n1, int64_t n2, int64_t n3,
                     int64_t num_points,  FloatType *kx, FloatType *ky,
                     FloatType *kz, SpreadParameters<FloatType> opts);

template<typename FloatType>
void bin_sort_singlethread(
    int64_t *ret, int64_t num_points, FloatType *kx, FloatType *ky,
    FloatType *kz, int64_t n1, int64_t n2, int64_t n3, int pirange,
    double bin_size_x, double bin_size_y, double bin_size_z, int debug);

template<typename FloatType>
void bin_sort_multithread(
    int64_t *ret, int64_t num_points, FloatType *kx, FloatType *ky, FloatType *kz,
    int64_t n1,int64_t n2,int64_t n3,int pirange,
    double bin_size_x, double bin_size_y, double bin_size_z, int debug,
    int num_threads);

template<typename FloatType>
int spreadinterpSorted(int64_t* sort_indices,int64_t N1, int64_t N2, int64_t N3,
		             FloatType *data_uniform,int64_t M, FloatType *kx, FloatType *ky, FloatType *kz,
		             FloatType *data_nonuniform, tensorflow::nufft::SpreadParameters<FloatType> opts, int did_sort);

template<typename FloatType>
int interpSorted(int64_t* sort_indices,int64_t N1, int64_t N2, int64_t N3,
		      FloatType *data_uniform,int64_t M, FloatType *kx, FloatType *ky, FloatType *kz,
		      FloatType *data_nonuniform, SpreadParameters<FloatType> opts, int did_sort);

template<typename FloatType>
int spreadSorted(int64_t* sort_indices,int64_t N1, int64_t N2, int64_t N3,
		      FloatType *data_uniform,int64_t M, FloatType *kx, FloatType *ky, FloatType *kz,
		      FloatType *data_nonuniform, SpreadParameters<FloatType> opts, int did_sort);

template<typename FloatType>
void deconvolveshuffle1d(
    SpreadDirection dir, FloatType prefac, FloatType* ker, int64_t ms,
		FloatType *fk, int64_t nf1, typename fftw::ComplexType<FloatType>::Type* fw,
    ModeOrder mode_order);

template<typename FloatType>
void deconvolveshuffle2d(
    SpreadDirection dir, FloatType prefac, FloatType *ker1, FloatType *ker2,
    int64_t ms, int64_t mt, FloatType *fk, int64_t nf1, int64_t nf2,
    typename fftw::ComplexType<FloatType>::Type* fw, ModeOrder mode_order);

template<typename FloatType>
void deconvolveshuffle3d(
    SpreadDirection dir, FloatType prefac, FloatType *ker1, FloatType *ker2,
    FloatType *ker3, int64_t ms, int64_t mt, int64_t mu,
    FloatType *fk, int64_t nf1, int64_t nf2, int64_t nf3,
    typename fftw::ComplexType<FloatType>::Type* fw, ModeOrder mode_order);

template<typename FloatType>
static inline void set_kernel_args(FloatType *args, FloatType x, const SpreadParameters<FloatType>& opts);

template<typename FloatType>
static inline void evaluate_kernel_vector(FloatType *ker, FloatType *args, const SpreadParameters<FloatType>& opts, const int N);

template<typename FloatType>
static inline void eval_kernel_vec_Horner(FloatType *ker, const FloatType z, const int w, const SpreadParameters<FloatType> &opts);

template<typename FloatType>
void interp_line(FloatType *out,FloatType *du, FloatType *ker,int64_t i1,int64_t N1,int ns);

template<typename FloatType>
void interp_square(FloatType *out,FloatType *du, FloatType *ker1, FloatType *ker2, int64_t i1,int64_t i2,int64_t N1,int64_t N2,int ns);

template<typename FloatType>
void interp_cube(FloatType *out,FloatType *du, FloatType *ker1, FloatType *ker2, FloatType *ker3,
		 int64_t i1,int64_t i2,int64_t i3,int64_t N1,int64_t N2,int64_t N3,int ns);

template<typename FloatType>
void spread_subproblem_1d(int64_t off1, int64_t size1,FloatType *du0,int64_t M0,FloatType *kx0,
                          FloatType *dd0,const SpreadParameters<FloatType>& opts);

template<typename FloatType>
void spread_subproblem_2d(int64_t off1, int64_t off2, int64_t size1,int64_t size2,
                          FloatType *du0,int64_t M0,
			  FloatType *kx0,FloatType *ky0,FloatType *dd0,const SpreadParameters<FloatType>& opts);

template<typename FloatType>
void spread_subproblem_3d(int64_t off1,int64_t off2, int64_t off3, int64_t size1,
                          int64_t size2,int64_t size3,FloatType *du0,int64_t M0,
			  FloatType *kx0,FloatType *ky0,FloatType *kz0,FloatType *dd0,
			  const SpreadParameters<FloatType>& opts);

template<typename FloatType>
void add_wrapped_subgrid(int64_t offset1,int64_t offset2,int64_t offset3,
			 int64_t size1,int64_t size2,int64_t size3,int64_t N1,
			 int64_t N2,int64_t N3,FloatType *data_uniform, FloatType *du0);

template<typename FloatType>
void add_wrapped_subgrid_thread_safe(int64_t offset1,int64_t offset2,int64_t offset3,
                                     int64_t size1,int64_t size2,int64_t size3,int64_t N1,
                                     int64_t N2,int64_t N3,FloatType *data_uniform, FloatType *du0);

template<typename FloatType>
void get_subgrid(int64_t &offset1,int64_t &offset2,int64_t &offset3,int64_t &size1,
		 int64_t &size2,int64_t &size3,int64_t M0,FloatType* kx0,FloatType* ky0,
		 FloatType* kz0,int ns, int ndims);

}  // namespace

template<typename FloatType>
Status Plan<CPUDevice, FloatType>::initialize(
    TransformType type,
    int rank,
    int* num_modes,
    FftDirection fft_direction,
    int num_transforms,
    FloatType tol,
    const InternalOptions& options) {

  if (type == TransformType::TYPE_3) {
    return errors::Unimplemented("type-3 transforms are not implemented");
  }
  if (rank < 1 || rank > 3) {
    return errors::Unimplemented("rank ", rank, " is not implemented");
  }
  if (num_transforms < 1) {
    return errors::InvalidArgument("num_transforms must be >= 1");
  }

  // Store input values to plan.
  this->rank_ = rank;
  this->type_ = type;
  this->fft_direction_ = fft_direction;
  this->tol_ = tol;
  this->num_transforms_ = num_transforms;
  this->options_ = options;

  this->num_modes_[0] = num_modes[0];
  this->num_modes_[1] = (this->rank_ > 1) ? num_modes[1] : 1;
  this->num_modes_[2] = (this->rank_ > 2) ? num_modes[2] : 1;
  this->mode_count_ = this->num_modes_[0] * this->num_modes_[1] * this->num_modes_[2];

  // Choose kernel evaluation method.
  if (this->options_.kernel_evaluation_method == KernelEvaluationMethod::AUTO) {
    this->options_.kernel_evaluation_method = KernelEvaluationMethod::HORNER;
  }

  // Choose overall number of threads.
  int num_threads = OMP_GET_MAX_THREADS();
  if (this->options_.num_threads > 0)
    num_threads = this->options_.num_threads; // user override
  this->options_.num_threads = num_threads;   // update options_ with actual number

  // Select batch size.
  if (this->options_.max_batch_size == 0) {
    this->num_batches_ = 1 + (num_transforms - 1) / num_threads;
    this->batch_size_ = 1 + (num_transforms - 1) / this->num_batches_;
  } else {
    this->batch_size_ = std::min(this->options_.max_batch_size, num_transforms);
    this->num_batches_ = 1 + (num_transforms - 1) / this->batch_size_;
  }

  // Choose default spreader threading configuration.
  if (this->options_.spread_threading == SpreadThreading::AUTO)
    this->options_.spread_threading = SpreadThreading::PARALLEL_SINGLE_THREADED;

  // Heuristic to choose default upsampling factor.
  if (this->options_.upsampling_factor == 0.0) {  // indicates auto-choose
    this->options_.upsampling_factor = 2.0;       // default, and need for tol small
    if (tol >= (FloatType)1E-9) {                   // the tol sigma=5/4 can reach
      if (type == TransformType::TYPE_3)
        this->options_.upsampling_factor = 1.25;  // faster b/c smaller RAM & FFT
      else if ((rank==1 && this->mode_count_>10000000) || (rank==2 && this->mode_count_>300000) || (rank==3 && this->mode_count_>3000000))  // type 1,2 heuristic cutoffs, double, typ tol, 12-core xeon
        this->options_.upsampling_factor = 1.25;
    }
  }

  // Populate the spreader options.
  TF_RETURN_IF_ERROR(setup_spreader_for_nufft(
      rank, tol, this->options_, this->spread_params_));

  // Initialize pointers to null.
  for (int i = 0; i < 3; i++) {
    this->points_[i] = nullptr;
    this->fseries_data_[i] = nullptr;
  }
  this->sort_indices_ = nullptr;

  // FFTW initialization must be done single-threaded.
  #pragma omp critical
  {
    static bool is_fftw_initialized = false;

    if (!is_fftw_initialized) {
      // Set up global FFTW state. Should be done only once.
      #ifdef _OPENMP
      // Initialize FFTW threads.
      fftw::init_threads<FloatType>();
      // Let FFTW use all threads.
      fftw::plan_with_nthreads<FloatType>(num_threads);
      #endif
      is_fftw_initialized = true;
    }
  }

  if (type == TransformType::TYPE_1)
    this->spread_params_.spread_direction = SpreadDirection::SPREAD;
  else // if (type == TransformType::TYPE_2)
    this->spread_params_.spread_direction = SpreadDirection::INTERP;

  // Determine fine grid sizes.
  TF_RETURN_IF_ERROR(set_grid_size(
      this->num_modes_[0], this->options_, this->spread_params_,
      &this->grid_dims_[0]));
  if (rank > 1) {
    TF_RETURN_IF_ERROR(set_grid_size(
        this->num_modes_[1], this->options_, this->spread_params_,
        &this->grid_dims_[1]));
  }
  if (rank > 2) {
    TF_RETURN_IF_ERROR(set_grid_size(
        this->num_modes_[2], this->options_, this->spread_params_,
        &this->grid_dims_[2]));
  }

  // Get Fourier coefficients of spreading kernel along each fine grid
  // dimension.
  for (int i = 0; i < this->rank_; i++) {
    // Number of Fourier coefficients.
    int num_coeffs = this->grid_dims_[i] / 2 + 1;
    // Allocate memory and calculate the Fourier series.
    TF_RETURN_IF_ERROR(this->context_->allocate_temp(
        DataTypeToEnum<FloatType>::value, TensorShape({num_coeffs}),
        &this->fseries_tensor_[i]));
    this->fseries_data_[i] = reinterpret_cast<FloatType*>(
        this->fseries_tensor_[i].flat<FloatType>().data());
    kernel_fseries_1d(this->grid_dims_[i], this->spread_params_,
                      this->fseries_data_[i]);
  }

  // Total number of points in the fine grid.
  this->grid_size_ = this->grid_dims_[0];
  if (rank > 1)
    this->grid_size_ *= this->grid_dims_[1];
  if (rank > 2)
    this->grid_size_ *= this->grid_dims_[2];

  if (this->grid_size_ * this->batch_size_ > kMaxArraySize) {
    return errors::Internal(
        "size of internal fine grid is larger than maximum allowed: ",
        this->grid_size_ * this->batch_size_, " > ",
        kMaxArraySize);
  }

  // Allocate the working fine grid through the op kernel context. We allocate a
  // flat array, since we'll only use this tensor through a raw pointer anyway.
  TensorShape fine_grid_shape({this->grid_size_ * this->batch_size_});
  TF_RETURN_IF_ERROR(this->context_->allocate_temp(
      DataTypeToEnum<DType>::value, fine_grid_shape, &this->grid_tensor_));
  this->grid_data_ = reinterpret_cast<FftwType*>(
      this->grid_tensor_.flat<DType>().data());

  int fft_dims[3] = {1, 1, 1};
  switch (this->rank_) {
    case 1:
      fft_dims[0] = this->grid_dims_[0];
      break;
    case 2:
      fft_dims[1] = this->grid_dims_[0];
      fft_dims[0] = this->grid_dims_[1];
      break;
    case 3:
      fft_dims[2] = this->grid_dims_[0];
      fft_dims[1] = this->grid_dims_[1];
      fft_dims[0] = this->grid_dims_[2];
      break;
  }

  #pragma omp critical
  {
    this->fft_plan_ = fftw::plan_many_dft<FloatType>(
        /* int rank */ rank, /* const int *n */ fft_dims,
        /* int howmany */ this->batch_size_,
        /* fftw_complex *in */ this->grid_data_,
        /* const int *inembed */ nullptr,
        /* int istride */ 1, /* int idist */ this->grid_size_,
        /* fftw_complex *out */ this->grid_data_,
        /* const int *onembed */ nullptr,
        /* int ostride */ 1, /* int odist */ this->grid_size_,
        /* int sign */ static_cast<int>(this->fft_direction_),
        /* unsigned flags */ this->options_.fftw_flags);
  }

  return Status::OK();
}

template<typename FloatType>
Plan<CPUDevice, FloatType>::~Plan() {

  // Destroy the FFTW plan. This must be done single-threaded.
  #pragma omp critical
  {
    fftw::destroy_plan<FloatType>(this->fft_plan_);
  }

  // Wait until all threads are done using FFTW, then clean up the FFTW state,
  // which only needs to be done once.
  #ifdef _OPENMP
  #pragma omp barrier
  #pragma omp critical
  {
    static bool is_fftw_finalized = false;
    if (!is_fftw_finalized) {
      fftw::cleanup_threads<FloatType>();
      is_fftw_finalized = true;
    }
  }
  #endif

  free(this->sort_indices_);
}

template<typename FloatType>
Status Plan<CPUDevice, FloatType>::set_points(
    int num_points, FloatType* points_x,
    FloatType* points_y, FloatType* points_z) {
  // The user only now chooses how many NU (x,y,z) points.
  this->num_points_ = num_points;

  int64_t grid_size_0 = this->grid_dims_[0];
  int64_t grid_size_1 = 1;
  if (this->rank_ > 1) grid_size_1 = this->grid_dims_[1];
  int64_t grid_size_2 = 1;
  if (this->rank_ > 2) grid_size_2 = this->grid_dims_[2];

  if (this->type_ != TransformType::TYPE_3) {
    // Type 1/2 transform.
    // All we can do is check and maybe bin-sort the non-uniform points.
    // Plan must keep pointers to user's fixed points.
    this->points_[0] = points_x;
    this->points_[1] = points_y;
    this->points_[2] = points_z;
    TF_RETURN_IF_ERROR(check_spread_inputs(
        grid_size_0, grid_size_1, grid_size_2,
        this->num_points_, points_x, points_y, points_z,
        this->spread_params_));

    this->sort_indices_ = (int64_t*) malloc(sizeof(int64_t) * this->num_points_);
    if (!this->sort_indices_) {
      fprintf(stderr,"[%s] failed to allocate sort_indices_!\n",__func__);
      // return ERR_SPREAD_ALLOC;
    }
    this->did_sort_ = bin_sort_points(
        this->sort_indices_, grid_size_0, grid_size_1, grid_size_2,
        this->num_points_, points_x, points_y, points_z, this->spread_params_);

  } else {
    // Type 3 transform.
    return errors::Unimplemented("Type-3 transforms not implemented yet.");
  }

  return Status::OK();
}

/* See ../docs/cguru.doc for current documentation.

   For given (stack of) weights cj or coefficients fk, performs NUFFTs with
   existing (sorted) NU pts and existing plan.
   For type 1 and 3: cj is input, fk is output.
   For type 2: fk is input, cj is output.
   Performs spread/interp, pre/post deconvolve, and fftw_execute as appropriate
   for each of the 3 types.
   For cases of num_transforms>1, performs work in blocks of size up to batch_size.
   Return value 0 (no error diagnosis yet).
   Barnett 5/20/20, based on Malleo 2019.
*/
template<typename FloatType>
Status Plan<CPUDevice, FloatType>::execute(DType* cj, DType* fk){

  if (this->type_ != TransformType::TYPE_3) {

    double t_sprint = 0.0, t_fft = 0.0, t_deconv = 0.0;  // accumulated timing

    for (int b=0; b*this->batch_size_ < this->num_transforms_; b++) { // .....loop b over batches

      // current batch is either batch_size, or possibly truncated if last one
      int thisBatchSize = std::min(this->num_transforms_ - b*this->batch_size_, this->batch_size_);
      int bB = b*this->batch_size_;         // index of vector, since batchsizes same
      DType* cjb = cj + bB*this->num_points_;        // point to batch of weights
      DType* fkb = fk + bB*this->mode_count_;         // point to batch of mode coeffs

      // STEP 1: (varies by type)
      if (this->type_ == TransformType::TYPE_1) {  // type 1: spread NU pts this->points_[0], weights cj, to fw grid
        TF_RETURN_IF_ERROR(this->spread_or_interp_sorted_batch(thisBatchSize, cjb));
      } else {          //  type 2: amplify Fourier coeffs fk into 0-padded fw
        TF_RETURN_IF_ERROR(this->deconvolve_batch(thisBatchSize, fkb));
      }

      // STEP 2: call the pre-planned FFT on this batch
      // This wastes some flops if thisBatchSize < batch_size.
      fftw::execute<FloatType>(this->fft_plan_);

      // STEP 3: (varies by type)
      if (this->type_ == TransformType::TYPE_1) {   // type 1: deconvolve (amplify) fw and shuffle to fk
        TF_RETURN_IF_ERROR(this->deconvolve_batch(thisBatchSize, fkb));
      } else {          // type 2: interpolate unif fw grid to NU target pts
        TF_RETURN_IF_ERROR(this->spread_or_interp_sorted_batch(thisBatchSize, cjb));
      }
    }                                                   // ........end b loop
  } else {
    // Type 3 transform.
    return errors::Unimplemented("Type-3 transforms not implemented yet.");
  }

  return Status::OK();
}

template<typename FloatType>
Status Plan<CPUDevice, FloatType>::interp(DType* c, DType* f) {
  return this->spread_or_interp(c, f);
}

template<typename FloatType>
Status Plan<CPUDevice, FloatType>::spread(DType* c, DType* f) {
  return this->spread_or_interp(c, f);
}

template<typename FloatType>
Status Plan<CPUDevice, FloatType>::spread_or_interp(DType* cj, DType* fk) {

  double t_sprint = 0.0, t_fft = 0.0, t_deconv = 0.0;  // accumulated timing

  for (int b=0; b*this->batch_size_ < this->num_transforms_; b++) { // .....loop b over batches

    // current batch is either batch_size, or possibly truncated if last one
    int thisBatchSize = std::min(this->num_transforms_ - b*this->batch_size_, this->batch_size_);
    int bB = b*this->batch_size_;         // index of vector, since batchsizes same
    DType* cjb = cj + bB*this->num_points_;        // point to batch of weights
    DType* fkb = fk + bB*this->mode_count_;         // point to batch of mode coeffs

    this->spread_or_interp_sorted_batch(thisBatchSize, cjb, fkb);
  }

  return Status::OK();
}

template<typename FloatType>
Status Plan<CPUDevice, FloatType>::spread_or_interp_sorted_batch(
    int batch_size, DType* cBatch, DType* fBatch) {
  // opts.spread_threading: 1 sequential multithread, 2 parallel single-thread.
  // omp_sets_nested deprecated, so don't use; assume not nested for 2 to work.
  // But when nthr_outer=1 here, omp par inside the loop sees all threads...
  int nthr_outer = this->options_.spread_threading == SpreadThreading::SEQUENTIAL_MULTI_THREADED ? 1 : batch_size;

  if (fBatch == nullptr) {
    fBatch = (DType*) this->grid_data_;
  }

  int64_t grid_size_0 = this->grid_dims_[0];
  int64_t grid_size_1 = 1;
  int64_t grid_size_2 = 1;
  if (this->rank_ > 1) grid_size_1 = this->grid_dims_[1];
  if (this->rank_ > 2) grid_size_2 = this->grid_dims_[2];

  #pragma omp parallel for num_threads(nthr_outer)
  for (int i=0; i<batch_size; i++) {
    DType *fwi = fBatch + i*this->grid_size_;  // start of i'th fw array in wkspace
    DType *ci = cBatch + i*this->num_points_;            // start of i'th c array in cBatch
    spreadinterpSorted(this->sort_indices_, grid_size_0, grid_size_1, grid_size_2,
                       (FloatType*)fwi, this->num_points_, this->points_[0], this->points_[1], this->points_[2],
                       (FloatType*)ci, this->spread_params_, this->did_sort_);
  }
  return Status::OK();
}

template<typename FloatType>
Status Plan<CPUDevice, FloatType>::deconvolve_batch(int batch_size, DType* fkBatch) {
  FloatType one = 1.0;
  // since deconvolveshuffle?d are single-thread, omp par seems to help here...
  #pragma omp parallel for num_threads(batch_size)
  for (int batch_index = 0; batch_index < batch_size; batch_index++) {
    FftwType *fwi = this->grid_data_ + batch_index * this->grid_size_;
    DType *fki = fkBatch + batch_index * this->mode_count_;
    if (this->rank_ == 1)
      deconvolveshuffle1d(this->spread_params_.spread_direction, one, this->fseries_data_[0],
                          this->num_modes_[0], (FloatType *)fki,
                          this->grid_dims_[0], fwi, this->options_.mode_order);
    else if (this->rank_ == 2)
      deconvolveshuffle2d(this->spread_params_.spread_direction, one, this->fseries_data_[0],
                          this->fseries_data_[1], this->num_modes_[0], this->num_modes_[1], (FloatType *)fki,
                          this->grid_dims_[0], this->grid_dims_[1], fwi, this->options_.mode_order);
    else
      deconvolveshuffle3d(this->spread_params_.spread_direction, one, this->fseries_data_[0],
                          this->fseries_data_[1], this->fseries_data_[2], this->num_modes_[0], this->num_modes_[1], this->num_modes_[2],
                          (FloatType *)fki, this->grid_dims_[0], this->grid_dims_[1], this->grid_dims_[2],
                          fwi, this->options_.mode_order);
  }
  return Status::OK();
}

namespace {

// Set 1D size of upsampled array, grid_size, given options and requested number of
// Fourier modes.
template<typename FloatType>
Status set_grid_size(int ms,
                     const InternalOptions& options,
                     SpreadParameters<FloatType> spread_params,
                     int* grid_size) {
  // for spread/interp only, we do not apply oversampling (Montalt 6/8/2021).
  if (options.spread_only) {
    *grid_size = ms;
  } else {
    *grid_size = static_cast<int>(options.upsampling_factor * ms);
  }

  // This is required to avoid errors.
  if (*grid_size < 2 * spread_params.kernel_width)
    *grid_size = 2 * spread_params.kernel_width;

  // Check if array size is too big.
  if (*grid_size > kMaxArraySize) {
    return errors::Internal(
        "Upsampled dim size too big: ", *grid_size, " > ", kMaxArraySize);
  }

  // Find the next smooth integer.
  *grid_size = next_smooth_int(*grid_size);

  // For spread/interp only mode, make sure that the grid size is valid.
  if (options.spread_only && *grid_size != ms) {
    return errors::Internal(
        "Invalid grid size: ", ms, ". Value should be even, "
        "larger than the kernel (", 2 * spread_params.kernel_width, ") and have no prime "
        "factors larger than 5.");
  }

  return Status::OK();
}

template<typename FloatType>
Status setup_spreader(
    int rank, FloatType eps, double upsampling_factor,
    int kerevalmeth, bool show_warnings, SpreadParameters<FloatType> &spread_params)
/* Initializes spreader kernel parameters given desired NUFFT tol eps,
   upsampling factor (=sigma in paper, or R in Dutt-Rokhlin), ker eval meth
   (either 0:exp(sqrt()), 1: Horner ppval), and some debug-level flags.
   Also sets all default options in SpreadParameters<FloatType>. See SpreadParameters<FloatType>.h for spread_params.
   rank is spatial dimension (1,2, or 3).
   See finufft.cpp:finufft_plan() for where upsampling_factor is set.
   Must call this before any kernel evals done, otherwise segfault likely.
   Returns:
     0  : success
     WARN_EPS_TOO_SMALL : requested eps cannot be achieved, but proceed with
                          best possible eps
     otherwise : failure (see codes in finufft_definitions.h); spreading must not proceed
   Barnett 2017. debug, loosened eps logic 6/14/20.
*/
{
  if (upsampling_factor != 2.0 && upsampling_factor != 1.25) {
    if (kerevalmeth == 1) {
      return errors::Internal(
          "Horner kernel evaluation only supports standard "
          "upsampling factors of 2.0 or 1.25, but got ", upsampling_factor);
    }
    if (upsampling_factor <= 1.0) {
      return errors::Internal(
          "upsampling_factor must be > 1.0, but is ", upsampling_factor);
    }
  }

  // write out default SpreadParameters<FloatType>
  spread_params.pirange = 1;             // user also should always set this
  spread_params.check_bounds = false;
  spread_params.sort_points = SortPoints::AUTO;
  spread_params.pad_kernel = 0;              // affects only evaluate_kernel_vector
  spread_params.kerevalmeth = kerevalmeth;
  spread_params.upsampling_factor = upsampling_factor;
  spread_params.num_threads = 0;            // all avail
  spread_params.sort_threads = 0;        // 0:auto-choice
  // heuristic dir=1 chunking for num_threads>>1, typical for intel i7 and skylake...
  spread_params.max_subproblem_size = (rank == 1) ? 10000 : 100000;
  spread_params.flags = 0;               // 0:no timing flags (>0 for experts only)
  spread_params.verbosity = 0;               // 0:no debug output
  // heuristic num_threads above which switch OMP critical to atomic (add_wrapped...):
  spread_params.atomic_threshold = 10;   // R Blackwell's value

  int ns = 0;  // Set kernel width w (aka ns, kernel_width) then copy to spread_params...
  if (eps < kEpsilon<FloatType>) {
    eps = kEpsilon<FloatType>;
  }

  // Select kernel width.
  if (upsampling_factor == 2.0)           // standard sigma (see SISC paper)
    ns = std::ceil(-log10(eps / (FloatType)10.0));          // 1 digit per power of 10
  else                          // custom sigma
    ns = std::ceil(-log(eps) / (kPi<FloatType> * sqrt(1.0 - 1.0 / upsampling_factor)));  // formula, gam=1
  ns = std::max(2, ns);               // (we don't have ns=1 version yet)
  if (ns > kMaxKernelWidth) {         // clip to fit allocated arrays, Horner rules
    ns = kMaxKernelWidth;
  }
  spread_params.kernel_width = ns;

  // setup for reference kernel eval (via formula): select beta width param...
  // (even when kerevalmeth=1, this ker eval needed for FTs in onedim_*_kernel)
  spread_params.kernel_half_width = (FloatType)ns / 2;   // constants to help (see below routines)
  spread_params.kernel_c = 4.0 / (FloatType)(ns * ns);
  FloatType beta_over_ns = 2.30;         // gives decent betas for default sigma=2.0
  if (ns == 2) beta_over_ns = 2.20;  // some small-width tweaks...
  if (ns == 3) beta_over_ns = 2.26;
  if (ns == 4) beta_over_ns = 2.38;
  if (upsampling_factor != 2.0) {          // again, override beta for custom sigma
    FloatType gamma = 0.97;              // must match devel/gen_all_horner_C_code.m !
    beta_over_ns = gamma * kPi<FloatType>*(1.0 - 1.0 / (2 * upsampling_factor));  // formula based on cutoff
  }
  spread_params.kernel_beta = beta_over_ns * (FloatType)ns;    // set the kernel beta parameter

  // Calculate scaling factor for spread/interp only mode.
  if (spread_params.spread_only)
    spread_params.kernel_scale = calculate_scale_factor<FloatType>(rank, spread_params);

  return Status::OK();
}

template<typename FloatType>
Status setup_spreader_for_nufft(int rank, FloatType eps,
                                const InternalOptions& options,
                                SpreadParameters<FloatType> &spread_params)
// Set up the spreader parameters given eps, and pass across various nufft
// options. Return status of setup_spreader. Uses pass-by-ref. Barnett 10/30/17
{
  // This must be set before calling setup_spreader
  spread_params.spread_only = options.spread_only;

  TF_RETURN_IF_ERROR(setup_spreader(
      rank, eps, options.upsampling_factor,
      static_cast<int>(options.kernel_evaluation_method) - 1, // We subtract 1 temporarily, as spreader expects values of 0 or 1 instead of 1 and 2.
      options.show_warnings, spread_params));

  // override various spread spread_params from their defaults...
  spread_params.sort_points = options.sort_points;
  spread_params.spread_method = options.spread_method;
  spread_params.verbosity = options.verbosity;
  spread_params.pad_kernel = options.pad_kernel; // (only applies to kerevalmeth=0)
  spread_params.check_bounds = options.check_bounds;
  spread_params.num_threads = options.num_threads;
  if (options.num_threads_for_atomic_spread >= 0) // overrides
    spread_params.atomic_threshold = options.num_threads_for_atomic_spread;
  if (options.max_spread_subproblem_size > 0)        // overrides
    spread_params.max_subproblem_size = options.max_spread_subproblem_size;

  return Status::OK();
}

// Checks spreader inputs.
// Split out by Melody Shih, Jun 2018. Finiteness chk Barnett 7/30/18.
// Bypass FOLD_AND_RESCALE macro which has inevitable rounding err even nr +pi,
// giving fake invalids well inside the [-3pi,3pi] domain, 4/9/21.
// Equivalent to spreadcheck in original FINUFFT code.
template<typename FloatType>
Status check_spread_inputs(int64_t n1, int64_t n2, int64_t n3, int64_t num_points,
                           FloatType *kx, FloatType *ky, FloatType *kz,
                           SpreadParameters<FloatType> opts) {
  // Check that cuboid is large enough for spreading.
  int min_n = 2 * opts.kernel_width;
  if (n1 < min_n || (n2 > 1 && n2 < min_n) || (n3 > 1 && n3 < min_n)) {
    return errors::InvalidArgument(
        "cuboid too small for spreading, got (", n1, ", ", n2, ", ", n3, ") ",
        "but need at least ", min_n, " in each non-trivial dimension");
  }
  int rank = get_transform_rank(n1, n2, n3);

  // Check bounds. Check that non-uniform points are valid (+-3pi if pirange, or [-grid_size,2N])
  // exit gracefully as soon as invalid is found.
  // Note: isfinite() breaks with -Ofast
  if (opts.check_bounds) {
    FloatType lower_bound, upper_bound;
    lower_bound = opts.pirange ? -3.0 * kPi<FloatType> : -(FloatType)n1;
    upper_bound = opts.pirange ? 3.0 * kPi<FloatType> : 2 * (FloatType)n1;
    for (int64_t i = 0; i < num_points; ++i) {
      if ((kx[i] < lower_bound || kx[i] > upper_bound) || !isfinite(kx[i])) {
        return errors::InvalidArgument(
            "points outside valid range: kx[", i, "] = ", kx[i],
            " is not in [", lower_bound, ", ", upper_bound, "]");
      }
    }
    if (rank > 1) {
      lower_bound = opts.pirange ? -3.0 * kPi<FloatType> : -(FloatType)n2;
      upper_bound = opts.pirange ? 3.0 * kPi<FloatType> : 2 * (FloatType)n2;
      for (int64_t i = 0; i<num_points; ++i) {
        if ((ky[i] < lower_bound || ky[i] > upper_bound) || !isfinite(ky[i])) {
          return errors::InvalidArgument(
              "points outside valid range: ky[", i, "] = ", ky[i],
              " is not in [", lower_bound, ", ", upper_bound, "]");
        }
      }
    }
    if (rank > 2) {
      lower_bound = opts.pirange ? -3.0 * kPi<FloatType> : -(FloatType)n3;
      upper_bound = opts.pirange ? 3.0 * kPi<FloatType> : 2 * (FloatType)n3;
      for (int64_t i = 0; i < num_points; ++i) {
        if ((kz[i] < lower_bound || kz[i] > upper_bound) || !isfinite(kz[i])) {
          return errors::InvalidArgument(
              "points outside valid range: kz[", i, "] = ", kz[i],
              " is not in [", lower_bound, ", ", upper_bound, "]");
        }
      }
    }
  }
  return Status::OK();
}

// This makes a decision whether or not to sort the NU pts (influenced by
// opts.sort_points), and if yes, calls either single- or multi-threaded bin sort,
// writing reordered index list to sort_indices. If decided not to sort, the
// identity permutation is written to sort_indices.
// The permutation is designed to make RAM access close to contiguous, to
// speed up spreading/interpolation, in the case of disordered NU points.

// Inputs:
// num_points        - number of input NU points.
// kx,ky,kz - length-num_points arrays of real coords of NU pts, in the domain
//             for FOLD_AND_RESCALE, which includes [0,n1], [0,n2], [0,n3]
//             respectively, if opts.pirange=0; or [-pi,pi] if opts.pirange=1.
//             (only kz used in 1D, only kx and ky used in 2D.)
//             These must have been bounds-checked already; see check_spread_inputs.
// n1,n2,n3 - integer sizes of overall box (set n2=n3=1 for 1D, n3=1 for 2D).
//             1 = x (fastest), 2 = y (medium), 3 = z (slowest).
// opts     - spreading options struct, documented in ../include/SpreadParameters<FloatType>.h
// Outputs:
// sort_indices - a good permutation of NU points. (User must preallocate
//                 to length num_points.) Ie, kx[sort_indices[j]], j=0,..,num_points-1, is a good
//                 ordering for the x-coords of NU pts, etc.
// returned value - true if sorting was done, false otherwise.

// Barnett 2017; split out by Melody Shih, Jun 2018.
// Called indexSort in original FINUFFT code.
template<typename FloatType>
bool bin_sort_points(int64_t* sort_indices, int64_t n1, int64_t n2, int64_t n3,
                     int64_t num_points,  FloatType *kx, FloatType *ky,
                     FloatType *kz, SpreadParameters<FloatType> opts) {
  int rank = get_transform_rank(n1, n2, n3);
  int64_t grid_size = n1 * n2 * n3;

  // Heuristic binning box size for uniform grid... affects performance:
  double bin_size_x = 16, bin_size_y = 4, bin_size_z = 4;
  // Put in heuristics based on cache sizes (only useful for single-thread).
  bool should_sort = !(rank == 1 && (opts.spread_direction == SpreadDirection::INTERP || (num_points > 1000 * n1)));  // 1D small-grid_size or dir=2 case: don't sort
  bool did_sort = false;

  int max_threads = OMP_GET_MAX_THREADS();
  if (opts.num_threads > 0)  // user override up to max threads
    max_threads = std::min(max_threads, opts.num_threads);

  if (opts.sort_points == SortPoints::YES ||
      (opts.sort_points == SortPoints::AUTO && should_sort)) {
    // store a good permutation ordering of all NU pts (rank=1,2 or 3)
    int sort_debug = (opts.verbosity>=2);   // show timing output?
    int sort_threads = opts.sort_threads;   // choose # threads for sorting
    if (sort_threads == 0)   // use auto choice: when grid_size >> num_points, one thread is better!
      sort_threads = (10 * num_points > grid_size) ? max_threads : 1;
    if (sort_threads == 1) {
      bin_sort_singlethread(sort_indices, num_points, kx, ky, kz, n1, n2, n3,
                            opts.pirange, bin_size_x, bin_size_y, bin_size_z,
                            sort_debug);
    }
    else {
      bin_sort_multithread(sort_indices, num_points, kx, ky, kz, n1, n2, n3,
                           opts.pirange, bin_size_x, bin_size_y, bin_size_z,
                           sort_debug, sort_threads);
    }
    did_sort = true;
  } else {
    // Set identity permutation. Here OMP helps Xeon, hinders i7.
    #pragma omp parallel for num_threads(max_threads) schedule(static,1000000)
    for (int64_t i = 0; i < num_points; i++)
      sort_indices[i] = i;
  }
  return did_sort;
}

/* Returns permutation of all nonuniform points with good RAM access,
 * ie less cache misses for spreading, in 1D, 2D, or 3D. Single-threaded version
 *
 * This is achieved by binning into cuboids (of given bin_size within the
 * overall box domain), then reading out the indices within
 * these bins in a Cartesian cuboid ordering (x fastest, y med, z slowest).
 * Finally the permutation is inverted, so that the good ordering is: the
 * NU pt of index ret[0], the NU pt of index ret[1],..., NU pt of index ret[num_points-1]
 *
 * Inputs: num_points - number of input NU points.
 *         kx,ky,kz - length-num_points arrays of real coords of NU pts, in the domain
 *                    for FOLD_AND_RESCALE, which includes [0,n1], [0,n2], [0,n3]
 *                    respectively, if pirange=0; or [-pi,pi] if pirange=1.
 *         n1,n2,n3 - integer sizes of overall box (n2=n3=1 for 1D, n3=1 for 2D)
 *         bin_size_x,y,z - what binning box size to use in each dimension
 *                    (in rescaled coords where ranges are [0,Ni] ).
 *                    For 1D, only bin_size_x is used; for 2D, it & bin_size_y.
 * Output:
 *         writes to ret a vector list of indices, each in the range 0,..,num_points-1.
 *         Thus, ret must have been preallocated for num_points int64_ts.
 *
 * Notes: I compared RAM usage against declaring an internal vector and passing
 * back; the latter used more RAM and was slower.
 * Avoided the bins array, as in JFM's spreader of 2016,
 * tidied up, early 2017, Barnett.
 *
 * Timings (2017): 3s for num_points=1e8 NU pts on 1 core of i7; 5s on 1 core of xeon.
 */
template<typename FloatType>
void bin_sort_singlethread(
    int64_t *ret, int64_t num_points, FloatType *kx, FloatType *ky, FloatType *kz,
    int64_t n1, int64_t n2, int64_t n3, int pirange,
    double bin_size_x, double bin_size_y, double bin_size_z, int debug) {
  bool isky = (n2 > 1), iskz = (n3 > 1);  // ky,kz avail? (cannot access if not)
  // here the +1 is needed to allow round-off error causing i1=n1/bin_size_x,
  // for kx near +pi, ie foldrescale gives n1 (exact arith would be 0 to n1-1).
  // Note that round-off near kx=-pi stably rounds negative to i1=0.
  int64_t nbins1 = n1 / bin_size_x + 1, nbins2, nbins3;
  nbins2 = isky ? n2 / bin_size_y + 1 : 1;
  nbins3 = iskz ? n3 / bin_size_z + 1 : 1;
  int64_t num_bins = nbins1 * nbins2 * nbins3;

  std::vector<int64_t> counts(num_bins,0);  // count how many pts in each bin
  for (int64_t i = 0; i < num_points; i++) {
    // find the bin index in however many dims are needed
    int64_t i1 = FOLD_AND_RESCALE(kx[i], n1, pirange) / bin_size_x, i2 = 0, i3 = 0;
    if (isky) i2 = FOLD_AND_RESCALE(ky[i], n2, pirange) / bin_size_y;
    if (iskz) i3 = FOLD_AND_RESCALE(kz[i], n3, pirange) / bin_size_z;
    int64_t bin = i1 + nbins1 * (i2 + nbins2 * i3);
    counts[bin]++;
  }
  std::vector<int64_t> offsets(num_bins);   // cumulative sum of bin counts
  offsets[0] = 0;     // do: offsets = [0 cumsum(counts(1:end-1)]
  for (int64_t i = 1; i < num_bins; i++)
    offsets[i] = offsets[i - 1] + counts[i-1];

  std::vector<int64_t> inv(num_points);           // fill inverse map
  for (int64_t i = 0; i < num_points; i++) {
    // find the bin index (again! but better than using RAM)
    int64_t i1 = FOLD_AND_RESCALE(kx[i], n1, pirange) / bin_size_x, i2 = 0, i3 = 0;
    if (isky) i2 = FOLD_AND_RESCALE(ky[i], n2, pirange) / bin_size_y;
    if (iskz) i3 = FOLD_AND_RESCALE(kz[i], n3, pirange) / bin_size_z;
    int64_t bin = i1 + nbins1 * (i2 + nbins2 * i3);
    int64_t offset = offsets[bin];
    offsets[bin]++;
    inv[i] = offset;
  }
  // invert the map, writing to output pointer (writing pattern is random)
  for (int64_t i = 0; i < num_points; i++)
    ret[inv[i]] = i;
}

// Mostly-OpenMP'ed version of bin_sort_singlethread.
// For documentation see: bin_sort_singlethread.
// Caution: when num_points (# NU pts) << N (# U pts), is SLOWER than single-thread.
// Barnett 2/8/18
// Explicit #threads control argument 7/20/20.
// Todo: if debug, print timing breakdowns.
template<typename FloatType>
void bin_sort_multithread(
    int64_t *ret, int64_t num_points, FloatType *kx, FloatType *ky, FloatType *kz,
    int64_t n1,int64_t n2,int64_t n3,int pirange,
    double bin_size_x, double bin_size_y, double bin_size_z, int debug,
    int num_threads) {
  bool isky = (n2 > 1), iskz = (n3 > 1);  // ky,kz avail? (cannot access if not)
  int64_t nbins1=n1 / bin_size_x + 1, nbins2, nbins3;  // see above note on why +1
  nbins2 = isky ? n2 / bin_size_y + 1 : 1;
  nbins3 = iskz ? n3 / bin_size_z + 1 : 1;
  int64_t num_bins = nbins1 * nbins2 * nbins3;
  if (num_threads == 0)
    fprintf(stderr, "[%s] num_threads (%d) must be positive!\n",
            __func__, num_threads);
  // handle case of less points than threads
  num_threads = std::min(num_points, (int64_t)num_threads);
  std::vector<int64_t> brk(num_threads+1);    // list of start NU pt indices per thread

  // distribute the NU pts to threads once & for all...
  for (int thread_index = 0; thread_index <= num_threads; ++thread_index)
    brk[thread_index] = (int64_t)(0.5 + num_points * thread_index / (double)num_threads);

  std::vector<int64_t> counts(num_bins, 0);     // global counts: # pts in each bin
  // offsets per thread, size num_threads * num_bins, init to 0 by copying the counts vec...
  std::vector< std::vector<int64_t> > ot(num_threads, counts);
  {    // scope for ct, the 2d array of counts in bins for each thread's NU pts
    std::vector< std::vector<int64_t> > ct(num_threads, counts);   // num_threads * num_bins, init to 0

    #pragma omp parallel num_threads(num_threads)
    {  // parallel binning to each thread's count. Block done once per thread
      int thread_index = OMP_GET_THREAD_NUM();
      //printf("\tt=%d: [%d,%d]\n",thread_index,jlo[thread_index],jhi[thread_index]);
      for (int64_t i = brk[thread_index]; i < brk[thread_index+1]; i++) {
        // find the bin index in however many dims are needed
        int64_t i1=FOLD_AND_RESCALE(kx[i],n1,pirange)/bin_size_x, i2=0, i3=0;
        if (isky) i2 = FOLD_AND_RESCALE(ky[i],n2,pirange)/bin_size_y;
        if (iskz) i3 = FOLD_AND_RESCALE(kz[i],n3,pirange)/bin_size_z;
        int64_t bin = i1+nbins1*(i2+nbins2*i3);
        ct[thread_index][bin]++;               // no clash btw threads
      }
    }
    // sum along thread axis to get global counts
    for (int64_t b = 0; b < num_bins; ++b)   // (not worth omp. Either loop order is ok)
      for (int thread_index = 0; thread_index < num_threads; ++thread_index)
	  counts[b] += ct[thread_index][b];

    std::vector<int64_t> offsets(num_bins);   // cumulative sum of bin counts
    // do: offsets = [0 cumsum(counts(1:end-1))] ...
    offsets[0] = 0;
    for (int64_t i = 1; i < num_bins; i++)
      offsets[i] = offsets[i-1] + counts[i-1];

    for (int64_t b = 0; b < num_bins; ++b)  // now build offsets for each thread & bin:
      ot[0][b] = offsets[b];                     // init
    for (int thread_index = 1; thread_index < num_threads; ++thread_index)   // (again not worth omp. Either loop order is ok)
      for (int64_t b = 0; b < num_bins; ++b)
	ot[thread_index][b] = ot[thread_index - 1][b]+ct[thread_index - 1][b];        // cumsum along thread_index axis

  }  // scope frees up ct here, before inv alloc

  std::vector<int64_t> inv(num_points);           // fill inverse map, in parallel
  #pragma omp parallel num_threads(num_threads)
  {
    int thread_index = OMP_GET_THREAD_NUM();
    for (int64_t i = brk[thread_index]; i < brk[thread_index+1]; i++) {
      // find the bin index (again! but better than using RAM)
      int64_t i1=FOLD_AND_RESCALE(kx[i], n1, pirange) / bin_size_x, i2=0, i3=0;
      if (isky) i2 = FOLD_AND_RESCALE(ky[i], n2, pirange) / bin_size_y;
      if (iskz) i3 = FOLD_AND_RESCALE(kz[i], n3, pirange) / bin_size_z;
      int64_t bin = i1 + nbins1 * (i2 + nbins2 * i3);
      inv[i] = ot[thread_index][bin];   // get the offset for this NU pt and thread
      ot[thread_index][bin]++;               // no clash
    }
  }
  // invert the map, writing to output pointer (writing pattern is random)
  #pragma omp parallel for num_threads(num_threads) schedule(dynamic,10000)
  for (int64_t i=0; i<num_points; i++)
    ret[inv[i]]=i;
}

static int get_transform_rank(int64_t n1, int64_t n2, int64_t n3) {
  int rank = 1;
  if (n2 > 1) ++rank;
  if (n3 > 1) ++rank;
  return rank;
}


// We macro because it has no FloatType args but gets compiled for both prec's...
template<typename FloatType>
void deconvolveshuffle1d(SpreadDirection dir, FloatType prefac,FloatType* ker, int64_t ms,
			 FloatType *fk, int64_t nf1, typename fftw::ComplexType<FloatType>::Type* fw, ModeOrder mode_order)
/*
  if dir == SpreadDirection::SPREAD: copies fw to fk with amplification by prefac/ker
  if dir == SpreadDirection::INTERP: copies fk to fw (and zero pads rest of it), same amplification.

  mode_order=0: use CMCL-compatible mode ordering in fk (from -N/2 up to N/2-1)
          1: use FFT-style (from 0 to N/2-1, then -N/2 up to -1).

  fk is size-ms FloatType complex array (2*ms FloatTypes alternating re,im parts)
  fw is a FFTW style complex array, ie FloatType [nf1][2], essentially FloatTypes
       alternating re,im parts.
  ker is real-valued FloatType array of length nf1/2+1.

  Single thread only, but shouldn't matter since mostly data movement.

  It has been tested that the repeated floating division in this inner loop
  only contributes at the <3% level in 3D relative to the fftw cost (8 threads).
  This could be removed by passing in an inverse kernel and doing mults.

  todo: rewrite w/ C++-complex I/O, check complex divide not slower than
        real divide, or is there a way to force a real divide?

  Barnett 1/25/17. Fixed ms=0 case 3/14/17. mode_order flag & clean 10/25/17
*/
{
  int64_t kmin = -ms/2, kmax = (ms-1)/2;    // inclusive range of k indices
  if (ms==0) kmax=-1;           // fixes zero-pad for trivial no-mode case
  // set up pp & pn as ptrs to start of pos(ie nonneg) & neg chunks of fk array
  int64_t pp = -2*kmin, pn = 0;       // CMCL mode-ordering case (2* since cmplx)
  if (mode_order==ModeOrder::FFT) { pp = 0; pn = 2*(kmax+1); }   // or, instead, FFT ordering
  if (dir == SpreadDirection::SPREAD) {    // read fw, write out to fk...
    for (int64_t k=0;k<=kmax;++k) {                    // non-neg freqs k
      fk[pp++] = prefac * fw[k][0] / ker[k];          // re
      fk[pp++] = prefac * fw[k][1] / ker[k];          // im
    }
    for (int64_t k=kmin;k<0;++k) {                     // neg freqs k
      fk[pn++] = prefac * fw[nf1+k][0] / ker[-k];     // re
      fk[pn++] = prefac * fw[nf1+k][1] / ker[-k];     // im
    }
  } else {    // read fk, write out to fw w/ zero padding...
    for (int64_t k=kmax+1; k<nf1+kmin; ++k) {  // zero pad precisely where needed
      fw[k][0] = fw[k][1] = 0.0; }
    for (int64_t k=0;k<=kmax;++k) {                    // non-neg freqs k
      fw[k][0] = prefac * fk[pp++] / ker[k];          // re
      fw[k][1] = prefac * fk[pp++] / ker[k];          // im
    }
    for (int64_t k=kmin;k<0;++k) {                     // neg freqs k
      fw[nf1+k][0] = prefac * fk[pn++] / ker[-k];     // re
      fw[nf1+k][1] = prefac * fk[pn++] / ker[-k];     // im
    }
  }
}

template<typename FloatType>
void deconvolveshuffle2d(SpreadDirection dir,FloatType prefac,FloatType *ker1, FloatType *ker2,
			 int64_t ms, int64_t mt,
			 FloatType *fk, int64_t nf1, int64_t nf2, typename fftw::ComplexType<FloatType>::Type* fw,
			 ModeOrder mode_order)
/*
  2D version of deconvolveshuffle1d, calls it on each x-line using 1/ker2 fac.

  if dir == SpreadDirection::SPREAD: copies fw to fk with amplification by prefac/(ker1(k1)*ker2(k2)).
  if dir == SpreadDirection::INTERP: copies fk to fw (and zero pads rest of it), same amplification.

  mode_order=0: use CMCL-compatible mode ordering in fk (each rank increasing)
          1: use FFT-style (pos then negative, on each rank)

  fk is complex array stored as 2*ms*mt FloatTypes alternating re,im parts, with
    ms looped over fast and mt slow.
  fw is a FFTW style complex array, ie FloatType [nf1*nf2][2], essentially FloatTypes
       alternating re,im parts; again nf1 is fast and nf2 slow.
  ker1, ker2 are real-valued FloatType arrays of lengths nf1/2+1, nf2/2+1
       respectively.

  Barnett 2/1/17, Fixed mt=0 case 3/14/17. mode_order 10/25/17
*/
{
  int64_t k2min = -mt/2, k2max = (mt-1)/2;    // inclusive range of k2 indices
  if (mt==0) k2max=-1;           // fixes zero-pad for trivial no-mode case
  // set up pp & pn as ptrs to start of pos(ie nonneg) & neg chunks of fk array
  int64_t pp = -2*k2min*ms, pn = 0;   // CMCL mode-ordering case (2* since cmplx)
  if (mode_order == ModeOrder::FFT) { pp = 0; pn = 2*(k2max+1)*ms; }  // or, instead, FFT ordering
  if (dir == SpreadDirection::INTERP)               // zero pad needed x-lines (contiguous in memory)
    for (int64_t j=nf1*(k2max+1); j<nf1*(nf2+k2min); ++j)  // sweeps all dims
      fw[j][0] = fw[j][1] = 0.0;
  for (int64_t k2=0;k2<=k2max;++k2, pp+=2*ms)          // non-neg y-freqs
    // point fk and fw to the start of this y value's row (2* is for complex):
    deconvolveshuffle1d(dir,prefac/ker2[k2],ker1,ms,fk + pp,nf1,&fw[nf1*k2],mode_order);
  for (int64_t k2=k2min;k2<0;++k2, pn+=2*ms)           // neg y-freqs
    deconvolveshuffle1d(dir,prefac/ker2[-k2],ker1,ms,fk + pn,nf1,&fw[nf1*(nf2+k2)],mode_order);
}

template<typename FloatType>
void deconvolveshuffle3d(SpreadDirection dir,FloatType prefac,FloatType *ker1, FloatType *ker2,
			 FloatType *ker3, int64_t ms, int64_t mt, int64_t mu,
			 FloatType *fk, int64_t nf1, int64_t nf2, int64_t nf3,
			 typename fftw::ComplexType<FloatType>::Type* fw, ModeOrder mode_order)
/*
  3D version of deconvolveshuffle2d, calls it on each xy-plane using 1/ker3 fac.

  if dir == SpreadDirection::SPREAD: copies fw to fk with ampl by prefac/(ker1(k1)*ker2(k2)*ker3(k3)).
  if dir == SpreadDirection::INTERP: copies fk to fw (and zero pads rest of it), same amplification.

  mode_order=0: use CMCL-compatible mode ordering in fk (each rank increasing)
          1: use FFT-style (pos then negative, on each rank)

  fk is complex array stored as 2*ms*mt*mu FloatTypes alternating re,im parts, with
    ms looped over fastest and mu slowest.
  fw is a FFTW style complex array, ie FloatType [nf1*nf2*nf3][2], effectively
       FloatTypes alternating re,im parts; again nf1 is fastest and nf3 slowest.
  ker1, ker2, ker3 are real-valued FloatType arrays of lengths nf1/2+1, nf2/2+1,
       and nf3/2+1 respectively.

  Barnett 2/1/17, Fixed mu=0 case 3/14/17. mode_order 10/25/17
*/
{
  int64_t k3min = -mu/2, k3max = (mu-1)/2;    // inclusive range of k3 indices
  if (mu==0) k3max=-1;           // fixes zero-pad for trivial no-mode case
  // set up pp & pn as ptrs to start of pos(ie nonneg) & neg chunks of fk array
  int64_t pp = -2*k3min*ms*mt, pn = 0; // CMCL mode-ordering (2* since cmplx)
  if (mode_order == ModeOrder::FFT) { pp = 0; pn = 2*(k3max+1)*ms*mt; }  // or FFT ordering
  int64_t np = nf1*nf2;  // # pts in an upsampled Fourier xy-plane
  if (dir == SpreadDirection::INTERP)           // zero pad needed xy-planes (contiguous in memory)
    for (int64_t j=np*(k3max+1);j<np*(nf3+k3min);++j)  // sweeps all dims
      fw[j][0] = fw[j][1] = 0.0;
  for (int64_t k3=0;k3<=k3max;++k3, pp+=2*ms*mt)      // non-neg z-freqs
    // point fk and fw to the start of this z value's plane (2* is for complex):
    deconvolveshuffle2d(dir,prefac/ker3[k3],ker1,ker2,ms,mt,
			fk + pp,nf1,nf2,&fw[np*k3],mode_order);
  for (int64_t k3=k3min;k3<0;++k3, pn+=2*ms*mt)       // neg z-freqs
    deconvolveshuffle2d(dir,prefac/ker3[-k3],ker1,ker2,ms,mt,
			fk + pn,nf1,nf2,&fw[np*(nf3+k3)],mode_order);
}

template<typename FloatType>
int spreadinterpSorted(int64_t* sort_indices, int64_t N1, int64_t N2, int64_t N3,
		      FloatType *data_uniform, int64_t M, FloatType *kx, FloatType *ky, FloatType *kz,
		      FloatType *data_nonuniform, SpreadParameters<FloatType> opts, int did_sort)
/* Logic to select the main spreading (dir=1) vs interpolation (dir=2) routine.
   See spreadinterp() above for inputs arguments and definitions.
   Return value should always be 0 (no error reporting).
   Split out by Melody Shih, Jun 2018; renamed Barnett 5/20/20.
*/
{
  if (opts.spread_direction == SpreadDirection::SPREAD)
    spreadSorted(sort_indices, N1, N2, N3, data_uniform, M, kx, ky, kz, data_nonuniform, opts, did_sort);
  else // if (opts.spread_direction == SpreadDirection::INTERP)
    interpSorted(sort_indices, N1, N2, N3, data_uniform, M, kx, ky, kz, data_nonuniform, opts, did_sort);

  return 0;
}


// --------------------------------------------------------------------------
template<typename FloatType>
int spreadSorted(int64_t* sort_indices,int64_t N1, int64_t N2, int64_t N3,
		      FloatType *data_uniform,int64_t M, FloatType *kx, FloatType *ky, FloatType *kz,
		      FloatType *data_nonuniform, SpreadParameters<FloatType> opts, int did_sort)
// Spread NU pts in sorted order to a uniform grid. See spreadinterp() for doc.
{
  int ndims = get_transform_rank(N1,N2,N3);
  int64_t N=N1*N2*N3;            // output array size
  int ns=opts.kernel_width;          // abbrev. for w, kernel width
  int nthr = OMP_GET_MAX_THREADS();  // # threads to use to spread
  if (opts.num_threads>0)
    nthr = std::min(nthr,opts.num_threads);     // user override up to max avail

  for (int64_t i=0; i<2*N; i++) // zero the output array. std::fill is no faster
    data_uniform[i]=0.0;

  // If there are no non-uniform points, we're done.
  if (M == 0) return 0;

  int spread_single = (nthr==1) || (M*100<N);     // low-density heuristic?
  spread_single = 0;                 // for now
  if (spread_single) {    // ------- Basic single-core t1 spreading ------
    for (int64_t j=0; j<M; j++) {
      // *** todo, not urgent
      // ... (question is: will the index wrapping per NU pt slow it down?)
    }

  } else {           // ------- Fancy multi-core blocked t1 spreading ----
                     // Splits sorted inds (jfm's advanced2), could double RAM.
    // choose nb (# subprobs) via used num_threads:
    int nb = std::min((int64_t)nthr,M);         // simply split one subprob per thr...
    if (nb*(int64_t)opts.max_subproblem_size<M) {  // ...or more subprobs to cap size
      nb = 1 + (M-1)/opts.max_subproblem_size;  // int div does ceil(M/opts.max_subproblem_size)
      if (opts.verbosity) printf("\tcapping subproblem sizes to max of %d\n",opts.max_subproblem_size);
    }
    if (M*1000<N) {         // low-density heuristic: one thread per NU pt!
      nb = M;
      if (opts.verbosity) printf("\tusing low-density speed rescue nb=M...\n");
    }
    if (!did_sort && nthr==1) {
      nb = 1;
      if (opts.verbosity) printf("\tunsorted nthr=1: forcing single subproblem...\n");
    }

    std::vector<int64_t> brk(nb+1); // NU index breakpoints defining nb subproblems
    for (int p = 0; p <= nb; ++p)
      brk[p] = (int64_t)(0.5 + M * p / (double)nb);

    #pragma omp parallel for num_threads(nthr) schedule(dynamic,1)  // each is big
    for (int isub=0; isub<nb; isub++) {   // Main loop through the subproblems
      int64_t M0 = brk[isub+1]-brk[isub];  // # NU pts in this subproblem
      // copy the location and data vectors for the nonuniform points
      FloatType *kx0=(FloatType*)malloc(sizeof(FloatType)*M0), *ky0=nullptr, *kz0=nullptr;
      if (N2>1)
        ky0=(FloatType*)malloc(sizeof(FloatType)*M0);
      if (N3>1)
        kz0=(FloatType*)malloc(sizeof(FloatType)*M0);
      FloatType *dd0=(FloatType*)malloc(sizeof(FloatType)*M0*2);    // complex strength data
      for (int64_t j=0; j<M0; j++) {           // todo: can avoid this copying?
        int64_t kk=sort_indices[j+brk[isub]];  // NU pt from subprob index list
        kx0[j]=FOLD_AND_RESCALE(kx[kk],N1,opts.pirange);
        if (N2>1) ky0[j]=FOLD_AND_RESCALE(ky[kk],N2,opts.pirange);
        if (N3>1) kz0[j]=FOLD_AND_RESCALE(kz[kk],N3,opts.pirange);
        dd0[j*2]=data_nonuniform[kk*2];     // real part
        dd0[j*2+1]=data_nonuniform[kk*2+1]; // imag part
      }
      // get the subgrid which will include padding by roughly kernel_width/2
      int64_t offset1,offset2,offset3,size1,size2,size3; // get_subgrid sets
      get_subgrid(offset1,offset2,offset3,size1,size2,size3,M0,kx0,ky0,kz0,ns,ndims);  // sets offsets and sizes

      // allocate output data for this subgrid
      FloatType *du0=(FloatType*)malloc(sizeof(FloatType)*2*size1*size2*size3); // complex

      // Spread to subgrid without need for bounds checking or wrapping
      if (ndims==1)
        spread_subproblem_1d(offset1,size1,du0,M0,kx0,dd0,opts);
      else if (ndims==2)
        spread_subproblem_2d(offset1,offset2,size1,size2,du0,M0,kx0,ky0,dd0,opts);
      else
        spread_subproblem_3d(offset1,offset2,offset3,size1,size2,size3,du0,M0,kx0,ky0,kz0,dd0,opts);

      // do the adding of subgrid to output
      if (nthr > opts.atomic_threshold)   // see above for debug reporting
        add_wrapped_subgrid_thread_safe(offset1,offset2,offset3,size1,size2,size3,N1,N2,N3,data_uniform,du0);   // R Blackwell's atomic version
      else {
        #pragma omp critical
        add_wrapped_subgrid(offset1,offset2,offset3,size1,size2,size3,N1,N2,N3,data_uniform,du0);
      }

      // free up stuff from this subprob... (that was malloc'ed by hand)
      free(dd0);
      free(du0);
      free(kx0);
      if (N2 > 1) free(ky0);
      if (N3 > 1) free(kz0);
    }     // end main loop over subprobs
  }   // end of choice of which t1 spread type to use

  // in spread/interp only mode, apply scaling factor (Montalt 6/8/2021).
  if (opts.spread_only) {
    for (int64_t i = 0; i < 2*N; i++)
      data_uniform[i] *= opts.kernel_scale;
  }

  return 0;
};


// --------------------------------------------------------------------------
template<typename FloatType>
int interpSorted(int64_t* sort_indices,int64_t N1, int64_t N2, int64_t N3,
		      FloatType *data_uniform,int64_t M, FloatType *kx, FloatType *ky, FloatType *kz,
		      FloatType *data_nonuniform, SpreadParameters<FloatType> opts, int did_sort)
// Interpolate to NU pts in sorted order from a uniform grid.
// See spreadinterp() for doc.
{
  int ndims = get_transform_rank(N1,N2,N3);
  int ns=opts.kernel_width;          // abbrev. for w, kernel width
  FloatType ns2 = (FloatType)ns/2;          // half spread width, used as stencil shift
  int nthr = OMP_GET_MAX_THREADS();   // # threads to use to interp
  if (opts.num_threads > 0)
    nthr = std::min(nthr, opts.num_threads);

  #pragma omp parallel num_threads(nthr)
  {
    #define CHUNK_SIZE 16     // Chunks of Type 2 targets (Ludvig found by expt)
    int64_t jlist[CHUNK_SIZE];
    FloatType xjlist[CHUNK_SIZE], yjlist[CHUNK_SIZE], zjlist[CHUNK_SIZE];
    FloatType outbuf[2 * CHUNK_SIZE];
    // Kernels: static alloc is faster, so we do it for up to 3D...
    FloatType kernel_args[3 * MAX_KERNEL_WIDTH];
    FloatType kernel_values[3 * MAX_KERNEL_WIDTH];
    FloatType *ker1 = kernel_values;
    FloatType *ker2 = kernel_values + ns;
    FloatType *ker3 = kernel_values + 2 * ns;

    // Loop over interpolation chunks
    #pragma omp for schedule (dynamic,1000)  // assign threads to NU targ pts:
    for (int64_t i=0; i<M; i+=CHUNK_SIZE) { // main loop over NU targs, interp each from U
      // Setup buffers for this chunk
      int bufsize = (i+CHUNK_SIZE > M) ? M-i : CHUNK_SIZE;
      for (int ibuf=0; ibuf<bufsize; ibuf++) {
        int64_t j = sort_indices[i+ibuf];
        jlist[ibuf] = j;
        xjlist[ibuf] = FOLD_AND_RESCALE(kx[j],N1,opts.pirange);
        if(ndims >=2)
          yjlist[ibuf] = FOLD_AND_RESCALE(ky[j],N2,opts.pirange);
        if(ndims == 3)
          zjlist[ibuf] = FOLD_AND_RESCALE(kz[j],N3,opts.pirange);
      }

      // Loop over targets in chunk
      for (int ibuf=0; ibuf<bufsize; ibuf++) {
        FloatType xj = xjlist[ibuf];
        FloatType yj = (ndims > 1) ? yjlist[ibuf] : 0;
        FloatType zj = (ndims > 2) ? zjlist[ibuf] : 0;

        FloatType *target = outbuf+2*ibuf;

        // coords (x,y,z), spread block corner index (i1,i2,i3) of current NU targ
        int64_t i1=(int64_t)std::ceil(xj-ns2); // leftmost grid index
        int64_t i2= (ndims > 1) ? (int64_t)std::ceil(yj-ns2) : 0; // min y grid index
        int64_t i3= (ndims > 1) ? (int64_t)std::ceil(zj-ns2) : 0; // min z grid index

        FloatType x1=(FloatType)i1-xj;           // shift of ker center, in [-w/2,-w/2+1]
        FloatType x2= (ndims > 1) ? (FloatType)i2-yj : 0 ;
        FloatType x3= (ndims > 2)? (FloatType)i3-zj : 0;

        // eval kernel values patch and use to interpolate from uniform data...
        if (opts.kerevalmeth==0) {               // choose eval method
          set_kernel_args(kernel_args, x1, opts);
          if(ndims > 1)  set_kernel_args(kernel_args+ns, x2, opts);
          if(ndims > 2)  set_kernel_args(kernel_args+2*ns, x3, opts);

          evaluate_kernel_vector(kernel_values, kernel_args, opts, ndims*ns);
        } else {
          eval_kernel_vec_Horner(ker1,x1,ns,opts);
          if (ndims > 1) eval_kernel_vec_Horner(ker2,x2,ns,opts);
          if (ndims > 2) eval_kernel_vec_Horner(ker3,x3,ns,opts);
        }

        switch (ndims) {
          case 1:
            interp_line(target,data_uniform,ker1,i1,N1,ns);
            break;
          case 2:
            interp_square(target,data_uniform,ker1,ker2,i1,i2,N1,N2,ns);
            break;
          case 3:
            interp_cube(target,data_uniform,ker1,ker2,ker3,i1,i2,i3,N1,N2,N3,ns);
            break;
          default: //can't get here
            break;
        }

        // in spread/interp only mode, apply scaling factor (Montalt 6/8/2021).
        if (opts.spread_only) {
          target[0] *= opts.kernel_scale;
          target[1] *= opts.kernel_scale;
        }
      }  // end loop over targets in chunk

      // Copy result buffer to output array
      for (int ibuf=0; ibuf<bufsize; ibuf++) {
        int64_t j = jlist[ibuf];
        data_nonuniform[2*j] = outbuf[2*ibuf];
        data_nonuniform[2*j+1] = outbuf[2*ibuf+1];
      }

    }  // end NU targ loop
  }  // end parallel section

  return 0;
};

///////////////////////////////////////////////////////////////////////////

template<typename FloatType>
static inline void set_kernel_args(FloatType *args, FloatType x, const SpreadParameters<FloatType>& opts)
// Fills vector args[] with kernel arguments x, x+1, ..., x+ns-1.
// needed for the vectorized kernel eval of Ludvig af K.
{
  int ns=opts.kernel_width;
  for (int i=0; i<ns; i++)
    args[i] = x + (FloatType) i;
}

template<typename FloatType>
static inline void evaluate_kernel_vector(FloatType *ker, FloatType *args, const SpreadParameters<FloatType>& opts, const int N)
/* Evaluate ES kernel for a vector of N arguments; by Ludvig af K.
   If opts.pad_kernel true, args and ker must be allocated for Npad, and args is
   written to (to pad to length Npad), only first N outputs are correct.
   Barnett 4/24/18 option to pad to mult of 4 for better SIMD vectorization.

   Obsolete (replaced by Horner), but keep around for experimentation since
   works for arbitrary beta. Formula must match reference implementation. */
{
  FloatType b = opts.kernel_beta;
  FloatType c = opts.kernel_c;

  // Note (by Ludvig af K): Splitting kernel evaluation into two loops
  // seems to benefit auto-vectorization.
  // gcc 5.4 vectorizes first loop; gcc 7.2 vectorizes both loops
  int Npad = N;
  if (opts.pad_kernel) {        // since always same branch, no speed hit
    Npad = 4*(1+(N-1)/4);   // pad N to mult of 4; help i7 GCC, not xeon
    for (int i=N;i<Npad;++i)    // pad with 1-3 zeros for safe eval
      args[i] = 0.0;
  }

  // Loop 1: Compute exponential arguments.
  for (int i = 0; i < Npad; i++) {
    ker[i] = b * sqrt(1.0 - c * args[i] * args[i]);
  }
  // Loop 2: Compute exponentials.
  for (int i = 0; i < Npad; i++) {
  	ker[i] = exp(ker[i]);
  }
  // Separate check from arithmetic (Is this really needed? doesn't slow down)
  for (int i = 0; i < N; i++) {
    if (abs(args[i])>=opts.kernel_half_width) ker[i] = 0.0;
  }
}

template<typename FloatType>
static inline void eval_kernel_vec_Horner(FloatType *ker, const FloatType x, const int w,
					  const SpreadParameters<FloatType> &opts)
/* Fill ker[] with Horner piecewise poly approx to [-w/2,w/2] ES kernel eval at
   x_j = x + j,  for j=0,..,w-1.  Thus x in [-w/2,-w/2+1].   w is aka ns.
   This is the current evaluation method, since it's faster (except i7 w=16).
   Two upsampfacs implemented. Params must match ref formula. Barnett 4/24/18 */
{
  FloatType z = 2 * x + w - 1.0;         // scale so local grid offset z in [-1,1]
  // insert the auto-generated code which expects z, w args, writes to ker...
  if (opts.upsampling_factor == 2.0) {     // floating point equality is fine here
    #include "kernel_horner_sigma2.inc"
  } else if (opts.upsampling_factor == 1.25) {
    #include "kernel_horner_sigma125.inc"
  } else
    fprintf(stderr,"%s: unknown upsampling_factor, failed!\n",__func__);
}

template<typename FloatType>
void interp_line(FloatType *target,FloatType *du, FloatType *ker,int64_t i1,int64_t N1,int ns)
// 1D interpolate complex values from du array to out, using real weights
// ker[0] through ker[ns-1]. out must be size 2 (real,imag), and du
// of size 2*N1 (alternating real,imag). i1 is the left-most index in [0,N1)
// Periodic wrapping in the du array is applied, assuming N1>=ns.
// dx is index into ker array, j index in complex du (data_uniform) array.
// Barnett 6/15/17
{
  FloatType out[] = {0.0, 0.0};
  int64_t j = i1;
  if (i1<0) {                               // wraps at left
    j+=N1;
    for (int dx=0; dx<-i1; ++dx) {
      out[0] += du[2*j]*ker[dx];
      out[1] += du[2*j+1]*ker[dx];
      ++j;
    }
    j-=N1;
    for (int dx=-i1; dx<ns; ++dx) {
      out[0] += du[2*j]*ker[dx];
      out[1] += du[2*j+1]*ker[dx];
      ++j;
    }
  } else if (i1+ns>=N1) {                    // wraps at right
    for (int dx=0; dx<N1-i1; ++dx) {
      out[0] += du[2*j]*ker[dx];
      out[1] += du[2*j+1]*ker[dx];
      ++j;
    }
    j-=N1;
    for (int dx=N1-i1; dx<ns; ++dx) {
      out[0] += du[2*j]*ker[dx];
      out[1] += du[2*j+1]*ker[dx];
      ++j;
    }
  } else {                                     // doesn't wrap
    for (int dx=0; dx<ns; ++dx) {
      out[0] += du[2*j]*ker[dx];
      out[1] += du[2*j+1]*ker[dx];
      ++j;
    }
  }
  target[0] = out[0];
  target[1] = out[1];
}

template<typename FloatType>
void interp_square(FloatType *target,FloatType *du, FloatType *ker1, FloatType *ker2, int64_t i1,int64_t i2,int64_t N1,int64_t N2,int ns)
// 2D interpolate complex values from du (uniform grid data) array to out value,
// using ns*ns square of real weights
// in ker. out must be size 2 (real,imag), and du
// of size 2*N1*N2 (alternating real,imag). i1 is the left-most index in [0,N1)
// and i2 the bottom index in [0,N2).
// Periodic wrapping in the du array is applied, assuming N1,N2>=ns.
// dx,dy indices into ker array, j index in complex du array.
// Barnett 6/16/17
{
  FloatType out[] = {0.0, 0.0};
  if (i1>=0 && i1+ns<=N1 && i2>=0 && i2+ns<=N2) {  // no wrapping: avoid ptrs
    for (int dy=0; dy<ns; dy++) {
      int64_t j = N1*(i2+dy) + i1;
      for (int dx=0; dx<ns; dx++) {
	FloatType k = ker1[dx]*ker2[dy];
	out[0] += du[2*j] * k;
	out[1] += du[2*j+1] * k;
	++j;
      }
    }
  } else {                         // wraps somewhere: use ptr list (slower)
    int64_t j1[MAX_KERNEL_WIDTH], j2[MAX_KERNEL_WIDTH];   // 1d ptr lists
    int64_t x=i1, y=i2;                 // initialize coords
    for (int d=0; d<ns; d++) {         // set up ptr lists
      if (x<0) x+=N1;
      if (x>=N1) x-=N1;
      j1[d] = x++;
      if (y<0) y+=N2;
      if (y>=N2) y-=N2;
      j2[d] = y++;
    }
    for (int dy=0; dy<ns; dy++) {      // use the pts lists
      int64_t oy = N1*j2[dy];           // offset due to y
      for (int dx=0; dx<ns; dx++) {
	FloatType k = ker1[dx]*ker2[dy];
	int64_t j = oy + j1[dx];
	out[0] += du[2*j] * k;
	out[1] += du[2*j+1] * k;
      }
    }
  }
  target[0] = out[0];
  target[1] = out[1];
}

template<typename FloatType>
void interp_cube(FloatType *target,FloatType *du, FloatType *ker1, FloatType *ker2, FloatType *ker3,
		 int64_t i1,int64_t i2,int64_t i3, int64_t N1,int64_t N2,int64_t N3,int ns)
// 3D interpolate complex values from du (uniform grid data) array to out value,
// using ns*ns*ns cube of real weights
// in ker. out must be size 2 (real,imag), and du
// of size 2*N1*N2*N3 (alternating real,imag). i1 is the left-most index in
// [0,N1), i2 the bottom index in [0,N2), i3 lowest in [0,N3).
// Periodic wrapping in the du array is applied, assuming N1,N2,N3>=ns.
// dx,dy,dz indices into ker array, j index in complex du array.
// Barnett 6/16/17
{
  FloatType out[] = {0.0, 0.0};
  if (i1>=0 && i1+ns<=N1 && i2>=0 && i2+ns<=N2 && i3>=0 && i3+ns<=N3) {
    // no wrapping: avoid ptrs
    for (int dz=0; dz<ns; dz++) {
      int64_t oz = N1*N2*(i3+dz);        // offset due to z
      for (int dy=0; dy<ns; dy++) {
	int64_t j = oz + N1*(i2+dy) + i1;
	FloatType ker23 = ker2[dy]*ker3[dz];
	for (int dx=0; dx<ns; dx++) {
	  FloatType k = ker1[dx]*ker23;
	  out[0] += du[2*j] * k;
	  out[1] += du[2*j+1] * k;
	  ++j;
	}
      }
    }
  } else {                         // wraps somewhere: use ptr list (slower)
    int64_t j1[MAX_KERNEL_WIDTH], j2[MAX_KERNEL_WIDTH], j3[MAX_KERNEL_WIDTH];   // 1d ptr lists
    int64_t x=i1, y=i2, z=i3;         // initialize coords
    for (int d=0; d<ns; d++) {          // set up ptr lists
      if (x<0) x+=N1;
      if (x>=N1) x-=N1;
      j1[d] = x++;
      if (y<0) y+=N2;
      if (y>=N2) y-=N2;
      j2[d] = y++;
      if (z<0) z+=N3;
      if (z>=N3) z-=N3;
      j3[d] = z++;
    }
    for (int dz=0; dz<ns; dz++) {             // use the pts lists
      int64_t oz = N1*N2*j3[dz];               // offset due to z
      for (int dy=0; dy<ns; dy++) {
	int64_t oy = oz + N1*j2[dy];           // offset due to y & z
	FloatType ker23 = ker2[dy]*ker3[dz];
	for (int dx=0; dx<ns; dx++) {
	  FloatType k = ker1[dx]*ker23;
	  int64_t j = oy + j1[dx];
	  out[0] += du[2*j] * k;
	  out[1] += du[2*j+1] * k;
	}
      }
    }
  }
  target[0] = out[0];
  target[1] = out[1];
}

template<typename FloatType>
void spread_subproblem_1d(int64_t off1, int64_t size1,FloatType *du,int64_t M,
			  FloatType *kx,FloatType *dd, const SpreadParameters<FloatType>& opts)
/* 1D spreader from nonuniform to uniform subproblem grid, without wrapping.
   Inputs:
   off1 - integer offset of left end of du subgrid from that of overall fine
          periodized output grid {0,1,..N-1}.
   size1 - integer length of output subgrid du
   M - number of NU pts in subproblem
   kx (length M) - are rescaled NU source locations, should lie in
                   [off1+ns/2,off1+size1-1-ns/2] so as kernels stay in bounds
   dd (length M complex, interleaved) - source strengths
   Outputs:
   du (length size1 complex, interleaved) - preallocated uniform subgrid array

   The reason periodic wrapping is avoided in subproblems is speed: avoids
   conditionals, indirection (pointers), and integer mod. Originally 2017.
   Kernel eval mods by Ludvig al Klinteberg.
   Fixed so rounding to integer grid consistent w/ get_subgrid, prevents
   chance of segfault when epsmach*N1>O(1), assuming max() and ceil() commute.
   This needed off1 as extra arg. AHB 11/30/20.
*/
{
  int ns=opts.kernel_width;          // a.k.a. w
  FloatType ns2 = (FloatType)ns/2;          // half spread width
  for (int64_t i=0;i<2*size1;++i)         // zero output
    du[i] = 0.0;
  FloatType kernel_args[MAX_KERNEL_WIDTH];
  FloatType ker[MAX_KERNEL_WIDTH];
  for (int64_t i=0; i<M; i++) {           // loop over NU pts
    FloatType re0 = dd[2*i];
    FloatType im0 = dd[2*i+1];
    // ceil offset, hence rounding, must match that in get_subgrid...
    int64_t i1 = (int64_t)std::ceil(kx[i] - ns2);    // fine grid start index
    FloatType x1 = (FloatType)i1 - kx[i];            // x1 in [-w/2,-w/2+1], up to rounding
    // However if N1*epsmach>O(1) then can cause O(1) errors in x1, hence ppoly
    // kernel evaluation will fall outside their designed domains, >>1 errors.
    // This can only happen if the overall error would be O(1) anyway. Clip x1??
    if (x1<-ns2) x1=-ns2;
    if (x1>-ns2+1) x1=-ns2+1;   // ***
    if (opts.kerevalmeth==0) {          // faster Horner poly method
      set_kernel_args(kernel_args, x1, opts);
      evaluate_kernel_vector(ker, kernel_args, opts, ns);
    } else
      eval_kernel_vec_Horner(ker,x1,ns,opts);
    int64_t j = i1-off1;    // offset rel to subgrid, starts the output indices
    // critical inner loop:
    for (int dx=0; dx<ns; ++dx) {
      FloatType k = ker[dx];
      du[2*j] += re0*k;
      du[2*j+1] += im0*k;
      ++j;
    }
  }
}

template<typename FloatType>
void spread_subproblem_2d(int64_t off1,int64_t off2,int64_t size1,int64_t size2,
                          FloatType *du,int64_t M, FloatType *kx,FloatType *ky,FloatType *dd,
			  const SpreadParameters<FloatType>& opts)
/* spreader from dd (NU) to du (uniform) in 2D without wrapping.
   See above docs/notes for spread_subproblem_2d.
   kx,ky (size M) are NU locations in [off+ns/2,off+size-1-ns/2] in both dims.
   dd (size M complex) are complex source strengths
   du (size size1*size2) is complex uniform output array
 */
{
  int ns=opts.kernel_width;
  FloatType ns2 = (FloatType)ns/2;          // half spread width
  for (int64_t i=0;i<2*size1*size2;++i)
    du[i] = 0.0;
  FloatType kernel_args[2*MAX_KERNEL_WIDTH];
  // Kernel values stored in consecutive memory. This allows us to compute
  // values in two directions in a single kernel evaluation call.
  FloatType kernel_values[2*MAX_KERNEL_WIDTH];
  FloatType *ker1 = kernel_values;
  FloatType *ker2 = kernel_values + ns;
  for (int64_t i=0; i<M; i++) {           // loop over NU pts
    FloatType re0 = dd[2*i];
    FloatType im0 = dd[2*i+1];
    // ceil offset, hence rounding, must match that in get_subgrid...
    int64_t i1 = (int64_t)std::ceil(kx[i] - ns2);   // fine grid start indices
    int64_t i2 = (int64_t)std::ceil(ky[i] - ns2);
    FloatType x1 = (FloatType)i1 - kx[i];
    FloatType x2 = (FloatType)i2 - ky[i];
    if (opts.kerevalmeth==0) {          // faster Horner poly method
      set_kernel_args(kernel_args, x1, opts);
      set_kernel_args(kernel_args+ns, x2, opts);
      evaluate_kernel_vector(kernel_values, kernel_args, opts, 2*ns);
    } else {
      eval_kernel_vec_Horner(ker1,x1,ns,opts);
      eval_kernel_vec_Horner(ker2,x2,ns,opts);
    }
    // Combine kernel with complex source value to simplify inner loop
    FloatType ker1val[2*MAX_KERNEL_WIDTH];    // here 2* is because of complex
    for (int i = 0; i < ns; i++) {
      ker1val[2*i] = re0*ker1[i];
      ker1val[2*i+1] = im0*ker1[i];
    }
    // critical inner loop:
    for (int dy=0; dy<ns; ++dy) {
      int64_t j = size1*(i2-off2+dy) + i1-off1;   // should be in subgrid
      FloatType kerval = ker2[dy];
      FloatType *trg = du+2*j;
      for (int dx=0; dx<2*ns; ++dx) {
	trg[dx] += kerval*ker1val[dx];
      }
    }
  }
}

template<typename FloatType>
void spread_subproblem_3d(int64_t off1,int64_t off2,int64_t off3,int64_t size1,
                          int64_t size2,int64_t size3,FloatType *du,int64_t M,
			  FloatType *kx,FloatType *ky,FloatType *kz,FloatType *dd,
			  const SpreadParameters<FloatType>& opts)
/* spreader from dd (NU) to du (uniform) in 3D without wrapping.
   See above docs/notes for spread_subproblem_2d.
   kx,ky,kz (size M) are NU locations in [off+ns/2,off+size-1-ns/2] in each rank.
   dd (size M complex) are complex source strengths
   du (size size1*size2*size3) is uniform complex output array
 */
{
  int ns=opts.kernel_width;
  FloatType ns2 = (FloatType)ns/2;          // half spread width
  for (int64_t i=0;i<2*size1*size2*size3;++i)
    du[i] = 0.0;
  FloatType kernel_args[3*MAX_KERNEL_WIDTH];
  // Kernel values stored in consecutive memory. This allows us to compute
  // values in all three directions in a single kernel evaluation call.
  FloatType kernel_values[3*MAX_KERNEL_WIDTH];
  FloatType *ker1 = kernel_values;
  FloatType *ker2 = kernel_values + ns;
  FloatType *ker3 = kernel_values + 2*ns;
  for (int64_t i=0; i<M; i++) {           // loop over NU pts
    FloatType re0 = dd[2*i];
    FloatType im0 = dd[2*i+1];
    // ceil offset, hence rounding, must match that in get_subgrid...
    int64_t i1 = (int64_t)std::ceil(kx[i] - ns2);   // fine grid start indices
    int64_t i2 = (int64_t)std::ceil(ky[i] - ns2);
    int64_t i3 = (int64_t)std::ceil(kz[i] - ns2);
    FloatType x1 = (FloatType)i1 - kx[i];
    FloatType x2 = (FloatType)i2 - ky[i];
    FloatType x3 = (FloatType)i3 - kz[i];
    if (opts.kerevalmeth==0) {          // faster Horner poly method
      set_kernel_args(kernel_args, x1, opts);
      set_kernel_args(kernel_args+ns, x2, opts);
      set_kernel_args(kernel_args+2*ns, x3, opts);
      evaluate_kernel_vector(kernel_values, kernel_args, opts, 3*ns);
    } else {
      eval_kernel_vec_Horner(ker1,x1,ns,opts);
      eval_kernel_vec_Horner(ker2,x2,ns,opts);
      eval_kernel_vec_Horner(ker3,x3,ns,opts);
    }
    // Combine kernel with complex source value to simplify inner loop
    FloatType ker1val[2*MAX_KERNEL_WIDTH];    // here 2* is because of complex
    for (int i = 0; i < ns; i++) {
      ker1val[2*i] = re0*ker1[i];
      ker1val[2*i+1] = im0*ker1[i];
    }
    // critical inner loop:
    for (int dz=0; dz<ns; ++dz) {
      int64_t oz = size1*size2*(i3-off3+dz);        // offset due to z
      for (int dy=0; dy<ns; ++dy) {
	int64_t j = oz + size1*(i2-off2+dy) + i1-off1;   // should be in subgrid
	FloatType kerval = ker2[dy]*ker3[dz];
	FloatType *trg = du+2*j;
	for (int dx=0; dx<2*ns; ++dx) {
	  trg[dx] += kerval*ker1val[dx];
	}
      }
    }
  }
}

template<typename FloatType>
void add_wrapped_subgrid(int64_t offset1,int64_t offset2,int64_t offset3,
			 int64_t size1,int64_t size2,int64_t size3,int64_t N1,
			 int64_t N2,int64_t N3,FloatType *data_uniform, FloatType *du0)
/* Add a large subgrid (du0) to output grid (data_uniform),
   with periodic wrapping to N1,N2,N3 box.
   offset1,2,3 give the offset of the subgrid from the lowest corner of output.
   size1,2,3 give the size of subgrid.
   Works in all dims. Not thread-safe and must be called inside omp critical.
   Barnett 3/27/18 made separate routine, tried to speed up inner loop.
*/
{
  std::vector<int64_t> o2(size2), o3(size3);
  int64_t y=offset2, z=offset3;    // fill wrapped ptr lists in slower dims y,z...
  for (int i=0; i<size2; ++i) {
    if (y<0) y+=N2;
    if (y>=N2) y-=N2;
    o2[i] = y++;
  }
  for (int i=0; i<size3; ++i) {
    if (z<0) z+=N3;
    if (z>=N3) z-=N3;
    o3[i] = z++;
  }
  int64_t nlo = (offset1<0) ? -offset1 : 0;          // # wrapping below in x
  int64_t nhi = (offset1+size1>N1) ? offset1+size1-N1 : 0;    // " above in x
  // this triple loop works in all dims
  for (int dz=0; dz<size3; dz++) {       // use ptr lists in each axis
    int64_t oz = N1*N2*o3[dz];            // offset due to z (0 in <3D)
    for (int dy=0; dy<size2; dy++) {
      int64_t oy = oz + N1*o2[dy];        // off due to y & z (0 in 1D)
      FloatType *out = data_uniform + 2*oy;
      FloatType *in  = du0 + 2*size1*(dy + size2*dz);   // ptr to subgrid array
      int64_t o = 2*(offset1+N1);         // 1d offset for output
      for (int j=0; j<2*nlo; j++)        // j is really dx/2 (since re,im parts)
	out[j+o] += in[j];
      o = 2*offset1;
      for (int j=2*nlo; j<2*(size1-nhi); j++)
	out[j+o] += in[j];
      o = 2*(offset1-N1);
      for (int j=2*(size1-nhi); j<2*size1; j++)
      	out[j+o] += in[j];
    }
  }
}

template<typename FloatType>
void add_wrapped_subgrid_thread_safe(int64_t offset1,int64_t offset2,int64_t offset3,
                                     int64_t size1,int64_t size2,int64_t size3,int64_t N1,
                                     int64_t N2,int64_t N3,FloatType *data_uniform, FloatType *du0)
/* Add a large subgrid (du0) to output grid (data_uniform),
   with periodic wrapping to N1,N2,N3 box.
   offset1,2,3 give the offset of the subgrid from the lowest corner of output.
   size1,2,3 give the size of subgrid.
   Works in all dims. Thread-safe variant of the above routine,
   using atomic writes (R Blackwell, Nov 2020).
*/
{
  std::vector<int64_t> o2(size2), o3(size3);
  int64_t y=offset2, z=offset3;    // fill wrapped ptr lists in slower dims y,z...
  for (int i=0; i<size2; ++i) {
    if (y<0) y+=N2;
    if (y>=N2) y-=N2;
    o2[i] = y++;
  }
  for (int i=0; i<size3; ++i) {
    if (z<0) z+=N3;
    if (z>=N3) z-=N3;
    o3[i] = z++;
  }
  int64_t nlo = (offset1<0) ? -offset1 : 0;          // # wrapping below in x
  int64_t nhi = (offset1+size1>N1) ? offset1+size1-N1 : 0;    // " above in x
  // this triple loop works in all dims
  for (int dz=0; dz<size3; dz++) {       // use ptr lists in each axis
    int64_t oz = N1*N2*o3[dz];            // offset due to z (0 in <3D)
    for (int dy=0; dy<size2; dy++) {
      int64_t oy = oz + N1*o2[dy];        // off due to y & z (0 in 1D)
      FloatType *out = data_uniform + 2*oy;
      FloatType *in  = du0 + 2*size1*(dy + size2*dz);   // ptr to subgrid array
      int64_t o = 2*(offset1+N1);         // 1d offset for output
      for (int j=0; j<2*nlo; j++) { // j is really dx/2 (since re,im parts)
#pragma omp atomic
        out[j + o] += in[j];
      }
      o = 2*offset1;
      for (int j=2*nlo; j<2*(size1-nhi); j++) {
#pragma omp atomic
        out[j + o] += in[j];
      }
      o = 2*(offset1-N1);
      for (int j=2*(size1-nhi); j<2*size1; j++) {
#pragma omp atomic
        out[j+o] += in[j];
      }
    }
  }
}

template<typename FloatType>
void get_subgrid(int64_t &offset1,int64_t &offset2,int64_t &offset3,int64_t &size1,int64_t &size2,int64_t &size3,int64_t M,FloatType* kx,FloatType* ky,FloatType* kz,int ns,int ndims)
/* Writes out the integer offsets and sizes of a "subgrid" (cuboid subset of
   Z^ndims) large enough to enclose all of the nonuniform points with
   (non-periodic) padding of half the kernel width ns to each side in
   each relevant dimension.

 Inputs:
   M - number of nonuniform points, ie, length of kx array (and ky if ndims>1,
       and kz if ndims>2)
   kx,ky,kz - coords of nonuniform points (ky only read if ndims>1,
              kz only read if ndims>2). To be useful for spreading, they are
              assumed to be in [0,Nj] for dimension j=1,..,ndims.
   ns - (positive integer) spreading kernel width.
   ndims - space dimension (1,2, or 3).

 Outputs:
   offset1,2,3 - left-most coord of cuboid in each dimension (up to ndims)
   size1,2,3   - size of cuboid in each dimension.
                 Thus the right-most coord of cuboid is offset+size-1.
   Returns offset 0 and size 1 for each unused dimension (ie when ndims<3);
   this is required by the calling code.

 Example:
      inputs:
          ndims=1, M=2, kx[0]=0.2, ks[1]=4.9, ns=3
      outputs:
          offset1=-1 (since kx[0] spreads to {-1,0,1}, and -1 is the min)
          size1=8 (since kx[1] spreads to {4,5,6}, so subgrid is {-1,..,6}
                   hence 8 grid points).
 Notes:
   1) Works in all dims 1,2,3.
   2) Rounding of the kx (and ky, kz) to the grid is tricky and must match the
   rounding step used in spread_subproblem_{1,2,3}d. Namely, the ceil of
   (the NU pt coord minus ns/2) gives the left-most index, in each dimension.
   This being done consistently is crucial to prevent segfaults in subproblem
   spreading. This assumes that max() and ceil() commute in the floating pt
   implementation.
   Originally by J Magland, 2017. AHB realised the rounding issue in
   6/16/17, but only fixed a rounding bug causing segfault in (highly
   inaccurate) single-precision with N1>>1e7 on 11/30/20.
   3) Requires O(M) RAM reads to find the k array bnds. Almost negligible in
   tests.
*/
{
  FloatType ns2 = (FloatType)ns/2;
  FloatType min_kx,max_kx;   // 1st (x) dimension: get min/max of nonuniform points
  array_range(M,kx,&min_kx,&max_kx);
  offset1 = (int64_t)std::ceil(min_kx-ns2);   // min index touched by kernel
  size1 = (int64_t)std::ceil(max_kx-ns2) - offset1 + ns;  // int(ceil) first!
  if (ndims>1) {
    FloatType min_ky,max_ky;   // 2nd (y) dimension: get min/max of nonuniform points
    array_range(M,ky,&min_ky,&max_ky);
    offset2 = (int64_t)std::ceil(min_ky-ns2);
    size2 = (int64_t)std::ceil(max_ky-ns2) - offset2 + ns;
  } else {
    offset2 = 0;
    size2 = 1;
  }
  if (ndims>2) {
    FloatType min_kz,max_kz;   // 3rd (z) dimension: get min/max of nonuniform points
    array_range(M,kz,&min_kz,&max_kz);
    offset3 = (int64_t)std::ceil(min_kz-ns2);
    size3 = (int64_t)std::ceil(max_kz-ns2) - offset3 + ns;
  } else {
    offset3 = 0;
    size3 = 1;
  }
}

}  // namespace

// Explicit instatiations.
template class Plan<CPUDevice, float>;
template class Plan<CPUDevice, double>;

}  // namespace nufft
}  // namespace tensorflow
