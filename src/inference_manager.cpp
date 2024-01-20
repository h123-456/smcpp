#include <vector>
#include <utility>
#include <map>

#include "inference_manager.h"
#include "transition.h"
#include "bin_key.h"
#include "marginalize_key.h"
#include "tensorslice.h"
#include "jcsfs.h"

PiecewiseConstantRateFunction<adouble>* defaultEta(const std::vector<double> &hidden_states)
{
    std::vector<std::vector<adouble> > params;
    std::vector<adouble> p{adouble(1.0)};
    params.push_back(p);
    params.push_back(p);
    return new PiecewiseConstantRateFunction<adouble>(params, hidden_states);
}

InferenceManager::InferenceManager(
        const int npop,
        const int sfs_dim,
        const std::vector<int> obs_lengths,
        const std::vector<int*> observations,
        const std::vector<double> hidden_states,
        ConditionedSFS<adouble> *csfs) :
    saveGamma(false),
    hidden_states(hidden_states),
    npop(npop),
    sfs_dim(sfs_dim),
    M(hidden_states.size() - 1),
    obs(map_obs(observations, obs_lengths)),
    csfs(csfs),
    hmms(obs.size()),
    pi(M),
    targets(fill_targets()),
    tb(targets, &emission_probs),
    ib{&pi, &tb, &emission_probs, &saveGamma},
    dirty({true, true, true}),
    eta(defaultEta(hidden_states))
{
    recompute_initial_distribution();
    transition = Matrix<adouble>::Zero(M, M);
    transition.setZero();
    InferenceBundle *ibp = &ib;
#pragma omp parallel for
    for (unsigned int i = 0; i < obs.size(); ++i)
    {
        DEBUG1 << "creating HMM i: " << i << " L:" <<
                  this->obs.at(i).rows() << " M:" << ibp->pi->rows();
        hmms.at(i).reset(new HMM(i, this->obs.at(i), ibp));
    }
}

void InferenceManager::recompute_initial_distribution()
{
    for (int m = 0; m < M - 1; ++m)
    {
        pi(m) = exp(-(eta->R(hidden_states.at(m)))) - exp(-(eta->R(hidden_states.at(m + 1))));
        assert(pi(m) >= 0.0);
        assert(pi(m) <= 1.0);
    }
    pi(M - 1) = exp(-(eta->R(hidden_states.at(M - 1))));
    adouble small = eta->zero() + 1e-20;
    pi = pi.unaryExpr([small] (const adouble &x) { if (x < 1e-20) return small; return x; });
    pi /= pi.sum();
    CHECK_NAN(pi);
}

void InferenceManager::setRho(const double rho)
{
    this->rho = rho;
    dirty.rho = true;
}

void InferenceManager::setAlpha(const double alpha)
{
    this->alpha = alpha;
    dirty.theta = true;
}

void InferenceManager::setTheta(const double theta)
{
    this->theta = theta;
    dirty.theta = true;
}

void InferenceManager::parallel_do(std::function<void(hmmptr&)> lambda)
{
#pragma omp parallel for
    for (auto it = hmms.begin(); it < hmms.end(); ++it)
        lambda(*it);
}

template <typename T>
std::vector<T> InferenceManager::parallel_select(std::function<T(hmmptr &)> lambda)
{
    std::vector<T> ret(hmms.size());
#pragma omp parallel for
    for (unsigned int i = 0; i < hmms.size(); ++i)
        ret[i] = lambda(hmms[i]);
    return ret;
}
template std::vector<double> InferenceManager::parallel_select(std::function<double(hmmptr &)>);
template std::vector<adouble> InferenceManager::parallel_select(std::function<adouble(hmmptr &)>);

void InferenceManager::Estep(bool fbonly)
{
    DEBUG1 << "E step";
    do_dirty_work();
    tb.update(transition, true);
    parallel_do([fbonly] (hmmptr &hmm) { hmm->Estep(fbonly); });
}

