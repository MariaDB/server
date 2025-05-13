// Custom hash function for char*
#include <cstddef>
#include <string.h>

struct table_name_hash_fn {
    size_t operator()(const char* str) const {
        size_t hash_value = 0;
        for (int i = 0; str[i] != '\0'; i++) {
            hash_value = (hash_value * 31) + str[i];
        }
        return hash_value;
    }
};

struct table_name_comparator {
    bool operator()(const char* str1, const char* str2) const {
        return strcmp(str1, str2) == 0;
    }
};