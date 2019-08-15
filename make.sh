# Module
gcc -fPIC -g -c src/e_mod_main.c $CFLAGS `pkg-config --cflags enlightenment elementary eeze` -o src/e_mod_main.o
[ $? -eq 0 ] || exit 1
gcc -shared -fPIC -DPIC src/e_mod_main.o `pkg-config --libs enlightenment elementary eeze` -Wl,-soname -Wl,module.so -o src/module.so
[ $? -eq 0 ] || exit 1

#Edje
edje_cc -v -id ./images e-module-dfu.edc e-module-dfu.edj
[ $? -eq 0 ] || exit 1
edje_cc -v -id ./images dfu.edc dfu.edj
[ $? -eq 0 ] || exit 1

prefix=$(pkg-config --variable=prefix enlightenment)
release=$(pkg-config --variable=release enlightenment)
host_cpu=$(uname -m)
MODULE_ARCH="linux-gnu-$host_cpu-$release"

sudo /usr/bin/mkdir -p $prefix'/lib/enlightenment/modules/e_dfu/'$MODULE_ARCH
sudo /usr/bin/install -c src/module.so $prefix/lib/enlightenment/modules/e_dfu/$MODULE_ARCH/module.so
sudo /usr/bin/install -c module.desktop $prefix/lib/enlightenment/modules/e_dfu/module.desktop
sudo /usr/bin/install -c -m 644 e-module-dfu.edj dfu.edj $prefix/lib/enlightenment/modules/e_dfu