std::vector<adouble> InferenceManager::Q(void)
{
    DEBUG1 << "InferenceManager::Q";
    do_dirty_work();
    std::vector<Vector<adouble> > ps = parallel_select<Vector<adouble> >([] (hmmptr &hmm) { return hmm->Q(); });
    std::vector<adouble> q(4, 0);
    for (unsigned int j = 0; j < 4; ++j)
        for (unsigned int i = 0; i < ps.size(); ++i)
            q[j] += ps[i][j];
    return q;
}

std::vector<std::map<block_key, Vector<double> >*> InferenceManager::getGammaSums()
{
    std::vector<std::map<block_key, Vector<double> >*> ret;
    for (auto &hmm : hmms)
        ret.push_back(&hmm->gamma_sums);
    return ret;
}

std::vector<Matrix<double>*> InferenceManager::getGammas()
{
    std::vector<Matrix<double>*> ret;
    for (auto &hmm : hmms)
        ret.push_back(&hmm->gamma);
    return ret;
}

std::vector<Matrix<double>*> InferenceManager::getXisums()
{
    std::vector<Matrix<double>*> ret;
    for (auto &hmm : hmms)
        ret.push_back(&hmm->xisum);
    return ret;
}

Matrix<adouble>& InferenceManager::getPi(void)
{
    static Matrix<adouble> mat;
    mat = pi;
    return mat;
}

Matrix<adouble>& InferenceManager::getTransition(void)
{
    return transition;
}

Matrix<adouble>& InferenceManager::getEmission(void)
{
    return emission;
}

std::map<block_key, Vector<adouble> >& InferenceManager::getEmissionProbs()
{
    return emission_probs;
}

std::vector<double> InferenceManager::loglik(void)
{
    return parallel_select<double>([] (hmmptr &hmm) { return hmm->loglik(); });
}

// Begin stuff for NPop inference manager
std::vector<Eigen::Map<Eigen::Matrix<int, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> > >
    InferenceManager::map_obs(const std::vector<int*> &observations, const std::vector<int> &obs_lengths)
{
    std::vector<Eigen::Map<Eigen::Matrix<int, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> > > ret;
    for (unsigned int i = 0; i < observations.size(); ++i)
        ret.push_back(Eigen::Matrix<int, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor>::Map(
                    observations[i], obs_lengths[i], 1 + 3 * npop));
    return ret;
}

template <size_t P>
void NPopInferenceManager<P>::populate_emission_probs()
{
    std::vector<std::map<block_key, Vector<adouble> > > eps(obs.size());
// #pragma omp parallel for
    for (unsigned int j = 0; j < obs.size(); ++j)
    {
        const Vector<adouble> tmp;
        const Eigen::Map<Eigen::Matrix<int, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> > ob = obs[j];
        const int q = ob.cols() - 1;
        for (int i = 0; i < ob.rows(); ++i)
        {
            block_key key(ob.row(i).tail(q).transpose());
            if (eps.at(j).count(key) == 0)
                eps.at(j).insert({key, tmp});
        }
    }
    for (const std::map<block_key, Vector<adouble> > &ep : eps)
        emission_probs.insert(ep.begin(), ep.end());
    for (const auto p : emission_probs)
        bpm_keys.push_back(p.first);
}

void InferenceManager::do_dirty_work()
{
    // Figure out what changed and recompute accordingly.
    if (dirty.eta)
    {
        recompute_initial_distribution();
        sfss = csfs->compute(*eta);
    }
    if (dirty.theta or dirty.eta)
        recompute_emission_probs();
    if (dirty.eta or dirty.rho)
        transition = compute_transition(*eta, rho);
    if (dirty.theta or dirty.eta or dirty.rho)
        tb.update(transition, false);
    // restore pristine status
    dirty = {false, false, false};
}


