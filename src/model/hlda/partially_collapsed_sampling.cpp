//
// Created by jianfei on 9/19/16.
//

#include "partially_collapsed_sampling.h"
#include "clock.h"
#include "corpus.h"
#include <iostream>
#include <omp.h>
#include "mkl_vml.h"
#include "utils.h"
#include <chrono>

using namespace std;

PartiallyCollapsedSampling::PartiallyCollapsedSampling(Corpus &corpus, Corpus &to_corpus, Corpus &th_corpus, int L, vector<TProb> alpha, vector<TProb> beta,
                                                       vector<double> gamma,
                                                       int num_iters, int mc_samples, int mc_iters,
                                                       size_t minibatch_size,
                                                       int topic_limit, int threshold, bool sample_phi, int process_id, int process_size, bool check) :
        CollapsedSampling(corpus, to_corpus, th_corpus, L, alpha, beta, gamma, num_iters, mc_samples, mc_iters,
                          topic_limit, process_id, process_size, check),
        minibatch_size(minibatch_size), threshold(threshold), sample_phi(sample_phi) {
    current_it = -1;
    delayed_update = false;
    tree.SetThreshold(threshold);
}

void PartiallyCollapsedSampling::Initialize() {
    //CollapsedSampling::Initialize();
    current_it = -1;

    if (minibatch_size == 0)
        minibatch_size = docs.size();

    if (!new_topic)
        SamplePhi();

    int num_threads = omp_get_max_threads();
    LOG(INFO) << "OpenMP: using " << num_threads << " threads";
    auto &generator = GetGenerator();
    int mb_count = 0;
    omp_set_dynamic(0);
    Clock clk;

    // Compute the minibatch size for this node
    size_t local_mb_size = std::min(static_cast<size_t>(minibatch_size),
                                    docs.size() / num_threads);

    size_t local_num_mbs, num_mbs;
    local_num_mbs = (docs.size() - 1) / local_mb_size + 1;
    MPI_Allreduce(&local_num_mbs, &num_mbs, 1, MPI_UNSIGNED_LONG_LONG,
                  MPI_MAX, MPI_COMM_WORLD);
    LOG_IF(INFO, process_id == 0) << "Each node has " << num_mbs << " minibatches.";

    int processed_node = 0, next_processed_node;
    for (int degree_of_parallelism = 1; processed_node < process_size;
         degree_of_parallelism++, processed_node = next_processed_node) {
        next_processed_node = std::min(process_size,
                                       processed_node + degree_of_parallelism);
        LOG_IF(INFO, process_id == 0)
                  << "Initializing node " << processed_node
                  << " to node " << next_processed_node;

        size_t minibatch_size = docs.size() / num_mbs + 1;
        if (processed_node <= process_id && process_id < next_processed_node) {
            for (size_t d_start = 0; d_start < docs.size(); 
                    d_start += minibatch_size) {
                auto d_end = min(docs.size(), d_start + minibatch_size);
                if (degree_of_parallelism == 1)
                    omp_set_num_threads(min(++mb_count, num_threads));

                num_instantiated = tree.GetNumInstantiated();
#pragma omp parallel for
                for (size_t d = d_start; d < d_end; d++) {
                    auto &doc = docs[d];

                    for (auto &k: doc.z)
                        k = generator() % L;

                    doc.initialized = true;
                    SampleC(doc, false, true);
                    SampleZ(doc, true, true);
                }
                AllBarrier();
                omp_set_num_threads(num_threads);
                SamplePhi();
                AllBarrier();
                //Check();

                auto ret = tree.GetTree();
                LOG(INFO) << "Node: " << process_id
                          << " Processed document [" << d_start << ", " << d_end
                          << ") documents, " << ret.nodes.size()
                          << " topics.";
                if ((int)ret.nodes.size() > (size_t) topic_limit)
                    throw runtime_error("There are too many topics");
            }
        } else {
            for (size_t i = 0; i < num_mbs; i++) {
                AllBarrier();
                SamplePhi();
                AllBarrier();
                //Check();
            }
        }
        MPI_Barrier(MPI_COMM_WORLD);
        auto ret = tree.GetTree();
        LOG_IF(INFO, process_id==0) << ANSI_YELLOW << "Num nodes: " << ret.num_nodes
                             << "    Num instantiated: " << num_instantiated << ANSI_NOCOLOR;
    }
    omp_set_num_threads(num_threads);

    SamplePhi();
    delayed_update = true;
    LOG_IF(INFO, process_id == 0) << "Initialized in " << clk.toc() << " seconds";
}

