//
// Created by Thejaka Kanewala on 3/24/22.
//

#ifndef MYSQL_LOG_READER_H
#define MYSQL_LOG_READER_H

#include "util.h"
#include "record_parser.h"
#include "record_scanner.h"

class Checkpoint {
public:
    // checkpoint number
    uint64_t m_checkpoint_no;
    // LSN related to the checkpoint
    uint64_t m_checkpoint_lsn;
    // checkpoint end LSN
    /** start LSN of the MLOG_CHECKPOINT mini-transaction corresponding
    to this checkpoint, or 0 if the information has not been written -- didnt quite understand the purpose of this ... */
    uint64_t m_checkpoint_end_lsn;
    /** Byte offset of the log record corresponding to LOG_CHECKPOINT_LSN */
    uint64_t m_checkpoint_offset;
    /** Actual physical log file index */
    uint32_t m_log_file_idx;
    /** Offset within the log file */
    uint64_t m_offset;

public:
    Checkpoint(const uint64_t& checkpoint,
               const uint64_t& checkpoint_lsn,
               const uint64_t& cp_end_lsn,
               const uint64_t& cp_offset,
               const uint32_t& file_index,
               const uint64_t& offset): m_checkpoint_no(checkpoint),
                        m_checkpoint_lsn(checkpoint_lsn),
                        m_checkpoint_end_lsn(cp_end_lsn),
                        m_checkpoint_offset(cp_offset),
                        m_log_file_idx(file_index),
                        m_offset(offset){}
};

class LogReader {
private:
    const char* m_data_dir;
    const char* m_log_file;
    const unsigned int m_log_index;

    bool is_file_multiple_of_block_size(const struct stat *stat_buf) const {
        return stat_buf->st_size & (OS_FILE_LOG_BLOCK_SIZE - 1);
    }

    char* file_name() {
        char* base_name = basename((char *)m_log_file);
        return base_name;
    }

    std::string file_name(unsigned int index) {
        std::string file(m_data_dir);
        file.append("ib_logfile").append(std::to_string(index));
        return file;
    }

    int log_index() {
        size_t ib_log_sz = strlen("ib_logfile");
        const char* base_file_name = file_name();
        size_t ib_log_file_sz = strlen(base_file_name);
        assert(ib_log_file_sz > ib_log_sz);

        char* sIndex = new char[ib_log_file_sz - ib_log_sz + 1];
        strncpy(sIndex, base_file_name + ib_log_sz, (ib_log_file_sz - ib_log_sz));
        sIndex[(ib_log_file_sz - ib_log_sz)] = '\0';

        int index = std::stoi(sIndex);
        delete[] sIndex;

        return index;
    }

    bool validate_block_checksum(unsigned char *block_start, const std::string& err_msg) {
        // checksum is stored in the last 4 bytes in a block
        ulint stored = mach_read_from_4(block_start + OS_FILE_LOG_BLOCK_SIZE
                - LOG_BLOCK_CHECKSUM);
        ulint calculated = ut_crc32(block_start, OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE);

        if (stored != calculated) {
            PRINT_ERR << "Checksum mismatch in: " << m_log_file
                << ", stored: " << stored << ", calculated: " << calculated
                << ", msg: " << err_msg;
            return false;
        }

        return true;
    }

    Checkpoint* read_checkpoint(unsigned char *checkpoint_log, const log_group_t* logGroup) {
        uint64_t checkpoint_no = mach_read_from_8(
                checkpoint_log + LOG_CHECKPOINT_NO);

        // checkpoint LSN
        uint64_t checkpoint_lsn = mach_read_from_8(
                checkpoint_log + LOG_CHECKPOINT_LSN);

        /** Byte offset of the log record corresponding to LOG_CHECKPOINT_LSN */
        uint64_t checkpoint_offset = mach_read_from_8(
                checkpoint_log + LOG_CHECKPOINT_OFFSET);

        uint64_t end_lsn = mach_read_from_8(
                checkpoint_log + LOG_CHECKPOINT_END_LSN);

        // lets find the file offset for cp->m_checkpoint_lsn
        uint32_t file_index = 0;
        uint64_t file_offset = 0;
        find_file_offset(checkpoint_lsn,
                         logGroup->file_size,
                         logGroup->n_files,
                         &file_index,
                         &file_offset);

        Checkpoint* cp = new Checkpoint(checkpoint_no, checkpoint_lsn, end_lsn, checkpoint_offset,
                                        file_index, file_offset);
        return cp;
    }

