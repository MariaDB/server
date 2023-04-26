# Build and Debug from source (Ubuntu 22.04)

## Install Dependencies
Firstly Create configuration file in this path '/etc/apt/sources.list.d/mariadb.list` using following command:

```
sudo gedit /etc/apt/sources.list.d/mariadb.list
```

then Save the following repository configuration contents in the file `/etc/apt/sources.list.d/mariadb.list`. Note that the configuration is valid for the mariadb branch 11.0 and Ubuntu 22.04:
```
# Retrieved from: https://mariadb.org/download/?t=repo-config

# MariaDB 11.0 [RC] repository list - created 2023-02-24 18:43 UTC
# https://mariadb.org/download/
deb https://mirror.its.dal.ca/mariadb/repo/11.0/ubuntu jammy main
deb-src https://mirror.its.dal.ca/mariadb/repo/11.0/ubuntu jammy main
deb https://mirror.its.dal.ca/mariadb/repo/11.0/ubuntu jammy main/debug
```

Install dependencies:
```
sudo apt-get install software-properties-common \
      devscripts \
      equivs \
      curl \
      git
sudo apt-get build-dep mariadb-server
```

Import the repository key and update apt. You should not get any error from the mariadb repository server:

```
sudo curl -o /etc/apt/trusted.gpg.d/mariadb_release_signing_key.asc 'https://mariadb.org/mariadb_release_signing_key.asc'
sudo apt-get update

```



## Build

Make sure you completed the previous steps without any failure before moving forward.

We will be working on 3 main directories `maria-server` (source directory), `maria-server-build` (build directory), and `maria-server-data` (data directory). You can choose any names. We will create them inside the home directory. You can choose any directory where root privilige is not required.
```
cd ~
```

Fork and, then clone the forked repo:

```
git clone https://github.com/{your-username}/maria-server
```

Create the build and data directory.
```
mkdir ~/maria-server-build
mkdir ~/maria-server-data
```

Run the following commands to start building. Notice the directory used in each command:
```
cd ~/maria-server-build
cmake ~/maria-server/ -DCMAKE_BUILD_TYPE=Debug
cmake --build ./ -j8
```

## Configure

Before proceeding, you should have mariadb installed without any error from cmake.

Copy the following mariadb configuration into the file `~/.my.cnf`. Set the absolute path to your data and build directory in the `datadir` and `language` entry before saving:
```
# Example MariadB config file.
# You can copy this to one of:
# /etc/my.cnf to set global options,
# /mysql-data-dir/my.cnf to get server specific options or
# ~/my.cnf for user specific options.
#
# One can use all long options that the program supports.
# Run the program with --help to get a list of available options

# This will be passed to all MariaDB clients
[client]
#password=my_password
#port=3306
#socket=/tmp/mysql.sock

# Here is entries for some specific programs
# The following values assume you have at least 32M ram

# The mariadb server  (both [mysqld] and [mariadb] works here)
[mariadb]
#port=3306
#socket=/tmp/mysql.sock

# The following three entries caused mysqld 10.0.1-MariaDB (and possibly other versions) to abort...
# skip-locking
# set-variable  = key_buffer=16M

loose-innodb_data_file_path = ibdata1:1000M
loose-mutex-deadlock-detector
gdb

######### Fix the two following paths

# Where you want to have your database
datadir={absolute-path-to-your-data-directory}

# Where you have your mysql/MariaDB source + sql/share/english
language={absolute-path-to-your-build-directory}/sql/share/english

########## One can also have a different path for different versions, to simplify development.

[mariadb-10.1]
lc-messages-dir=/my/maria-10.1/sql/share

[mariadb-10.2]
lc-messages-dir=/my/maria-10.2/sql/share

[mysqldump]
quick
set-variable = max_allowed_packet=16M

[mysql]
no-auto-rehash

[myisamchk]
set-variable= key_buffer=128M
```

Initialize data files in the data direcotry:
```
cd ~/maria-server-build
./scripts/mariadb-install-db --srcdir={absolute-path-to-your-source-directory} --user=$LOGNAME
```

## Run

Check whether the install was successful:
```
./sql/mariadbd
```

You should see something similar to this in the terminal:
```
...
...
2023-02-24 14:37:06 0 [Note] ./sql/mariadbd: ready for connections.
Version: '11.0.1-MariaDB-debug'  socket: '/tmp/mysql.sock'  port: 3306  Source distribution
```

Shutdown the server using `CTRL + C`.

