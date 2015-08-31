#ifndef MORAN_EIGENSYSTEM_H
#define MORAN_EIGENSYSTEM_H

#include <map>
#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <iostream>
#include <gmpxx.h>
#include "mpq_support.h"

typedef struct 
{
    MatrixXq U, Uinv;
    VectorXq D;
} 
MoranEigensystem;

MoranEigensystem& compute_moran_eigensystem(int);

#endif