    Checkpoint* read_checkpoint_1(unsigned char *log, const log_group_t* logGroup) {
        unsigned char* checkpoint_log = log + LOG_CHECKPOINT_1;
        if (!validate_block_checksum(checkpoint_log, "invalid checksum for checkpoint1 block"))
            return NULL;

        PRINT_INFO << "Checkpoint_1 checksum matched for log file: " << m_log_file << "\n";

        return read_checkpoint(checkpoint_log, logGroup);
    }

    Checkpoint* read_checkpoint_2(unsigned char *log, const log_group_t* logGroup) {
        unsigned char* checkpoint_log = log + LOG_CHECKPOINT_2;
        if (!validate_block_checksum(checkpoint_log, "invalid checksum for checkpoint2 block"))
            return NULL;

        PRINT_INFO << "Checkpoint_2 checksum matched for log file: " << m_log_file << "\n";

        return read_checkpoint(checkpoint_log, logGroup);
    }

    void find_file_offset2(const lsn_t& lsn,
                          const uint64_t& file_sz,
                          const uint32_t& num_files,
                          const uint64_t& start_lsn,
                          const uint64_t& start_offset, // the offset of the start_lsn within the logical file ...
                          uint32_t* file_index,
                          uint64_t* offset) {

        // Logical file size = accumulated log file sizes without their headers ....
        uint64_t logical_file_sz = (file_sz - LOG_FILE_HDR_SIZE) * num_files;

        // Logical start offset is without the file headers ....
        uint64_t logical_start_offset = start_offset - LOG_FILE_HDR_SIZE * (1 + start_offset / file_sz);

        uint64_t logical_offset = 0;

        if (lsn >= start_lsn) {
            uint64_t difference = lsn - start_lsn;
            logical_offset = (logical_start_offset + difference) % logical_file_sz;
        } else {
            uint64_t difference = start_lsn - lsn;
            difference = difference % logical_file_sz;

            if (logical_start_offset >= difference) {
                logical_offset = logical_start_offset - difference;
            } else {
                uint64_t rest = (difference - logical_start_offset) % logical_file_sz;
                logical_offset = (logical_file_sz - rest);
            }
        }

        // To calculate the physical offset, add file header sizes ...
        uint64_t physical_offset = logical_offset + LOG_FILE_HDR_SIZE * (1 + logical_offset / (file_sz - LOG_FILE_HDR_SIZE));

        (*file_index) = physical_offset / file_sz;
        // TODO -- i dont understand why we need -(OS_FILE_LOG_BLOCK_SIZE)
        (*offset) = physical_offset % file_sz;
    }


    void find_file_offset(const lsn_t& lsn,
                           const uint64_t& file_sz,
                           const uint32_t& num_files,
                           uint32_t* file_index,
                           uint64_t* offset) {
        uint64_t logical_file_sz = (file_sz - LOG_FILE_HDR_SIZE) * num_files;

        // lsn must not be less than LOG_START_LSN
        assert(lsn >= LOG_START_LSN);

        // logical file is the virtual file without log headers ...
        // the position of the lsn in the logical file ...
        lsn_t position_in_logical_file = lsn % logical_file_sz;

        uint64_t difference;
        // the difference between position_in_logical_file and the LOG_START_LSN
        if (position_in_logical_file >= LOG_START_LSN) {
            difference = (position_in_logical_file - LOG_START_LSN);
        } else {
            // less than means we have rolled over ...
            uint64_t front_diff = (logical_file_sz - LOG_START_LSN);
            difference = front_diff + position_in_logical_file;
        }

        (*file_index) = difference / (file_sz - LOG_FILE_HDR_SIZE);
        (*offset) = difference % (file_sz - LOG_FILE_HDR_SIZE) + LOG_FILE_HDR_SIZE;
    }

#define READ_BYTES_PER_ITERATION (UNIV_PAGE_SIZE_ORIG * 4)
#define NUM_BLOCKS_PER_ITERATION (READ_BYTES_PER_ITERATION / OS_FILE_LOG_BLOCK_SIZE)