While the server was running, you could also run the client using `./client/mariadb` to test SQL commands.

## Debug (GDB)

Run gdb and the server with debug flag:
```
gdb -tui --args ./sql/mariadbd --gdb
```

Inside gdb, set breakpoint at the `write_row` function. This function is likely used by the sql INSERT command:
```
b write_row
```

Start the server within gdb:
```
run
```

In a separate terminal, run the client:
```
./client/mariadb
```

Set up a sample table:
```
CREATE DATABASE mydb;
USE mydb;
CREATE TABLE mytable (id int);
```

Insert a row into mytable. If gdb is setup correctly, the INSERT command will pause the client and trigger the breakpoint in the `ha_innobase::write_row` function.
```
INSERT INTO mytable VALUES ('1');
```

## Debug (VSCode)

Use the following launch configuration. Make sure to set the build directory in the `program` entry. It should configure the default C\C++ extension to launch mariadb with debug flag:
```
"configurations": [
    ...,
    ...,
    {
        "name": "(gdb) Launch",
        "type": "cppdbg",
        "request": "launch",
        "program": "{absolute-path-to-your-build-directory}/sql/mariadbd",
        "args": ["--gdb", "--debug"],
        "stopAtEntry": false,
        "cwd": "${fileDirname}",
        "environment": [],
        "externalConsole": false,
        "MIMode": "gdb",
        "setupCommands": [
            {
                "description": "Enable pretty-printing for gdb",
                "text": "-enable-pretty-printing",
                "ignoreFailures": true
            },
            {
                "description": "Set Disassembly Flavor to Intel",
                "text": "-gdb-set disassembly-flavor intel",
                "ignoreFailures": true
            }
        ]
    }
]
```

Launch the server from the VSCode's Run and Debug tab. You can set breakpoints from the IDE's text editor. But, you have to run the sql commands from the client as shown in the previous section.

## Rebuild

If you want to rebuild, first go the the source directory and run this command:
```
cd ~/maria-server
git clean -xffd && git submodule foreach --recursive git clean -xffd
```

Then, follow all the previous steps from the beginning.

## Install MariaDB on Windows

There are few pre-requisite we need to install before even start building the server:

### **Visual C++**

Download and Install Visual C++ from [Microsoft](https://visualstudio.microsoft.com/). Community Version 2019 and 2022 are supported at this time.

Make sure to select and install "Desktop Development with C++" during the installation process.


![Visual Code Installation](https://dev-to-uploads.s3.amazonaws.com/uploads/articles/574kwg6tz8xl994lqf96.png)


### **CMake**

Download CMake from [cmake-v3.26.0](https://github.com/Kitware/CMake/releases/download/v3.26.0/cmake-3.26.0-windows-x86_64.msi) and install on the machine. 

- Minimum version required: 3.14 

**Check out one of my blog on** [CMake installation](https://dev.to/arun3sh/install-cmake-on-windows-4eo5) 

Make sure to add `C:\GnuWin32\bin` to your windows system path. 

### **Git**

Download and Install git using [Git-2.39.2-64-bit.exe](https://github.com/git-for-windows/git/releases/download/v2.39.2.windows.1/Git-2.39.2-64-bit.exe)

**While installing git, make sure to select the PATH environment as follows:**
![Git Installation](https://dev-to-uploads.s3.amazonaws.com/uploads/articles/08hgxcmd6pcmbqroo4hs.png)

### **Bison**

Download the executable from the link:
[Download Gnu Diff](https://ftp.gnu.org/gnu/diffutils/)

Follow the steps to install the executable.

## Build

- Get the source code

You will be able to find the source code of MariaDB from their GitHub repo [MariaDB Github Repository](https://github.com/MariaDB/server) 

- Clone the repo to your system

```bash
git clone git@github.com:MariaDB/server.git 
```

After the clone is complete, you will have a complete copy of the repository in the server directory. You can then work on the code locally, commit changes, and push them back to the remote repository as needed.

- Create Build directory

```bash
# create a directory with name "build-dir"
mkdir build-dir

# go into "build-dir"
cd build-dir

# Run the following commands in sequence

cmake ../server

# If you want to build with Debugging option
# otherwise just omit  --config Debug
cmake --build . --config Debug
```
Build will take a while to complete and make sure there are no errors while building.

```bash
# go to the directory
cd ..\build-dir\mysql-test

# Test your build
perl mysql-test-run.pl --suite=main --parallel=auto

```

If all the tests came out as passed, we have installed the MariaDB perfectly on Windows machine.