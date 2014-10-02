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
CHECK_INSTALL_PACKAGE=mysql-server-mroonga
CHECK_BUILD=0
CHECK_DEPENDS=0
CHECK_PROVIDES=0
ENABLE_REPOSITORY=0
DISABLE_REPOSITORY=0
INSTALL_SCRIPT=0
INSTALL_MROONGA=0
UNINSTALL_MROONGA=0

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

echo_packages_repository_address ()
{
    root_dir=$1
    code=$2
    arch=$3
    address=`grep "packages.groonga.org" $root_dir/etc/hosts | grep -v "#"`
    if [ -z "$address" ]; then
	echo "$code-$arch: default"
    else
	echo "$code-$arch: $address"
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
    root_dir=$4
    echo_packages_repository_address "$root_dir" "$code" "$arch"
}

check_packages_rpm_repository_address ()
{
    dist=$1
    arch=$2
    ver=$3
    root_dir=$4
    echo_packages_repository_address "$root_dir" "$dist-$ver" "$arch"
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
    BASE_VERSION=`cat ../version`
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
    BASE_VERSION=`cat ../version`
    FIND_PATH=yum/repositories/$dist/$ver/$arch
    RESULT_SET=`find $FIND_PATH -name "*$BASE_VERSION*"`
    if [ -z "$RESULT_SET" ]; then
        printf "%8s %6s %s =>  0 rpm\n" $dist$ver $arch $BASE_VERSION
    else
        PACKAGE_COUNT=`find $FIND_PATH -name "*$BASE_VERSION*" | wc -l`
        printf "%8s %6s %s => %2d rpms\n" $dist$ver $arch $BASE_VERSION $PACKAGE_COUNT
    fi
}

check_depends_packages ()
{
    common_deb_procedure "check_depends_deb_packages"
    common_rpm_procedure "check_depends_rpm_packages"
}

