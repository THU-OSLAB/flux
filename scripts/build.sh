#!/bin/bash

set -e

SCRIPT_DIR=$(dirname $(readlink -f $0))
ROOT_DIR=$(realpath ${SCRIPT_DIR}/../)

CORES=`getconf _NPROCESSORS_ONLN`

if ! hash meson 2> /dev/null; then
  echo "Missing meson. Please install meson!"
  exit 1
fi

build_glibc() {
    echo "Building glibc..."
    
    GLIBC_DIR=${ROOT_DIR}/third-party/glibc
    GLIBC_PATCHES_DIR=${ROOT_DIR}/lib/patches/glibc
    GLIBC_INSTALL_DIR=${ROOT_DIR}/third-party/install

    mkdir -p $GLIBC_INSTALL_DIR

    prev=$(cat "$ROOT_DIR/lib/.glibc_installed_ver" 2>&1 || true)
    cur=$(cat "$GLIBC_PATCHES_DIR"/* | sha256sum)

    if [ "$prev" == "$cur" ] && [ -f $GLIBC_INSTALL_DIR/lib/ld-linux-x86-64.so.2 ] && [ -f $GLIBC_INSTALL_DIR/lib/libc.so.6 ]; then
      exit 0
    fi

    unset LD_LIBRARY_PATH

    pushd $GLIBC_DIR

    # Build and install glibc
    mkdir -p build
    pushd build
    ../configure --prefix $GLIBC_INSTALL_DIR
    # make -j $CORES CFLAGS="-g3 -U_FORTIFY_SOURCE -O3"
    # make install -j $CORES INSTALL_STRIP_PROGRAM=true
    make -j $CORES CFLAGS="-U_FORTIFY_SOURCE -O3"
    make install -j $CORES

    # ld.so.cache needs to be regenerated after glibc installation
    cp /etc/ld.so.conf $GLIBC_INSTALL_DIR/etc/ld.so.conf

    popd
    popd
}

build_rdma() {
    echo "Building rdma-core..."
    pushd $ROOT_DIR/third-party/rdma-core
    if ! EXTRA_CMAKE_FLAGS="-DENABLE_STATIC=1 -DNO_PYVERBS=1" MAKEFLAGS=-j$CORES ./build.sh; then
        echo "Building rdma-core failed"
        echo "If you see \"Does not match the generator used previously\" try running \"make submodules-clean\" first"
        exit 1
    fi
    popd
}

build_dpdk() {
    disable_driver='crypto/*,raw/*,baseband/*,net/af_packet,net/af_xdp,net/ark,net/atlantic,net/avp,net/axgbe,net/bnx2x,net/bonding,net/cnxk,net/cxgbe,net/dpaa,net/dpaa2,net/e1000,net/ena,net/enetc,net/enetfec,net/enic,net/fm10k,net/hinic,net/hns3,net/iavf,net/ice,net/igc,net/ionic,net/ipn3ke,net/kni,net/liquidio,net/memif,net/mlx4,net/mvneta,net/mvpp2,net/nfb,net/nfp,net/ngbe,net/octeontx,net/octeontx_ep,net/pcap,net/pfe,net/qede,net/sfc,net/softnic,net/thunderx,net/txgbe,net/vhost,net/virtio,net/vmxnet3'

    export EXTRA_CFLAGS=-I$PWD/rdma-core/build/include
    export EXTRA_LDFLAGS=-L$PWD/rdma-core/build/lib
    export PKG_CONFIG_PATH=$PWD/rdma-core/build/lib/pkgconfig

    echo "Building DPDK..."

    pushd $ROOT_DIR/third-party/dpdk
    meson build
    meson configure -Ddisable_drivers=$disable_driver -Dexamples='' -Denable_kmods=false -Dtests=false build
    meson configure -Dprefix=$PWD/build build
    ninja -C build
    ninja -C build install
    popd

    export EXTRA_CFLAGS=
    export EXTRA_LDFLAGS=
    export PKG_CONFIG_PATH=
}

build_spdk() {
    echo "Building SPDK..."

    pushd $ROOT_DIR/third-party/spdk
    # sudo ./scripts/pkgdep.sh
    ./configure --without-crypto --without-fuse --without-aio-fsdev --without-nvme-cuse --without-vhost --without-virtio --with-dpdk=${ROOT_DIR}/third-party/dpdk/build
    make -j "$(nproc)"
    make -f Makefile.sharedlib clean
    make -f Makefile.sharedlib
    install -m 755 libspdk.so build/lib/libspdk.so
    popd
}

if [ "$1" == "rdma" ]; then
    build_rdma
elif [ "$1" == "dpdk" ]; then
    build_dpdk
elif [ "$1" == "spdk" ]; then
    build_spdk
elif [ "$1" == "glibc" ]; then
    build_glibc
else
    echo "Usage: $0 rdma|dpdk|spdk|flux|glibc"
    exit 1
fi
