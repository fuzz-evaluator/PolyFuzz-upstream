

export ROOT=`pwd`
export target=bind9
export FUZZ_HOME="$ROOT/fuzz_root"

#dependences
apt-get install -y python-ply
apt-get install -y libuv1.dev
apt-get install -y libnghttp2-dev

function compile ()
{
	if [ -d "$ROOT/$target" ]; then
		rm -rf $ROOT/$target*
	fi
	
	if [ ! -d "$FUZZ_HOME" ]; then
	    mkdir "$FUZZ_HOME"
	fi
	
	git clone https://gitlab.isc.org/isc-projects/bind9
	cd $target

	export CC="afl-cc -lxFuzztrace"
	export CXX="afl-c++ -lxFuzztrace"
	
	autoreconf -fi
	./configure --disable-shared --enable-static --enable-developer --without-cmocka --without-zlib --disable-linux-caps --prefix="$FUZZ_HOME"
	

	make -j4
	make install

	cd -
}


cd $ROOT
compile