    bool validate_block(const unsigned char* block, ulint block_calculated, lsn_t block_lsn,
                        const log_group_t* const logGroup,
                        ulint* rec_grp_offset) {
        ulint block_num = (~LOG_BLOCK_FLUSH_BIT_MASK
                & mach_read_from_4(block + LOG_BLOCK_HDR_NO));

        // is flush bit set ? TODO -- what is flush bit ?
        // -- when the redo log is written to the file, it write it as a buffer -- this buffer
        // is called the redo log buffer.
        // redo log buffer contains multiple blocks -- the first block in the buffer, we set the flush bit
        // for other blocks the flush bit is not set.
        if (LOG_BLOCK_FLUSH_BIT_MASK
                & mach_read_from_4(block + LOG_BLOCK_HDR_NO)) {
            PRINT_INFO << "Block number: " << block_num << ", flush bit set.\n";
        }

        PRINT_INFO  << " retrieved block number: " << block_num
                    << ", calculated block number: " << block_calculated << std::endl;

        if (block_calculated != block_num) {
            PRINT_WARN << " the calculated block number and the block number retrieved are different."
             << std::endl;
            return false;
        }

        // check block checksum ...
        ulint calculated_cksum = log_block_calc_checksum_crc32(block);
        ulint stored_cksum = mach_read_from_4(block + OS_FILE_LOG_BLOCK_SIZE
                - LOG_BLOCK_CHECKSUM);
        if (calculated_cksum != stored_cksum) {
            PRINT_ERR << "invalid checksum for block: " << block_num << " calculated checksum: " << calculated_cksum
                << ", stored checksum: " << stored_cksum << std::endl;
            return false;
        }

        //======= DATA_LENGTH (2)
        // About data length -- data length includes the block header and trailer as well.
        // Therefore, if a block is full of data, then the data length = 512 = Block size.
        // get the data length ...
        ulint data_length = mach_read_from_2(block + LOG_BLOCK_HDR_DATA_LEN);

        PRINT_INFO << "block number: " << block_num << " data length: " << data_length << std::endl;

        if (data_length < LOG_BLOCK_HDR_SIZE) {
            PRINT_ERR << "data length is less than LOG_BLOCK_HDR_SIZE (" << LOG_BLOCK_HDR_SIZE << ") data length: "
                      << data_length << std::endl;
            return false;
        }

        if (data_length > OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE
                && data_length != OS_FILE_LOG_BLOCK_SIZE) {
            PRINT_ERR << "data length greater than (OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE) " << (OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE)
                    << " the data length: " << data_length << std::endl;
            return false;
        }

        // found a checkpointed block ...
        if (data_length == (LOG_BLOCK_HDR_SIZE + SIZE_OF_MLOG_CHECKPOINT) &&
                (block[LOG_BLOCK_HDR_SIZE] == MLOG_CHECKPOINT)) {
            // this is a checkpoint block ... --> after MLOG_CHECKPOINT byte, we have the checkpoint lsn ...
            lsn_t stored_cp = mach_read_from_8(LOG_BLOCK_HDR_SIZE + 1 + block);
            if (logGroup->lsn != stored_cp) {
                PRINT_ERR << "The checkpoint LSN does not match with the store checkpoint LSN for the checkpoint block. "
                    <<  "store cp: " << stored_cp << ", checkpoint LSN: " << logGroup->lsn << std::endl;
                return false;
            }

            // reached cp block -- rest is null
            PRINT_INFO << "Reached a MLOG_CHECKPOINT block. Stop scanning from here ..." << std::endl;
            return false;
        }

        // End DATA_LENGTH validations ...

        // FIRST_RECORD_OFFSET ...
        *rec_grp_offset = mach_read_from_2(block + LOG_BLOCK_FIRST_REC_GROUP);
        PRINT_INFO << "Block: " << block_num << ",first record group offset: " << *rec_grp_offset << std::endl;

        // CHECKPOINT number ...
        uint32_t cp_num = mach_read_from_4(block + LOG_BLOCK_CHECKPOINT_NO);
        PRINT_INFO << "Block: " << block_num << ", log checkpoint number: " << cp_num << std::endl;

        if (*rec_grp_offset >= 0)
            return true;
        else
            return false;
    }

