#!/bin/sh

# Usage: check-utility.sh [--install-groonga]
#                         [--check-install]
#                         [--check-address]
#                         [--enable-repository]
#
# CODES="squeeze wheezy unstable lucid natty oneiric precise"
# DISTRIBUTIONS="centos fedora"

CHROOT_ROOT=/var/lib/chroot
CHECK_ADDRESS=0
CHECK_INSTALL=0
CHECK_INSTALL_PACKAGE=groonga
CHECK_BUILD=0
ENABLE_REPOSITORY=0
DISABLE_REPOSITORY=0
INSTALL_SCRIPT=0
INSTALL_GROONGA=0
UNINSTALL_GROONGA=0

common_deb_procedure ()
{
    for code in $CODES; do
	for arch in $DEB_ARCHITECTURES; do
	    root_dir=$CHROOT_ROOT/$code-$arch
	    eval $1 $code $arch $root_dir
	done
    done
}

common_rpm_procedure ()
{
    for dist in $DISTRIBUTIONS; do
	case $dist in
	    "fedora")
		DISTRIBUTIONS_VERSION="19"
		;;
	    "centos")
		DISTRIBUTIONS_VERSION="5 6"
		;;
	esac
	for ver in $DISTRIBUTIONS_VERSION; do
	    for arch in $RPM_ARCHITECTURES; do
		root_dir=$CHROOT_ROOT/$dist-$ver-$arch
		eval $1 $dist $arch $ver $root_dir
	    done
	done
    done
}

copy_and_exec_script ()
{
    chroot_host=$1
    root_dir=$2
    script=$3
    if [ -d $root_dir ]; then
	echo "copy script $script to $root_dir/tmp"
	sudo rm -f $root_dir/tmp/$script
	cp tmp/$script $root_dir/tmp
	sudo chmod 755 $root_dir/tmp/$script
	sudo chname $chroot_host chroot $root_dir /tmp/$script
    fi
}

echo_packages_repository_address ()
{
    root_dir=$1
    code=$2
    arch=$3
    address=`grep "packages.groonga.org" $root_dir/etc/hosts | grep -v "#"`
    nameserver=`grep "nameserver" $root_dir/etc/resolv.conf | grep -v "#" | cut -d ' ' -f 2`
    if [ -z "$address" ]; then
	echo "$code-$arch: default nameserver:$nameserver"
    else
	echo "$code-$arch: $address nameserver:$nameserver"
    fi
}

setup_distributions ()
{
    if [ -z "$DISTRIBUTIONS" ]; then
	DISTRIBUTIONS="centos fedora"
    fi
}

setup_rpm_architectures ()
{
    if [ -z "$RPM_ARCHITECTURES" ]; then
	RPM_ARCHITECTURES="i386 x86_64"
    fi
}

setup_codes ()
{
    if [ -z "$CODES" ]; then
	CODES="squeeze wheezy jessie unstable lucid precise quantal raring"
    fi
}
setup_deb_architectures ()
{
    if [ -z "$DEB_ARCHITECTURES" ]; then
	DEB_ARCHITECTURES="i386 amd64"
    fi
}

check_packages_repository_address ()
{
    common_deb_procedure "check_packages_deb_repository_address"
    common_rpm_procedure "check_packages_rpm_repository_address"
}

check_packages_deb_repository_address ()
{
    code=$1
    arch=$2
    root_dir=$3
    if [ -d "$root_dir" ]; then
	echo_packages_repository_address "$root_dir" "$code" "$arch"
    fi
}

check_packages_rpm_repository_address ()
{
    dist=$1
    arch=$2
    ver=$3
    root_dir=$4
    if [ -d "$root_dir" ]; then
	echo_packages_repository_address "$root_dir" "$dist-$ver" "$arch"
    fi
}


host_address ()
{
    ifconfig_result=`LANG=C /sbin/ifconfig wlan0`
    inet_addr=`echo "$ifconfig_result" | grep "inet addr:192"`
    address=`echo $inet_addr | ruby -ne '/inet addr:(.+?)\s/ =~ $_ && puts($1)'`
    HOST_ADDRESS=$address
}

check_build_packages ()
{
    common_deb_procedure "check_build_deb_packages"
    common_rpm_procedure "check_build_rpm_packages"
}