spp::sparse_hash_set<std::pair<int, block_key> > InferenceManager::fill_targets()
{
    DEBUG1 << "parallel filling targets";
    std::vector<spp::sparse_hash_set<std::pair<int, block_key> > > v(obs.size());
#pragma omp parallel for
    for (unsigned int j = 0; j < obs.size(); ++j)
    {
        const Eigen::Map<Eigen::Matrix<int, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> > ob = obs.at(j);
        const int q = ob.cols() - 1;
        for (int i = 0; i < ob.rows(); ++i)
        {
            if (ob(i, 0) <= 0)
                throw std::runtime_error("data are malformed: span <= 0");
            if (ob(i, 0) > 1)
                v.at(j).insert({ob(i, 0), block_key(ob.row(i).tail(q).transpose())});
        }
    }
    spp::sparse_hash_set<std::pair<int, block_key> > ret;
    DEBUG1 << "reducing targets";
    for (const spp::sparse_hash_set<std::pair<int, block_key> > &s : v)
        ret.insert(s.begin(), s.end());
    return ret;
}

void InferenceManager::setParams(const ParameterVector &params)
{
    eta.reset(new PiecewiseConstantRateFunction<adouble>(params, hidden_states));
    dirty.eta = true;
}

template <size_t P>
FixedVector<int, 2 * P> NPopInferenceManager<P>::make_tensordims()
{
    FixedVector<int, 2 * P> ret;
    for (size_t p = 0; p < P; ++p)
    {
        ret(2 * p) = na(p) + 1;
        ret(2 * p + 1) = n(p) + 1;
    }
    return ret;
}

template <size_t P>
block_key NPopInferenceManager<P>::bk_to_map_key(const block_key &bk)
{
    // Convert block_key to "map key" (a,b), which indexes into the CSFS
    Vector<int> ret(2 * P);
    for (size_t p = 0; p < P; ++p)
    {
        ret(2 * p) = bk(3 * p);
        ret(2 * p + 1) = bk(3 * p + 1);
    }
    return block_key(ret);
}

template <size_t P>
bool NPopInferenceManager<P>::is_monomorphic(const block_key &bk)
{
    for (unsigned int p = 0; p < P; ++p)
    {
        const int ind = 3 * p;
        if (bk(ind) != na(p) or bk(ind + 1) != bk(ind + 2))
            return false;
    }
    return true;
}

template <size_t P>
block_key NPopInferenceManager<P>::convert_monomorphic(const block_key &bk)
{
    if (!is_monomorphic(bk)) return bk;
    Vector<int> ret(3 * P);
    for (unsigned int p = 0; p < P; ++p)
    {
        const int ind = 3 * p;
        ret(ind) = 0;
        ret(ind + 1) = 0;
        ret(ind + 2) = bk(ind + 2);
    }
    return block_key(ret);
}


template <size_t P>
block_key NPopInferenceManager<P>::folded_key(const block_key &bk)
{
    block_key ret = bk;
    for (unsigned int p = 0; p < P; ++p)
    {
        const int ind = 3 * p;
        ret(ind) = na(p) - bk(ind);
        ret(ind + 1) = bk(ind + 2) - bk(ind + 1);
        ret(ind + 2) = bk(ind + 2);
    }
    return ret;
}