check_depends_deb_packages ()
{
    code=$1
    arch=$2
    BASE_VERSION=`cat ../version`
    FIND_PATH=apt/repositories/*/pool/$code
    RESULT_SET=`find $FIND_PATH -name "*$BASE_VERSION*.deb"`
    if [ -z "$RESULT_SET" ]; then
	printf "%8s %5s %s => 404 deb\n" $code $arch $BASE_VERSION
    else
	for pkg in $RESULT_SET; do
	    DEB_NAME=`basename $pkg`
	    DEPENDS=`dpkg -I $pkg | grep "Depends"`
	    printf "%8s %5s %s => %s\n" $code $arch $DEB_NAME "$DEPENDS"
	done
    fi
}

check_depends_rpm_packages ()
{
    dist=$1
    arch=$2
    ver=$3
    BASE_VERSION=`cat ../version`
    FIND_PATH=yum/repositories/$dist/$ver/$arch
    RESULT_SET=`find $FIND_PATH -name "*$BASE_VERSION*"`
    if [ -z "$RESULT_SET" ]; then
        printf "%8s %6s %s => 404 rpm\n" $dist$ver $arch $BASE_VERSION
    else
	for pkg in $RESULT_SET; do
	    RPM_NAME=`basename $pkg`
	    DEPENDS=`rpm -qp --requires $pkg | grep -i "mysql" | tr -t '\n' ' '`
	    printf "%9s %6s %s => %s\n" $dist$ver $arch $RPM_NAME "$DEPENDS"
	done
    fi
}

check_provided_mysql_packages ()
{
    common_deb_procedure "check_provided_mysql_deb_packages"
    common_rpm_procedure "check_provided_mysql_rpm_packages"
    for code in $CODES; do
	echo $code
	cat tmp/$code-amd64-mysql-server.txt
    done
    for dist in $DISTRIBUTIONS; do
	echo $dist
	cat tmp/$dist-x86_64-mysql-server.txt
    done
}

check_provided_mysql_deb_packages ()
{
    code=$1
    arch=$2
    root_dir=$3
    cat > tmp/check-provided-mysql.sh <<EOF
#!/bin/sh
apt-get update > /dev/null
apt-cache show mysql-server | grep "Version" | head -1 > /tmp/$code-$arch-mysql-server.txt
EOF
    if [ -d $root_dir ]; then
	CHECK_SCRIPT=check-provided-mysql.sh
	echo "copy check script $CHECK_SCRIPT to $root_dir/tmp"
	sudo rm -f $root_dir/tmp/$CHECK_SCRIPT
	cp tmp/$CHECK_SCRIPT $root_dir/tmp
	sudo chmod 755 $root_dir/tmp/$CHECK_SCRIPT
	sudo chname $code-$arch chroot $root_dir /tmp/$CHECK_SCRIPT
	cp $root_dir/tmp/$code-$arch-mysql-server.txt tmp
    fi
}

check_provided_mysql_rpm_packages ()
{
    dist=$1
    arch=$2
    ver=$3
    root_dir=$4
    cat > tmp/check-provided-mysql.sh <<EOF
#!/bin/sh
yum update > /dev/null
yum info mysql-server | grep "Version" > /tmp/$code-$arch-mysql-server.txt
EOF
    if [ -d $root_dir ]; then
	CHECK_SCRIPT=check-provided-mysql.sh
	echo "copy check script $CHECK_SCRIPT to $root_dir/tmp"
	sudo rm -f $root_dir/tmp/$CHECK_SCRIPT
	cp tmp/$CHECK_SCRIPT $root_dir/tmp
	sudo chmod 755 $root_dir/tmp/$CHECK_SCRIPT
	sudo chname $code-$arch chroot $root_dir /tmp/$CHECK_SCRIPT
	cp $root_dir/tmp/$code-$arch-mysql-server.txt tmp
    fi
}

check_installed_mroonga_packages ()
{
    common_deb_procedure "check_installed_mroonga_deb_packages"
    common_rpm_procedure "check_installed_mroonga_rpm_packages"
}

check_installed_mroonga_deb_packages ()
{
    code=$1
    arch=$2
    root_dir=$3
    cat > tmp/check-deb-mroonga.sh <<EOF
#!/bin/sh
dpkg -l | grep $CHECK_INSTALL_PACKAGE
EOF
    if [ -d $root_dir ]; then
	CHECK_SCRIPT=check-deb-mroonga.sh
	echo "copy check script $CHECK_SCRIPT to $root_dir/tmp"
	sudo rm -f $root_dir/tmp/$CHECK_SCRIPT
	cp tmp/$CHECK_SCRIPT $root_dir/tmp
	sudo chmod 755 $root_dir/tmp/$CHECK_SCRIPT
	sudo chname $code-$arch chroot $root_dir /tmp/$CHECK_SCRIPT
    fi
}

check_installed_mroonga_rpm_packages ()
{
    dist=$1
    arch=$2
    ver=$3
    root_dir=$4
    cat > tmp/check-rpm-mroonga.sh <<EOF
#!/bin/sh
rpm -qa | grep $CHECK_INSTALL_PACKAGE
EOF
    CHECK_SCRIPT=check-rpm-mroonga.sh
    if [ -d $root_dir ]; then
	echo "copy check script $CHECK_SCRIPT to $root_dir/tmp"
	sudo rm -f $root_dir/tmp/$CHECK_SCRIPT
	cp tmp/$CHECK_SCRIPT $root_dir/tmp
	sudo chmod 755 $root_dir/tmp/$CHECK_SCRIPT
	sudo chname $code-$ver-$arch chroot $root_dir /tmp/$CHECK_SCRIPT
    fi
}

install_mroonga_packages ()
{
    common_deb_procedure "install_mroonga_deb_packages"
    common_rpm_procedure "install_mroonga_rpm_packages"
}
    
install_mroonga_deb_packages ()
{
    code=$1
    arch=$2
    root_dir=$4
    cat > tmp/install-aptitude-mroonga.sh <<EOF
#!/bin/sh
sudo aptitude clean
rm -f /var/lib/apt/lists/packages.groonga.org_*
rm -f /var/lib/apt/lists/partial/packages.groonga.org_*
sudo aptitude update
sudo aptitude -V -D -y --allow-untrusted install groonga-keyring
sudo aptitude update
sudo aptitude -V -D install mysql-server-mroonga
sudo aptitude -V -D install groonga-tokenizer-mecab
EOF
    cat > tmp/install-aptget-mroonga.sh <<EOF
#!/bin/sh
sudo apt-get clean
rm -f /var/lib/apt/lists/packages.groonga.org_*
rm -f /var/lib/apt/lists/partial/packages.groonga.org_*
sudo apt-get update
sudo apt-get -y --allow-unauthenticated install groonga-keyring
sudo apt-get update
sudo apt-get -V -y install mysql-server-mroonga
sudo apt-get -V -y install groonga-tokenizer-mecab
EOF
    root_dir=$CHROOT_ROOT/$code-$arch
    INSTALL_SCRIPT=""
    case $code in
	squeeze|unstable)
	    INSTALL_SCRIPT=install-aptitude-mroonga.sh
	    ;;
	*)
	    INSTALL_SCRIPT=install-aptget-mroonga.sh
	    ;;
    esac
    if [ -d $root_dir ]; then
	echo "copy install script $INSTALL_SCRIPT to $root_dir/tmp"
	sudo rm -f $root_dir/tmp/$INSTALL_SCRIPT
	cp tmp/$INSTALL_SCRIPT $root_dir/tmp
	chmod 755 $root_dir/tmp/$INSTALL_SCRIPT
	sudo chname $code-$arch chroot $root_dir /tmp/$INSTALL_SCRIPT
    fi
}

install_mroonga_rpm_packages ()
{
    dist=$1
    arch=$2
    ver=$3
    root_dir=$4
    cat > tmp/install-centos5-mroonga.sh <<EOF
sudo rpm -ivh http://packages.groonga.org/centos/groonga-release-1.1.0-0.noarch.rpm
sudo yum makecache
sudo yum install -y MySQL-server
sudo service mysql start
sudo yum install -y mysql-mroonga
sudo yum install -y groonga-tokenizer-mecab
EOF
    cat > tmp/install-centos6-mroonga.sh <<EOF
sudo rpm -ivh http://packages.groonga.org/centos/groonga-release-1.1.0-0.noarch.rpm
sudo yum makecache
sudo yum install -y mysql-server
sudo service mysql start
sudo yum install -y mysql-mroonga
sudo yum install -y groonga-tokenizer-mecab
EOF
    cat > tmp/install-fedora-mroonga.sh <<EOF
sudo rpm -ivh http://packages.groonga.org/fedora/groonga-release-1.1.0-0.noarch.rpm
sudo yum makecache
sudo yum install -y mysql-mroonga
sudo yum install -y groonga-tokenizer-mecab
EOF
    INSTALL_SCRIPT=""
    case "$dist-$ver" in
	centos-5)
	    INSTALL_SCRIPT=install-centos5-mroonga.sh
	    ;;
	centos-6)
	    INSTALL_SCRIPT=install-centos6-mroonga.sh
	    ;;
	fedora-18)
	    INSTALL_SCRIPT=install-fedora-mroonga.sh
	    ;;
	*)
	    ;;
    esac
    if [ -d $root_dir ]; then
	echo "copy install script $INSTALL_SCRIPT to $root_dir/tmp"
	sudo rm -f $root_dir/tmp/$INSTALL_SCRIPT
	cp tmp/$INSTALL_SCRIPT $root_dir/tmp
	chmod 755 $root_dir/tmp/$INSTALL_SCRIPT
	sudo chname $code-$ver-$arch chroot $root_dir /tmp/$INSTALL_SCRIPT
    fi
}


uninstall_mroonga_packages ()
{
    common_deb_procedure "uninstall_mroonga_deb_packages"
    common_rpm_procedure "uninstall_mroonga_rpm_packages"
}

uninstall_mroonga_deb_packages ()
{
    code=$1
    arch=$2
    root_dir=$4
    UNINSTALL_SCRIPT=uninstall-deb-mroonga.sh
    cat > $UNINSTALL_SCRIPT <<EOF
#!/bin/sh
sudo apt-get purge mroonga-* mysql-*
EOF
    if [ -d $root_dir ]; then
	echo "copy uninstall script $UNINSTALL_SCRIPT to $root_dir/tmp"
	sudo rm -f $root_dir/tmp/$UNINSTALL_SCRIPT
	cp $UNINSTALL_SCRIPT $root_dir/tmp
	chmod 755 $root_dir/tmp/$UNINSTALL_SCRIPT
	sudo chname $code-$arch chroot $root_dir /tmp/$UNINSTALL_SCRIPT
    fi
}

uninstall_mroonga_rpm_packages ()
{
    dist=$1
    arch=$2
    ver=$3
    root_dir=$4
    UNINSTALL_SCRIPT=uninstall-rpm-mroonga.sh
    cat > tmp/$UNINSTALL_SCRIPT <<EOF
#!/bin/sh
sudo yum remove mroonga-* mysql-*
EOF
    if [ -d $root_dir ]; then
	echo "copy install script $UNINSTALL_SCRIPT to $root_dir/tmp"
	sudo rm -f $root_dir/tmp/$UNINSTALL_SCRIPT
	cp tmp/$UNINSTALL_SCRIPT $root_dir/tmp
	chmod 755 $root_dir/tmp/$UNINSTALL_SCRIPT
	sudo chname $code-$ver-$arch chroot $root_dir /tmp/$UNINSTALL_SCRIPT
    fi
}


enable_temporaly_mroonga_repository ()
{
    cat > tmp/enable-repository.sh <<EOF
#!/bin/sh

grep -v "packages.groonga.org" /etc/hosts > /tmp/hosts
echo "$HOST_ADDRESS packages.groonga.org" >> /tmp/hosts
cp -f /tmp/hosts /etc/hosts
EOF
    common_deb_procedure "enable_temporaly_mroonga_deb_repository"
    common_rpm_procedure "enable_temporaly_mroonga_rpm_repository"
    check_packages_repository_address
}

enable_temporaly_mroonga_deb_repository ()
{
    code=$1
    arch=$2
    root_dir=$4
    today=`date '+%Y%m%d.%s'`
    if [ -d $root_dir ]; then
	sudo cp $root_dir/etc/hosts $root_dir/etc/hosts.$today
	sudo cp tmp/enable-repository.sh $root_dir/tmp
	sudo chname $code-$arch chroot $root_dir /tmp/enable-repository.sh
    fi
}

enable_temporaly_mroonga_rpm_repository ()
{
    dist=$1
    arch=$2
    ver=$3
    root_dir=$4
    today=`date '+%Y%m%d.%s'`
    if [ -d $root_dir ]; then
	sudo cp $root_dir/etc/hosts $root_dir/etc/hosts.$today
	sudo cp tmp/enable-repository.sh $root_dir/tmp
	sudo chname $code-$arch chroot $root_dir /tmp/enable-repository.sh
    fi
}

disable_temporaly_mroonga_repository ()
{
    cat > tmp/disable-repository.sh <<EOF
#!/bin/sh

grep -v "packages.groonga.org" /etc/hosts > /tmp/hosts
cp -f /tmp/hosts /etc/hosts
EOF
    common_deb_procedure "disable_temporaly_mroonga_deb_repository"
    common_rpm_procedure "disable_temporaly_mroonga_rpm_repository"
    check_packages_repository_address
}

disable_temporaly_mroonga_deb_repository ()
{
    code=$1
    arch=$2
    root_dir=$4
    DISABLE_SCRIPT=disable-repository.sh
    today=`date '+%Y%m%d.%s'`
    if [ -d $root_dir ]; then
	sudo cp $root_dir/etc/hosts $root_dir/etc/hosts.$today
	cp tmp/$DISABLE_SCRIPT $root_dir/tmp
	chmod 755 $root_dir/tmp/$DISABLE_SCRIPT
	sudo chname $code-$arch chroot $root_dir /tmp/$DISABLE_SCRIPT
    fi

}

disable_temporaly_mroonga_rpm_repository ()
{
    dist=$1
    arch=$2
    ver=$3
    root_dir=$4
    DISABLE_SCRIPT=disable-repository.sh
    today=`date '+%Y%m%d.%s'`
    if [ -d $root_dir ]; then
	sudo cp $root_dir/etc/hosts $root_dir/etc/hosts.$today
	cp tmp/$DISABLE_SCRIPT $root_dir/tmp
	chmod 755 $root_dir/tmp/$DISABLE_SCRIPT
	sudo chname $code-$arch chroot $root_dir /tmp/$DISABLE_SCRIPT
    fi
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
		    groonga|mroonga|roonga|mecab|mysql)
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
	--check-depends)
	    CHECK_DEPENDS=1
	    shift
	    ;;
	--check-provides)
	    CHECK_PROVIDES=1
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
	--install-mroonga)
	    INSTALL_MROONGA=1
	    shift
	    ;;
	--uninstall-mroonga)
	    UNINSTALL_MROONGA=1
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
    check_installed_mroonga_packages
fi
if [ $CHECK_ADDRESS -ne 0 ]; then
    check_packages_repository_address
fi
if [ $CHECK_BUILD -ne 0 ]; then
    check_build_packages
fi
if [ $CHECK_DEPENDS -ne 0 ]; then
    check_depends_packages
fi
if [ $CHECK_PROVIDES -ne 0 ]; then
    check_provided_mysql_packages
fi
if [ $ENABLE_REPOSITORY -ne 0 ]; then
    enable_temporaly_mroonga_repository
fi
if [ $DISABLE_REPOSITORY -ne 0 ]; then
    disable_temporaly_mroonga_repository
fi
if [ $INSTALL_MROONGA -ne 0 ]; then
    install_mroonga_packages
fi
if [ $UNINSTALL_MROONGA -ne 0 ]; then
    uninstall_mroonga_packages
fi

