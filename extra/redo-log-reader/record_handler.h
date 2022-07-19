//
// Created by Thejaka Kanewala on 6/17/22.
//

#ifndef MYSQL_RECORD_HANDLER_H
#define MYSQL_RECORD_HANDLER_H

#include <trx0undo.h>
#include "util.h"

class RecordHandler {
private:
    bool m_continue;

    int64_t calculate_bytes_consumed_4bytes(const unsigned char* buffer, ulint* val, const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        (*val) = mach_parse_compressed(&ptr, end_ptr);
        return (ptr - buffer);
    }
protected:
    int64_t handle_mlog_file_name(const unsigned char* buffer, uint32_t space_id, uint32_t page_id, ulint len,
                                  const unsigned char* end_ptr) {
        // nothing to do ..
        return len;
    }

    int64_t handle_mlog_file_delete(const unsigned char* buffer, uint32_t space_id, uint32_t page_id, ulint len,
                                    const unsigned char* end_ptr) {
        return len;
    }

    int64_t handle_mlog_file_create2(const unsigned char* buffer, uint32_t space_id, uint32_t page_id, ulint len,
                                     const unsigned char* end_ptr) {
        return len;
    }

    int64_t handle_mlog_file_rename2(const unsigned char* buffer, uint32_t space_id, uint32_t page_id, ulint len,
                                     const unsigned char* end_ptr) {
        // we have the new name ...
        // new name length ...
        ulint new_name_len = mach_read_from_2(buffer + len);
        return len + 2 + new_name_len;
    }

    int64_t handle_mlog_file_x(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id, uint32_t page_id,
                               const unsigned char* end_ptr) {
        uint64_t offset = 0;
        ulint len = mach_read_from_2(buffer);
        offset += 2; // length 2 bytes ...

        switch (type) {
            case MLOG_FILE_NAME:
                return (2 + handle_mlog_file_name(buffer, space_id, page_id, len, end_ptr));
            case MLOG_FILE_DELETE:
                return (2 + handle_mlog_file_delete(buffer, space_id, page_id, len, end_ptr));
            case MLOG_FILE_CREATE2:
                return (2 + handle_mlog_file_create2(buffer, space_id, page_id, len, end_ptr));
            case MLOG_FILE_RENAME2:
                return (2 + handle_mlog_file_rename2(buffer, space_id, page_id, len, end_ptr));
            default:{
                assert(false);
            }
        }
    }

    int64_t handle_mlog_index_load(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id, uint32_t page_id,
                                   const unsigned char* end_ptr) {
        return 0;
    }

    int64_t handle_mlog_truncate(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id, uint32_t page_id,
                                 const unsigned char* end_ptr) {
        // truncate contains the truncated LSN
        return sizeof (lsn_t);
    }

    int64_t handle_mlog_nbytes(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id, uint32_t page_id,
                               const unsigned char* end_ptr) {
        // mlog_nbytes contain an offset and then the value ...
        // offset is 2 bytes -- this is the offset within the page ...
        ulint page_offset = mach_read_from_2(buffer);
        int64_t buffer_offset = 2; //cos 2 bytes is for the page offset ...

        switch(type) {
            case MLOG_1BYTE:
                return (buffer_offset + handle_mlog_1byte(type, (buffer + buffer_offset), space_id, page_id,
                                                          page_offset, end_ptr));
            case MLOG_2BYTES:
                return (buffer_offset + handle_mlog_2bytes(type, (buffer + buffer_offset), space_id, page_id,
                                                           page_offset, end_ptr));
            case MLOG_4BYTES:
                return (buffer_offset + handle_mlog_4bytes(type, (buffer + buffer_offset), space_id, page_id,
                                                           page_offset, end_ptr));
            case MLOG_8BYTES:
                return (buffer_offset + handle_mlog_8bytes(type, (buffer + buffer_offset), space_id, page_id,
                                                           page_offset, end_ptr));
            default:
                assert(false);
        }
    }

    int64_t handle_mlog_1byte(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                               uint32_t page_id,
                               ulint page_offset,
                              const unsigned char* end_ptr) {
        ulint val;
        return calculate_bytes_consumed_4bytes(buffer, &val, end_ptr);
    }

