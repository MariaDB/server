//
// Created by Thejaka Kanewala on 6/9/22.
//

#ifndef MYSQL_RECORD_PARSER_H
#define MYSQL_RECORD_PARSER_H

#include "record_scanner.h"
#include "record_handler.h"

template<typename RecordHandler>
class RecordParser {
private:
    RecordScanner* p_Scanner;
    RecordHandler* p_Handler;
    uint32_t record_offset;

private:
    bool get_record_type(const unsigned char* buffer, mlog_id_t* type) {

        *type = mlog_id_t(buffer[record_offset] & ~MLOG_SINGLE_REC_FLAG);
        if (UNIV_UNLIKELY(*type > MLOG_BIGGEST_TYPE
                                  && !EXTRA_CHECK_MLOG_NUMBER(*type))) {
            PRINT_ERR << "Found an invalid redo log record type at offset: " << record_offset << "." << std::endl;
            return false;
        }

        // increase the offset
        ++record_offset;
        return true;
    }

    lsn_t offset_to_lsn(const lsn_t& chunk_start_lsn) {

        const lsn_t block_sz_wo_hdr_trl = (OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE - LOG_BLOCK_TRL_SIZE);
        // how many blocks are there in the offset ?
        lsn_t blocks = record_offset / block_sz_wo_hdr_trl;
        lsn_t remainder = record_offset % block_sz_wo_hdr_trl;

        lsn_t adjustment = 0;
        if (remainder > 0) {
            // is the start lsn is in the middle of the block ?
            lsn_t lsn_mod = chunk_start_lsn % OS_FILE_LOG_BLOCK_SIZE;

            if (lsn_mod == 0)
                adjustment = LOG_BLOCK_HDR_SIZE;
            else {
                assert (lsn_mod > LOG_BLOCK_HDR_SIZE);

                lsn_t lsn_remain = OS_FILE_LOG_BLOCK_SIZE - lsn_mod;
                assert (lsn_remain > LOG_BLOCK_TRL_SIZE);
                lsn_remain -= LOG_BLOCK_TRL_SIZE;

                if (lsn_remain == 0)
                    adjustment = LOG_BLOCK_HDR_SIZE;
                else if (remainder > lsn_remain)
                    adjustment = (LOG_BLOCK_HDR_SIZE + LOG_BLOCK_TRL_SIZE);
                else if (remainder == lsn_remain)
                    adjustment = LOG_BLOCK_TRL_SIZE;
                else
                    adjustment = 0;
            }
        }

        lsn_t lsn_at_offset = chunk_start_lsn + record_offset + blocks * (LOG_BLOCK_HDR_SIZE + LOG_BLOCK_TRL_SIZE) + adjustment;
        return lsn_at_offset;
    }

    bool parse_record(const unsigned char* buffer, const lsn_t& chunk_start_lsn) {

        // get the type of the record -- type is 1 byte
        mlog_id_t type;
        if (!get_record_type(buffer, &type))
            return false;

        PRINT_INFO << "[Handler] [LSN=" << offset_to_lsn(chunk_start_lsn) - 1 << "] Record Type: " << get_mlog_string(type) << std::endl;

        // handle record types that does not involve a page/space id
        switch(type) {
            case MLOG_MULTI_REC_END:
            case MLOG_DUMMY_RECORD:
            case MLOG_CHECKPOINT: {
                record_offset += p_Handler->handle_system_records(type, buffer + record_offset, offset_to_lsn(chunk_start_lsn), p_Scanner->end_ptr());
                return true;
            }
        }

        // read the space id and page id
        // space id and page id is stored in 2 bytes, 2 bytes fields in some compressed format ...
        byte* ptr = const_cast<byte *>(buffer + record_offset);
        uint32_t space_id = mach_parse_compressed(const_cast<const byte **>(&ptr), p_Scanner->end_ptr());

        uint32_t page_id = 0;
        if (ptr != NULL) {
            page_id = mach_parse_compressed(const_cast<const byte **>(&ptr), p_Scanner->end_ptr());
        } else {
            PRINT_ERR << "LSN: " << (record_offset) << ", unable to retrieve page id for space id: " << space_id << std::endl;
            return false;
        }

        record_offset += (ptr - (buffer + record_offset));

        int64_t length = (*p_Handler)(type, buffer+record_offset, space_id, page_id, offset_to_lsn(chunk_start_lsn), p_Scanner->end_ptr());
        if (length < 0) {
            PRINT_ERR << "Error parsing record " << get_mlog_string(type) << std::endl;
            return false;
        }

        record_offset += length;

        return true;
    }

public:
    RecordParser(RecordScanner* _pScanner, RecordHandler* _pHandler): p_Scanner(_pScanner), p_Handler(_pHandler),
                                                    record_offset(0){
    }

    // returns false if a parse record failed
    bool parse_records(const lsn_t& chunk_start_lsn) {
        while ((record_offset < p_Scanner->get_length()) &&
                p_Handler->is_continue_processing())
            if (!parse_record(p_Scanner->parse_buffer, chunk_start_lsn)) {
                return false;
            }

        return true;
    }