template <size_t P>
std::map<block_key, block_key_prob_map>
NPopInferenceManager<P>::construct_bins(const double polarization_error)
{
    assert(polarization_error >= 0.);
    assert(polarization_error <= 1.);
    std::vector<std::set<block_key> > bks(obs.size());
    for (unsigned int j = 0; j < obs.size(); ++j)
    {
        const Eigen::Matrix<int, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> ob = obs.at(j);
        const int q = ob.cols() - 1;
        for (int i = 0; i < ob.rows(); ++i)
            bks.at(j).emplace(ob.row(i).tail(q).transpose());
    }
    std::set<block_key> bksc;
    for (const std::set<block_key> &sbk : bks)
        bksc.insert(sbk.begin(), sbk.end());
    const std::vector<block_key> vbk(bksc.begin(), bksc.end());
    std::map<block_key, block_key_prob_map> ret;
    for (auto it = vbk.begin(); it < vbk.end(); ++it)
    {
        block_key bk = *it;
        block_key_prob_map m, m2;
        const std::set<block_key> bins = bin_key<P>::run(bk, na, 1.0);
        DEBUG1 << "block_key: " << bk << " bins:" << bins;
        for (const block_key &k : bins)
        {
            const std::map<block_key, double> probs =
                marginalize_key<P>::run(k.vals, n, na);
            for (const auto &p : probs)
            {
                const block_key mbk = convert_monomorphic(p.first);
                m[mbk] += (1. - polarization_error) * p.second;
                m[folded_key(mbk)] += polarization_error * p.second;
            }
        }
        double s = 0.0;
        DEBUG1 << "polarization error: " << polarization_error;
        for (const auto &p : m)
        {
            if (p.second <= 0 or is_monomorphic(p.first))
                continue;
            m2[p.first] = p.second;
            s += p.second;
        }
        block_key_prob_map bkpm;
        if (s <= 0)
        {
            DEBUG1 << bk;
            throw std::runtime_error("s<=0");
        }
        for (const auto &p : m2)
            bkpm[bk_to_map_key(p.first)] += p.second / s;
        ret.emplace(bk, bkpm);
    }
    DEBUG1 << ret;
    return ret;
}


template <size_t P>
void NPopInferenceManager<P>::recompute_emission_probs()
{
    // Initialize emission matrix
    // Due to lack of good support for tensors, we store the emission
    // tensor in "flattened" matrix form. Note that this is actually
    // 1 larger along each axis than the true number, because the SFS
    // ranges in {0, 1, ..., n_pop_k}.
    emission = Matrix<adouble>::Zero(M, (na(0) + 1) * sfs_dim);
    emission.setZero();

    Eigen::Matrix<adouble, Eigen::Dynamic, Eigen::Dynamic, Eigen::RowMajor> em_tmp(na(0) + 1, sfs_dim);
    std::vector<Matrix<adouble> > new_sfss = incorporate_theta(sfss, theta);
    for (int m = 0; m < M; ++m)
    {
        CHECK_NAN(new_sfss.at(m));
        em_tmp = new_sfss.at(m);
        emission.row(m) = Matrix<adouble>::Map(em_tmp.data(), 1, (na(0) + 1) * sfs_dim);
    }
    
    DEBUG1 << "recompute B";
    Matrix<adouble> e2 = Matrix<adouble>::Zero(M, 2);
    std::vector<adouble> avg_ct = eta->average_coal_times();
    adouble small = eta->zero() + 1e-20;
    for (int m = 0; m < M; ++m)
    {
        if (std::isnan(avg_ct.at(m).value()))
        {
            // if the two lineages are separated by a split, their
            // average coalescence time within each interval before
            // (more recently than) the split is undefined. in this
            // case, assign very low probabilities to all such
            // observations.
            e2(m, 0) = small;
            e2(m, 1) = small;
        }
        else
        {
            adouble log_e2m = -alpha * theta * avg_ct.at(m);
            e2(m, 0) = exp(log_e2m);
            e2(m, 1) = -expm1(log_e2m);
        }
    }
    const adouble zero = eta->zero();
    const adouble one = zero + 1.;
    DEBUG1 << "bpm_keys";
#pragma omp parallel for
for (auto it = bpm_keys.begin(); it < bpm_keys.end(); ++it)
    {
        const block_key k = *it;
        std::array<std::set<FixedVector<int, 3> >, P> s;
        Vector<adouble> tmp(M);
        tmp.fill(zero);
        bool reduced = true;
        bool miss = true;
        FixedVector<int, P> a, b, nb;
        for (unsigned int p = 0; p < P; ++p)
        {
            a(p) = k(3 * p);
            b(p) = k(1 + 3 * p);
            nb(p) = k(2 + 3 * p);
            reduced &= nb(p) == 0;
            if (na(p) > 0)
                miss &= a(p) == -1;
        }
        if (reduced and (miss or (a.minCoeff() >= 0)))
        {
            if (miss)
                tmp.fill(one);
            else // if (a.minCoeff() >= 0)
                tmp = e2.col(a.sum() % 2);
        }
        else
        {
            for (const auto &p : bins.at(k))
                tmp += p.second * tensorRef(p.first);
        }
        if (tmp.maxCoeff() > 1.0 or tmp.minCoeff() <= 0.0)
        {
            std::cout << k << std::endl;
            std::cout << miss << std::endl;
            std::cout << tmp.template cast<double>().transpose() << std::endl;
            std::cout << tmp.maxCoeff() << std::endl;
            std::cout << bins.at(k) << std::endl;
            throw std::runtime_error("probability vector not in [0, 1]");
        }
        CHECK_NAN(tmp);
#pragma omp critical(assign_emission_probs)
        {
            this->emission_probs.at(k) = tmp;
        }
    }
    DEBUG1 << "recompute done";
}


