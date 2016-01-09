#ifndef INFERENCE_BUNDLE_H
#define INFERENCE_BUNDLE_H

#include "common.h"

class TransitionBundle;

struct InferenceBundle
{
    Vector<adouble> *pi;
    TransitionBundle *tb;
    std::map<block_key, Vector<adouble> > *emission_probs;
    bool *saveGamma;
    int *spanCutoff;
};

#endif
