//
// Created by Thejaka Kanewala on 6/9/22.
//

#ifndef MYSQL_REDO_READER_UTIL_H
#define MYSQL_REDO_READER_UTIL_H

#include <stdio.h>
#include <sys/mman.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <os0file.h>
#include <libgen.h>
#include <log0log.h>

#define PRINT_INFO std::cout << "[INFO] "
#define PRINT_ERR std::cout << "[ERROR] "
#define PRINT_WARN std::cout << "[WARN] "

static const char* get_mlog_string(mlog_id_t type)
{
    switch (type) {
        case MLOG_SINGLE_REC_FLAG:
            return("MLOG_SINGLE_REC_FLAG");

        case MLOG_1BYTE:
            return("MLOG_1BYTE");

        case MLOG_2BYTES:
            return("MLOG_2BYTES");

        case MLOG_4BYTES:
            return("MLOG_4BYTES");

        case MLOG_8BYTES:
            return("MLOG_8BYTES");

        case MLOG_REC_INSERT:
            return("MLOG_REC_INSERT");

        case MLOG_REC_CLUST_DELETE_MARK:
            return("MLOG_REC_CLUST_DELETE_MARK");

        case MLOG_REC_SEC_DELETE_MARK:
            return("MLOG_REC_SEC_DELETE_MARK");

        case MLOG_REC_UPDATE_IN_PLACE:
            return("MLOG_REC_UPDATE_IN_PLACE");

        case MLOG_REC_DELETE:
            return("MLOG_REC_DELETE");

        case MLOG_LIST_END_DELETE:
            return("MLOG_LIST_END_DELETE");

        case MLOG_LIST_START_DELETE:
            return("MLOG_LIST_START_DELETE");

        case MLOG_LIST_END_COPY_CREATED:
            return("MLOG_LIST_END_COPY_CREATED");

        case MLOG_PAGE_REORGANIZE:
            return("MLOG_PAGE_REORGANIZE");

        case MLOG_PAGE_CREATE:
            return("MLOG_PAGE_CREATE");

        case MLOG_UNDO_INSERT:
            return("MLOG_UNDO_INSERT");

        case MLOG_UNDO_ERASE_END:
            return("MLOG_UNDO_ERASE_END");

        case MLOG_UNDO_INIT:
            return("MLOG_UNDO_INIT");

        case MLOG_UNDO_HDR_REUSE:
            return("MLOG_UNDO_HDR_REUSE");

        case MLOG_UNDO_HDR_CREATE:
            return("MLOG_UNDO_HDR_CREATE");

        case MLOG_REC_MIN_MARK:
            return("MLOG_REC_MIN_MARK");

        case MLOG_IBUF_BITMAP_INIT:
            return("MLOG_IBUF_BITMAP_INIT");

        case MLOG_WRITE_STRING:
            return("MLOG_WRITE_STRING");

        case MLOG_MULTI_REC_END:
            return("MLOG_MULTI_REC_END");

        case MLOG_DUMMY_RECORD:
            return("MLOG_DUMMY_RECORD");

        case MLOG_FILE_DELETE:
            return("MLOG_FILE_DELETE");

        case MLOG_COMP_REC_MIN_MARK:
            return("MLOG_COMP_REC_MIN_MARK");

        case MLOG_COMP_PAGE_CREATE:
            return("MLOG_COMP_PAGE_CREATE");

        case MLOG_COMP_REC_INSERT:
            return("MLOG_COMP_REC_INSERT");

        case MLOG_COMP_REC_CLUST_DELETE_MARK:
            return("MLOG_COMP_REC_CLUST_DELETE_MARK");

        case MLOG_COMP_REC_UPDATE_IN_PLACE:
            return("MLOG_COMP_REC_UPDATE_IN_PLACE");

        case MLOG_COMP_REC_DELETE:
            return("MLOG_COMP_REC_DELETE");

        case MLOG_COMP_LIST_END_DELETE:
            return("MLOG_COMP_LIST_END_DELETE");

        case MLOG_COMP_LIST_START_DELETE:
            return("MLOG_COMP_LIST_START_DELETE");

        case MLOG_COMP_LIST_END_COPY_CREATED:
            return("MLOG_COMP_LIST_END_COPY_CREATED");

        case MLOG_COMP_PAGE_REORGANIZE:
            return("MLOG_COMP_PAGE_REORGANIZE");

        case MLOG_FILE_CREATE2:
            return("MLOG_FILE_CREATE2");

        case MLOG_ZIP_WRITE_NODE_PTR:
            return("MLOG_ZIP_WRITE_NODE_PTR");

        case MLOG_ZIP_WRITE_BLOB_PTR:
            return("MLOG_ZIP_WRITE_BLOB_PTR");

        case MLOG_ZIP_WRITE_HEADER:
            return("MLOG_ZIP_WRITE_HEADER");

        case MLOG_ZIP_PAGE_COMPRESS:
            return("MLOG_ZIP_PAGE_COMPRESS");

        case MLOG_ZIP_PAGE_COMPRESS_NO_DATA:
            return("MLOG_ZIP_PAGE_COMPRESS_NO_DATA");

        case MLOG_ZIP_PAGE_REORGANIZE:
            return("MLOG_ZIP_PAGE_REORGANIZE");

        case MLOG_FILE_RENAME2:
            return("MLOG_FILE_RENAME2");

        case MLOG_FILE_NAME:
            return("MLOG_FILE_NAME");

        case MLOG_CHECKPOINT:
            return("MLOG_CHECKPOINT");

        case MLOG_PAGE_CREATE_RTREE:
            return("MLOG_PAGE_CREATE_RTREE");

        case MLOG_COMP_PAGE_CREATE_RTREE:
            return("MLOG_COMP_PAGE_CREATE_RTREE");

        case MLOG_INIT_FILE_PAGE2:
            return("MLOG_INIT_FILE_PAGE2");

        case MLOG_INDEX_LOAD:
            return("MLOG_INDEX_LOAD");

        case MLOG_TRUNCATE:
            return("MLOG_TRUNCATE");

        case MLOG_FILE_WRITE_CRYPT_DATA:
            return("MLOG_FILE_WRITE_CRYPT_DATA");

        default: {
            PRINT_ERR << "Invalid log record type: " << type << std::endl;
            return NULL;
        }
    }
}

#endif