    bool scanner_full() {
        return (record_offset >= p_Scanner->get_length());
    }
};

/*class RecordParser {
public:
    RecordParser(const unsigned char* _block, uint32_t _offset, const uint32_t& _data_length):
        block(_block), record_offset(_offset), data_length(_data_length){}

private:
    bool is_single_record(const unsigned char* record, const uint32_t& offset) {
        // What is a single (mtr) record ? -- this means mtr makes only change a single
        // page. If the mtr has multiple page modifications, then it will not be a single record ...

        // by default MLOG_CHECKPOINT and MLOG_DUMMY_RECORD are considered single record mtrs
        if ((record[offset] == MLOG_CHECKPOINT) || (record[offset] == MLOG_DUMMY_RECORD))
            return true;
        else
            return (record[offset] & MLOG_SINGLE_REC_FLAG); // otherwise, first bit of type says whether
        // it is a single record or not ...
    }

    bool get_record_type(const unsigned char* record, uint32_t& offset, mlog_id_t* type) {

        *type = mlog_id_t(record[offset] & ~MLOG_SINGLE_REC_FLAG);
        if (UNIV_UNLIKELY(*type > MLOG_BIGGEST_TYPE
                                  && !EXTRA_CHECK_MLOG_NUMBER(*type))) {
            PRINT_ERR << "Found an invalid redo log record type." << std::endl;
            return false;
        }

        // increase the offset
        ++offset;
        return true;
    }

    bool parse_mlog_file_name(const unsigned char *block, uint32_t space_id, uint32_t& record_offset, const lsn_t& record_lsn) {
        ulint len = mach_read_from_2(block);
        std::string file_name(reinterpret_cast<const char *>(block + 2), len - 1);
        record_offset += (2 + len);

        PRINT_INFO << "LSN: " << (record_lsn + record_offset) << ", Space Id: " << space_id << " file name: " << file_name << std::endl;

        return true;
    }

    bool parse_mlog_checkpoint(const unsigned char *block, uint32_t& record_offset, const lsn_t& record_lsn) {
        lsn_t cp_lsn = mach_read_from_8(block);
        record_offset += 8;

        PRINT_INFO << "LSN: " << (record_lsn + record_offset) << ", Found MLOG_CHECKPOINT and LSN=" << cp_lsn << std::endl;
        return true;
    }

    bool parse_record(const uint32_t& _block_id, const lsn_t& record_lsn) {

        // within a block we cannot have an offset greater than 508
        // cos block size = 512
        // block trailer has a checksum field of 4 bytes
        // then, offset can go to the maximum 512 - 4 = 508

        if (record_offset >= 506)
            return false;

        // get the type of the record -- type is 1 byte
        mlog_id_t type;
        if (!get_record_type(block, record_offset, &type))
            return false;

        if (type == MLOG_CHECKPOINT) {
            return parse_mlog_checkpoint(block + record_offset, record_offset, record_lsn);
        }

        // for dummy records and checkpoint records we do not have the space id and the page id ...
        if (type == MLOG_DUMMY_RECORD) {
            PRINT_INFO << "LSN: " << (record_lsn + record_offset) << ", Found a single record of type: " << get_mlog_string(type) << std::endl;
            return true;
        }

        if (type == MLOG_MULTI_REC_END) {
            // do nothing ..
            return true;
        }

        // read the space id and page id
        // space id and page id is stored in 2 bytes, 2 bytes fields in some compressed format ...
        byte* ptr = const_cast<byte *>(block + record_offset);
        uint32_t space_id = mach_parse_compressed(const_cast<const byte **>(&ptr), (block + data_length));

        uint32_t page_id = 0;
        if (ptr != NULL) {
            page_id = mach_parse_compressed(const_cast<const byte **>(&ptr), (block + data_length));
        } else {
            PRINT_ERR << "LSN: " << (record_lsn + record_offset) << ", Block: " << _block_id << ", unable to retrieve page id for space id: " << space_id << std::endl;
            return false;
        }

        record_offset += 2;

        if (is_single_record(block, record_offset)) {
            PRINT_INFO << "LSN: " << (record_lsn + record_offset) << ", Found a single record of type: " << get_mlog_string(type) << " space id: " << space_id
                       << " page id: " << page_id << std::endl;
        } else {
            PRINT_INFO << "LSN: " << (record_lsn + record_offset) << ", Found a multi record of type: " << get_mlog_string(type) << " space id: " << space_id
                       << " page id: " << page_id << std::endl;
            if (type == MLOG_FILE_NAME)
                parse_mlog_file_name(block + record_offset, space_id, record_offset, record_lsn);
        }

        return true;
    }

public:
    bool parse_records(const uint32_t& _block_id, const lsn_t record_lsn) {
        while (record_offset < data_length)
            if (!parse_record(_block_id, record_lsn))
                break;
    }
private:
    const unsigned char* block;
    uint32_t record_offset;
    const uint32_t& data_length;
};*/
#endif //MYSQL_RECORD_PARSER_H