void PartiallyCollapsedSampling::SampleZ(Document &doc,
                                         bool decrease_count, bool increase_count,
                                         bool allow_new_topic) {
    //std::lock_guard<std::mutex> lock(model_mutex);
    std::vector<TCount> cdl((size_t) L);
    std::vector<TProb> prob((size_t) L);
    for (auto k: doc.z) cdl[k]++;

    auto &pos = doc.c;
    std::vector<bool> is_collapsed((size_t) L);
    for (int l = 0; l < L; l++) {
        is_collapsed[l] = !allow_new_topic ? false :
                             doc.c[l] >= num_instantiated[l];
    }

    // TODO: the first few topics will have a huge impact...
    // Read out all the required data
    auto tid = omp_get_thread_num();
    LockDoc(doc);

    auto &generator = GetGenerator();
    for (size_t n = 0; n < doc.z.size(); n++) {
        TWord v = doc.w[n];
        TTopic l = doc.z[n];
        if (decrease_count) {
            if (pos[l] >= num_instantiated[l])
                count.Dec(tid, l, v, pos[l]);
            --cdl[l];
        }

        for (TLen i = 0; i < L; i++)
            if (is_collapsed[i])
                prob[i] = (cdl[i] + alpha[i]) *
                          (count.Get(i, v, pos[i]) + beta[i]) /
                          (count.GetSum(i, pos[i]) + beta[i] * corpus.V);
            else {
                prob[i] = (alpha[i] + cdl[i]) * phi[i](v, pos[i]);
            }

        l = (TTopic) DiscreteSample(prob.begin(), prob.end(), generator);
        doc.z[n] = l;

        if (increase_count) {
            if (pos[l] >= num_instantiated[l])
                count.Inc(tid, l, v, pos[l]);
            ++cdl[l];
        }
    }
    UnlockDoc(doc);
    /*double sum = 0;
    for (TLen l = 0; l < L; l++)
        sum += (doc.theta[l] = cdl[l] + alpha[l]);
    for (TLen l = 0; l < L; l++)
        doc.theta[l] /= sum;*/
    count.Publish(tid);
}

void PartiallyCollapsedSampling::SamplePhi() {
    // Output the tree and the assignment for every document
    auto perm = tree.Compress();
    num_instantiated = tree.GetNumInstantiated();
    auto ret = tree.GetTree();
    PermuteC(perm);

    for (TLen l = 0; l < L; l++) {
        phi[l].SetC(ret.num_nodes[l]);
        log_phi[l].SetC(ret.num_nodes[l]);
    }

    AllBarrier();
    UpdateICount();
    Clock clk;
    ComputePhi();
    compute_phi_time = clk.toc();
}

void PartiallyCollapsedSampling::ComputePhi() {
    auto ret = tree.GetTree();
    auto &generator = GetGenerator();

    if (!sample_phi) {
        for (TLen l = 0; l < L; l++) {
            TTopic K = (TTopic) ret.num_nodes[l];
            auto offset = icount_offset[l];

            vector<float> inv_normalization(K);
            for (TTopic k = 0; k < K; k++)
                inv_normalization[k] = 1.f / (beta[l] * corpus.V + ck_dense[k+offset]);
#pragma omp parallel for
            for (TWord v = 0; v < corpus.V; v++) {
                for (TTopic k = 0; k < K; k++) {
                    TProb prob = (icount(v, k+offset) + beta[l]) 
                                 * inv_normalization[k];
                    phi[l](v, k) = prob;
                    log_phi[l](v, k) = prob;
                }
                vsLn(K, &log_phi[l](v, 0), &log_phi[l](v, 0));
            }
        }
    } else {
        for (TLen l = 0; l < L; l++) {
            TTopic K = (TTopic) ret.num_nodes[l];
            auto offset = icount_offset[l];

            for (TTopic k = 0; k < K; k++) {
                TProb sum = 0;
                for (TWord v = 0; v < corpus.V; v++) {
                    TProb concentration = (TProb)(icount(v, k+offset) + beta[l]);
                    gamma_distribution<TProb> gammarnd(concentration);
                    TProb p = gammarnd(generator);
                    phi[l](v, k) = p;
                    sum += p;
                }
                TProb inv_sum = 1.0f / sum;
                for (TWord v = 0; v < corpus.V; v++) {
                    phi[l](v, k) *= inv_sum;
                    log_phi[l](v, k) = phi[l](v, k);
                }
            }

            for (TWord v = 0; v < corpus.V; v++)
                vsLn(K, &log_phi[l](v, 0), &log_phi[l](v, 0));
        }
    }
}