check_build_deb_packages ()
{
    code=$1
    arch=$2
    BASE_VERSION=`cat ../base_version`
    RESULT_SET=`find apt/repositories -name "*$BASE_VERSION*" | grep $code | grep $arch`
    if [ -z "$RESULT_SET" ]; then
	printf "%8s %5s %s =>  0 deb\n" $code $arch $BASE_VERSION
    else
	PACKAGE_COUNT=`find apt/repositories -name "*$BASE_VERSION*" | grep $code | grep $arch | wc | awk '{print \$1}'`
	printf "%8s %5s %s => %2d debs\n" $code $arch $BASE_VERSION $PACKAGE_COUNT
    fi
}

check_build_rpm_packages ()
{
    dist=$1
    arch=$2
    ver=$3
    BASE_VERSION=`cat ../base_version`
    FIND_PATH=yum/repositories/$dist/$ver/$arch
    RESULT_SET=`find $FIND_PATH -name "*$BASE_VERSION*"`
    if [ -z "$RESULT_SET" ]; then
	printf "%8s %6s %s =>  0 rpm\n" $dist$ver $arch $BASE_VERSION
    else
	PACKAGE_COUNT=`find $FIND_PATH -name "*$BASE_VERSION*" | wc -l`
	printf "%8s %6s %s => %2d rpms\n" $dist$ver $arch $BASE_VERSION $PACKAGE_COUNT
    fi
}

check_installed_groonga_packages ()
{
    common_deb_procedure "check_installed_groonga_deb_packages"
    common_rpm_procedure "check_installed_groonga_rpm_packages"
}

check_installed_groonga_deb_packages ()
{
    code=$1
    arch=$2
    root_dir=$3
    CHECK_SCRIPT=check-deb-groonga.sh
    cat > tmp/$CHECK_SCRIPT <<EOF
#!/bin/sh
dpkg -l | grep $CHECK_INSTALL_PACKAGE
EOF
    copy_and_exec_script $code-$arch $root_dir $CHECK_SCRIPT
}    


check_installed_groonga_rpm_packages ()
{
    dist=$1
    arch=$2
    ver=$3
    root_dir=$4
    CHECK_SCRIPT=check-rpm-groonga.sh
    cat > tmp/$CHECK_SCRIPT <<EOF
#!/bin/sh
rpm -qa | grep $CHECK_INSTALL_PACKAGE
EOF
    copy_and_exec_script $code-$ver-$arch $root_dir $CHECK_SCRIPT 
}

install_groonga_packages ()
{
    common_deb_procedure "install_groonga_deb_packages"
    common_rpm_procedure "install_groonga_rpm_packages"
}

install_groonga_deb_packages ()
{
    code=$1
    arch=$2
    root_dir=$3
    cat > tmp/install-aptitude-groonga.sh <<EOF
#!/bin/sh
sudo aptitude clean
rm -f /var/lib/apt/lists/packages.groonga.org_*
rm -f /var/lib/apt/lists/partial/packages.groonga.org_*
sudo aptitude update
sudo aptitude -V -D -y --allow-untrusted install groonga-keyring
sudo aptitude update
sudo aptitude -V -D -y install groonga
sudo aptitude -V -D -y install groonga-tokenizer-mecab
sudo apt-get -y install libgroonga-dev
#sudo aptitude -V -D -y install groonga-munin-plugins
EOF
    cat > tmp/install-aptget-groonga.sh <<EOF
#!/bin/sh
sudo apt-get clean
rm -f /var/lib/apt/lists/packages.groonga.org_*
rm -f /var/lib/apt/lists/partial/packages.groonga.org_*
sudo apt-get update
sudo apt-get -y --allow-unauthenticated install groonga-keyring
sudo apt-get update
sudo apt-get -y install groonga
sudo apt-get -y install groonga-tokenizer-mecab
sudo apt-get -y install libgroonga-dev
#sudo apt-get -y install groonga-munin-plugins
EOF
    INSTALL_SCRIPT=""
    case $code in
	squeeze|unstable)
	    INSTALL_SCRIPT=install-aptitude-groonga.sh
	    ;;
	*)
	    INSTALL_SCRIPT=install-aptget-groonga.sh
	    ;;
    esac
    copy_and_exec_script $code-$arch $root_dir $INSTALL_SCRIPT
}