    ulint block_for_aligned_lsn(const lsn_t& aligned_lsn) {
        return aligned_lsn / OS_FILE_LOG_BLOCK_SIZE;
    }

    bool read_redo_log(const lsn_t& lsn, const log_group_t* const logGroup) {
        // Iterate REDO log twice:
        // 1. to find the mlog_checkpoint
        // 2. to scan the records ...
        RecordScanner scanner(READ_BYTES_PER_ITERATION);
        scanner.init();
        MLogRecordHandler mlog_handler(0);
        if (read_from_lsn(lsn, logGroup, scanner, mlog_handler)) {
            PRINT_INFO << "Checkpoint found. Checkpoint LSN: " << mlog_handler.checkpoint_lsn() << std::endl;

            // found MLog checkpoint -- now read from the checkpoint lsn ...
            scanner.init();
            RecordHandler default_handler;
            if (read_from_lsn(mlog_handler.checkpoint_lsn(), logGroup, scanner, default_handler)) {
                return true;
            }
        }

        return false;
    }

    uint32_t get_num_blocks_per_iteration(uint64_t current_pos, uint64_t end_pos) {
        uint64_t num_blocks = (end_pos - current_pos) / OS_FILE_LOG_BLOCK_SIZE;
        if (num_blocks > NUM_BLOCKS_PER_ITERATION)
            return NUM_BLOCKS_PER_ITERATION;
        else
            return num_blocks;
    }