    int64_t handle_mlog_2bytes(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                              uint32_t page_id,
                              ulint page_offset,
                               const unsigned char* end_ptr) {
        ulint val;
        return calculate_bytes_consumed_4bytes(buffer, &val, end_ptr);
    }

    int64_t handle_mlog_4bytes(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                               uint32_t page_id,
                               ulint page_offset,
                               const unsigned char* end_ptr) {
        ulint val;
        return calculate_bytes_consumed_4bytes(buffer, &val, end_ptr);
    }

    int64_t handle_mlog_8bytes(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                               uint32_t page_id,
                               ulint page_offset,
                               const unsigned char* end_ptr) {
        // could take variable number of bytes ...
        const unsigned char* ptr = buffer;
        ib_uint64_t	dval = mach_u64_parse_compressed(&ptr, end_ptr);

        int64_t buffer_offset = (ptr - buffer);
        return buffer_offset;
    }

    int64_t handle_mlog_init_file_page2(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                        uint32_t page_id,
                                        const unsigned char* end_ptr) {
        return 0;
    }

    int64_t handle_mlog_write_string(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                     uint32_t page_id,
                                     const unsigned char* end_ptr) {
        // a string includes the page offset; i.e., to where the string should be written in a page
        // and the string's length.
        int64_t page_offset = mach_read_from_2(buffer);
        int64_t len = mach_read_from_2(buffer + 2);
        std::string s((const char*)(buffer+4), len);
        return (2 + 2 + len);
    }

    int64_t handle_mlog_undo_hdr_reuse(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                       uint32_t page_id,
                                       const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        trx_id_t trx_id = mach_u64_parse_compressed(&ptr, end_ptr);
        return (ptr - buffer);
    }

    int64_t handle_mlog_undo_hdr_create(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                       uint32_t page_id,
                                        const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        trx_id_t trx_id = mach_u64_parse_compressed(&ptr, end_ptr);
        return (ptr - buffer);
    }

    int64_t handle_index_info(const char* operation,
                              const mlog_id_t& type,
                              const unsigned char* buffer,
                              uint32_t space_id,
                              uint32_t page_id,
                              const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        // 1. first the index ...

        // number of fields in the index ..
        ulint n_fields = mach_read_from_2(ptr);
        ptr += 2;
        ulint n_uniq_fields = mach_read_from_2(ptr);
        ptr += 2;

        // if n_fields == n_uniq_fields, then this is a normal index; otherwise
        // we are dealing with a clustered index ...
        for (ulint i = 0; i < n_fields; i++) {
            ulint	len = mach_read_from_2(ptr);
            ptr += 2;
            /* The high-order bit of len is the NOT NULL flag;
            the rest is 0 or 0x7fff for variable-length fields,
            and 1..0x7ffe for fixed-length fields. */
        }

        return (ptr - buffer);
    }

    int64_t handle_mlog_rec_insert(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                        uint32_t page_id,
                                        const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        // 1. first the index ...
        // no index stuff for non-compact records ....

        // 2. now the record itself ...
        // page offset ...
        ulint offset = mach_read_from_2(ptr);
        ptr += 2;

        ulint end_seg_len = mach_parse_compressed(&ptr, end_ptr);

        if (end_seg_len & 0x1UL) {
            /* Read the info bits */
            ulint info_and_status_bits = mach_read_from_1(ptr);
            ptr++;

            ulint origin_offset = mach_parse_compressed(&ptr, end_ptr);
            ulint mismatch_index = mach_parse_compressed(&ptr, end_ptr);
        }

        ptr += (end_seg_len >> 1);

        return (ptr - buffer);
    }

    int64_t handle_mlog_rec_insert_comp(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                        uint32_t page_id,
                                        const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        // 1. first the index ...
        ptr += handle_index_info("insert_comp", type, buffer, space_id, page_id, end_ptr);
        ptr += handle_mlog_rec_insert(type, ptr, space_id, page_id, end_ptr);

        return (ptr - buffer);
    }