install_groonga_rpm_packages ()
{
    dist=$1
    arch=$2
    ver=$3
    root_dir=$4
    cat > tmp/install-centos5-groonga.sh <<EOF
sudo rpm -ivh http://packages.groonga.org/centos/groonga-release-1.1.0-1.noarch.rpm
sudo yum makecache
sudo yum install -y groonga
sudo yum install -y groonga-tokenizer-mecab
rm -f epel-release-*.rpm
wget http://download.fedoraproject.org/pub/epel/5/i386/epel-release-5-4.noarch.rpm
sudo rpm -ivh epel-release-5-4.noarch.rpm
sudo yum install -y groonga-devel
#sudo yum install -y groonga-munin-plugins
EOF
    cat > tmp/install-centos6-groonga.sh <<EOF
sudo rpm -ivh http://packages.groonga.org/centos/groonga-release-1.1.0-1.noarch.rpm
sudo yum makecache
sudo yum install -y groonga
sudo yum install -y groonga-tokenizer-mecab
sudo rpm -ivh http://download.fedoraproject.org/pub/epel/6/i386/epel-release-6-8.noarch.rpm
sudo yum install -y groonga-devel
#sudo yum install -y groonga-munin-plugins
EOF
    cat > tmp/install-fedora-groonga.sh <<EOF
sudo rpm -ivh http://packages.groonga.org/fedora/groonga-release-1.1.0-1.noarch.rpm
sudo yum makecache
sudo yum install -y groonga
sudo yum install -y groonga-tokenizer-mecab
sudo yum install -y groonga-devel
#sudo yum install -y groonga-munin-plugins
EOF
    case "$dist-$ver" in
	centos-5)
	    INSTALL_SCRIPT=install-centos5-groonga.sh
	    ;;
	centos-6)
	    INSTALL_SCRIPT=install-centos6-groonga.sh
	    ;;
	fedora-19)
	    INSTALL_SCRIPT=install-fedora-groonga.sh
	    ;;
	*)
	    ;;
    esac
    copy_and_exec_script $dist-$ver-$arch $root_dir $INSTALL_SCRIPT
}

uninstall_groonga_packages ()
{
    UNINSTALL_SCRIPT=uninstall-deb-groonga.sh
    cat > $UNINSTALL_SCRIPT <<EOF
#!/bin/sh
sudo apt-get purge -y groonga-* mysql-*
EOF
    for code in $CODES; do
	for arch in $DEB_ARCHITECTURES; do
	    root_dir=$CHROOT_ROOT/$code-$arch
	    if [ -d $root_dir ]; then
		echo "copy uninstall script $UNINSTALL_SCRIPT to $root_dir/tmp"
		sudo rm -f $root_dir/tmp/$UNINSTALL_SCRIPT
		cp $UNINSTALL_SCRIPT $root_dir/tmp
		chmod 755 $root_dir/tmp/$UNINSTALL_SCRIPT
		sudo chname $code-$arch chroot $root_dir /tmp/$UNINSTALL_SCRIPT
	    fi
	done
    done
    UNINSTALL_SCRIPT=uninstall-rpm-groonga.sh
    cat > $UNINSTALL_SCRIPT <<EOF
#!/bin/sh
sudo yum remove -y groonga-* mysql-*
EOF
    for dist in $DISTRIBUTIONS; do
	case $dist in
	    "fedora")
		DISTRIBUTIONS_VERSION="19"
		;;
	    "centos")
		DISTRIBUTIONS_VERSION="5 6"
		;;
	esac
	for ver in $DISTRIBUTIONS_VERSION; do
	    for arch in $RPM_ARCHITECTURES; do
		root_dir=$CHROOT_ROOT/$dist-$ver-$arch
		if [ -d $root_dir ]; then
		    echo "copy install script $UNINSTALL_SCRIPT to $root_dir/tmp"
		    sudo rm -f $root_dir/tmp/$UNINSTALL_SCRIPT
		    cp $UNINSTALL_SCRIPT $root_dir/tmp
		    chmod 755 $root_dir/tmp/$UNINSTALL_SCRIPT
		    sudo chname $code-$ver-$arch chroot $root_dir /tmp/$UNINSTALL_SCRIPT
		fi
	    done
	done
    done
}



