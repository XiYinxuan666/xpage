# 用于 X-pageOS 内核交叉编译的环境变量设置
# 使用前请在 xpage 目录下 source buildenv.sh

export BASEDIR="$(pwd)/x86_64-elf"
export CPPFLAGS="-I$BASEDIR/include/c++/v1 -I$BASEDIR/include -nostdlibinc -D__ELF__ -D_LDBL_EQ_DBL -D_GNU_SOURCE -D_POSIX_TIMERS"
export LDFLAGS="-L$BASEDIR/lib"