template <size_t P>
Vector<adouble> NPopInferenceManager<P>::tensorRef(const block_key &key)
{
    return tensorSlice<2 * P>::run(emission, key.vals, tensordims);
}


Matrix<adouble> sfs_cython(const int n, const ParameterVector p, 
        const double t1, const double t2, bool below_only)
{
    std::vector<double> hs{t1, t2};
    OnePopConditionedSFS<adouble> csfs(n);
    std::vector<Matrix<adouble> > v;
    PiecewiseConstantRateFunction<adouble> eta(p, hs);
    if (below_only)
        v = csfs.compute_below(eta);
    else
        v = csfs.compute(eta);
    return v.at(0);
}

OnePopInferenceManager::OnePopInferenceManager(
            const int n,
            const std::vector<int> obs_lengths,
            const std::vector<int*> observations,
            const std::vector<double> hidden_states,
            const double polarization_error) :
        NPopInferenceManager(
                FixedVector<int, 1>::Constant(n),
                FixedVector<int, 1>::Constant(2),
                obs_lengths, observations, hidden_states, polarization_error,
                new OnePopConditionedSFS<adouble>(n)) {}

JointCSFS<adouble>* create_jcsfs(int n1, int n2, int a1, int a2, const std::vector<double> &hidden_states)
{
    if (a1 == 0 and a2 == 2)
        throw std::runtime_error("(0,2) not supported");
    return new JointCSFS<adouble>(n1, n2, a1, a2, hidden_states);
}

TwoPopInferenceManager::TwoPopInferenceManager(
            const int n1, const int n2,
            const int a1, const int a2,
            const std::vector<int> obs_lengths,
            const std::vector<int*> observations,
            const std::vector<double> hidden_states,
            const double polarization_error) :
        NPopInferenceManager(
                (FixedVector<int, 2>() << n1, n2).finished(),
                (FixedVector<int, 2>() << a1, a2).finished(),
                obs_lengths, observations, hidden_states, polarization_error,
                create_jcsfs(n1, n2, a1, a2, hidden_states)), a1(a1), a2(a2)
{
    if (a1 + a2 != 2)
        throw std::runtime_error("configuration not supported");
}

void TwoPopInferenceManager::setParams(
        const ParameterVector &distinguished_params,
        const ParameterVector &params1,
        const ParameterVector &params2,
        const double split)
{
    InferenceManager::setParams(distinguished_params);
    dynamic_cast<JointCSFS<adouble>*>(csfs.get())->pre_compute(params1, params2, split);
}

template class NPopInferenceManager<1>;
template class NPopInferenceManager<2>;
