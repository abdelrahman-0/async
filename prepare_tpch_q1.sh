#!/bin/bash

SCRIPT=$(realpath "$0")
SCRIPTPATH=$(dirname "$SCRIPT")
TPCH_DATA_DIR="tpch-dbgen"
RAW_DATA_DIR_PREFIX="data"

cd ..
if [ ! -d "$TPCH_DATA_DIR" ] ; then
    git clone https://github.com/electrum/tpch-dbgen.git
fi
cd $TPCH_DATA_DIR
make -s
for i in 1
do
  DIR="$RAW_DATA_DIR_PREFIX"_sf_"$i"
  if [ ! -d "$DIR" ] ; then
      mkdir -p $DIR
      DSS_PATH=$DIR ./dbgen -s $i
  fi
  cd $SCRIPTPATH
  mkdir -p $DIR
  ./build/storage/load_data lineitemQ1 ../"$TPCH_DATA_DIR"/"$DIR"/lineitem.tbl "$DIR"/lineitemQ1.dat
  cd ../$TPCH_DATA_DIR
done
