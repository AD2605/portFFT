/***************************************************************************
 *
 *  Copyright (C) Codeplay Software Ltd.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *  Codeplay's portFFT
 *
 **************************************************************************/

#ifndef PORTFFT_DISPATCHER_GLOBAL_DISPATCHER_HPP
#define PORTFFT_DISPATCHER_GLOBAL_DISPATCHER_HPP

#include <sycl/sycl.hpp>

#include <cstring>

#include "portfft/common/bluestein.hpp"
#include "portfft/common/global.hpp"
#include "portfft/common/subgroup.hpp"
#include "portfft/defines.hpp"
#include "portfft/enums.hpp"
#include "portfft/specialization_constant.hpp"

namespace portfft {
namespace detail {

/**
 * Helper function to obtain the global and local range for kernel corresponding to the factor
 * @param fft_size length of the factor
 * @param num_batches number of corresposing batches
 * @param level The implementation for the factor
 * @param n_compute_units compute_units available
 * @param subgroup_size Subgroup size chosen
 * @param n_sgs_in_wg Number of subgroups in a workgroup.
 * @return std::pair containing global and local range
 */
inline std::pair<IdxGlobal, IdxGlobal> get_launch_params(IdxGlobal fft_size, IdxGlobal num_batches, detail::level level,
                                                         Idx n_compute_units, Idx subgroup_size, Idx n_sgs_in_wg) {
  IdxGlobal n_available_sgs = 8 * n_compute_units * 64;
  IdxGlobal wg_size = n_sgs_in_wg * subgroup_size;
  if (level == detail::level::WORKITEM) {
    IdxGlobal n_ffts_per_wg = wg_size;
    IdxGlobal n_wgs_required = divide_ceil(num_batches, n_ffts_per_wg);
    return std::make_pair(std::min(n_wgs_required * wg_size, n_available_sgs), wg_size);
  }
  if (level == detail::level::SUBGROUP) {
    IdxGlobal n_ffts_per_sg = static_cast<IdxGlobal>(subgroup_size) / detail::factorize_sg(fft_size, subgroup_size);
    IdxGlobal n_ffts_per_wg = n_ffts_per_sg * n_sgs_in_wg;
    IdxGlobal n_wgs_required = divide_ceil(num_batches, n_ffts_per_wg);
    return std::make_pair(std::min(n_wgs_required * wg_size, n_available_sgs), wg_size);
  }
  if (level == detail::level::WORKGROUP) {
    return std::make_pair(std::min(num_batches * wg_size, n_available_sgs), wg_size);
  }
  throw internal_error("illegal level encountered");
}

/**
 * Transposes A into B, for complex inputs only
 * @param a Input pointer a
 * @param b Input pointer b
 * @param lda leading dimension A
 * @param ldb leading Dimension B
 * @param num_elements Total number of complex values in the matrix
 */
template <typename T>
void complex_transpose(const T* a, T* b, IdxGlobal lda, IdxGlobal ldb, IdxGlobal num_elements) {
  for (IdxGlobal i = 0; i < num_elements; i++) {
    IdxGlobal j = i / ldb;
    IdxGlobal k = i % ldb;
    b[2 * i] = a[2 * k * lda + 2 * j];
    b[2 * i + 1] = a[2 * k * lda + 2 * j + 1];
  }
}

/**
 * Helper function to determine the increment of twiddle pointer between factors
 * @param level Corresponding implementation for the previous factor
 * @param factor_size length of the factor
 * @return value to increment the pointer by
 */
inline IdxGlobal increment_twiddle_offset(detail::level level, Idx factor_size) {
  if (level == detail::level::SUBGROUP) {
    return 2 * factor_size;
  }
  if (level == detail::level::WORKGROUP) {
    Idx n = detail::factorize(factor_size);
    Idx m = factor_size / n;
    return 2 * (factor_size + m + n);
  }
  return 0;
}

/**
 * Utility function to copy data between pointers with different distances between each batch.
 * @tparam T scalar type
 * @param src source pointer
 * @param dst destination pointer
 * @param num_elements_to_copy number of elements to copy
 * @param src_stride stride of the source pointer
 * @param dst_stride stride of the destination pointer
 * @param num_copies number of batches to copy
 * @param event_vector vector to store the generated events
 * @param queue queue
 */
template <typename T>
void trigger_device_copy(const T* src, T* dst, std::size_t num_elements_to_copy, std::size_t src_stride,
                         std::size_t dst_stride, std::size_t num_copies, std::vector<sycl::event>& event_vector,
                         sycl::queue& queue) {
  for (std::size_t i = 0; i < num_copies; i++) {
    event_vector.at(i) = queue.copy(src + i * src_stride, dst + i * dst_stride, num_elements_to_copy);
  }
}

}  // namespace detail

template <typename Scalar, domain Domain>
template <typename Dummy>
struct committed_descriptor<Scalar, Domain>::calculate_twiddles_struct::inner<detail::level::GLOBAL, Dummy> {
  static Scalar* execute(committed_descriptor& desc, dimension_struct& /*dimension_data*/,
                         std::vector<kernel_data_struct>& kernels) {
    std::vector<IdxGlobal> factors_idx_global;
    // Get factor sizes per level;
    for (const auto& kernel_data : kernels) {
      factors_idx_global.push_back(static_cast<IdxGlobal>(
          std::accumulate(kernel_data.factors.begin(), kernel_data.factors.end(), 1, std::multiplies<Idx>())));
    }

    /**
     * Helper Lambda to calculate twiddles
     */
    auto calculate_twiddles = [](IdxGlobal N, IdxGlobal M, IdxGlobal& offset, Scalar* ptr) {
      for (IdxGlobal i = 0; i < N; i++) {
        for (IdxGlobal j = 0; j < M; j++) {
          double theta = -2 * M_PI * static_cast<double>(i * j) / static_cast<double>(N * M);
          ptr[offset++] = static_cast<Scalar>(std::cos(theta));
          ptr[offset++] = static_cast<Scalar>(std::sin(theta));
        }
      }
    };

    /**
     * Gets cumulative global memory requirements for provided set of factors and sub batches.
     */
    auto get_cumulative_memory_requirememts = [&](std::vector<IdxGlobal>& factors, std::vector<IdxGlobal>& sub_batches,
                                                  direction dir) -> IdxGlobal {
      // calculate sizes for modifiers
      std::size_t num_factors = static_cast<std::size_t>(dir == direction::FORWARD ? dimension_data.forward_factors
                                                                                   : dimension_data.backward_factors);
      std::size_t offset = static_cast<std::size_t>(dir == direction::FORWARD ? 0 : dimension_data.backward_factors);
      IdxGlobal total_memory = 0;

      // get memory for modifiers
      for (std::size_t i = 0; i < num_factors - 1; i++) {
        total_memory += 2 * factors.at(offset + i) * sub_batches.at(offset + i);
      }

      // Get memory required for twiddles per sub-level
      for (std::size_t i = 0; i < num_factors; i++) {
        auto level = kernels.at(offset + i).level;
        if (level == detail::level::SUBGROUP) {
          total_memory += 2 * factors.at(offset + i);
        } else if (level == detail::level::WORKGROUP) {
          IdxGlobal factor_1 = detail::factorize(factors.at(offset + i));
          IdxGlobal factor_2 = factors.at(offset + i) / factor_1;
          total_memory += 2 * (factor_1 * factor_2) + 2 * (factor_1 + factor_2);
        }
      }
      return total_memory;
    };

    /**
     * populates and rearranges twiddles on host pointer and populates
     * the kernel specific metadata (launch params and local mem requirements for twiddles only)
     */
    auto populate_twiddles_and_metadata = [&](Scalar* ptr, std::vector<IdxGlobal>& factors,
                                              std::vector<IdxGlobal>& sub_batches, IdxGlobal& ptr_offset,
                                              Scalar* scratch_ptr, direction dir) -> void {
      std::size_t num_factors = static_cast<std::size_t>(dir == direction::FORWARD ? dimension_data.forward_factors
                                                                                   : dimension_data.backward_factors);
      std::size_t offset = static_cast<std::size_t>(dir == direction::FORWARD ? 0 : dimension_data.backward_factors);

      for (std::size_t i = 0; i < num_factors - 1; i++) {
        if (kernels.at(offset + i).level == detail::level::WORKITEM) {
          // Use coalesced loads from global memory to ensure optimal accesses. Avoid extraneous write to local memory.
          // Local memory option provided for devices which do not support coalesced accesses.
          calculate_twiddles(factors.at(offset + i), sub_batches.at(offset + i), ptr_offset, ptr);
        } else {
          calculate_twiddles(sub_batches.at(offset + i), factors.at(offset + i), ptr_offset, ptr);
        }
      }

      // Calculate twiddles for the implementation corresponding to per factor;
      for (std::size_t i = 0; i < num_factors; i++) {
        const auto& kernel_data = kernels.at(offset + i);
        if (kernel_data.level == detail::level::SUBGROUP) {
          for (std::size_t j = 0; j < std::size_t(kernel_data.factors.at(0)); j++) {
            for (std::size_t k = 0; k < std::size_t(kernel_data.factors.at(1)); k++) {
              double theta = -2 * M_PI * static_cast<double>(j * k) /
                             static_cast<double>(kernel_data.factors.at(0) * kernel_data.factors.at(1));
              auto twiddle =
                  std::complex<Scalar>(static_cast<Scalar>(std::cos(theta)), static_cast<Scalar>(std::sin(theta)));
              ptr[static_cast<std::size_t>(ptr_offset) + k * static_cast<std::size_t>(kernel_data.factors.at(0)) + j] =
                  twiddle.real();
              ptr[static_cast<std::size_t>(ptr_offset) +
                  (k + static_cast<std::size_t>(kernel_data.factors.at(1))) *
                      static_cast<std::size_t>(kernel_data.factors.at(0)) +
                  j] = twiddle.imag();
            }
          }
          ptr_offset += 2 * kernel_data.factors.at(0) * kernel_data.factors.at(1);
        } else if (kernels.at(offset + i).level == detail::level::WORKGROUP) {
          Idx factor_n = kernel_data.factors.at(0) * kernel_data.factors.at(1);
          Idx factor_m = kernel_data.factors.at(2) * kernel_data.factors.at(3);
          calculate_twiddles(static_cast<IdxGlobal>(kernel_data.factors.at(0)),
                             static_cast<IdxGlobal>(kernel_data.factors.at(1)), ptr_offset, ptr);
          calculate_twiddles(static_cast<IdxGlobal>(kernel_data.factors.at(2)),
                             static_cast<IdxGlobal>(kernel_data.factors.at(3)), ptr_offset, ptr);
          // Calculate wg twiddles and transpose them
          calculate_twiddles(static_cast<IdxGlobal>(factor_n), static_cast<IdxGlobal>(factor_m), ptr_offset, ptr);
          for (Idx j = 0; j < factor_n; j++) {
            detail::complex_transpose(ptr + ptr_offset + 2 * j * factor_n, scratch_ptr, factor_m, factor_n,
                                      factor_n * factor_m);
            std::memcpy(ptr + ptr_offset + 2 * j * factor_n, scratch_ptr,
                        static_cast<std::size_t>(2 * factor_n * factor_m) * sizeof(float));
          }
        }
      }

      // Populate Metadata
      for (std::size_t i = 0; i < num_factors; i++) {
        auto& kernel_data = kernels.at(offset + i);
        kernel_data.batch_size = sub_batches.at(offset + i);
        kernel_data.length = static_cast<std::size_t>(factors.at(offset + i));
        if (kernel_data.level == detail::level::WORKITEM) {
          Idx num_sgs_in_wg = PORTFFT_SGS_IN_WG;
          if (i < kernels.size() - 1) {
            kernel_data.local_mem_required = static_cast<std::size_t>(1);
          } else {
            kernel_data.local_mem_required = desc.num_scalars_in_local_mem<detail::layout::PACKED>(
                detail::level::WORKITEM, static_cast<std::size_t>(factors.at(offset + i)), kernel_data.used_sg_size,
                {static_cast<Idx>(factors.at(offset + i))}, num_sgs_in_wg);
          }
          auto [global_range, local_range] =
              detail::get_launch_params(factors.at(offset + i), sub_batches.at(offset + i), detail::level::WORKITEM,
                                        desc.n_compute_units, kernel_data.used_sg_size, num_sgs_in_wg);
          kernel_data.global_range = global_range;
          kernel_data.local_range = local_range;
        } else if (kernel_data.level == detail::level::SUBGROUP) {
          Idx num_sgs_in_wg = PORTFFT_SGS_IN_WG;
          IdxGlobal factor_sg = detail::factorize_sg(factors.at(offset + i), kernel_data.used_sg_size);
          IdxGlobal factor_wi = factors.at(offset + i) / factor_sg;
          if (i < kernels.size() - 1) {
            kernel_data.local_mem_required = desc.num_scalars_in_local_mem<detail::layout::BATCH_INTERLEAVED>(
                detail::level::SUBGROUP, static_cast<std::size_t>(factors.at(offset + i)), kernel_data.used_sg_size,
                {static_cast<Idx>(factor_wi), static_cast<Idx>(factor_sg)}, num_sgs_in_wg);
          } else {
            kernel_data.local_mem_required = desc.num_scalars_in_local_mem<detail::layout::PACKED>(
                detail::level::SUBGROUP, static_cast<std::size_t>(factors.at(offset + i)), kernel_data.used_sg_size,
                {static_cast<Idx>(factor_wi), static_cast<Idx>(factor_sg)}, num_sgs_in_wg);
          }
          auto [global_range, local_range] =
              detail::get_launch_params(factors.at(offset + i), sub_batches.at(offset + i), detail::level::SUBGROUP,
                                        desc.n_compute_units, kernel_data.used_sg_size, num_sgs_in_wg);
          kernel_data.global_range = global_range;
          kernel_data.local_range = local_range;
        }
      }
      free(scratch_ptr);
    };

    std::vector<IdxGlobal> factors_idx_global;
    IdxGlobal temp_acc = 1;
    // Get factor sizes per level;
    for (const auto& kernel_data : kernels) {
      factors_idx_global.push_back(static_cast<IdxGlobal>(
          std::accumulate(kernel_data.factors.begin(), kernel_data.factors.end(), 1, std::multiplies<Idx>())));
      temp_acc *= factors_idx_global.back();
      if (temp_acc == static_cast<IdxGlobal>(dimension_data.committed_length)) {
        break;
      }
    }
    dimension_data.forward_factors = static_cast<Idx>(factors_idx_global.size());
    dimension_data.backward_factors = static_cast<Idx>(kernels.size()) - dimension_data.forward_factors;
    for (std::size_t i = 0; i < std::size_t(dimension_data.backward_factors); i++) {
      factors_idx_global.push_back(static_cast<IdxGlobal>(
          std::accumulate(kernels.at(i + static_cast<std::size_t>(dimension_data.forward_factors)).factors.begin(),
                          kernels.at(i + static_cast<std::size_t>(dimension_data.forward_factors)).factors.end(), 1,
                          std::multiplies<Idx>())));
    }

    // Get sub batches per direction
    std::vector<IdxGlobal> sub_batches;
    for (Idx i = 0; i < dimension_data.forward_factors - 1; i++) {
      sub_batches.push_back(
          std::accumulate(factors_idx_global.begin() + i + 1,
                          factors_idx_global.begin() + static_cast<long>(dimension_data.forward_factors), IdxGlobal(1),
                          std::multiplies<IdxGlobal>()));
    }
    sub_batches.push_back(factors_idx_global.at(static_cast<std::size_t>(dimension_data.forward_factors - 2)));
    if (dimension_data.backward_factors > 0) {
      for (Idx i = 0; i < dimension_data.backward_factors - 1; i++) {
        sub_batches.push_back(
            std::accumulate(factors_idx_global.begin() + static_cast<long>(dimension_data.forward_factors + i + 1),
                            factors_idx_global.end(), IdxGlobal(1), std::multiplies<IdxGlobal>()));
      }
      sub_batches.push_back(factors_idx_global.at(factors_idx_global.size() - 2));
    }

    // Get total Global memory required to store all the twiddles and multipliers.
    IdxGlobal mem_required_for_twiddles =
        get_cumulative_memory_requirememts(factors_idx_global, sub_batches, direction::FORWARD);
    if (dimension_data.backward_factors > 0) {
      mem_required_for_twiddles +=
          get_cumulative_memory_requirememts(factors_idx_global, sub_batches, direction::BACKWARD);
      // Presence of backward factors signifies that Bluestein will be used.
      // Thus take into account memory required for load modifiers as well.
      mem_required_for_twiddles += static_cast<IdxGlobal>(4 * dimension_data.length);
    }

    std::vector<Scalar> host_memory(static_cast<std::size_t>(mem_required_for_twiddles));
    Scalar* device_twiddles =
        sycl::malloc_device<Scalar>(static_cast<std::size_t>(mem_required_for_twiddles), desc.queue);
    Scalar* scratch_ptr = (Scalar*)malloc(2 * dimension_data.length * sizeof(Scalar));

    IdxGlobal offset = 0;
    if (dimension_data.is_prime) {
      // first populate load modifiers for bluestein.
      detail::get_fft_chirp_signal(host_memory.data() + offset, static_cast<IdxGlobal>(dimension_data.committed_length),
                                   static_cast<IdxGlobal>(dimension_data.length));
      offset += static_cast<IdxGlobal>(2 * dimension_data.length);
      detail::populate_bluestein_input_modifiers(host_memory.data() + offset,
                                                 static_cast<IdxGlobal>(dimension_data.committed_length),
                                                 static_cast<IdxGlobal>(dimension_data.length));
      offset += static_cast<IdxGlobal>(2 * dimension_data.length);
      // set the layout of the load modifiers according the requirement of the sub-impl.
      if (kernels.at(0).level == detail::level::SUBGROUP) {
        IdxGlobal base_offset = static_cast<IdxGlobal>(2 * dimension_data.length);
        for (IdxGlobal i = 0; i < kernels.at(0).batch_size; i++) {
          detail::complex_transpose(host_memory.data() + base_offset, scratch_ptr, kernels.at(0).factors[0],
                                    kernels.at(0).factors[1], kernels.at(0).factors[0] * kernels.at(0).factors[1]);
          std::memcpy(
              host_memory.data() + base_offset, scratch_ptr,
              static_cast<std::size_t>(2 * kernels.at(0).factors[0] * kernels.at(0).factors[1]) * sizeof(Scalar));
          base_offset += 2 * kernels.at(0).factors[0] * kernels.at(0).factors[1];
        }
      }
    }

    populate_twiddles_and_metadata(host_memory.data(), factors_idx_global, sub_batches, offset, scratch_ptr,
                                   direction::FORWARD);
    if (dimension_data.backward_factors) {
      populate_twiddles_and_metadata(host_memory.data(), factors_idx_global, sub_batches, offset, scratch_ptr,
                                     direction::BACKWARD);
    }
    desc.queue.copy(host_memory.data(), device_twiddles, static_cast<std::size_t>(mem_required_for_twiddles)).wait();
    return device_twiddles;
  }
};

template <typename Scalar, domain Domain>
template <typename Dummy>
struct committed_descriptor<Scalar, Domain>::set_spec_constants_struct::inner<detail::level::GLOBAL, Dummy> {
  static void execute(committed_descriptor& /*desc*/, sycl::kernel_bundle<sycl::bundle_state::input>& in_bundle,
                      std::size_t length, const std::vector<Idx>& factors, detail::level level, Idx factor_num,
                      Idx num_factors) {
    Idx length_idx = static_cast<Idx>(length);
    in_bundle.template set_specialization_constant<detail::GlobalSubImplSpecConst>(level);
    in_bundle.template set_specialization_constant<detail::GlobalSpecConstNumFactors>(num_factors);
    in_bundle.template set_specialization_constant<detail::GlobalSpecConstLevelNum>(factor_num);
    if (level == detail::level::WORKITEM || level == detail::level::WORKGROUP) {
      in_bundle.template set_specialization_constant<detail::SpecConstFftSize>(length_idx);
    } else if (level == detail::level::SUBGROUP) {
      in_bundle.template set_specialization_constant<detail::SubgroupFactorWISpecConst>(factors[1]);
      in_bundle.template set_specialization_constant<detail::SubgroupFactorSGSpecConst>(factors[0]);
    }
  }
};

template <typename Scalar, domain Domain>
template <detail::layout LayoutIn, typename Dummy>
struct committed_descriptor<Scalar, Domain>::num_scalars_in_local_mem_struct::inner<detail::level::GLOBAL, LayoutIn,
                                                                                    Dummy> {
  static std::size_t execute(committed_descriptor& /*desc*/, std::size_t /*length*/, Idx /*used_sg_size*/,
                             const std::vector<Idx>& /*factors*/, Idx& /*num_sgs_per_wg*/) {
    // No work required as all work done in calculate_twiddles;
    return 0;
  }
};

template <typename Scalar, domain Domain>
template <detail::layout LayoutIn, detail::layout LayoutOut, Idx SubgroupSize, typename TIn, typename TOut>
template <typename Dummy>
struct committed_descriptor<Scalar, Domain>::run_kernel_struct<LayoutIn, LayoutOut, SubgroupSize, TIn,
                                                               TOut>::inner<detail::level::GLOBAL, Dummy> {
  static sycl::event execute(committed_descriptor& desc, const TIn& in, TOut& out, const TIn& in_imag, TOut& out_imag,
                             const std::vector<sycl::event>& dependencies, IdxGlobal n_transforms,
                             IdxGlobal input_offset, IdxGlobal output_offset, dimension_struct& dimension_data,
                             direction compute_direction) {
    complex_storage storage = desc.params.complex_storage;
    const IdxGlobal vec_size = storage == complex_storage::INTERLEAVED_COMPLEX ? 2 : 1;
    const auto& kernels =
        compute_direction == direction::FORWARD ? dimension_data.forward_kernels : dimension_data.backward_kernels;
    const Scalar* twiddles_ptr = static_cast<const Scalar*>(kernels.at(0).twiddles_forward.get());
    const IdxGlobal* factors_and_scan = static_cast<const IdxGlobal*>(dimension_data.factors_and_scan.get());
    std::size_t num_batches = desc.params.number_of_transforms;
    std::size_t max_batches_in_l2 = static_cast<std::size_t>(dimension_data.num_batches_in_l2);
    std::size_t imag_offset = dimension_data.length * max_batches_in_l2;
    IdxGlobal initial_impl_twiddle_offset = 0;
    Idx num_factors = dimension_data.forward_factors;
    IdxGlobal committed_size = static_cast<IdxGlobal>(dimension_data.length);
    Idx num_transposes = num_factors - 1;
    std::vector<sycl::event> current_events;
    std::vector<sycl::event> previous_events;
    current_events.resize(static_cast<std::size_t>(dimension_data.num_batches_in_l2));
    previous_events.resize(static_cast<std::size_t>(dimension_data.num_batches_in_l2));
    current_events[0] = desc.queue.submit([&](sycl::handler& cgh) {
      cgh.depends_on(dependencies);
      cgh.host_task([&]() {});
    });
    for (std::size_t i = 0; i < static_cast<std::size_t>(num_factors - 1); i++) {
      initial_impl_twiddle_offset += 2 * kernels.at(i).batch_size * static_cast<IdxGlobal>(kernels.at(i).length);
    }

    auto global_impl_driver = [&]<direction Direction>(const std::vector<kernel_data_struct>& kernels,
                                                       const std::size_t& i) {
      IdxGlobal intermediate_twiddles_offset = 0;
      IdxGlobal impl_twiddle_offset = initial_impl_twiddle_offset;
      auto& kernel0 = kernels.at(0);
      l2_events = detail::compute_level<Scalar, Domain, detail::layout::BATCH_INTERLEAVED,
                                        detail::layout::BATCH_INTERLEAVED, SubgroupSize>(
          kernel0, in, desc.scratch_ptr_1.get(), in_imag, desc.scratch_ptr_1.get() + imag_offset, twiddles_ptr,
          factors_and_scan, intermediate_twiddles_offset, impl_twiddle_offset,
          vec_size * static_cast<IdxGlobal>(i) * committed_size + input_offset, committed_size,
          static_cast<Idx>(max_batches_in_l2), static_cast<IdxGlobal>(num_batches), static_cast<IdxGlobal>(i), 0,
          dimension_data.num_factors, storage, {event}, desc.queue);
      detail::dump_device(desc.queue, "after factor 0:", desc.scratch_ptr_1.get(),
                          desc.params.number_of_transforms * dimension_data.length * 2, l2_events);
      intermediate_twiddles_offset += 2 * kernel0.batch_size * static_cast<IdxGlobal>(kernel0.length);
      impl_twiddle_offset += detail::increment_twiddle_offset(kernel0.level, static_cast<Idx>(kernel0.length));
      for (std::size_t factor_num = 1; factor_num < static_cast<std::size_t>(dimension_data.num_factors);
           factor_num++) {
        auto& current_kernel = kernels.at(factor_num);
        if (static_cast<Idx>(factor_num) == dimension_data.num_factors - 1) {
          l2_events =
              detail::compute_level<Scalar, Domain, detail::layout::PACKED, detail::layout::PACKED, SubgroupSize>(
                  current_kernel, desc.scratch_ptr_1.get(), desc.scratch_ptr_1.get(),
                  desc.scratch_ptr_1.get() + imag_offset, desc.scratch_ptr_1.get() + imag_offset, twiddles_ptr,
                  factors_and_scan, intermediate_twiddles_offset, impl_twiddle_offset, 0, committed_size,
                  static_cast<Idx>(max_batches_in_l2), static_cast<IdxGlobal>(num_batches), static_cast<IdxGlobal>(i),
                  static_cast<Idx>(factor_num), dimension_data.num_factors, storage, l2_events, desc.queue);
        } else {
          l2_events = detail::compute_level<Scalar, Domain, detail::layout::BATCH_INTERLEAVED,
                                            detail::layout::BATCH_INTERLEAVED, SubgroupSize>(
              current_kernel, desc.scratch_ptr_1.get(), desc.scratch_ptr_1.get(),
              desc.scratch_ptr_1.get() + imag_offset, desc.scratch_ptr_1.get() + imag_offset, twiddles_ptr,
              factors_and_scan, intermediate_twiddles_offset, impl_twiddle_offset, 0, committed_size,
              static_cast<Idx>(max_batches_in_l2), static_cast<IdxGlobal>(num_batches), static_cast<IdxGlobal>(i),
              static_cast<Idx>(factor_num), dimension_data.num_factors, storage, l2_events, desc.queue);
          intermediate_twiddles_offset += 2 * current_kernel.batch_size * static_cast<IdxGlobal>(current_kernel.length);
          impl_twiddle_offset +=
              detail::increment_twiddle_offset(current_kernel.level, static_cast<Idx>(current_kernel.length));
        }
        detail::dump_device(desc.queue, "after factor:", desc.scratch_ptr_1.get(),
                            desc.params.number_of_transforms * dimension_data.length * 2, l2_events);
      }
      current_events[0] = desc.queue.submit([&](sycl::handler& cgh) {
        cgh.depends_on(previous_events);
        cgh.host_task([&]() {});
      });
      for (Idx num_transpose = num_transposes - 1; num_transpose > 0; num_transpose--) {
        event = detail::transpose_level<Scalar, Domain>(
            dimension_data.transpose_kernels.at(static_cast<std::size_t>(num_transpose)), desc.scratch_ptr_1.get(),
            desc.scratch_ptr_2.get(), factors_and_scan, committed_size, static_cast<Idx>(max_batches_in_l2),
            n_transforms, static_cast<IdxGlobal>(i), num_factors, 0, desc.queue, {event}, storage);
        if (storage == complex_storage::SPLIT_COMPLEX) {
          event = detail::transpose_level<Scalar, Domain>(
              dimension_data.transpose_kernels.at(static_cast<std::size_t>(num_transpose)),
              desc.scratch_ptr_1.get() + imag_offset, desc.scratch_ptr_2.get() + imag_offset, factors_and_scan,
              committed_size, static_cast<Idx>(max_batches_in_l2), n_transforms, static_cast<IdxGlobal>(i), num_factors,
              0, desc.queue, {event}, storage);
        }
        desc.scratch_ptr_1.swap(desc.scratch_ptr_2);
      }
      event = detail::transpose_level<Scalar, Domain>(
          dimension_data.transpose_kernels.at(0), desc.scratch_ptr_1.get(), out, factors_and_scan, committed_size,
          static_cast<Idx>(max_batches_in_l2), n_transforms, static_cast<IdxGlobal>(i), num_factors,
          vec_size * static_cast<IdxGlobal>(i) * committed_size + output_offset, desc.queue, {event}, storage);
      if (storage == complex_storage::SPLIT_COMPLEX) {
        event = detail::transpose_level<Scalar, Domain>(
            dimension_data.transpose_kernels.at(0), desc.scratch_ptr_1.get() + imag_offset, out_imag, factors_and_scan,
            committed_size, static_cast<Idx>(max_batches_in_l2), n_transforms, static_cast<IdxGlobal>(i), num_factors,
            vec_size * static_cast<IdxGlobal>(i) * committed_size + output_offset, desc.queue, {event}, storage);
      }
    } return desc.queue.submit([&](sycl::handler& cgh) {
      cgh.depends_on(current_events);
      cgh.host_task([]() {});
    });
#pragma clang diagnostic pop
  }
};

}  // namespace portfft

#endif