    /**
     * Reads the redo log from given LSN.
     * Logic:
     * 1. Find the Log Block which the LSN belongs to
     * 2. Read data in blocks until all the LSNs are read
     * @param log
     * @param lsn
     */
    template<typename RecordHandler>
    bool read_from_lsn(const lsn_t& lsn, const log_group_t* const logGroup,
                       RecordScanner& scanner,
                       RecordHandler& handler) {
        // align lsn to 512 blocks ... -- cos we read blocks ...
        lsn_t aligned_lsn = ut_uint64_align_down(lsn,
                                         OS_FILE_LOG_BLOCK_SIZE);

        lsn_t offset_lsn = (lsn - aligned_lsn);

        // find where is the block
        // lets find the file offset for cp->m_checkpoint_lsn
        uint32_t file_index = 0;
        uint64_t file_offset = 0;
        find_file_offset2(aligned_lsn,
                         logGroup->file_size,
                         logGroup->n_files,
                         logGroup->lsn,
                         logGroup->lsn_offset,
                         &file_index,
                         &file_offset);

        uint64_t calc_offset = log_group_calc_lsn_offset(aligned_lsn, logGroup);
        assert((file_offset == calc_offset) || ((file_offset + logGroup->file_size) == calc_offset));

        std::string file = file_name(file_index);
        PRINT_INFO << "reading " << READ_BYTES_PER_ITERATION << " bytes from " << file << std::endl;

        FILE* stream;
        stream = fopen(file.c_str(), "r");
        if(stream == NULL){
            PRINT_ERR << " unable to open the file: " << file << ", error no.: " << strerror(errno) << std::endl;
            return false;
        }

        if (fseek(stream, file_offset, SEEK_SET) == -1) {
            PRINT_ERR << " unable to seek to the offset position:" << file_offset << " of the file: "
            << file << ", error no.: " << strerror(errno) << std::endl;
            return false;
        }

        unsigned char *buffer = new unsigned char[READ_BYTES_PER_ITERATION];
        std::fill(buffer, buffer + READ_BYTES_PER_ITERATION, 0);

        lsn_t last_lsn_read = aligned_lsn;
        ulint block_for_lsn;

        // read blocks ...
        size_t blocks_read = fread(buffer, OS_FILE_LOG_BLOCK_SIZE, NUM_BLOCKS_PER_ITERATION, stream);

        uint64_t current_file_end = logGroup->file_size;
        uint32_t current_file_index = file_index;
        uint num_reads = 0;
        while(blocks_read > 0) {
            for (size_t i = 0; i < blocks_read; ++i) {
                block_for_lsn = block_for_aligned_lsn(last_lsn_read + OS_FILE_LOG_BLOCK_SIZE);
                ulint record_grp_offset;
                if (!validate_block(buffer + (i * OS_FILE_LOG_BLOCK_SIZE), block_for_lsn, last_lsn_read, logGroup,
                                    &record_grp_offset)) {
                    break;
                }

                if ((i == 0) && (num_reads == 0))
                    scanner.scan(buffer + (i * OS_FILE_LOG_BLOCK_SIZE), offset_lsn);
                else
                    scanner.scan(buffer + (i * OS_FILE_LOG_BLOCK_SIZE));

                last_lsn_read += OS_FILE_LOG_BLOCK_SIZE;
            }

            // done scanning -- lets parse ...
            RecordParser<RecordHandler> parser(&scanner, &handler);

            if (!parser.parse_records(lsn)) {
                PRINT_ERR << "Error parsing log records ..." << std::endl;
                return false;
            }

            // done reading NUM_BLOCKS_PER_ITERATION
            ++num_reads;

            // initialize for the next round ...
            std::fill(buffer, buffer + READ_BYTES_PER_ITERATION, 0);
            scanner.init();

            // ready for the next iteration ...
            if (handler.is_continue_processing()) {
                // what is the current position ?
                uint64_t current_pos = ftell(stream);
                uint32_t blocks_to_read = get_num_blocks_per_iteration(current_pos, current_file_end);
                assert(blocks_to_read <= NUM_BLOCKS_PER_ITERATION);

                blocks_read = fread(buffer, OS_FILE_LOG_BLOCK_SIZE, blocks_to_read, stream);
                // if we are end of file, then switch to the next file ...
                if (blocks_read == 0) {
                    if (feof(stream)) {
                        fclose(stream);

                        // reached eof -- get the next file
                        uint32_t f_index = (++current_file_index) % logGroup->n_files;

                        file = file_name(f_index);
                        // next file is same as the file we started
                        if (f_index == file_index) {
                            // done reading all the files ...
                            // read the file from the beginning but until the offset ...
                            current_file_end = file_offset;
                        } else
                            current_file_end = logGroup->file_size;

                        stream = fopen(file.c_str(), "r");
                        if(stream == NULL){
                            PRINT_ERR << " unable to open the file: " << file << ", error no.: " << strerror(errno) << std::endl;
                            return false;
                        }
                    }
                }
            } else
                break;
        }



        PRINT_INFO << "Last LSN read: " << last_lsn_read << std::endl;

        delete[] buffer;
        fclose(stream);

        return true;
    }

    void print_log_format(const uint32_t& format) {
        switch (format) {
            case 0:
                PRINT_INFO << "Log format: 0 (the old format) \n";
                break;
            case LOG_HEADER_FORMAT_10_2:
                PRINT_INFO << "Log format: 10.2 \n";
                break;
            case LOG_HEADER_FORMAT_10_2 | LOG_HEADER_FORMAT_ENCRYPTED:
                PRINT_INFO << "Log format: 10.2 with encrypted header \n";
                break;
            case LOG_HEADER_FORMAT_10_3:
                PRINT_INFO << "Log format: 10.3 \n";
                break;
            case LOG_HEADER_FORMAT_10_3 | LOG_HEADER_FORMAT_ENCRYPTED:
                PRINT_INFO << "Log format: 10.3 with encrypted header \n";
                break;
            case LOG_HEADER_FORMAT_10_4:
                PRINT_INFO << "Log format: 10.4 \n";
                break;
            default:
                PRINT_ERR << "Unsupported redo log format.";
        }
    }

