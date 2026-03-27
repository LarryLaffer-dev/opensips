#!/bin/sh

BUILD_ROOT=/tmp/opensips_build
FILELIST=/tmp/filelist
FILELIST_BINARY=/tmp/filelist_binary
TMP_TAR=/tmp/opensips_min.tar.gz
IMG_TAR=/tmp/opensips_img.tar.gz

clean_build_root() {
    rm -Rf $BUILD_ROOT
    mkdir -p $BUILD_ROOT
    rm -f $TMP_TAR
    rm -f $IMG_TAR
}

fs_files_debian() {
    local PACKAGES
    PACKAGES=$(dpkg-query -f '${binary:Package}\n' -W 'opensips*')
    PACKAGES="libc6 $PACKAGES"
    for pkg in $PACKAGES
    do
        dpkg-query -L $pkg 2> /dev/null
    done
}

extra_files_debian() {
    cat << EOF
/etc
/bin
/usr/bin
/usr/lib
EOF
}

sort_filelist() {
    sort $FILELIST | uniq > $FILELIST.new
    mv -f $FILELIST.new $FILELIST
}

filter_unnecessary_files() {
    if [ -f /tmp/modules.lst ]; then
	rm -f /tmp/modules.lst.new
	for f in $(cat /tmp/modules.lst)
    	do
		f=$(echo $f | sed "s/.so//")
		echo "$f.so" >> /tmp/modules.lst.new
	done
	ls -1 /usr/lib/x86_64-linux-gnu/opensips/modules/ > /tmp/modules.installed
	for f in $(grep -Fvf /tmp/modules.lst.new /tmp/modules.installed)
	do
		echo "Removing module $f..."
		sed -i \
			-e "\|^/usr/lib/x86_64-linux-gnu/opensips/modules/$f|d" \
			$FILELIST
	done
    fi

    sed -i \
        -e '\|^/\.$|d' \
        -e '\|^/lib/systemd|d' \
        -e '\|^/etc|d' \
        -e '\|^/usr/share|d' \
        -e '\|^/usr/lib/x86_64-linux-gnu/gconv/|d' \
        -e '\|^/.*\.flac$|d' \
        -e '\|^/.*/flac$|d' \
        -e '\|^/usr/sbin/opensipsctl|d' \
        -e '\|^/usr/sbin/opensipsdbctl|d' \
        -e '\|^/usr/sbin/osipsconsole|d' \
        $FILELIST
}

ldd_helper() {
    TESTFILE=$1
    ldd $TESTFILE 2> /dev/null > /dev/null || return

    RESULT=$(ldd $TESTFILE | grep -oP '\s\S+\s\(\S+\)' | sed -e 's/^\s//' -e 's/\s.*$//') #'
    echo "$RESULT"
}

find_binaries() {
    rm -f $FILELIST_BINARY
    for f in $(cat $FILELIST)
    do
        echo $f
        ldd_helper $f >> $FILELIST_BINARY
    done

    sort $FILELIST_BINARY | sort | uniq | sed -e '/linux-vdso.so.1/d' > $FILELIST_BINARY.new
    mv -f $FILELIST_BINARY.new $FILELIST_BINARY
    cat $FILELIST_BINARY | xargs realpath > $FILELIST_BINARY.new
    cat $FILELIST_BINARY.new >> $FILELIST_BINARY
    rm -f $FILELIST_BINARY.new
}

tar_files() {
    local TARLIST=/tmp/tarlist
    cat $FILELIST > $TARLIST
    cat $FILELIST_BINARY >> $TARLIST
    tar -czf $TMP_TAR --no-recursion -T $TARLIST
    rm -f $TARLIST
}

make_image_tar() {
    local CURDIR=`pwd`
    cd $BUILD_ROOT
    tar xzf $TMP_TAR
    tar czf $IMG_TAR *
    cd $CURDIR
}

clean_build_root
fs_files_debian > $FILELIST
extra_files_debian >> $FILELIST
sort_filelist
filter_unnecessary_files
find_binaries
tar_files
make_image_tar
mv $IMG_TAR .
clean_build_root
