/*******************************************************************************
 * tlx/sort/parallel_mergesort.hpp
 *
 * Parallel multiway mergesort.
 *
 * Copied and modified from STXXL, see http://stxxl.org, which itself extracted
 * it from MCSTL http://algo2.iti.uni-karlsruhe.de/singler/mcstl/. Both are
 * distributed under the Boost Software License, Version 1.0.
 *
 * Part of tlx - http://panthema.net/tlx
 *
 * Copyright (C) 2007 Johannes Singler <singler@ira.uka.de>
 * Copyright (C) 2014-2018 Timo Bingmann <tb@panthema.net>
 *
 * All rights reserved. Published under the Boost Software License, Version 1.0
 ******************************************************************************/

#ifndef TLX_SORT_PARALLEL_MERGESORT_HEADER
#define TLX_SORT_PARALLEL_MERGESORT_HEADER

#include <algorithm>
#include <vector>

#include <tlx/algorithm/multisequence_selection.hpp>
#include <tlx/algorithm/parallel_multiway_merge.hpp>
#include <tlx/simple_vector.hpp>

#include <omp.h>

namespace tlx {

//! \addtogroup tlx_sort
//! \{
//! \name Comparison-Based Parallel Algorithms
//! \{

//! Subsequence description.
template <typename DiffType>
struct PMWMSPiece {
    //! Begin of subsequence.
    DiffType begin;
    //! End of subsequence.
    DiffType end;
};

/*!
 * Data accessed by all threads.
 *
 * PMWMS = parallel multiway mergesort
 */
template <typename RandomAccessIterator>
struct PMWMSSortingData {
    using ValueType =
        typename std::iterator_traits<RandomAccessIterator>::value_type;
    using DiffType =
        typename std::iterator_traits<RandomAccessIterator>::difference_type;

    //! Input begin.
    RandomAccessIterator source;
    //! Start indices, per thread.
    simple_vector<DiffType> starts;

    /*!
     *  Temporary arrays for each thread.
     *
     * Indirection Allows using the temporary storage in different ways,
     * without code duplication.  \see STXXL_MULTIWAY_MERGESORT_COPY_LAST
     */
    simple_vector<ValueType*> temporary;
#if STXXL_MULTIWAY_MERGESORT_COPY_LAST
    /** Storage in which to sort. */
    simple_vector<RandomAccessIterator> sorting_places;
    /** Storage into which to merge. */
    simple_vector<ValueType*> merging_places;
#else
    /** Storage in which to sort. */
    simple_vector<ValueType*> sorting_places;
    /** Storage into which to merge. */
    simple_vector<RandomAccessIterator> merging_places;
#endif
    /** Samples. */
    simple_vector<ValueType> samples;
    /** Offsets to add to the found positions. */
    simple_vector<DiffType> offsets;
    /** PMWMSPieces of data to merge \c [thread][sequence] */
    simple_vector<std::vector<PMWMSPiece<DiffType> > > pieces;