    void print_log_subformat(const uint32_t& subformat) {
        if (subformat == 1)
            PRINT_INFO << "Fully crash-safe redo log truncate enabled \n";
        else
            PRINT_INFO << "Truncate is separately logged\n";
    }

    void print_checkpoint_info(const Checkpoint* cp,
                               const log_group_t* const logGroup) {
        PRINT_INFO << "checkpoint number: " << cp->m_checkpoint_no << "\n";
        PRINT_INFO << "checkpoint lsn number: " << cp->m_checkpoint_lsn << "\n";
        PRINT_INFO << "physical file index : " << cp->m_log_file_idx << "\n";
        PRINT_INFO << "physical file offset : " << cp->m_offset << "\n";
        //PRINT_INFO << "physical file offset (from log_group_calc_lsn_offset) : " << log_group_calc_lsn_offset(cp->m_checkpoint_lsn, logGroup) << "\n";
        PRINT_INFO << "checkpoint end lsn number (should be 0 or number >= " << cp->m_checkpoint_lsn << "):"
                    << cp->m_checkpoint_end_lsn << "\n";
        PRINT_INFO << "checkpoint offset number: " << cp->m_checkpoint_offset << "\n";
    }

    void print_file0_log_header(const log_group_t* logGroup,
                                const Checkpoint* cp1,
                                const Checkpoint* cp2) {
        PRINT_INFO << "Printing Log Header for the file: " << m_log_file << "\n";
        PRINT_INFO << "Number of Log Files: " << logGroup->n_files << "\n";
        PRINT_INFO << "Size of a log file: " << logGroup->file_size << "\n";

        print_log_format(logGroup->format);
        print_log_subformat(logGroup->subformat);

        PRINT_INFO << "Printing checkpoint-1 info ...\n{\n";
        print_checkpoint_info(cp1, logGroup);
        PRINT_INFO << "\n}\n";

        PRINT_INFO << "Printing checkpoint-2 info ...\n{\n";
        print_checkpoint_info(cp2, logGroup);
        PRINT_INFO << "\n}\n";
    }