    int64_t handle_mlog_rec_delete_mark(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                        uint32_t page_id,
                                        const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;

        // 2. now the record itself ...
        // page offset ...
        ulint flags = mach_read_from_1(ptr);
        ptr++;
        ulint val = mach_read_from_1(ptr);
        ptr++;

        ulint pos = mach_parse_compressed(&ptr, end_ptr);

        ulint roll_ptr = trx_read_roll_ptr(ptr);
        ptr += DATA_ROLL_PTR_LEN;

        ulint trx_id = mach_u64_parse_compressed(&ptr, end_ptr);

        ulint offset = mach_read_from_2(ptr);
        ptr += 2;

        offset = mach_read_from_2(ptr);
        ptr += 2;

        return (ptr - buffer);
    }

    int64_t handle_mlog_rec_delete_mark_comp(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                             uint32_t page_id,
                                             const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        // 1. first the index ...
        ptr += handle_index_info("delete_comp", type, buffer, space_id, page_id, end_ptr);
        ptr += handle_mlog_rec_delete_mark(type, ptr, space_id, page_id, end_ptr);

        return (ptr - buffer);
    }

    int64_t handle_secondary_index_delete(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                        uint32_t page_id,
                                        const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        ulint val = mach_read_from_1(ptr);
        ptr++;

        ulint offset = mach_read_from_2(ptr);
        ptr += 2;

        return (ptr - buffer);
    }

    int64_t handle_rec_update_inplace(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                          uint32_t page_id,
                                          const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        ulint flags = mach_read_from_1(ptr);
        ptr++;

        ulint pos = mach_parse_compressed(&ptr, end_ptr);

        roll_ptr_t roll_ptr = trx_read_roll_ptr(ptr);
        ptr += DATA_ROLL_PTR_LEN;

        trx_id_t trx_id = mach_u64_parse_compressed(&ptr, end_ptr);

        ulint rec_offset = mach_read_from_2(ptr);
        ptr += 2;

        // index info ....
        ulint info_bits = mach_read_from_1(ptr);
        ptr++;
        ulint n_fields = mach_parse_compressed(&ptr, end_ptr);

        for (ulint i = 0; i < n_fields; i++) {
            ulint	field_no;
            field_no = mach_parse_compressed(&ptr, end_ptr);
            ulint len = mach_parse_compressed(&ptr, end_ptr);

            if (len != UNIV_SQL_NULL) {
                ptr += len;
            }
        }

        return (ptr - buffer);
    }

    int64_t handle_rec_update_inplace_comp(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                      uint32_t page_id,
                                      const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        // 1. first the index ...
        ptr += handle_index_info("update_inplace_comp", type, buffer, space_id, page_id, end_ptr);
        ptr += handle_rec_update_inplace(type, ptr, space_id, page_id, end_ptr);
        return (ptr - buffer);
    }

    int64_t handle_delete_record_list(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                                   uint32_t page_id,
                                                   const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        ulint offset = mach_read_from_2(ptr);
        ptr += 2;
        return (ptr - buffer);
    }

    int64_t handle_delete_record_list_comp(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                      uint32_t page_id,
                                      const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        // 1. first the index ...
        ptr += handle_index_info("delete_record_list", type, buffer, space_id, page_id, end_ptr);
        ptr += handle_delete_record_list(type, ptr, space_id, page_id, end_ptr);
        return (ptr - buffer);
    }

    int64_t handle_copy_rec_list_to_created_page(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                      uint32_t page_id,
                                      const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        ulint log_data_len = mach_read_from_4(ptr);
        ptr += 4;
        ptr += log_data_len;
        return (ptr - buffer);
    }

    int64_t handle_copy_rec_list_to_created_page_comp(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                                 uint32_t page_id,
                                                 const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        // 1. first the index ...
        ptr += handle_index_info("copy_rec_list_to_created_page", type, buffer, space_id, page_id, end_ptr);
        ptr += handle_copy_rec_list_to_created_page(type, ptr, space_id, page_id, end_ptr);
        return (ptr - buffer);
    }

    int64_t handle_page_reorganize(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                                 uint32_t page_id,
                                                 const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;

        if (type != MLOG_PAGE_REORGANIZE) {
            ptr += handle_index_info("page_reorganize", type, buffer, space_id, page_id, end_ptr);
        }

        if (type == MLOG_ZIP_PAGE_REORGANIZE) {
            ulint level = mach_read_from_1(ptr);
            ++ptr;
        }

        return (ptr - buffer);
    }