    PMWMSSortingData(size_t num_threads)
        : starts(num_threads + 1),
          temporary(num_threads),
          sorting_places(num_threads), merging_places(num_threads),
          offsets(num_threads - 1),
          pieces(num_threads)
    { }
};

//! Thread local data for PMWMS.
template <typename RandomAccessIterator>
struct PMWMSSorterPU {
    /** Total number of thread involved. */
    size_t num_threads;
    /** Number of owning thread. */
    size_t iam;
    /** Pointer to global data. */
    PMWMSSortingData<RandomAccessIterator>* sd;
};

size_t sort_mwms_oversampling = 10;

/*!
 * Select samples from a sequence.
 * \param d Pointer to thread-local data. Result will be placed in \c d->ds->samples.
 * \param num_samples Number of samples to select.
 */
template <typename RandomAccessIterator, typename DiffType>
void determine_samples(PMWMSSorterPU<RandomAccessIterator>* d,
                       DiffType& num_samples) {
    PMWMSSortingData<RandomAccessIterator>* sd = d->sd;

    num_samples = sort_mwms_oversampling * d->num_threads - 1;

    std::vector<DiffType> es(num_samples + 2);
    equally_split(sd->starts[d->iam + 1] - sd->starts[d->iam],
                  static_cast<size_t>(num_samples + 1), es.begin());

    for (DiffType i = 0; i < num_samples; i++)
        sd->samples[d->iam * num_samples + i] = sd->source[sd->starts[d->iam] + es[i + 1]];
}

/*!
 * PMWMS code executed by each thread.
 * \param d Pointer to thread-local data.
 * \param comp Comparator.
 */
template <bool Stable, typename RandomAccessIterator, typename Comparator>
void parallel_sort_mwms_pu(PMWMSSorterPU<RandomAccessIterator>* d,
                           Comparator& comp) {
    using ValueType =
        typename std::iterator_traits<RandomAccessIterator>::value_type;
    using DiffType =
        typename std::iterator_traits<RandomAccessIterator>::difference_type;

    PMWMSSortingData<RandomAccessIterator>* sd = d->sd;
    size_t iam = d->iam;

    // length of this thread's chunk, before merging
    DiffType length_local = sd->starts[iam + 1] - sd->starts[iam];

#if STXXL_MULTIWAY_MERGESORT_COPY_LAST
    using SortingPlacesIterator = RandomAccessIterator;
    // sort in input storage
    sd->sorting_places[iam] = sd->source + sd->starts[iam];
#else
    using SortingPlacesIterator = ValueType *;
    // sort in temporary storage, leave space for sentinel
    sd->sorting_places[iam] = sd->temporary[iam] =
                                  static_cast<ValueType*>(::operator new (sizeof(ValueType) * (length_local + 1)));
    // copy there
    std::uninitialized_copy(sd->source + sd->starts[iam], sd->source + sd->starts[iam] + length_local, sd->sorting_places[iam]);
#endif

    // sort locally
    if (Stable)
        std::stable_sort(sd->sorting_places[iam], sd->sorting_places[iam] + length_local, comp);
    else
        std::sort(sd->sorting_places[iam], sd->sorting_places[iam] + length_local, comp);

    // invariant: locally sorted subsequence in sd->sorting_places[iam], sd->sorting_places[iam] + length_local

    MultiwayMergeSplittingAlgorithm mwmsa = MWMSA_SAMPLING;

    if (mwmsa == MWMSA_SAMPLING)
    {
        DiffType num_samples;
        determine_samples(d, num_samples);

#pragma omp barrier

#pragma omp single
        std::sort(sd->samples.begin(), sd->samples.end(), comp);

#pragma omp barrier

        for (size_t s = 0; s < d->num_threads; s++)
        {
            // for each sequence
            if (num_samples * iam > 0)
                sd->pieces[iam][s].begin =
                    std::lower_bound(sd->sorting_places[s],
                                     sd->sorting_places[s] + sd->starts[s + 1] - sd->starts[s],
                                     sd->samples[num_samples * iam],
                                     comp)
                    - sd->sorting_places[s];
            else
                // absolute beginning
                sd->pieces[iam][s].begin = 0;

            if ((num_samples * (iam + 1)) < (num_samples * d->num_threads))
                sd->pieces[iam][s].end =
                    std::lower_bound(sd->sorting_places[s],
                                     sd->sorting_places[s] + sd->starts[s + 1] - sd->starts[s],
                                     sd->samples[num_samples * (iam + 1)],
                                     comp)
                    - sd->sorting_places[s];
            else
                // absolute end
                sd->pieces[iam][s].end = sd->starts[s + 1] - sd->starts[s];
        }
    }
    else if (mwmsa == MWMSA_EXACT)
    {
#pragma omp barrier

        std::vector<std::pair<SortingPlacesIterator, SortingPlacesIterator> > seqs(d->num_threads);
        for (size_t s = 0; s < d->num_threads; s++)
            seqs[s] = std::make_pair(sd->sorting_places[s], sd->sorting_places[s] + sd->starts[s + 1] - sd->starts[s]);

        std::vector<SortingPlacesIterator> offsets(d->num_threads);

        // if not last thread
        if (iam < d->num_threads - 1)
            multisequence_partition(seqs.begin(), seqs.end(), sd->starts[iam + 1], offsets.begin(), comp);

        for (size_t seq = 0; seq < d->num_threads; seq++)
        {
            // for each sequence
            if (iam < (d->num_threads - 1))
                sd->pieces[iam][seq].end = offsets[seq] - seqs[seq].first;
            else
                // absolute end of this sequence
                sd->pieces[iam][seq].end = sd->starts[seq + 1] - sd->starts[seq];
        }

#pragma omp barrier

        for (size_t seq = 0; seq < d->num_threads; seq++)
        {
            // for each sequence
            if (iam > 0)
                sd->pieces[iam][seq].begin = sd->pieces[iam - 1][seq].end;
            else
                // absolute beginning
                sd->pieces[iam][seq].begin = 0;
        }
    }

    // offset from target begin, length after merging
    DiffType offset = 0, length_am = 0;
    for (size_t s = 0; s < d->num_threads; s++)
    {
        length_am += sd->pieces[iam][s].end - sd->pieces[iam][s].begin;
        offset += sd->pieces[iam][s].begin;
    }

#if STXXL_MULTIWAY_MERGESORT_COPY_LAST
    // merge to temporary storage, uninitialized creation not possible since
    // there is no multiway_merge calling the placement new instead of the
    // assignment operator
    sd->merging_places[iam] = sd->temporary[iam] = new ValueType[length_am];
#else
    // merge directly to target
    sd->merging_places[iam] = sd->source + offset;
#endif
    std::vector<std::pair<SortingPlacesIterator, SortingPlacesIterator> > seqs(d->num_threads);

    for (size_t s = 0; s < d->num_threads; s++)
    {
        seqs[s] = std::make_pair(sd->sorting_places[s] + sd->pieces[iam][s].begin, sd->sorting_places[s] + sd->pieces[iam][s].end);
    }

    multiway_merge_base<Stable, false>(seqs.begin(), seqs.end(), sd->merging_places[iam], length_am, comp);

#pragma omp barrier

#if STXXL_MULTIWAY_MERGESORT_COPY_LAST
    // write back
    std::copy(sd->merging_places[iam], sd->merging_places[iam] + length_am, sd->source + offset);
#endif

    delete sd->temporary[iam];
}

/*!
 * PMWMS main call.
 * \param begin Begin iterator of sequence.
 * \param end End iterator of sequence.
 * \param comp Comparator.
 * \param num_threads Number of threads to use.
 * \tparam Stable Stable sorting.
 */
template <bool Stable,
          typename RandomAccessIterator, typename Comparator>
void parallel_mergesort(RandomAccessIterator begin,
                        RandomAccessIterator end,
                        Comparator comp,
                        size_t num_threads) {
    using DiffType =
        typename std::iterator_traits<RandomAccessIterator>::difference_type;

    DiffType n = end - begin;

    if (n <= 1)
        return;

    // at least one element per thread
    if (num_threads > static_cast<size_t>(n))
        num_threads = static_cast<size_t>(n);

    PMWMSSortingData<RandomAccessIterator> sd(num_threads);

    sd.source = begin;

    MultiwayMergeSplittingAlgorithm mwmsa = MWMSA_SAMPLING;

    if (mwmsa == MWMSA_SAMPLING)
        sd.samples.resize(
            num_threads * (sort_mwms_oversampling * num_threads - 1));

    for (size_t s = 0; s < num_threads; s++)
        sd.pieces[s].resize(num_threads);

    simple_vector<PMWMSSorterPU<RandomAccessIterator> > pus(num_threads);
    DiffType* starts = sd.starts.data();

    DiffType chunk_length = n / num_threads, split = n % num_threads, start = 0;
    for (size_t i = 0; i < num_threads; i++)
    {
        starts[i] = start;
        start += (i < static_cast<size_t>(split))
            ? (chunk_length + 1) : chunk_length;
        pus[i].num_threads = num_threads;
        pus[i].iam = i;
        pus[i].sd = &sd;
    }
    starts[num_threads] = start;

    // now sort in parallel
#pragma omp parallel num_threads(num_threads)
    parallel_sort_mwms_pu<Stable>(&(pus[omp_get_thread_num()]), comp);
}

//! \}

} // namespace tlx

#endif // !TLX_SORT_PARALLEL_MERGESORT_HEADER

/******************************************************************************/