    bool read_log_file_header(unsigned char *log, const uint64_t& file_sz, const uint32_t& num_files) {

        if (!validate_block_checksum(log, "checksum error in header"))
            return false;

        PRINT_INFO << "Header checksum matched for log file: " << m_log_file << "\n";

        log_group_t* logGroup = new log_group_t;

        // Lets hard-code this for now -- ideally, we should
        // read the directory and find how many ib_logfile_% are there.
        logGroup->n_files = 2;
        logGroup->file_size = file_sz;

        // set start LSN and its offset ...
        logGroup->lsn = LOG_START_LSN;
        logGroup->lsn_offset = LOG_FILE_HDR_SIZE;

        // read the log format ..
        /** Log file header format identifier (32-bit unsigned big-endian integer).
        This used to be called LOG_GROUP_ID and always written as 0,
        because InnoDB never supported more than one copy of the redo log. */
        logGroup->format = mach_read_from_4(log + LOG_HEADER_FORMAT);

        // the sub-format -- subformat is present for log formats > 0
        if (logGroup -> format > 0) {
            // read subformat
            /** Redo log subformat (originally 0). In format version 0, the
                LOG_FILE_START_LSN started here, 4 bytes earlier than LOG_HEADER_START_LSN,
                which the LOG_FILE_START_LSN was renamed to.
                Subformat 1 is for the fully redo-logged TRUNCATE
                (no MLOG_TRUNCATE records or extra log checkpoints or log files) */
            logGroup->subformat = mach_read_from_4(log + LOG_HEADER_SUBFORMAT);
        } else
            logGroup->subformat = 0;

        // read the creator ...
        char creator[LOG_HEADER_CREATOR_END - LOG_HEADER_CREATOR + 1];

        memcpy(creator, log + LOG_HEADER_CREATOR, sizeof creator);
        /* Ensure that the string is NUL-terminated. */
        creator[LOG_HEADER_CREATOR_END - LOG_HEADER_CREATOR] = 0;

        Checkpoint* cp1 = read_checkpoint_1(log, logGroup);
        if (cp1 == NULL)
            return false;

        Checkpoint* cp2 = read_checkpoint_2(log, logGroup);
        if (cp2 == NULL)
            return false;

        print_file0_log_header(logGroup, cp1, cp2);

        if (cp1->m_checkpoint_no >= cp2->m_checkpoint_lsn) {
            // set start LSN and its offset ...
            logGroup->lsn = cp1->m_checkpoint_lsn;
            logGroup->lsn_offset = cp1->m_checkpoint_offset;
            read_redo_log(cp1->m_checkpoint_lsn, logGroup);

            if (cp1->m_checkpoint_end_lsn > cp1->m_checkpoint_lsn)
                read_redo_log(cp1->m_checkpoint_end_lsn, logGroup);
            else
                read_redo_log(cp1->m_checkpoint_lsn, logGroup);

        } else {
            // set start LSN and its offset ...
            logGroup->lsn = cp2->m_checkpoint_lsn;
            logGroup->lsn_offset = cp2->m_checkpoint_offset;

            if (cp2->m_checkpoint_end_lsn > cp2->m_checkpoint_lsn)
                read_redo_log(cp2->m_checkpoint_end_lsn, logGroup);
            else
                read_redo_log(cp2->m_checkpoint_lsn, logGroup);
        }

        return true;
    }

    /**
     * Read ib_logfile0 -- ib_logfile0 is special because it contains
     * following blocks: header, checkpoint page 1, empty,
     *      checkpoint page 2, redo log page(s)
     * They all are 512 byte pages.
     */
    void parse_ib_log_0(unsigned char *log, const uint64_t& file_size, const uint32_t& num_files) {
        read_log_file_header(log, file_size, num_files);
    }

    void parse_ib_log_n(unsigned char *log) {

    }

public:
    LogReader(const char* _data_dir,
              const char* _file) : m_data_dir(_data_dir), m_log_file(_file), m_log_index(log_index()){}

    bool init() {
        ut_crc32_init();
        return true;
    }

    bool read() {
        int fd = open(m_log_file, O_RDONLY);
        if(fd < 0){
            PRINT_ERR << " unable to open the file: " << m_log_file << std::endl;
            return false;
        }

        struct stat stat_buf;
        int err = fstat(fd, &stat_buf);
        if(err < 0){
            PRINT_ERR << " unable to read file stats. Descriptor:" << fd << ", file: "
                    << m_log_file << std::endl;
            close(fd);
            return false;
        }

        // The log file must be a multiple of block size
        if (is_file_multiple_of_block_size(&stat_buf)) {
            PRINT_ERR << "Log file " << m_log_file
                        << " size " << stat_buf.st_size << " is not a"
                                               " multiple of 512 bytes";
            return -1;
        }

        unsigned char *log = (unsigned char *)mmap(NULL, stat_buf.st_size,
                         PROT_READ,MAP_SHARED,
                         fd,0);
        if(log == MAP_FAILED){
            PRINT_ERR << " Memory mapping failed for the file: " << m_log_file
                        << std::endl;
            close(fd);
            return false;
        }

        uint32_t num_files = 2;
        if (m_log_index == 0)
            parse_ib_log_0(log, stat_buf.st_size, num_files);
        else
            parse_ib_log_n(log);

        // Free up the space ...
        err = munmap(log, stat_buf.st_size);
        if(err != 0){
            PRINT_ERR << " Error un-mapping redo log."
                      << std::endl;
            close(fd);
            return false;
        }

        close(fd);
        return true;
    }
};

#endif //MYSQL_LOG_READER_H