    int64_t handle_page_create(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                   uint32_t page_id,
                                   const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        return (ptr - buffer);
    }

    int64_t handle_add_undo_rec(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                               uint32_t page_id,
                               const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;

        ulint len = mach_read_from_2(ptr);
        ptr += 2;

        ptr += len;
        return (ptr - buffer);
    }

    int64_t handle_undo_erase_page_end(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                uint32_t page_id,
                                const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        return (ptr - buffer);
    }

    int64_t handle_undo_init(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                       uint32_t page_id,
                                       const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;

        ulint undo_type = mach_parse_compressed(&ptr, end_ptr);
        return (ptr - buffer);
    }

    int64_t handle_rec_min_mark(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                             uint32_t page_id,
                             const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;

        ulint offset = mach_read_from_2(ptr);
        ptr += 2;
        return (ptr - buffer);
    }

    int64_t handle_mlog_rec_delete(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                uint32_t page_id,
                                const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;

        if (type == MLOG_COMP_REC_DELETE)
            ptr += handle_index_info("delete_rec", type, buffer, space_id, page_id, end_ptr);

        ulint offset = mach_read_from_2(ptr);
        ptr += 2;

        return (ptr - buffer);
    }

    int64_t handle_bitmap_init(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                   uint32_t page_id,
                                   const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        return (ptr - buffer);
    }

    int64_t handle_zip_write_node_ptr(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                               uint32_t page_id,
                               const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;

        ulint offset = mach_read_from_2(ptr);
        ptr += 2;
        ulint z_offset = mach_read_from_2(ptr);
        ptr += 2;

        ptr += REC_NODE_PTR_SIZE;

        return (ptr - buffer);
    }

    int64_t handle_zip_write_blob_ptr(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                      uint32_t page_id,
                                      const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;

        ulint offset = mach_read_from_2(ptr);
        ptr += 2;
        ulint z_offset = mach_read_from_2(ptr);
        ptr += 2;

        ptr += BTR_EXTERN_FIELD_REF_SIZE;

        return (ptr - buffer);
    }

    int64_t handle_zip_write_header(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                      uint32_t page_id,
                                      const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;

        ulint offset = (ulint) *ptr++;
        ulint len = (ulint) *ptr++;
        ptr += len;

        return (ptr - buffer);
    }

    int64_t handle_zip_page_compress(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                    uint32_t page_id,
                                    const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;

        ulint size = mach_read_from_2(ptr);
        ptr += 2;
        ulint trailer_size = mach_read_from_2(ptr);
        ptr += 2;

        ptr += (8 + size + trailer_size);

        return (ptr - buffer);
    }

    int64_t handle_zip_page_compress_no_data(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                     uint32_t page_id,
                                     const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        ptr += handle_index_info("zip_page_compress_no_data", type, buffer, space_id, page_id, end_ptr);
        ulint level = mach_read_from_1(ptr);
        ptr += 1;
        return (ptr - buffer);
    }

    int64_t handle_file_crypt_data(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id,
                                             uint32_t page_id,
                                             const unsigned char* end_ptr) {
        const unsigned char* ptr = buffer;
        uint entry_size =
                4 + // size of space_id
                        2 + // size of offset
                        1 + // size of type
                        1 + // size of iv-len
                        4 +  // size of min_key_version
                        4 +  // size of key_id
                        1; // fil_encryption_t

        ulint en_space_id = mach_read_from_4(ptr);
        ptr += 4;
        uint offset = mach_read_from_2(ptr);
        ptr += 2;
        uint en_type = mach_read_from_1(ptr);
        ptr += 1;
        uint len = mach_read_from_1(ptr);
        ptr += 1;

        uint min_key_version = mach_read_from_4(ptr);
        ptr += 4;

        uint key_id = mach_read_from_4(ptr);
        ptr += 4;

        fil_encryption_t encryption = (fil_encryption_t)mach_read_from_1(ptr);
        ptr +=1;

        ptr += len;
        //TODO

        return (ptr - buffer);
    }

