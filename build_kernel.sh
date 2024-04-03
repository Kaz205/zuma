export ARCH=arm64
export SUBARCH=arm64
export ak=$HOME/AnyKernel3_zuma

NAME=$(git describe --exact-match --tags 2> /dev/null || git rev-parse --short HEAD)
git submodule init && git submodule update

cd "$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
make $MAKE_PARAMS zuma_defconfig -j$(nproc --all)
make $MAKE_PARAMS Image.lz4 dtbs -j$(nproc --all)

if [ $? -ne 0 ]
then
    exit 1
fi

cp out/arch/arm64/boot/Image.lz4 $ak/Image.lz4
find out/google-modules/soc/gs/arch/arm64/boot/dts/google/ -name '*.dtb' -exec cat {} + > $ak/dtb
cd $ak
zip -FSr9 $NAME.zip ./*  -x "*.zip"
echo "Kernel zip name: $NAME"
