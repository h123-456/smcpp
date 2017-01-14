#!/bin/bash

VERSION=$(git describe --tags)
BINARY=dist/smcpp-$VERSION-$TRAVIS_OS_NAME

if [[ $TRAVIS_OS_NAME == 'osx' ]]; then
    OS=MacOSX
else
    OS=Linux
fi

wget http://repo.continuum.io/miniconda/Miniconda3-latest-$OS-x86_64.sh -O miniconda.sh
bash miniconda.sh -b -p $HOME/miniconda
export PATH="$HOME/miniconda/bin:$PATH"
hash -r
conda config --set always_yes yes --set changeps1 no
conda update -q conda
conda info -a
conda env create -f .conda.yml
source activate smcpp
CC=gcc CXX=g++ python setup.py develop
pip install git+https://github.com/pyinstaller/pyinstaller@483c819
pyinstaller --clean -s -F --exclude-module PyQt5 --exclude-module PyQt4 --exclude-module mpl_toolkits.tests --exclude-module pyside --exclude-module matplotlib.tests scripts/smc++
test/integration/test.sh dist/smc++
[[ $? -ne 0 ]] && exit
mv dist/smc++ $BINARY

# Build conda package
conda install conda-build anaconda-client
conda/meta.yaml.py $VERSION > conda/meta.yaml
conda build -c bioconda -c conda-forge conda
conda convert --platform all $HOME/miniconda2/conda-bld/**/smcpp-*.tar.bz2 --output-dir conda-bld/
anaconda -t $ANACONDA_TOKEN upload --force conda-bld/**/smcpp-*.tar.bz2