enable_temporary_groonga_repository ()
{
    SCRIPT=enable-repository.sh
    cat > tmp/$SCRIPT <<EOF
#!/bin/sh

grep -v "packages.groonga.org" /etc/hosts > /tmp/hosts
echo "$HOST_ADDRESS packages.groonga.org" >> /tmp/hosts
cp -f /tmp/hosts /etc/hosts
EOF
    common_deb_procedure "enable_temporary_groonga_deb_repository"
    common_rpm_procedure "enable_temporary_groonga_rpm_repository"
    check_packages_repository_address
}

enable_temporary_groonga_deb_repository ()
{
    code=$1
    arch=$2
    root_dir=$3
    SCRIPT=enable-repository.sh
    today=`date '+%Y%m%d.%s'`
    copy_and_exec_script $code-$arch $root_dir $SCRIPT
}

enable_temporary_groonga_rpm_repository ()
{
    dist=$1
    arch=$2
    ver=$3
    root_dir=$4
    SCRIPT=enable-repository.sh
    today=`date '+%Y%m%d.%s'`
    copy_and_exec_script $dist-$ver-$arch $root_dir $SCRIPT
}

disable_temporary_groonga_repository ()
{
    SCRIPT=disable-repository.sh
    cat > tmp/$SCRIPT <<EOF
#!/bin/sh

grep -v "packages.groonga.org" /etc/hosts > /tmp/hosts
cp -f /tmp/hosts /etc/hosts
EOF
    common_deb_procedure "disable_temporary_groonga_deb_repository"
    common_rpm_procedure "disable_temporary_groonga_rpm_repository"
    check_packages_repository_address
}

disable_temporary_groonga_deb_repository ()
{
    code=$1
    arch=$2
    root_dir=$3
    copy_and_exec_script $code-$arch $root_dir $SCRIPT
}

disable_temporary_groonga_rpm_repository ()
{
    dist=$1
    arch=$2
    ver=$3
    root_dir=$4
    copy_and_exec_script $code-$arch $root_dir $SCRIPT
}

host_address
echo $HOST_ADDRESS

while [ $# -ne 0 ]; do
    case $1 in
	--check-install)
	    CHECK_INSTALL=1
	    shift
	    if [ ! -z "$1" ]; then
		case $1 in
		    groonga|mroonga|roonga|mecab)
			CHECK_INSTALL_PACKAGE=$1
			;;
		    *)
			;;
		esac
	    fi
	    ;;
	--check-address)
	    CHECK_ADDRESS=1
	    shift
	    ;;
	--check-build)
	    CHECK_BUILD=1
	    shift
	    ;;
	--enable-repository)
	    ENABLE_REPOSITORY=1
	    shift
	    ;;
	--disable-repository)
	    DISABLE_REPOSITORY=1
	    shift
	    ;;
	--install-groonga)
	    INSTALL_GROONGA=1
	    shift
	    ;;
	--uninstall-groonga)
	    UNINSTALL_GROONGA=1
	    shift
	    ;;
	--code)
	    shift
	    if [ "$1" = "all" ]; then
		setup_codes
	    else
		CODES=$1
	    fi
	    shift
	    ;;
	--code-arch)
	    shift
	    if [ "$1" = "all" ]; then
		setup_deb_architectures
	    else 
		DEB_ARCHITECTURES=$1
	    fi
	    shift
	    ;;
	--dist)
	    shift
	    if [ "$1" = "all" ]; then
		setup_distributions
	    else
		DISTRIBUTIONS=$1
	    fi
	    shift
	    ;;
	--dist-arch)
	    shift
	    if [ "$1" = "all" ]; then
		setup_rpm_architectures
	    else
		RPM_ARCHITECTURES=$1
	    fi
	    shift
	    ;;
	*)
	    shift
	    ;;
    esac
done

mkdir -p tmp
setup_deb_architectures
setup_rpm_architectures

if [ $CHECK_INSTALL -ne 0 ]; then
    check_installed_groonga_packages
fi
if [ $CHECK_ADDRESS -ne 0 ]; then
    check_packages_repository_address
fi
if [ $CHECK_BUILD -ne 0 ]; then
    check_build_packages
fi
if [ $ENABLE_REPOSITORY -ne 0 ]; then
    enable_temporary_groonga_repository
fi
if [ $DISABLE_REPOSITORY -ne 0 ]; then
    disable_temporary_groonga_repository
fi
if [ $INSTALL_GROONGA -ne 0 ]; then
    install_groonga_packages
fi
if [ $UNINSTALL_GROONGA -ne 0 ]; then
    uninstall_groonga_packages
fi

