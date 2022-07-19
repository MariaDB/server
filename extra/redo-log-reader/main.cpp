//
// Created by Thejaka Kanewala on 3/24/22.
//
#include <iostream>
#include "log_reader.h"

int main() {
    std::cout << "Starting redo-log reader ..." << std::endl;
    LogReader logReader("/Users/thejaka.kanewala/git/mysql/mariadb-build/configurations/mysql-replication/primary/data/",
                        "/Users/thejaka.kanewala/git/mysql/mariadb-build/configurations/mysql-replication/primary/data/ib_logfile0");

    if (!logReader.init()) {
        std::cout << "[ERROR] Error initializing the log reader!" << std::endl;
        return -1;
    }

    if (logReader.read())
        return 0;
    else {
        std::cout << "[ERROR] Log reader failed!" << std::endl;
        return -1;
    }
}