    virtual int64_t handle_mlog_checkpoint(const unsigned char* buffer, const unsigned char* end_ptr) {
        // mlog checkpoint contains the checkpoint LSN which is 8 bytes ...
        lsn_t lsn = mach_read_from_8(buffer);
        PRINT_INFO << "Checkpoint LSN: " << lsn << std::endl;
        return (SIZE_OF_MLOG_CHECKPOINT - 1);
    }

public:

    RecordHandler():m_continue(true){}

    int64_t handle_system_records(const mlog_id_t& type, const unsigned char* buffer, const lsn_t& lsn,
                                  const unsigned char* end_ptr) {
        if (type == MLOG_CHECKPOINT)
            return handle_mlog_checkpoint(buffer, end_ptr);

        return 0;
    }

    void suspend_processing() {
        m_continue = false;
    }

    void resume_processing() {
        m_continue = true;
    }

    bool is_continue_processing() {
        return m_continue;
    }

    template<typename mlog_id_t>
    int64_t operator()(const mlog_id_t& type, const unsigned char* buffer, uint32_t space_id, uint32_t page_id,
            const lsn_t& lsn, const unsigned char* end_ptr) {

        switch (type) {
            case MLOG_FILE_NAME:
            case MLOG_FILE_DELETE:
            case MLOG_FILE_CREATE2:
            case MLOG_FILE_RENAME2:
                return handle_mlog_file_x(type, buffer, space_id, page_id, end_ptr);
            case MLOG_INDEX_LOAD:
                return handle_mlog_index_load(type, buffer, space_id, page_id, end_ptr);
            case MLOG_TRUNCATE:
                return handle_mlog_truncate(type, buffer, space_id, page_id, end_ptr);
            case MLOG_1BYTE:
            case MLOG_2BYTES:
            case MLOG_4BYTES:
            case MLOG_8BYTES:
                return handle_mlog_nbytes(type, buffer, space_id, page_id, end_ptr);
            case MLOG_COMP_REC_INSERT:
                return handle_mlog_rec_insert_comp(type, buffer, space_id, page_id, end_ptr);
            case MLOG_REC_INSERT:
                return handle_mlog_rec_insert(type, buffer, space_id, page_id, end_ptr);
            case MLOG_REC_CLUST_DELETE_MARK:
                return handle_mlog_rec_delete_mark(type, buffer, space_id, page_id, end_ptr);
            case MLOG_COMP_REC_CLUST_DELETE_MARK:
                return handle_mlog_rec_delete_mark_comp(type, buffer, space_id, page_id, end_ptr);
            case MLOG_REC_SEC_DELETE_MARK:
                return handle_secondary_index_delete(type, buffer, space_id, page_id, end_ptr);
            case MLOG_REC_UPDATE_IN_PLACE:
                return handle_rec_update_inplace(type, buffer, space_id, page_id, end_ptr);
            case MLOG_COMP_REC_UPDATE_IN_PLACE:
                return handle_rec_update_inplace_comp(type, buffer, space_id, page_id, end_ptr);
            case MLOG_LIST_END_DELETE:
            case MLOG_LIST_START_DELETE:
                return handle_delete_record_list(type, buffer, space_id, page_id, end_ptr);
            case MLOG_COMP_LIST_END_DELETE:
            case MLOG_COMP_LIST_START_DELETE:
                return handle_delete_record_list_comp(type, buffer, space_id, page_id, end_ptr);
            case MLOG_LIST_END_COPY_CREATED:
                return handle_copy_rec_list_to_created_page(type, buffer, space_id, page_id, end_ptr);
            case MLOG_COMP_LIST_END_COPY_CREATED:
                return handle_copy_rec_list_to_created_page_comp(type, buffer, space_id, page_id, end_ptr);
            case MLOG_PAGE_REORGANIZE:
            case MLOG_COMP_PAGE_REORGANIZE:
            case MLOG_ZIP_PAGE_REORGANIZE:
                return handle_page_reorganize(type, buffer, space_id, page_id, end_ptr);
            case MLOG_PAGE_CREATE:
            case MLOG_COMP_PAGE_CREATE:
            case MLOG_PAGE_CREATE_RTREE:
            case MLOG_COMP_PAGE_CREATE_RTREE:
                return handle_page_create(type, buffer, space_id, page_id, end_ptr);
            case MLOG_UNDO_INSERT:
                return handle_add_undo_rec(type, buffer, space_id, page_id, end_ptr);
            case MLOG_UNDO_ERASE_END:
                return handle_undo_erase_page_end(type, buffer, space_id, page_id, end_ptr);
            case MLOG_UNDO_INIT:
                return handle_undo_init(type, buffer, space_id, page_id, end_ptr);
            case MLOG_UNDO_HDR_REUSE:
                return handle_mlog_undo_hdr_reuse(type, buffer, space_id, page_id, end_ptr);
            case MLOG_UNDO_HDR_CREATE:
                return handle_mlog_undo_hdr_create(type, buffer, space_id, page_id, end_ptr);
            case MLOG_REC_MIN_MARK:
            case MLOG_COMP_REC_MIN_MARK:
                return handle_rec_min_mark(type, buffer, space_id, page_id, end_ptr);
            case MLOG_REC_DELETE:
            case MLOG_COMP_REC_DELETE:
                return handle_mlog_rec_delete(type, buffer, space_id, page_id, end_ptr);
            case MLOG_IBUF_BITMAP_INIT:
                return handle_bitmap_init(type, buffer, space_id, page_id, end_ptr);
            case MLOG_INIT_FILE_PAGE2:
                return handle_mlog_init_file_page2(type, buffer, space_id, page_id, end_ptr);
            case MLOG_WRITE_STRING:
                return handle_mlog_write_string(type, buffer, space_id, page_id, end_ptr);
            case MLOG_ZIP_WRITE_NODE_PTR:
                return handle_zip_write_node_ptr(type, buffer, space_id, page_id, end_ptr);
            case MLOG_ZIP_WRITE_BLOB_PTR:
                return handle_zip_write_blob_ptr(type, buffer, space_id, page_id, end_ptr);
            case MLOG_ZIP_WRITE_HEADER:
                return handle_zip_write_header(type, buffer, space_id, page_id, end_ptr);
            case MLOG_ZIP_PAGE_COMPRESS:
                return handle_zip_page_compress(type, buffer, space_id, page_id, end_ptr);
            case MLOG_ZIP_PAGE_COMPRESS_NO_DATA:
                return handle_zip_page_compress_no_data(type, buffer, space_id, page_id, end_ptr);
            case MLOG_FILE_WRITE_CRYPT_DATA:
                return handle_file_crypt_data(type, buffer, space_id, page_id, end_ptr);
            default:
                ib::error() << "Unidentified redo log record type "
                            << ib::hex(unsigned(type));
                return -1;
        }

    }
};


