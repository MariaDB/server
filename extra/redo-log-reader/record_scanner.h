//
// Created by Thejaka Kanewala on 6/17/22.
//

#ifndef MYSQL_RECORD_SCANNER_H
#define MYSQL_RECORD_SCANNER_H

class RecordScanner {
private:
    const uint64_t& size;
    uint64_t length;

public:
    RecordScanner(const uint64_t& _size): size(_size), parse_buffer(NULL){}

    bool init() {
        if (parse_buffer == NULL)
            parse_buffer = new unsigned char[size];

        std::fill(parse_buffer, parse_buffer + size, 0);
        length = 0;
        return true;
    }

    unsigned char* end_ptr() {
        return (parse_buffer + size);
    }

    /**
     * Scans a block and add necessary parts to the parse_buffer
     * @param block
     * @return
     */
    bool scan(const unsigned char* block, const uint32_t& offset=LOG_BLOCK_HDR_SIZE) {
        assert(offset >= LOG_BLOCK_HDR_SIZE);
        assert(offset < (OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE));

        ulint data_length = mach_read_from_2(block + LOG_BLOCK_HDR_DATA_LEN);
        assert(data_length >= offset);

        uint32_t cp_sz;
        if (data_length == OS_FILE_LOG_BLOCK_SIZE)
            cp_sz = data_length - (offset + LOG_BLOCK_TRL_SIZE);
        else {
            assert(data_length < OS_FILE_LOG_BLOCK_SIZE);
            cp_sz = data_length - offset;
        }


        // not enough space in the buffer to copy the data
        if ((size - length) < cp_sz)
            return false;

        memcpy(parse_buffer+length, (block+offset), cp_sz);
        length += cp_sz;

        return true;
    }

    uint64_t get_length() {
        return length;
    }

    unsigned char* parse_buffer;

};

#endif //MYSQL_RECORD_SCANNER_H
