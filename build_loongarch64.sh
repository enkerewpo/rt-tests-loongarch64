git submodule update --init --recursive
cd numactl
./autogen.sh
./configure --host=loongarch64-unknown-linux-gnu CC=loongarch64-unknown-linux-gnu-gcc --prefix=$(pwd)/install
make -j$(nproc)
make install
cd ..
CFLAGS="-static -L$(pwd)/numactl/install/lib -I$(pwd)/numactl/install/include" make ARCH=loongarch64 CROSS_COMPILE=loongarch64-unknown-linux-gnu- all
qemu-loongarch64 -L . ./cyclictest -h
file ./cyclictest