class MLogRecordHandler : public RecordHandler {
private:
    bool mlog_checkpoint_found;
    lsn_t given_cp_lsn;
public:
    MLogRecordHandler(const lsn_t& checkpoint_lsn): mlog_checkpoint_found(false), given_cp_lsn(checkpoint_lsn) {
    }

    bool is_mlog_cp_found() {
        return mlog_checkpoint_found;
    }

    lsn_t checkpoint_lsn() {
        return given_cp_lsn;
    }

    virtual int64_t handle_mlog_checkpoint(const unsigned char* buffer, const unsigned char* end_ptr) {

        // mlog checkpoint contains the checkpoint LSN which is 8 bytes ...
        lsn_t lsn = mach_read_from_8(buffer);

        if (given_cp_lsn == 0) {
            given_cp_lsn = lsn;
            mlog_checkpoint_found = true;
        } else {
            if (given_cp_lsn == lsn)
                mlog_checkpoint_found = true;
            else {
                PRINT_INFO << "Checkpoints mismatch. Given CP LSN: " << given_cp_lsn << ", actual lsn read: " << lsn
                           << std::endl;
                return -1;
            }
        }

        PRINT_INFO << "Checkpoint LSN: " << lsn << std::endl;

        // once we find the m_log_checkpoint, we do not want to continue the processing ...
        suspend_processing();

        return (SIZE_OF_MLOG_CHECKPOINT - 1);
    }


};

#endif //MYSQL_RECORD_HANDLER_H
